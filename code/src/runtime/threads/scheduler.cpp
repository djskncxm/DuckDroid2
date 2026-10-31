// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/threads/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <utility>

#include "common/log/logger.hpp"
#include "runtime/backend/unicorn/unicorn_backend.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/svc/dispatcher.hpp"
#include "runtime/svc/syscall_numbers.hpp"
#include "runtime/threads/clone/clone_flags.hpp"
#include "runtime/threads/pending_syscall.hpp"

namespace duckemu::runtime::threads
{
namespace
{

constexpr int kArgRegs[8] = {
	UC_ARM64_REG_X0,
	UC_ARM64_REG_X1,
	UC_ARM64_REG_X2,
	UC_ARM64_REG_X3,
	UC_ARM64_REG_X4,
	UC_ARM64_REG_X5,
	UC_ARM64_REG_X6,
	UC_ARM64_REG_X7,
};

// M3's only blocking syscall. FUTEX_WAIT has already checked *uaddr == val
// before this object exists, so a wake completes with 0 (the kernel does not
// re-check after a wake either).
class FutexWaitOp final : public PendingSyscall
{
    public:
	std::optional<int64_t> complete(WakeReason reason) override
	{
		switch (reason)
		{
		case WakeReason::FutexWoke:
			return 0;
		case WakeReason::TimerExpired:
			return svc::kError(svc::kEtimedout);
		case WakeReason::Signal:
			return svc::kError(svc::kEintr);
		case WakeReason::ThreadExited:
		case WakeReason::PollWoke: // futex waiters are not registered as such
			return std::nullopt;
		}
		return std::nullopt;
	}
};

} // namespace

ThreadScheduler::ThreadScheduler(RuntimeMemory &memory,
	UnicornBackend &backend,
	ThunkTable &thunks,
	observer::EventBus &events,
	const SchedulerConfig &config)
	: memory_(memory)
	, backend_(backend)
	, thunks_(thunks)
	, events_(events)
	, config_(config)
{
}

ThreadScheduler::~ThreadScheduler()
{
	stop();
}

Result<void> ThreadScheduler::start()
{
	if (config_.vfs == nullptr)
	{
		return Error{ ErrorCode::Internal, "the scheduler requires a file system" };
	}
	if (config_.clock == nullptr)
	{
		return Error{ ErrorCode::Internal, "the scheduler requires a clock" };
	}
	if (config_.random == nullptr)
	{
		return Error{ ErrorCode::Internal, "the scheduler requires an entropy source" };
	}
	next_tid_ = config_.tid + 1;

	auto created = createThreadObject(config_.tid, 0, true);
	if (!created.ok())
	{
		return created.error();
	}
	std::unique_ptr<RuntimeThread> thread = std::move(created).value();
	thread->tls_tp = config_.main_tls_tp.value();
	thread->state = ThreadState::Idle;
	main_ = thread.get();
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		threads_[config_.tid] = std::move(thread);
	}
	main_->host = std::thread(&ThreadScheduler::hostThreadLoop, this, std::ref(*main_));
	publishThreadEvent(observer::ThreadEventKind::Created, *main_);
	return {};
}

void ThreadScheduler::stop()
{
	stopping_.store(true, std::memory_order_release);

	std::vector<RuntimeThread *> all;
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		for (auto &entry : threads_)
		{
			all.push_back(entry.second.get());
		}
	}
	for (RuntimeThread *thread : all)
	{
		if (thread->engine)
		{
			thread->engine->stop();
		}
		thread->cv.notify_all();
	}

	// Join outside the instance lock: an exiting host thread finalizes itself
	// and takes instance_mutex_ (wait-queue cleanup), so joining under the lock
	// would deadlock.
	for (RuntimeThread *thread : all)
	{
		if (thread->host.joinable())
		{
			thread->host.join();
		}
	}
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		for (auto &entry : threads_)
		{
			if (entry.second->engine)
			{
				backend_.destroyEngine(entry.second->engine.get());
			}
		}
		threads_.clear();
		wait_queue_.clear();
	}
	main_ = nullptr;
}

Result<std::unique_ptr<RuntimeThread> > ThreadScheduler::createThreadObject(
	uint32_t tid, uint32_t parent_tid, bool is_main)
{
	auto thread = std::make_unique<RuntimeThread>();
	thread->tid = tid;
	thread->parent_tid = parent_tid;
	thread->is_main = is_main;
	if (config_.record_memory_access)
	{
		thread->diagnostics = std::make_unique<observer::RingBuffer<observer::DiagnosticEvent> >(
			config_.diagnostic_capacity);
	}
	thread->dispatcher = std::make_unique<svc::SvcDispatcher>(memory_,
		thunks_,
		events_,
		clock_,
		config_.pid,
		*thread,
		*this,
		*config_.vfs,
		*config_.clock,
		config_.device,
		*config_.random);

	auto engine = createEngineFor(*thread);
	if (!engine.ok())
	{
		return engine.error();
	}
	thread->engine = std::move(engine).value();
	return thread;
}

Result<std::unique_ptr<UnicornEngine> > ThreadScheduler::createEngineFor(RuntimeThread &thread)
{
	UnicornEngine::DiagnosticSink sink;
	if (thread.diagnostics)
	{
		sink = [&thread](const observer::DiagnosticEvent &source) {
			observer::DiagnosticEvent event = source;
			event.tid = thread.tid;
			(void)thread.diagnostics->tryPush(event);
		};
	}
	return backend_.createEngine(
		[&thread](uc_engine *uc, uint32_t intno) {
			thread.dispatcher->onTrap(uc, intno);
		},
		std::move(sink),
		thread.tid);
}

void ThreadScheduler::publishThreadEvent(observer::ThreadEventKind kind, const RuntimeThread &thread)
{
	observer::ThreadEvent event;
	event.kind = kind;
	event.tid = thread.tid;
	event.parent_tid = thread.parent_tid;
	events_.publishThread(event);
}

std::unique_lock<std::recursive_mutex> ThreadScheduler::lockExecution()
{
	return std::unique_lock<std::recursive_mutex>(execution_mutex_);
}

Result<CallResult> ThreadScheduler::callFunction(GuestVA entry, const std::array<uint64_t, 8> &args)
{
	// Listeners run inside the Unicorn hook of whichever guest thread is
	// executing. Starting a guest call from one would corrupt that engine's
	// state (M0 probe 6). delivering() is thread-local, so a listener on the
	// main thread is refused here just like in M2.
	if (events_.delivering())
	{
		return Error{ ErrorCode::InvalidArgument,
			"callFunction from an event listener would re-enter the emulator" };
	}
	if (backend_.inHookCallback())
	{
		return Error{ ErrorCode::InvalidArgument,
			"callFunction from an address hook would re-enter the emulator" };
	}
	if (main_ == nullptr)
	{
		return Error{ ErrorCode::Internal, "scheduler is not started" };
	}

	std::unique_lock<std::mutex> lock(main_->mutex);
	if (main_->call_active)
	{
		return Error{ ErrorCode::InvalidArgument, "a call is already in progress" };
	}
	if (stopping_.load(std::memory_order_acquire))
	{
		return Error{ ErrorCode::InvalidArgument, "the instance is stopping" };
	}
	if (main_->state == ThreadState::Exited)
	{
		return Error{ ErrorCode::InvalidArgument, "the main thread has exited" };
	}

	main_->call_active = true;
	main_->call_done = false;
	main_->call_error.reset();
	main_->request.requested = true;
	main_->request.entry = entry;
	main_->request.args = args;
	main_->cv.notify_all();
	main_->cv.wait(lock, [this] {
		return main_->call_done;
	});
	main_->call_active = false;

	if (main_->call_error.has_value())
	{
		return main_->call_error.value();
	}
	return main_->call_result;
}

void ThreadScheduler::initializeMainCall(RuntimeThread &thread)
{
	// A top-level call is the unit the address-hook hit counter resets on.
	// Called under the execution lock, so no engine is running.
	backend_.resetHookHitCounts();

	CallRequest request;
	{
		std::lock_guard<std::mutex> lock(thread.mutex);
		request = thread.request;
		thread.request.requested = false;
		thread.call_done = false;
		thread.call_error.reset();
		thread.exit_requested = false;
		thread.exit_code = 0;
		thread.pending.reset();
		thread.pending_event.reset();
		thread.pending_completed = false;
	}

	auto finishWithError = [&thread](const Error &error) {
		std::lock_guard<std::mutex> lock(thread.mutex);
		thread.call_error = error;
		thread.call_done = true;
		thread.state = ThreadState::Idle;
		thread.cv.notify_all();
	};

	thread.dispatcher->resetRunState();
	auto write = [&thread](int reg, uint64_t value) {
		return thread.engine->writeRegister(reg, value);
	};
	if (auto result = write(UC_ARM64_REG_SP, config_.stack_top); !result.ok())
	{
		finishWithError(result.error());
		return;
	}
	if (auto result = write(UC_ARM64_REG_X30, config_.return_trap.value()); !result.ok())
	{
		finishWithError(result.error());
		return;
	}
	// Terminates the guest frame chain: the unwinder stops at fp == 0 instead
	// of reading whatever the previous run left in x29.
	if (auto result = write(UC_ARM64_REG_X29, 0); !result.ok())
	{
		finishWithError(result.error());
		return;
	}
	if (auto result = write(UC_ARM64_REG_TPIDR_EL0, thread.tls_tp); !result.ok())
	{
		finishWithError(result.error());
		return;
	}
	for (size_t i = 0; i < request.args.size(); ++i)
	{
		if (auto result = write(kArgRegs[i], request.args[i]); !result.ok())
		{
			finishWithError(result.error());
			return;
		}
	}

	thread.next_pc = request.entry;
	std::lock_guard<std::mutex> lock(thread.mutex);
	thread.state = ThreadState::Runnable;
}

void ThreadScheduler::applyPendingCompletion(RuntimeThread &thread)
{
	std::optional<observer::SvcEvent> event;
	int64_t result = 0;
	{
		std::lock_guard<std::mutex> lock(thread.mutex);
		if (!thread.pending_completed)
		{
			return;
		}
		result = thread.pending_result;
		event = std::move(thread.pending_event);
		thread.pending_event.reset();
		thread.pending.reset();
		thread.pending_completed = false;
	}

	// The woken thread writes x0 and publishes the event at the moment it
	// actually resumes, so the trace order is the guest's serial execution
	// order, not the waker's.
	if (auto written = thread.engine->writeRegister(UC_ARM64_REG_X0, static_cast<uint64_t>(result)); !written.ok())
	{
		DUCKEMU_WARN("cannot write x0 to woken thread %u: %s", thread.tid, written.error().message.c_str());
	}
	if (event.has_value())
	{
		event->result = static_cast<uint64_t>(result);
		event->id = clock_.next_id++;
		events_.publishSvc(event.value());
	}
}

void ThreadScheduler::hostThreadLoop(RuntimeThread &thread)
{
	for (;;)
	{
		bool has_request = false;
		bool exit_requested = false;
		ThreadState state = ThreadState::Created;
		{
			std::unique_lock<std::mutex> lock(thread.mutex);
			auto ready = [this, &thread] {
				return stopping_.load(std::memory_order_acquire) || thread.exit_requested ||
				       thread.state == ThreadState::Exited ||
				       (thread.is_main && thread.request.requested) ||
				       thread.state == ThreadState::Runnable;
			};
			if (thread.state == ThreadState::Blocked && thread.has_deadline)
			{
				// Sleep until woken or the syscall's deadline. Expiry only
				// flags the thread; the completion callback runs once this
				// thread holds the execution lock again.
				if (!thread.cv.wait_until(lock, thread.deadline, ready) && !ready())
				{
					thread.deadline_expired = true;
					thread.state = ThreadState::Runnable;
				}
			}
			else
			{
				thread.cv.wait(lock, ready);
			}
			has_request = thread.is_main && thread.request.requested;
			exit_requested = thread.exit_requested;
			state = thread.state;
		}

		if (stopping_.load(std::memory_order_acquire) || exit_requested || state == ThreadState::Exited)
		{
			break;
		}

		std::unique_lock<std::recursive_mutex> execution(execution_mutex_);
		if (stopping_.load(std::memory_order_acquire))
		{
			execution.unlock();
			break;
		}
		if (has_request)
		{
			initializeMainCall(thread);
		}
		{
			std::lock_guard<std::mutex> lock(thread.mutex);
			state = thread.state;
		}
		if (state != ThreadState::Runnable)
		{
			execution.unlock();
			continue;
		}

		expireDeadline(thread);
		applyPendingCompletion(thread);
		{
			std::lock_guard<std::mutex> lock(thread.mutex);
			thread.state = ThreadState::Executing;
		}

		const uint64_t until = thread.is_main ? config_.return_trap.value() : 0;
		// Deliberately no Unicorn timeout: uc.c's timeout thread polls the
		// non-atomic `emulation_done` and races with the next run of the same
		// engine, occasionally stopping it right after an SVC (observed as
		// "stopped outside the return trap" at svc+4). Threads leave through an
		// SVC stop or a fault; a spinning guest hangs the caller until the host
		// watchdog lands (M3 design §12).
		auto outcome = thread.engine->run(thread.next_pc, GuestVA{ until }, 0);
		handleRunOutcome(thread, outcome);
		execution.unlock();
	}

	finalizeThread(thread);
}

void ThreadScheduler::handleRunOutcome(RuntimeThread &thread, const Result<RunOutcome> &outcome)
{
	std::lock_guard<std::mutex> lock(thread.mutex);

	auto fail = [&thread](Error error) {
		if (thread.is_main)
		{
			thread.call_error = std::move(error);
			thread.call_done = true;
			thread.state = ThreadState::Idle;
		}
		else
		{
			thread.exit_requested = true;
			thread.state = ThreadState::Exited;
		}
		thread.cv.notify_all();
	};

	if (stopping_.load(std::memory_order_acquire))
	{
		if (thread.is_main)
		{
			thread.call_result.exit_code = exit_code_;
			thread.call_done = true;
		}
		else
		{
			thread.exit_requested = true;
			thread.state = ThreadState::Exited;
		}
		thread.cv.notify_all();
		return;
	}

	if (!outcome.ok())
	{
		fail(outcome.error());
		return;
	}

	const RunOutcome &value = outcome.value();
	thread.next_pc = GuestVA{ value.pc };

	// libc owns the thread pointer: __libc_init_main_thread_final and each
	// pthread_create switch it with __set_tls (a plain TPIDR_EL0 write, no
	// syscall). Track the engine's value so the next host-initiated call does
	// not restore a stale TLS block.
	if (auto tp = thread.engine->readRegister(UC_ARM64_REG_TPIDR_EL0); tp.ok())
	{
		thread.tls_tp = tp.value();
	}

	// A handler may have blocked or exited the thread from inside the hook.
	if (thread.state != ThreadState::Executing)
	{
		if (thread.state == ThreadState::Exited && thread.is_main)
		{
			thread.call_result.exit_code = thread.exit_code;
			thread.call_done = true;
		}
		thread.cv.notify_all();
		return;
	}

	if (value.err != UC_ERR_OK)
	{
		fail(Error{ ErrorCode::EmulationFault, std::string("uc_emu_start: ") + uc_strerror(value.err) });
		return;
	}

	if (thread.is_main)
	{
		if (value.pc != config_.return_trap.value())
		{
			char message[128];
			std::snprintf(message,
				sizeof(message),
				"guest stopped outside the return trap: pc=0x%llx err=%s tid=%u",
				static_cast<unsigned long long>(value.pc),
				uc_strerror(value.err),
				thread.tid);
			fail(Error{ ErrorCode::EmulationFault, message });
			return;
		}
		auto x0 = thread.engine->readRegister(UC_ARM64_REG_X0);
		if (!x0.ok())
		{
			fail(x0.error());
			return;
		}
		thread.call_result.return_value = x0.value();
		thread.state = ThreadState::Idle;
		thread.call_done = true;
		thread.cv.notify_all();
		return;
	}

	// A child thread has no return trap: stopping without blocking or exiting
	// is a fault, but there is no caller to report it to.
	DUCKEMU_WARN("thread %u stopped outside an exit or blocking syscall at 0x%llx",
		thread.tid,
		static_cast<unsigned long long>(value.pc));
	thread.exit_requested = true;
	thread.state = ThreadState::Exited;
	thread.cv.notify_all();
}

void ThreadScheduler::finalizeThread(RuntimeThread &thread)
{
	removeFromWaitQueue(thread);

	if (thread.is_main)
	{
		std::lock_guard<std::mutex> lock(thread.mutex);
		if (stopping_.load(std::memory_order_acquire) && !thread.call_done)
		{
			thread.call_result.exit_code = exit_code_;
			thread.call_done = true;
		}
		thread.cv.notify_all();
	}
	else
	{
		publishThreadEvent(observer::ThreadEventKind::Exited, thread);
	}

	// The engine is destroyed by the reaper on another host thread. Closing it
	// from this (exiting) thread races with the parent opening a new engine
	// inside its clone hook: both touch Unicorn's opener/TCG state.

	// CLEARTID and the join wake come last: by the time a joiner is woken the
	// Exited event has been published, so the host observes a stable thread
	// lifecycle after callFunction returns. The engine itself is reaped later
	// by whichever host thread runs reapExited().
	if (thread.clear_child_tid != 0)
	{
		const uint32_t zero = 0;
		(void)memory_.write(GuestVA{ thread.clear_child_tid }, &zero, sizeof(zero));
		(void)futexWake(GuestVA{ thread.clear_child_tid }, 1);
		thread.clear_child_tid = 0;
	}
}

void ThreadScheduler::removeFromWaitQueue(RuntimeThread &thread)
{
	std::lock_guard<std::mutex> lock(instance_mutex_);
	for (auto it = wait_queue_.begin(); it != wait_queue_.end();)
	{
		auto &waiters = it->second;
		waiters.erase(std::remove(waiters.begin(), waiters.end(), &thread), waiters.end());
		if (waiters.empty())
		{
			it = wait_queue_.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void ThreadScheduler::reapExited()
{
	std::vector<RuntimeThread *> joining;
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		for (auto &entry : threads_)
		{
			if (!entry.second->is_main && entry.second->state == ThreadState::Exited)
			{
				joining.push_back(entry.second.get());
			}
		}
	}
	for (RuntimeThread *thread : joining)
	{
		if (thread->host.joinable())
		{
			thread->host.join();
		}
	}
	std::lock_guard<std::mutex> lock(instance_mutex_);
	for (auto it = threads_.begin(); it != threads_.end();)
	{
		if (!it->second->is_main && it->second->state == ThreadState::Exited && !it->second->host.joinable())
		{
			if (it->second->engine)
			{
				backend_.destroyEngine(it->second->engine.get());
			}
			it = threads_.erase(it);
		}
		else
		{
			++it;
		}
	}
}

int64_t ThreadScheduler::cloneThread(RuntimeThread &parent, const CloneRequest &request)
{
	using namespace clone_flags;

	if ((request.flags & (kCloneVm | kCloneThread)) != (kCloneVm | kCloneThread))
	{
		DUCKEMU_WARN("clone(flags=0x%llx) without CLONE_VM|CLONE_THREAD is not supported",
			static_cast<unsigned long long>(request.flags));
		return svc::kError(svc::kEinval);
	}

	auto saved = parent.engine->saveContext();
	if (!saved.ok())
	{
		return svc::kError(svc::kEnomem);
	}

	reapExited();

	const uint32_t tid = next_tid_++;
	auto created = createThreadObject(tid, parent.tid, false);
	if (!created.ok())
	{
		return svc::kError(svc::kEnomem);
	}
	std::unique_ptr<RuntimeThread> child = std::move(created).value();

	if (auto restored = child->engine->restoreContext(*saved.value()); !restored.ok())
	{
		DUCKEMU_WARN("clone: cannot restore child context: %s", restored.error().message.c_str());
		return svc::kError(svc::kEnomem);
	}
	if (auto written = child->engine->writeRegister(UC_ARM64_REG_SP, request.stack_top.value()); !written.ok())
	{
		return svc::kError(svc::kEfault);
	}
	if ((request.flags & kCloneSetTls) != 0)
	{
		if (auto written = child->engine->writeRegister(UC_ARM64_REG_TPIDR_EL0, request.tls.value());
			!written.ok())
		{
			return svc::kError(svc::kEfault);
		}
		child->tls_tp = request.tls.value();
	}
	if (auto written = child->engine->writeRegister(UC_ARM64_REG_X0, 0); !written.ok())
	{
		return svc::kError(svc::kEfault);
	}

	auto pc = child->engine->readRegister(UC_ARM64_REG_PC);
	if (!pc.ok())
	{
		return svc::kError(svc::kEfault);
	}
	child->next_pc = GuestVA{ pc.value() };

	if ((request.flags & kCloneParentSetTid) != 0 && !request.parent_tid.is_null())
	{
		(void)memory_.write(request.parent_tid, &tid, sizeof(tid));
	}
	if ((request.flags & kCloneChildSetTid) != 0 && !request.child_tid.is_null())
	{
		(void)memory_.write(request.child_tid, &tid, sizeof(tid));
	}
	if ((request.flags & kCloneChildClearTid) != 0 && !request.child_tid.is_null())
	{
		child->clear_child_tid = request.child_tid.value();
	}

	child->state = ThreadState::Runnable;
	RuntimeThread *raw = child.get();
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		threads_[tid] = std::move(child);
	}
	raw->host = std::thread(&ThreadScheduler::hostThreadLoop, this, std::ref(*raw));
	publishThreadEvent(observer::ThreadEventKind::Created, *raw);
	return static_cast<int64_t>(tid);
}

int64_t ThreadScheduler::futexWait(RuntimeThread &thread,
	GuestVA uaddr,
	uint32_t expected,
	uint64_t timeout_us)
{
	auto value = memory_.readU32(uaddr);
	if (!value.ok())
	{
		return svc::kError(svc::kEfault);
	}
	if (value.value() != expected)
	{
		return svc::kError(svc::kEagain);
	}
	if (thread.pending)
	{
		return svc::kError(svc::kEdeadlk);
	}

	thread.pending = std::make_unique<FutexWaitOp>();
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		wait_queue_[uaddr.value()].push_back(&thread);
	}
	{
		std::lock_guard<std::mutex> lock(thread.mutex);
		thread.state = ThreadState::Blocked;
		thread.futex_key = uaddr.value();
		thread.has_deadline = timeout_us != 0;
		thread.deadline_expired = false;
		if (timeout_us != 0)
		{
			thread.deadline =
				std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
		}
	}
	publishThreadEvent(observer::ThreadEventKind::Blocked, thread);
	return 0;
}

int64_t ThreadScheduler::parkThread(RuntimeThread &thread,
	std::unique_ptr<PendingSyscall> pending,
	uint64_t timeout_us)
{
	if (thread.pending)
	{
		return svc::kError(svc::kEdeadlk);
	}
	if (!pending)
	{
		return svc::kError(svc::kEinval);
	}
	thread.pending = std::move(pending);
	{
		std::lock_guard<std::mutex> lock(thread.mutex);
		thread.state = ThreadState::Blocked;
		thread.has_deadline = timeout_us != 0;
		thread.deadline_expired = false;
		if (timeout_us != 0)
		{
			thread.deadline =
				std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
		}
	}
	publishThreadEvent(observer::ThreadEventKind::Blocked, thread);
	return 0;
}

bool ThreadScheduler::wakeThread(RuntimeThread &thread, WakeReason reason)
{
	if (!thread.pending)
	{
		return false;
	}
	auto result = thread.pending->complete(reason);
	if (!result.has_value())
	{
		// Spurious: the condition the waiter tests still does not hold.
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(thread.mutex);
		thread.pending_result = result.value();
		thread.pending_completed = true;
		thread.has_deadline = false;
		thread.deadline_expired = false;
		thread.futex_key = 0;
		thread.state = ThreadState::Runnable;
	}
	publishThreadEvent(observer::ThreadEventKind::Runnable, thread);
	thread.cv.notify_all();
	return true;
}

void ThreadScheduler::expireDeadline(RuntimeThread &thread)
{
	uint64_t futex_key = 0;
	{
		std::lock_guard<std::mutex> lock(thread.mutex);
		if (!thread.deadline_expired)
		{
			return;
		}
		thread.deadline_expired = false;
		thread.has_deadline = false;
		if (!thread.pending)
		{
			return;
		}
		auto result = thread.pending->complete(WakeReason::TimerExpired);
		if (!result.has_value())
		{
			// The syscall wants to keep waiting; re-arm nothing, the caller
			// will be parked again by its next iteration.
			thread.state = ThreadState::Blocked;
			return;
		}
		thread.pending_result = result.value();
		thread.pending_completed = true;
		futex_key = thread.futex_key;
		thread.futex_key = 0;
	}
	if (futex_key != 0)
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		auto it = wait_queue_.find(futex_key);
		if (it != wait_queue_.end())
		{
			auto &waiters = it->second;
			waiters.erase(std::remove(waiters.begin(), waiters.end(), &thread), waiters.end());
			if (waiters.empty())
			{
				wait_queue_.erase(it);
			}
		}
	}
}

int64_t ThreadScheduler::futexWake(GuestVA uaddr, int32_t count)
{
	if (count <= 0)
	{
		return 0;
	}

	std::vector<RuntimeThread *> woken;
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		auto it = wait_queue_.find(uaddr.value());
		if (it == wait_queue_.end())
		{
			return 0;
		}
		auto &waiters = it->second;
		const size_t take = std::min<size_t>(static_cast<size_t>(count), waiters.size());
		woken.assign(waiters.begin(), waiters.begin() + static_cast<std::ptrdiff_t>(take));
		waiters.erase(waiters.begin(), waiters.begin() + static_cast<std::ptrdiff_t>(take));
		if (waiters.empty())
		{
			wait_queue_.erase(it);
		}
	}

	int64_t woken_count = 0;
	for (RuntimeThread *thread : woken)
	{
		if (!thread->pending)
		{
			continue;
		}
		auto result = thread->pending->complete(WakeReason::FutexWoke);
		if (!result.has_value())
		{
			std::lock_guard<std::mutex> lock(instance_mutex_);
			wait_queue_[uaddr.value()].push_back(thread);
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(thread->mutex);
			thread->pending_result = result.value();
			thread->pending_completed = true;
			thread->futex_key = 0;
			thread->has_deadline = false;
			thread->state = ThreadState::Runnable;
		}
		publishThreadEvent(observer::ThreadEventKind::Runnable, *thread);
		thread->cv.notify_all();
		++woken_count;
	}
	return woken_count;
}

void ThreadScheduler::exitThread(RuntimeThread &thread, int32_t code)
{
	std::lock_guard<std::mutex> lock(thread.mutex);
	thread.exit_requested = true;
	thread.exit_code = code;
	thread.state = ThreadState::Exited;
}

void ThreadScheduler::exitGroup(RuntimeThread &, int32_t code)
{
	exit_code_ = code;
	stopping_.store(true, std::memory_order_release);

	std::vector<RuntimeThread *> all;
	{
		std::lock_guard<std::mutex> lock(instance_mutex_);
		for (auto &entry : threads_)
		{
			all.push_back(entry.second.get());
		}
	}
	for (RuntimeThread *thread : all)
	{
		if (thread->engine)
		{
			thread->engine->stop();
		}
		thread->cv.notify_all();
	}
}

void ThreadScheduler::setClearChildTid(RuntimeThread &thread, GuestVA address)
{
	std::lock_guard<std::mutex> lock(thread.mutex);
	thread.clear_child_tid = address.value();
}

size_t ThreadScheduler::drainDiagnostics(std::vector<observer::DiagnosticEvent> &out)
{
	std::lock_guard<std::mutex> lock(instance_mutex_);
	size_t drained = 0;
	observer::DiagnosticEvent event;
	for (auto &entry : threads_)
	{
		if (!entry.second->diagnostics)
		{
			continue;
		}
		while (entry.second->diagnostics->tryPop(event))
		{
			out.push_back(event);
			++drained;
		}
	}
	return drained;
}

uint64_t ThreadScheduler::droppedDiagnostics() const
{
	std::lock_guard<std::mutex> lock(instance_mutex_);
	uint64_t dropped = 0;
	for (const auto &entry : threads_)
	{
		if (entry.second->diagnostics)
		{
			dropped += entry.second->diagnostics->dropped();
		}
	}
	return dropped;
}

size_t ThreadScheduler::liveThreadCount() const
{
	std::lock_guard<std::mutex> lock(instance_mutex_);
	return threads_.size();
}

} // namespace duckemu::runtime::threads
