// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/error/result.hpp"
#include "runtime/backend/unicorn/unicorn_engine.hpp"
#include "runtime/observer/semantic/thread_event.hpp"
#include "runtime/device/device_profile.hpp"
#include "runtime/observer/trace/trace_clock.hpp"
#include "runtime/threads/runtime_thread.hpp"
#include "runtime/threads/thread_system.hpp"

namespace duckemu::runtime
{

class RandomSource;
class RuntimeMemory;
class UnicornBackend;
class ThunkTable;
class RuntimeClock;

namespace observer
{
class EventBus;
}

namespace vfs
{
class VirtualFileSystem;
}

namespace threads
{

struct SchedulerConfig
{
	uint32_t pid = 0;
	uint32_t tid = 0;
	uint64_t stack_top = 0;
	GuestVA return_trap;
	GuestVA main_tls_tp;
	uint64_t default_timeout_us = 5000000;
	bool record_memory_access = false;
	size_t diagnostic_capacity = 1u << 14;
	// Process-wide file table; every dispatcher gets the same instance.
	vfs::VirtualFileSystem *vfs = nullptr;
	// Device identity for uname/getuid/prctl and /proc; owned by the facade.
	const device::DeviceProfile *device = nullptr;
	// Instance entropy stream (getrandom and /dev/{u,}random); owned by the
	// facade.
	RandomSource *random = nullptr;
	// Instance-wide wall/monotonic clock (injectable for replays).
	RuntimeClock *clock = nullptr;
};

// Owns the guest threads of one RuntimeInstance. One host thread per guest
// thread; a recursive execution lock plays the role of "the single CPU" (M3
// design §3, modelled on the kernel's mmap_lock vs. CPU ownership split).
class ThreadScheduler final : public ThreadSystem
{
    public:
	ThreadScheduler(RuntimeMemory &memory,
		UnicornBackend &backend,
		ThunkTable &thunks,
		observer::EventBus &events,
		const SchedulerConfig &config);
	~ThreadScheduler() override;

	ThreadScheduler(const ThreadScheduler &) = delete;
	ThreadScheduler &operator=(const ThreadScheduler &) = delete;
	ThreadScheduler(ThreadScheduler &&) = delete;
	ThreadScheduler &operator=(ThreadScheduler &&) = delete;

	// Starts the dedicated host thread that owns the main guest engine.
	Result<void> start();
	// Stops every engine, wakes every waiter and joins every host thread.
	void stop();

	// Posts (entry, args) to the main guest thread and waits for the call to
	// return, exit or fault. Refused while another call is active.
	Result<CallResult> callFunction(GuestVA entry, const std::array<uint64_t, 8> &args);

	// Serializes a host API call with guest execution: while a guest thread is
	// running, the caller waits until it blocks or returns. Recursive, so an
	// SVC handler on the running thread may call back into the runtime.
	[[nodiscard]] std::unique_lock<std::recursive_mutex> lockExecution();

	size_t drainDiagnostics(std::vector<observer::DiagnosticEvent> &out);
	[[nodiscard]] uint64_t droppedDiagnostics() const;
	[[nodiscard]] size_t liveThreadCount() const;

	// ThreadSystem
	int64_t cloneThread(RuntimeThread &parent, const CloneRequest &request) override;
	int64_t futexWait(RuntimeThread &thread, GuestVA uaddr, uint32_t expected, uint64_t timeout_us) override;
	int64_t futexWake(GuestVA uaddr, int32_t count) override;
	int64_t parkThread(RuntimeThread &thread,
		std::unique_ptr<PendingSyscall> pending,
		uint64_t timeout_us) override;
	bool wakeThread(RuntimeThread &thread, WakeReason reason) override;
	void exitThread(RuntimeThread &thread, int32_t code) override;
	void exitGroup(RuntimeThread &thread, int32_t code) override;
	void setClearChildTid(RuntimeThread &thread, GuestVA address) override;
	[[nodiscard]] bool stopping() const override
	{
		return stopping_.load(std::memory_order_acquire);
	}
	[[nodiscard]] GuestVA returnTrap() const override
	{
		return config_.return_trap;
	}

    private:
	void hostThreadLoop(RuntimeThread &thread);
	void initializeMainCall(RuntimeThread &thread);
	void applyPendingCompletion(RuntimeThread &thread);
	void handleRunOutcome(RuntimeThread &thread, const Result<RunOutcome> &outcome);
	void finalizeThread(RuntimeThread &thread);
	// Runs a timed-out blocking syscall's completion under the execution lock.
	void expireDeadline(RuntimeThread &thread);
	void reapExited();
	void removeFromWaitQueue(RuntimeThread &thread);

	Result<std::unique_ptr<RuntimeThread> > createThreadObject(uint32_t tid, uint32_t parent_tid, bool is_main);
	Result<std::unique_ptr<UnicornEngine> > createEngineFor(RuntimeThread &thread);
	void publishThreadEvent(observer::ThreadEventKind kind, const RuntimeThread &thread);

	RuntimeMemory &memory_;
	UnicornBackend &backend_;
	ThunkTable &thunks_;
	observer::EventBus &events_;
	SchedulerConfig config_;

	// "The single CPU". Held across uc_emu_start by the thread that is running
	// guest code; released when that thread blocks or exits. Recursive because
	// an SVC handler may call back into the runtime (mmap) on the same thread.
	std::recursive_mutex execution_mutex_;
	// Address-space and thread-table metadata (the mmap_lock analogue).
	mutable std::mutex instance_mutex_;
	std::map<uint32_t, std::unique_ptr<RuntimeThread> > threads_;
	std::unordered_map<uint64_t, std::vector<RuntimeThread *> > wait_queue_;
	RuntimeThread *main_ = nullptr;
	uint32_t next_tid_ = 0;
	observer::TraceClock clock_;
	std::atomic<bool> stopping_{ false };
	int32_t exit_code_ = 0;
};

} // namespace threads
} // namespace duckemu::runtime
