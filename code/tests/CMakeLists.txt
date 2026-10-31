# SPDX-License-Identifier: GPL-2.0-only
# M0 gate: re-run all nine Unicorn probes.
add_test(NAME m0_probes COMMAND unicorn_spike all)

# ---------------------------------------------------------------------------
# Guest blobs: real aarch64 assembly, assembled to a flat .text binary.
# 01-03 use bare blobs (no ELF loader); the loader arrives in M4.
# ---------------------------------------------------------------------------
find_program(DUCKEMU_CLANG_EXECUTABLE clang REQUIRED)
find_program(DUCKEMU_LLVM_OBJCOPY llvm-objcopy REQUIRED)

set(DUCKEMU_BLOB_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/blobs")

function(duckemu_guest_blob out_var source)
  get_filename_component(name "${source}" NAME_WE)
  set(object "${DUCKEMU_BLOB_OUTPUT_DIR}/${name}.o")
  set(binary "${DUCKEMU_BLOB_OUTPUT_DIR}/${name}.bin")
  add_custom_command(
    OUTPUT "${binary}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_BLOB_OUTPUT_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} --target=aarch64-linux-android31
            -c "${source}" -o "${object}"
    COMMAND ${DUCKEMU_LLVM_OBJCOPY} -O binary --only-section=.text
            "${object}" "${binary}"
    DEPENDS "${source}"
    COMMENT "Assembling guest blob ${name}.bin")
  set(${out_var} "${binary}" PARENT_SCOPE)
endfunction()

duckemu_guest_blob(BLOB_TLS_READ
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/01_tls_read.S")
duckemu_guest_blob(BLOB_PURE_NATIVE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/02_pure_native.S")
duckemu_guest_blob(BLOB_EXIT_GROUP
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/02_exit_group.S")
duckemu_guest_blob(BLOB_STACK_DUMP
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/02_stack_dump.S")
duckemu_guest_blob(BLOB_PTHREAD_JOIN
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/03_pthread_join.S")
duckemu_guest_blob(BLOB_TRACE_MAIN
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/08_trace_main.S")
duckemu_guest_blob(BLOB_TRACE_EXT
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/08_trace_ext.S")
duckemu_guest_blob(BLOB_ADDRESS_HOOK
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/08_hook.S")

add_custom_target(guest_blobs ALL
  DEPENDS ${BLOB_TLS_READ} ${BLOB_PURE_NATIVE} ${BLOB_EXIT_GROUP}
          ${BLOB_STACK_DUMP} ${BLOB_PTHREAD_JOIN}
          ${BLOB_TRACE_MAIN} ${BLOB_TRACE_EXT} ${BLOB_ADDRESS_HOOK})

# ---------------------------------------------------------------------------
# Guest shared objects for the M4 loader gate. Built for aarch64-linux-android
# with -nostdlib, so every libc import stays UND and the loader has to satisfy
# it from the virtual bionic table.
#
# --pack-dyn-relocs=none keeps the output in plain RELA form; aarch64-android
# defaults to DT_ANDROID_RELA (APS2), which the M4 loader refuses on purpose.
# ---------------------------------------------------------------------------
set(DUCKEMU_SO_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/so")
set(DUCKEMU_SO_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/so-build")
set(DUCKEMU_SO_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/so-runtime")

set(DUCKEMU_GUEST_SO_FLAGS
    --target=aarch64-linux-android31 -fPIC -shared -nostdlib -O1
    -Wl,--pack-dyn-relocs=none)

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libb.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libb.so "${DUCKEMU_SO_SOURCE_DIR}/libb.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libb.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libb.c"
    COMMENT "Building guest .so libb.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/liblog.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,liblog.so "${DUCKEMU_SO_SOURCE_DIR}/liblog_stub.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/liblog.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/liblog_stub.c"
    COMMENT "Building link-only stub liblog.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libc.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libc.so "${DUCKEMU_SO_SOURCE_DIR}/libc_stub.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libc.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libc_stub.c"
    COMMENT "Building link-only stub libc.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/liba.so"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,liba.so "${DUCKEMU_SO_SOURCE_DIR}/liba.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/liba.so"
            -L"${DUCKEMU_SO_BUILD_DIR}" -Wl,--no-as-needed
            -l:libc.so -l:liblog.so -l:libb.so
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/liba.c"
            "${DUCKEMU_SO_BUILD_DIR}/libb.so"
            "${DUCKEMU_SO_BUILD_DIR}/liblog.so"
            "${DUCKEMU_SO_BUILD_DIR}/libc.so"
    COMMENT "Building guest .so liba.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libtls.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libtls.so "${DUCKEMU_SO_SOURCE_DIR}/libtls.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libtls.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libtls.c"
    COMMENT "Building guest .so libtls.so (TLSDESC)")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libtlsdtv.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -fno-stack-protector
            -Wl,-soname,libtlsdtv.so "${DUCKEMU_SO_SOURCE_DIR}/libtlsdtv.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libtlsdtv.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libtlsdtv.c"
    COMMENT "Building guest .so libtlsdtv.so (dynamic TLS/DTV)")

# The same source with DT_NEEDED libc.so, so it can be loaded before libc in
# one graph and receive a static TLS slot. It links against the link-only stub
# purely to record the dependency; the runtime resolves libc.so to the ROM.
add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libtlsdtv_static.so"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -fno-stack-protector
            -Wl,-soname,libtlsdtv_static.so "${DUCKEMU_SO_SOURCE_DIR}/libtlsdtv.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libtlsdtv_static.so"
            -L"${DUCKEMU_SO_BUILD_DIR}" -Wl,--no-as-needed -l:libc.so
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libtlsdtv.c"
            "${DUCKEMU_SO_BUILD_DIR}/libc.so"
    COMMENT "Building guest .so libtlsdtv_static.so (static TLS slot)")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libmissing.so"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libmissing.so "${DUCKEMU_SO_SOURCE_DIR}/libmissing.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libmissing.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libmissing.c"
    COMMENT "Building guest .so libmissing.so")

# 04d_dlopen: a runtime-loaded plugin (own constructor) and a caller that uses
# the ld-android.so entry points the way libc's dlopen/dlsym do.
add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libplug.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libplug.so "${DUCKEMU_SO_SOURCE_DIR}/libplug.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libplug.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libplug.c"
    COMMENT "Building guest .so libplug.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libdlcaller.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libdlcaller.so "${DUCKEMU_SO_SOURCE_DIR}/libdlcaller.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libdlcaller.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libdlcaller.c"
    COMMENT "Building guest .so libdlcaller.so")

# 04e_dup_fcntl: raw-syscall fixture for dup/dup3/fcntl.
add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libfd.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -fno-stack-protector -fno-builtin
            -Wl,-soname,libfd.so "${DUCKEMU_SO_SOURCE_DIR}/libfd.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libfd.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libfd.c"
    COMMENT "Building guest .so libfd.so")

# 04f_vfs_write: raw-syscall fixture for the writable sandbox.
add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libwr.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -fno-stack-protector -fno-builtin
            -Wl,-soname,libwr.so "${DUCKEMU_SO_SOURCE_DIR}/libwr.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libwr.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libwr.c"
    COMMENT "Building guest .so libwr.so")

# 04g_event_loop: raw-syscall fixture for eventfd/timerfd/poll/epoll.
add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libev.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -fno-stack-protector -fno-builtin
            -Wl,-soname,libev.so "${DUCKEMU_SO_SOURCE_DIR}/libev.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libev.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libev.c"
    COMMENT "Building guest .so libev.so")

# 04h_device_profile: raw-syscall fixture for uname/proc/profile.
add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libdev.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -fno-stack-protector -fno-builtin
            -Wl,-soname,libdev.so "${DUCKEMU_SO_SOURCE_DIR}/libdev.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libdev.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libdev.c"
    COMMENT "Building guest .so libdev.so")

# The runtime view of the fixture: only what the loader may legitimately open.
# liblog.so and libc.so exist but are not ELF, so a loader that consults the
# filesystem before the virtual module table fails here instead of passing.
file(MAKE_DIRECTORY "${DUCKEMU_SO_RUNTIME_DIR}")
file(WRITE "${DUCKEMU_SO_RUNTIME_DIR}/liblog.so" "must not be read\n")
file(WRITE "${DUCKEMU_SO_RUNTIME_DIR}/libc.so" "must not be read\n")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_RUNTIME_DIR}/liba.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libb.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libmissing.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libtls.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libtlsdtv.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/static/libtlsdtv_static.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libplug.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libdlcaller.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libfd.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libwr.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libev.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libdev.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/liba.so" "${DUCKEMU_SO_RUNTIME_DIR}/liba.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libb.so" "${DUCKEMU_SO_RUNTIME_DIR}/libb.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libmissing.so" "${DUCKEMU_SO_RUNTIME_DIR}/libmissing.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libtls.so" "${DUCKEMU_SO_RUNTIME_DIR}/libtls.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libtlsdtv.so" "${DUCKEMU_SO_RUNTIME_DIR}/libtlsdtv.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_RUNTIME_DIR}/static"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libtlsdtv_static.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/static/libtlsdtv_static.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libplug.so" "${DUCKEMU_SO_RUNTIME_DIR}/libplug.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libdlcaller.so" "${DUCKEMU_SO_RUNTIME_DIR}/libdlcaller.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libfd.so" "${DUCKEMU_SO_RUNTIME_DIR}/libfd.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libwr.so" "${DUCKEMU_SO_RUNTIME_DIR}/libwr.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libev.so" "${DUCKEMU_SO_RUNTIME_DIR}/libev.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libdev.so" "${DUCKEMU_SO_RUNTIME_DIR}/libdev.so"
    DEPENDS "${DUCKEMU_SO_BUILD_DIR}/liba.so"
            "${DUCKEMU_SO_BUILD_DIR}/libb.so"
            "${DUCKEMU_SO_BUILD_DIR}/libmissing.so"
            "${DUCKEMU_SO_BUILD_DIR}/libtls.so"
            "${DUCKEMU_SO_BUILD_DIR}/libtlsdtv.so"
            "${DUCKEMU_SO_BUILD_DIR}/libtlsdtv_static.so"
            "${DUCKEMU_SO_BUILD_DIR}/libplug.so"
            "${DUCKEMU_SO_BUILD_DIR}/libdlcaller.so"
            "${DUCKEMU_SO_BUILD_DIR}/libfd.so"
            "${DUCKEMU_SO_BUILD_DIR}/libwr.so"
            "${DUCKEMU_SO_BUILD_DIR}/libev.so"
            "${DUCKEMU_SO_BUILD_DIR}/libdev.so")

add_custom_target(guest_so ALL
    DEPENDS "${DUCKEMU_SO_RUNTIME_DIR}/liba.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libb.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libmissing.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libtls.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libtlsdtv.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/static/libtlsdtv_static.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libplug.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libdlcaller.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libfd.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libwr.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libev.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libdev.so")

# ---------------------------------------------------------------------------
# Test support
# ---------------------------------------------------------------------------
add_library(duckemu_test_support STATIC support/blob_loader.cpp)
target_include_directories(duckemu_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_test_support PUBLIC duckemu_common)
target_compile_definitions(duckemu_test_support PRIVATE
  DUCKEMU_BLOB_DIR="${DUCKEMU_BLOB_OUTPUT_DIR}")
duckemu_set_warnings(duckemu_test_support)

function(duckemu_add_integration_test name)
  add_executable(${name} "integration/${name}.cpp")
  target_link_libraries(${name} PRIVATE duckemu_runtime duckemu_test_support)
  target_compile_definitions(${name} PRIVATE
    DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces")
  add_dependencies(${name} guest_blobs)
  add_test(NAME ${name} COMMAND ${name})
  # Guest threads have no host timeout by design (M3 design §12): a spinning
  # guest hangs the caller, so the test runner is the backstop.
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
  duckemu_set_warnings(${name})
endfunction()

duckemu_add_integration_test(01_tls_read)
duckemu_add_integration_test(02_pure_native)
duckemu_add_integration_test(02_stack_dump)
duckemu_add_integration_test(03_pthread_join)
duckemu_add_integration_test(08_address_hook)

# The instruction trace needs capstone; only register its gate when the
# analysis build is enabled.
if(DUCKEMU_ENABLE_TRACE)
  duckemu_add_integration_test(08_insn_trace)
  duckemu_add_integration_test(08_insn_trace_threads)
endif()

function(duckemu_add_loader_test name)
  add_executable(${name} "integration/${name}.cpp")
  target_link_libraries(${name} PRIVATE duckemu_runtime duckemu_test_support)
  target_compile_definitions(${name} PRIVATE
    DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
    DUCKEMU_SO_DIR="${DUCKEMU_SO_RUNTIME_DIR}")
  add_dependencies(${name} guest_so)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
  duckemu_set_warnings(${name})
endfunction()

duckemu_add_loader_test(04_so_load)
duckemu_add_loader_test(04b_tlsdesc)
duckemu_add_loader_test(04d_dlopen)
duckemu_add_loader_test(04e_dup_fcntl)
duckemu_add_loader_test(04f_vfs_write)
duckemu_add_loader_test(04g_event_loop)
duckemu_add_loader_test(04h_device_profile)

# ---------------------------------------------------------------------------
# M4.5 gate: real bionic from the local Android environment.
#
# Paths are cache variables so another machine can point them elsewhere; the
# defaults match this workspace's layout. The test fails loudly when an asset
# is missing rather than silently skipping.
# ---------------------------------------------------------------------------
set(DUCKEMU_ANDROID_ENV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Root of the Android environment (contains DuckEmu/ and ROM/)")
set(DUCKEMU_BIONIC_DIR "${DUCKEMU_ANDROID_ENV_ROOT}/DuckEmu/bionic"
    CACHE PATH "Directory with the Android 13/14 libc/libm/libdl sample")
set(DUCKEMU_ROM_LIB64
    "${DUCKEMU_ANDROID_ENV_ROOT}/ROM/out/target/product/lemonade/symbols/system/lib64"
    CACHE PATH "ROM system/lib64 (libz/liblog/libc++)")
set(DUCKEMU_SGMAIN_SO
    "${DUCKEMU_ANDROID_ENV_ROOT}/DuckDroid/code/user_code/DaMai/ida/libsgmainso-6.8.260704.so"
    CACHE FILEPATH "libsgmainso sample with damaged section headers")
set(DUCKEMU_ROM_BOOTSTRAP "${DUCKEMU_ROM_LIB64}/bootstrap")
set(DUCKEMU_ROM_SYMBOLS
    "${DUCKEMU_ANDROID_ENV_ROOT}/ROM/out/target/product/lemonade/symbols"
    CACHE PATH "ROM symbols directory; mounted as guest / for file syscalls")
set(DUCKEMU_JNI_INCLUDE
    "${DUCKEMU_ANDROID_ENV_ROOT}/ROM/libnativehelper/include_jni"
    CACHE PATH "Directory containing the ROM's jni.h")
set(DUCKEMU_LIBELEVEN_CASE_SO
    "${CMAKE_CURRENT_SOURCE_DIR}/../user_code/libeleven_case/assets/libeleven.so"
    CACHE FILEPATH "libeleven sample used by the M5/M6 JNI tests")

# ---------------------------------------------------------------------------
# M5: JNI fixture. Compiled against the ROM's real jni.h, with no DT_NEEDED;
# every JNI call goes through the table JNI_OnLoad receives.
# ---------------------------------------------------------------------------
add_custom_command(
  OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libjnifixture.so"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
  COMMAND ${DUCKEMU_CLANG_EXECUTABLE}
          --target=aarch64-linux-android31 -fPIC -shared -nostdlib -O1
          -fno-stack-protector -fno-builtin
          -I"${DUCKEMU_JNI_INCLUDE}"
          -Wl,--pack-dyn-relocs=none -Wl,-soname,libjnifixture.so
          "${DUCKEMU_SO_SOURCE_DIR}/jni_fixture.c"
          -o "${DUCKEMU_SO_BUILD_DIR}/libjnifixture.so"
  DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/jni_fixture.c"
  COMMENT "Building guest .so libjnifixture.so")

add_custom_command(
  OUTPUT "${DUCKEMU_SO_RUNTIME_DIR}/libjnifixture.so"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_RUNTIME_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${DUCKEMU_SO_BUILD_DIR}/libjnifixture.so"
          "${DUCKEMU_SO_RUNTIME_DIR}/libjnifixture.so"
  DEPENDS "${DUCKEMU_SO_BUILD_DIR}/libjnifixture.so")

add_custom_target(jni_fixture ALL DEPENDS "${DUCKEMU_SO_RUNTIME_DIR}/libjnifixture.so")

function(duckemu_add_jni_fixture_test name)
  add_executable(${name} "integration/${name}.cpp" "fixtures/envs/fixture_env.cpp")
  target_link_libraries(${name} PRIVATE duckemu_runtime duckemu_test_support)
  target_compile_definitions(${name} PRIVATE
    DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
    DUCKEMU_JNI_FIXTURE_SO="${DUCKEMU_SO_RUNTIME_DIR}/libjnifixture.so")
  add_dependencies(${name} jni_fixture)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
  duckemu_set_warnings(${name})
endfunction()

duckemu_add_jni_fixture_test(05_jni_env)
duckemu_add_jni_fixture_test(05_jni_stub)
duckemu_add_jni_fixture_test(05_jni_arrays)

# M5 S3: the real libeleven JNI_OnLoad (ROM libc + the library's closure).
add_executable(05_jni_real integration/05_jni_real.cpp)
target_link_libraries(05_jni_real PRIVATE
  duckemu_runtime duckemu_test_support duckemu_user_libeleven)
target_compile_definitions(05_jni_real PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_CASE_SO="${DUCKEMU_LIBELEVEN_CASE_SO}")
add_test(NAME 05_jni_real COMMAND 05_jni_real)
set_tests_properties(05_jni_real PROPERTIES TIMEOUT 120)
duckemu_set_warnings(05_jni_real)

# M6: deterministic wall clock; libeleven's data(n) becomes byte-reproducible.
add_executable(06_clock_injection integration/06_clock_injection.cpp)
target_link_libraries(06_clock_injection PRIVATE
  duckemu_runtime duckemu_test_support duckemu_user_libeleven)
target_compile_definitions(06_clock_injection PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${DUCKEMU_LIBELEVEN_CASE_SO}")
add_test(NAME 06_clock_injection COMMAND 06_clock_injection)
set_tests_properties(06_clock_injection PROPERTIES TIMEOUT 120)
duckemu_set_warnings(06_clock_injection)

# The JD target through the worker's AppPlugin seam, with the same config the
# instance host builds (search paths + the guest root holding the trimmed APK).
add_executable(07e_jdgs_plugin integration/07e_jdgs_plugin.cpp)
target_link_libraries(07e_jdgs_plugin PRIVATE
  duckemu_runtime duckemu_test_support duckemu_user_registry)
target_compile_definitions(07e_jdgs_plugin PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_JDGS_SO="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/jdgs_case/assets/libjdg.so"
  DUCKEMU_JDGS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/jdgs_case/assets/root"
  DUCKEMU_JDGS_PROPERTIES="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/jdgs_case/jdgs.properties")
add_test(NAME 07e_jdgs_plugin COMMAND 07e_jdgs_plugin)
set_tests_properties(07e_jdgs_plugin PROPERTIES TIMEOUT 180)
duckemu_set_warnings(07e_jdgs_plugin)

# The dewu target through the same seam: device snapshot as the guest root,
# the MI 8 property picture, and a pinned clock/random source for byte-level
# replay. Two fresh instances must produce the same body.
add_executable(07f_szstone_plugin integration/07f_szstone_plugin.cpp)
target_link_libraries(07f_szstone_plugin PRIVATE
  duckemu_runtime duckemu_test_support duckemu_user_registry)
target_compile_definitions(07f_szstone_plugin PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_SZSTONE_SO="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/szstone_case/assets/libszstone.so"
  DUCKEMU_SZSTONE_BLOB="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/szstone_case/assets/lf_in.bin"
  DUCKEMU_SZSTONE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/szstone_case/assets/root"
  DUCKEMU_SZSTONE_PROPERTIES="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/szstone_case/szstone.properties")
add_test(NAME 07f_szstone_plugin COMMAND 07f_szstone_plugin)
set_tests_properties(07f_szstone_plugin PROPERTIES TIMEOUT 240)
duckemu_set_warnings(07f_szstone_plugin)

# The Douyin target through the same seam: default instance-host configuration
# (including the 5 s per-call timeout), injected clock and random seed. Two
# fresh instances must produce the same stable headers, and the strict Java
# world must report nothing but the MS.a()V probe the APK itself also misses.
add_executable(07g_dy7_plugin integration/07g_dy7_plugin.cpp)
target_link_libraries(07g_dy7_plugin PRIVATE
  duckemu_runtime duckemu_test_support duckemu_user_registry)
target_compile_definitions(07g_dy7_plugin PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_DY7_SO="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/dy7_case/assets/libmetasec_ml.so"
  DUCKEMU_DY7_PROPERTIES="${CMAKE_CURRENT_SOURCE_DIR}/../user_code/dy7_case/dy7.properties")
add_test(NAME 07g_dy7_plugin COMMAND 07g_dy7_plugin)
set_tests_properties(07g_dy7_plugin PROPERTIES TIMEOUT 300)
duckemu_set_warnings(07g_dy7_plugin)

add_executable(jni_world_test unit/runtime/jni_world_test.cpp)
target_link_libraries(jni_world_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME jni_world_test COMMAND jni_world_test)
duckemu_set_warnings(jni_world_test)

add_executable(jni_platform_env_test unit/runtime/jni_platform_env_test.cpp)
target_link_libraries(jni_platform_env_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME jni_platform_env_test COMMAND jni_platform_env_test)
duckemu_set_warnings(jni_platform_env_test)

add_executable(runtime_clock_test unit/runtime/runtime_clock_test.cpp)
target_link_libraries(runtime_clock_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME runtime_clock_test COMMAND runtime_clock_test)
duckemu_set_warnings(runtime_clock_test)

add_executable(jni_layout_test unit/runtime/jni_layout_test.cpp)
target_include_directories(jni_layout_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_include_directories(jni_layout_test SYSTEM PRIVATE "${DUCKEMU_JNI_INCLUDE}")
add_test(NAME jni_layout_test COMMAND jni_layout_test)
duckemu_set_warnings(jni_layout_test)

add_executable(04b_libc_map integration/04b_libc_map.cpp)
target_link_libraries(04b_libc_map PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04b_libc_map PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_BIONIC_DIR="${DUCKEMU_BIONIC_DIR}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_LIB64}/bootstrap"
  DUCKEMU_SGMAIN_SO="${DUCKEMU_SGMAIN_SO}")
add_test(NAME 04b_libc_map COMMAND 04b_libc_map)
set_tests_properties(04b_libc_map PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04b_libc_map)

# M4.6 S1: libc_shared_globals + auxv, exercised through the ROM libc's
# getauxval. Initializers stay off: preinit is a later step.
add_executable(04c_globals integration/04c_globals.cpp)
target_link_libraries(04c_globals PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04c_globals PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_test(NAME 04c_globals COMMAND 04c_globals)
set_tests_properties(04c_globals PROPERTIES TIMEOUT 60)
duckemu_set_warnings(04c_globals)

# M4.6 S4: preinit + scudo malloc with initializers enabled.
add_executable(04c_preinit integration/04c_preinit.cpp)
target_link_libraries(04c_preinit PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04c_preinit PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_test(NAME 04c_preinit COMMAND 04c_preinit)
set_tests_properties(04c_preinit PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04c_preinit)

# M4.6: system properties. The runtime writes the serialized
# /dev/__properties__ images; libc's preinit (already running by 04c_preinit)
# parses them, so every read below goes through bionic's own trie walk.
add_executable(04i_properties integration/04i_properties.cpp)
target_link_libraries(04i_properties PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04i_properties PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_PROPERTY_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/props/properties.conf")
add_test(NAME 04i_properties COMMAND 04i_properties)
set_tests_properties(04i_properties PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04i_properties)

# M4.6: /dev character nodes through libc's open/read/write/fstat/stat/access.
add_executable(04j_dev_nodes integration/04j_dev_nodes.cpp)
target_link_libraries(04j_dev_nodes PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04j_dev_nodes PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_test(NAME 04j_dev_nodes COMMAND 04j_dev_nodes)
set_tests_properties(04j_dev_nodes PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04j_dev_nodes)

# M4.6: /proc/self magic links through readlinkat(78).
add_executable(04k_readlink integration/04k_readlink.cpp)
target_link_libraries(04k_readlink PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04k_readlink PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_test(NAME 04k_readlink COMMAND 04k_readlink)
set_tests_properties(04k_readlink PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04k_readlink)

# M4.6: seeded entropy across getrandom / /dev/urandom / AT_RANDOM.
add_executable(04l_random_seed integration/04l_random_seed.cpp)
target_link_libraries(04l_random_seed PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04l_random_seed PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_test(NAME 04l_random_seed COMMAND 04l_random_seed)
set_tests_properties(04l_random_seed PROPERTIES TIMEOUT 180)
duckemu_set_warnings(04l_random_seed)

# M4.6: statfs(43)/fstatfs(44) mount classification.
add_executable(04m_statfs integration/04m_statfs.cpp)
target_link_libraries(04m_statfs PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04m_statfs PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_test(NAME 04m_statfs COMMAND 04m_statfs)
set_tests_properties(04m_statfs PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04m_statfs)

# M4.6 S3: VFS + /proc. A ROM symbols tree is mounted as guest "/" so the
# read-only file mmap half of the gate has a real file to map.
add_executable(04c_vfs integration/04c_vfs.cpp)
target_link_libraries(04c_vfs PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04c_vfs PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_ROM_SYMBOLS="${DUCKEMU_ROM_SYMBOLS}")
add_test(NAME 04c_vfs COMMAND 04c_vfs)
set_tests_properties(04c_vfs PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04c_vfs)

# M4.6 S2: the light syscall surface, per-call success/errno assertions.
add_executable(04c_syscalls integration/04c_syscalls.cpp)
target_link_libraries(04c_syscalls PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04c_syscalls PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_test(NAME 04c_syscalls COMMAND 04c_syscalls)
set_tests_properties(04c_syscalls PROPERTIES TIMEOUT 60)
duckemu_set_warnings(04c_syscalls)

# M4.6 S5: real bionic pthread_create/join with per-thread errno.
add_executable(04c_bionic integration/04c_bionic.cpp)
target_link_libraries(04c_bionic PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04c_bionic PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${DUCKEMU_ANDROID_ENV_ROOT}/DuckEmu/user_code/libeleven_run/libeleven.so")
add_test(NAME 04c_bionic COMMAND 04c_bionic)
set_tests_properties(04c_bionic PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04c_bionic)

# TLS/DTV full semantics: static slot in the startup graph (libc copies the
# image into new threads), dynamic TLSDESC + DTV growth for dlopen modules.
add_executable(04n_tls_dtv integration/04n_tls_dtv.cpp)
target_link_libraries(04n_tls_dtv PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04n_tls_dtv PRIVATE
  DUCKEMU_SO_DIR="${DUCKEMU_SO_RUNTIME_DIR}"
  DUCKEMU_SO_STATIC_DIR="${DUCKEMU_SO_RUNTIME_DIR}/static"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_dependencies(04n_tls_dtv guest_so)
add_test(NAME 04n_tls_dtv COMMAND 04n_tls_dtv)
set_tests_properties(04n_tls_dtv PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04n_tls_dtv)

# Regression: an unresolved DT_NEEDED takes the loader's failure path, which
# used to read the Module after erasing it.
add_executable(module_missing_dep_test unit/runtime/loader/module_missing_dep_test.cpp)
target_link_libraries(module_missing_dep_test PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(module_missing_dep_test PRIVATE
  DUCKEMU_SO_DIR="${DUCKEMU_SO_RUNTIME_DIR}")
add_dependencies(module_missing_dep_test guest_so)
add_test(NAME module_missing_dep_test COMMAND module_missing_dep_test)
duckemu_set_warnings(module_missing_dep_test)

add_executable(elf_image_test unit/runtime/loader/elf_image_test.cpp)
target_link_libraries(elf_image_test PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(elf_image_test PRIVATE
  DUCKEMU_SO_DIR="${DUCKEMU_SO_RUNTIME_DIR}")
add_dependencies(elf_image_test guest_so)
add_test(NAME elf_image_test COMMAND elf_image_test)
duckemu_set_warnings(elf_image_test)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
add_executable(runtime_memory_test unit/runtime/memory_test.cpp)
target_link_libraries(runtime_memory_test PRIVATE
  duckemu_runtime
  duckemu_test_support)
add_test(NAME runtime_memory_test COMMAND runtime_memory_test)
duckemu_set_warnings(runtime_memory_test)

add_executable(ring_buffer_test unit/runtime/ring_buffer_test.cpp)
target_link_libraries(ring_buffer_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME ring_buffer_test COMMAND ring_buffer_test)
duckemu_set_warnings(ring_buffer_test)

add_executable(guest_stack_test unit/runtime/guest_stack_test.cpp)
target_link_libraries(guest_stack_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME guest_stack_test COMMAND guest_stack_test)
duckemu_set_warnings(guest_stack_test)

add_executable(svc_trace_format_test unit/observer/svc_trace_format_test.cpp)
target_link_libraries(svc_trace_format_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME svc_trace_format_test COMMAND svc_trace_format_test)
duckemu_set_warnings(svc_trace_format_test)

if(DUCKEMU_ENABLE_TRACE)
  add_executable(insn_decode_test unit/runtime/insn_decode_test.cpp)
  target_link_libraries(insn_decode_test PRIVATE duckemu_runtime duckemu_test_support)
  add_test(NAME insn_decode_test COMMAND insn_decode_test)
  duckemu_set_warnings(insn_decode_test)
endif()

# M7 S1: wire framing and JSON message validation.
add_executable(protocol_test unit/protocol/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE duckemu_protocol duckemu_test_support)
add_test(NAME protocol_test COMMAND protocol_test)
duckemu_set_warnings(protocol_test)

# M7 S2: one instance process driven over its control channel. The child runs
# the same run_instance_host() the supervisor will exec.
add_executable(07a_instance_host integration/07a_instance_host.cpp)
target_link_libraries(07a_instance_host PRIVATE
  duckemu_worker_core
  duckemu_user_registry
  duckemu_test_support)
target_compile_definitions(07a_instance_host PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${DUCKEMU_LIBELEVEN_CASE_SO}")
add_test(NAME 07a_instance_host COMMAND 07a_instance_host)
set_tests_properties(07a_instance_host PROPERTIES TIMEOUT 120)
duckemu_set_warnings(07a_instance_host)

# M7 S3: the worker supervisor, with real instance processes spawned through
# the built duckemu_worker binary. The test plays System over the channel.
add_executable(07b_worker_supervisor integration/07b_worker_supervisor.cpp)
target_link_libraries(07b_worker_supervisor PRIVATE
  duckemu_worker_core
  duckemu_test_support)
target_compile_definitions(07b_worker_supervisor PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${DUCKEMU_LIBELEVEN_CASE_SO}"
  DUCKEMU_WORKER_EXEC="$<TARGET_FILE:duckemu_worker>")
add_dependencies(07b_worker_supervisor duckemu_worker)
add_test(NAME 07b_worker_supervisor COMMAND 07b_worker_supervisor)
set_tests_properties(07b_worker_supervisor PROPERTIES TIMEOUT 180)
duckemu_set_warnings(07b_worker_supervisor)

# M7 S4: the gateway in process. The client speaks the same framed JSON a
# Python Web API would; System spawns worker and instance processes for real.
add_executable(07c_system_gateway integration/07c_system_gateway.cpp)
target_link_libraries(07c_system_gateway PRIVATE
  duckemu_system_core
  duckemu_test_support)
target_compile_definitions(07c_system_gateway PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${DUCKEMU_LIBELEVEN_CASE_SO}"
  DUCKEMU_WORKER_EXEC="$<TARGET_FILE:duckemu_worker>")
add_dependencies(07c_system_gateway duckemu_worker)
add_test(NAME 07c_system_gateway COMMAND 07c_system_gateway)
set_tests_properties(07c_system_gateway PROPERTIES TIMEOUT 240)
duckemu_set_warnings(07c_system_gateway)

# M7 S5: session affinity and recycling (M6 warm-pool reset semantics).
add_executable(07d_session_reset integration/07d_session_reset.cpp)
target_link_libraries(07d_session_reset PRIVATE
  duckemu_worker_core
  duckemu_test_support)
target_compile_definitions(07d_session_reset PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${DUCKEMU_LIBELEVEN_CASE_SO}"
  DUCKEMU_WORKER_EXEC="$<TARGET_FILE:duckemu_worker>")
add_dependencies(07d_session_reset duckemu_worker)
add_test(NAME 07d_session_reset COMMAND 07d_session_reset)
set_tests_properties(07d_session_reset PROPERTIES TIMEOUT 180)
duckemu_set_warnings(07d_session_reset)

# M7 gate: Web API -> System -> Worker -> Runtime, plus a cross-check against
# the DuckDroid reference runner when it is present.
set(DUCKEMU_DUCK_DROID_ELEVEN_CASE
    "${DUCKEMU_ANDROID_ENV_ROOT}/DuckDroid/build/code/user_code/libeleven_case/libeleven_case"
    CACHE FILEPATH "DuckDroid reference runner for libeleven (empty skips parity)")
add_executable(07_end_to_end integration/07_end_to_end.cpp)
target_link_libraries(07_end_to_end PRIVATE
  duckemu_system_core
  duckemu_test_support)
target_compile_definitions(07_end_to_end PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${DUCKEMU_LIBELEVEN_CASE_SO}"
  DUCKEMU_WORKER_EXEC="$<TARGET_FILE:duckemu_worker>")
if(EXISTS "${DUCKEMU_DUCK_DROID_ELEVEN_CASE}")
  target_compile_definitions(07_end_to_end PRIVATE
    DUCKEMU_DUCK_DROID_ELEVEN_CASE="${DUCKEMU_DUCK_DROID_ELEVEN_CASE}")
endif()
add_dependencies(07_end_to_end duckemu_worker)
add_test(NAME 07_end_to_end COMMAND 07_end_to_end)
set_tests_properties(07_end_to_end PROPERTIES TIMEOUT 300)
duckemu_set_warnings(07_end_to_end)
