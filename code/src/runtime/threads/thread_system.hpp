// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <memory>

#include "common/id/ids.hpp"

namespace duckemu::runtime::threads
{

class RuntimeThread;
class PendingSyscall;
enum class WakeReason : uint8_t;

// clone(2) arguments after the dispatcher has read x0..x4. arm64 selects
// CONFIG_CLONE_BACKWARDS, so the kernel order is
// (flags, stack, parent_tid, tls, child_tid) -- tls is x3, child_tid is x4.
// bionic's __bionic_clone.S issues the syscall with exactly this order.
struct CloneRequest
{
	uint64_t flags = 0;
	GuestVA stack_top;
	GuestVA parent_tid;
	GuestVA child_tid;
	GuestVA tls;
};

// The part of the scheduler the SVC handlers are allowed to see. Handlers run
// inside the dispatcher, under the instance execution lock.
class ThreadSystem
{
    public:
	virtual ~ThreadSystem() = default;

	// Returns the new tid in the parent, or a negative errno.
	virtual int64_t cloneThread(RuntimeThread &parent, const CloneRequest &request) = 0;

	// Parks the thread on the futex word until woken or, when timeout_us is
	// non-zero, until the relative deadline. The syscall layer owns the
	// timespec semantics: FUTEX_WAIT carries a relative one, FUTEX_WAIT_BITSET
	// an absolute one on the instance clock.
	// Returns 0 / -EAGAIN / -ENOSYS / -EFAULT. Blocks the thread on success by
	// setting its state and letting the dispatcher stop the engine.
	virtual int64_t futexWait(RuntimeThread &thread, GuestVA uaddr, uint32_t expected, uint64_t timeout_us) = 0;
	virtual int64_t futexWake(GuestVA uaddr, int32_t count) = 0;

	// Parks `thread` on `pending` (state Blocked) until wakeThread() or, when
	// `timeout_us` is non-zero, the deadline. Returns 0 when parked, or a
	// negative errno (the thread already has a pending syscall).
	virtual int64_t parkThread(RuntimeThread &thread,
		std::unique_ptr<PendingSyscall> pending,
		uint64_t timeout_us) = 0;

	// Tries to complete the thread's pending syscall with `reason`. Returns
	// true when the thread actually woke; false for a spurious wake that left
	// it blocked, or when there is nothing pending.
	virtual bool wakeThread(RuntimeThread &thread, WakeReason reason) = 0;

	virtual void exitThread(RuntimeThread &thread, int32_t code) = 0;
	virtual void exitGroup(RuntimeThread &thread, int32_t code) = 0;
	virtual void setClearChildTid(RuntimeThread &thread, GuestVA address) = 0;

	[[nodiscard]] virtual bool stopping() const = 0;

	// Guest address that ends a host-initiated guest call (callFunction and the
	// nested calls a host stub makes, e.g. pthread_once).
	[[nodiscard]] virtual GuestVA returnTrap() const = 0;
};

} // namespace duckemu::runtime::threads
