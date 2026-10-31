// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/device/device_profile.hpp"
#include "runtime/memory/thunk/thunk_table.hpp"
#include "runtime/observer/diagnostic/diagnostic_event.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/observer/hook/hook_context.hpp"
#include "runtime/observer/trace/insn_trace.hpp"

namespace duckemu::runtime
{

namespace jni
{
class JavaWorld;
}

// A project's Java environment pack (one function that declares its classes,
// methods, fields and shims). Target packs live in code/user_code and are
// installed explicitly by their runner; the runtime never names a target.
using JavaEnvironmentInstaller = Result<void> (*)(jni::JavaWorld &world);

struct RuntimeConfig
{
	uint32_t pid = 1000;
	uint32_t tid = 1;
	// Real targets build deep frames: metasec's v5 signer alone uses more than
	// 64 KiB below the entry SP, which used to run off a 64 KiB stack. The
	// DuckDroid reference engine raised the same limit to 4 MiB.
	uint64_t stack_size = 4 * 1024 * 1024;
	uint64_t default_timeout_us = 5000000;

	// Observation. An empty trace_path disables svc.log; record_memory_access
	// enables the (lossy) diagnostic memory-access feed.
	std::string trace_path;
	bool record_memory_access = false;
	size_t diagnostic_capacity = 1u << 14;

	// Default directory for instruction-level trace files. A runner still has
	// to call startInsnTrace() with the range; an empty InsnTraceOptions
	// directory falls back to this. Requires a DUCKEMU_ENABLE_TRACE build.
	std::string insn_trace_directory;

	// Loader search path for DT_NEEDED names that are not virtual modules. The
	// directory of the requesting module is always searched first.
	std::vector<std::string> library_search_paths;

	// Host directories exposed to guest file syscalls, read-only. Each root
	// represents guest "/" (so a root holding `system/lib64/libc.so` answers
	// /system/lib64/libc.so). Empty means only /proc is readable.
	std::vector<std::string> filesystem_roots;

	// Per-instance writable sandbox. One host directory that overlays the
	// read-only roots for guest "/": a file created through open(O_CREAT) or
	// written lives here, and reads/stat see it before the read-only roots.
	// Empty keeps the whole namespace read-only (EROFS).
	std::string filesystem_write_root;

	// Device identity: uname, /proc synthesis, stat owner, auxv hwcap and the
	// process name. Target packs override the fields they need.
	device::DeviceProfile device;

	// Guest-visible system properties. Identity entries (ro.product.*,
	// ro.build.*) are derived from `device`; this build.prop-format file
	// layers over them, in file order. Empty keeps the derived set only.
	std::string property_file;

	// Pins the instance entropy stream (getrandom, /dev/{u,}random and
	// AT_RANDOM), making a replay byte-reproducible. Null keeps host entropy.
	std::optional<uint64_t> random_seed;

	// Re-enables the M4 fixture virtual table (libc/liblog stubs). Off in
	// production; 04_so_load turns it on to keep testing the thunk path.
	bool enable_test_import_stubs = false;

	// Run DT_INIT/DT_INIT_ARRAY after loadLibrary. M4.5's gate turns this off:
	// real libc constructors are M4.6 work. When off, pending initializers are
	// discarded (not run later).
	bool run_module_initializers = true;
};

// The single facade over one guest world. M1 owns one host thread and one
// Unicorn engine; the guest memory, TLS, thunk page and SVC dispatcher are
// private implementation details.
class RuntimeInstance
{
    public:
	static Result<std::unique_ptr<RuntimeInstance> > create(const RuntimeConfig &config = {});

	~RuntimeInstance();
	RuntimeInstance(const RuntimeInstance &) = delete;
	RuntimeInstance &operator=(const RuntimeInstance &) = delete;
	RuntimeInstance(RuntimeInstance &&) = delete;
	RuntimeInstance &operator=(RuntimeInstance &&) = delete;

	Result<GuestVA> mapMemory(uint64_t size, uint32_t perms, std::string_view name);
	Result<void> writeMemory(GuestVA address, const void *data, uint64_t size);
	Result<void> readMemory(GuestVA address, void *data, uint64_t size) const;

	// Installs a project's Java environment. Must be called before the first
	// loadLibrary; this is the only wiring point for target packs. Platform
	// packs are installed automatically at create.
	Result<void> installJavaEnvironment(JavaEnvironmentInstaller installer);

	// Loads a shared object and its DT_NEEDED graph, applies relocations and
	// runs the constructors. Returns a handle for lookupSymbol. Must be called
	// before the first callFunction (static TLS is not installed afterwards).
	Result<uint64_t> loadLibrary(std::string_view path);

	struct ModuleRange
	{
		GuestVA base;
		uint64_t span = 0;
	};

	// Address range of a loaded module (analysis callers that need to cover a
	// whole obfuscated target; the range is chosen in target code, not by CLI).
	Result<ModuleRange> moduleRange(uint64_t handle) const;

	// Address of a symbol exported by the module behind `handle`, or the first
	// module in load order that exports it.
	Result<GuestVA> lookupSymbol(uint64_t handle, std::string_view name) const;
	Result<GuestVA> lookupSymbol(std::string_view name) const;

	// Registers a host implementation and returns the guest thunk address.
	// Calling it from the guest goes SVC -> dispatcher -> stub, then `ret`.
	Result<GuestVA> registerHostFunction(std::string_view name, HostStub stub);

	struct CallResult
	{
		uint64_t return_value = 0;
		std::optional<int32_t> exit_code;
	};

	// Calls a guest function with x0..x7, SP set to the instance stack and
	// LR pointing at an internal return trap.
	Result<CallResult> callFunction(GuestVA entry, const std::array<uint64_t, 8> &args = {});

	// Invokes a native method registered through RegisterNatives. The runtime
	// supplies JNIEnv* (x0) and jclass (x1), so up to six arguments go to
	// x2..x7. Strict: an undeclared class or an unregistered method is an error,
	// never a default value.
	Result<uint64_t> callRegisteredNative(std::string_view class_descriptor,
		std::string_view name,
		std::string_view signature,
		const std::array<uint64_t, 8> &args = {});
	// Address behind a registered native method, or NotFound.
	Result<GuestVA> lookupRegisteredNative(std::string_view class_descriptor,
		std::string_view name,
		std::string_view signature) const;
	// UTF-8 contents of a jstring handle returned by guest code.
	Result<std::string> jniStringUtf8(uint64_t handle) const;

	// Object address behind a live JNI reference. Host runners use it to
	// inspect an object returned by callRegisteredNative; the environment pack
	// then reads the object through JavaWorld.
	Result<uint64_t> jniResolveHandle(uint64_t handle) const;

	// Deterministic time (DuckDroid parity). Default: the host clock. After
	// setWallTimeNs/setMonotonicTimeNs every read returns the current value and
	// advances exactly 1 ms, so a replay is byte-identical. Setting the value
	// to 0 is a valid injected base; useHost*Clock restores host time.
	void setWallTimeNs(uint64_t ns);
	void setMonotonicTimeNs(uint64_t ns);
	void useHostWallClock();
	void useHostMonotonicClock();

	// Strict-mode failures (missing class/method/field, unsupported JNI slot).
	uint32_t addJniListener(observer::EventBus::JniListener listener);
	bool removeJniListener(uint32_t id);

	// Semantic observation. Listeners run synchronously on the emulating
	// thread, in guest order, and must not call back into the emulator.
	uint32_t addSvcListener(observer::EventBus::SvcListener listener);
	uint32_t addMemoryListener(observer::EventBus::MemoryListener listener);
	uint32_t addThreadListener(observer::EventBus::ThreadListener listener);
	uint32_t addModuleListener(observer::EventBus::ModuleListener listener);
	bool removeSvcListener(uint32_t id);
	bool removeMemoryListener(uint32_t id);
	bool removeThreadListener(uint32_t id);
	bool removeModuleListener(uint32_t id);

	// Diagnostic channel. Returns the number of events moved into `out`;
	// droppedDiagnostics() counts events lost because the ring was full.
	size_t drainDiagnostics(std::vector<observer::DiagnosticEvent> &out);
	[[nodiscard]] uint64_t droppedDiagnostics() const;

	// Address hook: the callback runs before the instruction at `address`.
	// All engines, including threads created later, receive it. Must be called
	// from host code, not from inside a hook or event listener.
	Result<uint32_t> addAddressHook(GuestVA address, observer::AddressHook hook);
	bool removeAddressHook(uint32_t id);

	// Instruction-level trace over [begin, begin + size). The range must stay
	// inside one loaded module. One session at a time; the files live in
	// options.directory (or config.insn_trace_directory when empty).
	Result<void> startInsnTrace(observer::InsnTraceOptions options);
	void stopInsnTrace();
	[[nodiscard]] bool insnTraceActive() const;

    private:
	struct Impl;
	explicit RuntimeInstance(std::unique_ptr<Impl> impl);

	std::unique_ptr<Impl> impl_;
};

} // namespace duckemu::runtime
