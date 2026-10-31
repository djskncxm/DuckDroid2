# SPDX-License-Identifier: GPL-2.0-only
# Target projects.
#
# The installer is referenced explicitly by the project's main.cpp and by the
# target-specific tests, so a plain STATIC library is enough; no static-init
# registration and no whole-archive tricks.

set(DUCKEMU_ANDROID_ENV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Root of the Android environment (contains DuckEmu/ and ROM/)")
set(DUCKEMU_ROM_LIB64
    "${DUCKEMU_ANDROID_ENV_ROOT}/ROM/out/target/product/lemonade/symbols/system/lib64"
    CACHE PATH "ROM system/lib64")
set(DUCKEMU_ROM_BOOTSTRAP "${DUCKEMU_ROM_LIB64}/bootstrap")

add_library(duckemu_user_libeleven STATIC
  libeleven_case/libeleven_env.cpp
)
target_include_directories(duckemu_user_libeleven PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_libeleven PRIVATE duckemu_runtime)
duckemu_set_warnings(duckemu_user_libeleven)

# JD 16.1.50: libjdg.so Bridge.main. The trimmed guest APK and the .so live in
# this project's assets/; nothing here is shared with the runtime.
set(DUCKEMU_JDGS_CASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/jdgs_case")
add_library(duckemu_user_jdgs STATIC
  jdgs_case/jdgs_env.cpp
)
target_include_directories(duckemu_user_jdgs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_jdgs PRIVATE duckemu_runtime)
duckemu_set_warnings(duckemu_user_jdgs)

# 得物 6.1.5.10: libszstone.so SzSdk.lf. The UPX-packed .so, the captured
# profile blob and the device snapshot live in this project's assets/.
set(DUCKEMU_SZSTONE_CASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/szstone_case")
add_library(duckemu_user_szstone STATIC
  szstone_case/szstone_env.cpp
)
target_include_directories(duckemu_user_szstone PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_szstone PRIVATE duckemu_runtime)
duckemu_set_warnings(duckemu_user_szstone)

# Douyin 40.8.0: libmetasec_ml.so seven-header signer. The .so and the device
# property picture live in this project's assets/.
set(DUCKEMU_DY7_CASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dy7_case")
add_library(duckemu_user_dy7 STATIC
  dy7_case/dy7_env.cpp
  dy7_case/dy7_case.cpp
)
target_include_directories(duckemu_user_dy7 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_dy7 PRIVATE duckemu_runtime)
duckemu_set_warnings(duckemu_user_dy7)

# M7: every project also implements the worker's AppPlugin seam. The registry
# is the only place app ids are named on this side of the boundary.
add_library(duckemu_user_registry STATIC
  registry.cpp
  libeleven_case/plugin.cpp
  jdgs_case/plugin.cpp
  szstone_case/plugin.cpp
  dy7_case/plugin.cpp
)
target_include_directories(duckemu_user_registry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_registry PUBLIC
  duckemu_user_libeleven
  duckemu_user_jdgs
  duckemu_user_szstone
  duckemu_user_dy7
  duckemu_common
)
duckemu_set_warnings(duckemu_user_registry)

# The project runner: loads ROM libc + assets/libeleven.so and calls the
# dynamically registered data(int). The .so is part of the project, not a
# test fixture.
add_executable(duckemu_libeleven_case
  libeleven_case/main.cpp
)
target_link_libraries(duckemu_libeleven_case PRIVATE
  duckemu_runtime
  duckemu_user_libeleven)
target_compile_definitions(duckemu_libeleven_case PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${CMAKE_CURRENT_SOURCE_DIR}/libeleven_case/assets/libeleven.so")
duckemu_set_warnings(duckemu_libeleven_case)

# JD 16.1.50 runner: ROM libc + libz and the project's libjdg.so, with the
# trimmed APK mounted at its device path through a guest file system root.
add_executable(duckemu_jdgs_case
  jdgs_case/main.cpp
)
target_link_libraries(duckemu_jdgs_case PRIVATE
  duckemu_runtime
  duckemu_user_jdgs)
target_compile_definitions(duckemu_jdgs_case PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_JDGS_SO="${DUCKEMU_JDGS_CASE_DIR}/assets/libjdg.so"
  DUCKEMU_JDGS_ROOT="${DUCKEMU_JDGS_CASE_DIR}/assets/root"
  DUCKEMU_JDGS_PROPERTIES="${DUCKEMU_JDGS_CASE_DIR}/jdgs.properties")
duckemu_set_warnings(duckemu_jdgs_case)

# 得物 6.1.5.10 runner: ROM libc/libz + the project's libszstone.so, the
# hook-captured profile blob and its output, and the device snapshot mounted
# as the guest file system root.
add_executable(duckemu_szstone_case
  szstone_case/main.cpp
)
target_link_libraries(duckemu_szstone_case PRIVATE
  duckemu_runtime
  duckemu_user_szstone)
target_compile_definitions(duckemu_szstone_case PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_SZSTONE_SO="${DUCKEMU_SZSTONE_CASE_DIR}/assets/libszstone.so"
  DUCKEMU_SZSTONE_BLOB="${DUCKEMU_SZSTONE_CASE_DIR}/assets/lf_in.bin"
  DUCKEMU_SZSTONE_EXPECT="${DUCKEMU_SZSTONE_CASE_DIR}/assets/lf_out.bin"
  DUCKEMU_SZSTONE_ROOT="${DUCKEMU_SZSTONE_CASE_DIR}/assets/root"
  DUCKEMU_SZSTONE_PROPERTIES="${DUCKEMU_SZSTONE_CASE_DIR}/szstone.properties")
duckemu_set_warnings(duckemu_szstone_case)

# 抖音 40.8.0 runner: ROM libc + the project's libmetasec_ml.so, with the Mi 8
# property picture the signer was validated against.
add_executable(duckemu_dy7_case
  dy7_case/main.cpp
)
target_link_libraries(duckemu_dy7_case PRIVATE
  duckemu_runtime
  duckemu_user_dy7)
target_compile_definitions(duckemu_dy7_case PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_DY7_SO="${DUCKEMU_DY7_CASE_DIR}/assets/libmetasec_ml.so"
  DUCKEMU_DY7_PROPERTIES="${DUCKEMU_DY7_CASE_DIR}/dy7.properties")
duckemu_set_warnings(duckemu_dy7_case)
