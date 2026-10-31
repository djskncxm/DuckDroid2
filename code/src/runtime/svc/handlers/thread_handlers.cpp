// SPDX-License-Identifier: GPL-2.0-only
#include <cstdint>

#include "runtime/clock/runtime_clock.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"
#include "runtime/threads/thread_system.hpp"

namespace duckemu::runtime::svc
{
namespace
{

// futex(2) operation encoding. The private/realtime bits are modifiers, not
// operations. WAIT/WAKE cover the plain mutex path; WAIT_BITSET/WAKE_BITSET
// are what bionic's condition variables use.
constexpr uint32_t kFutexWait = 0;
constexpr uint32_t kFutexWake = 1;
constexpr uint32_t kFutexWaitBitset = 9;
constexpr uint32_t kFutexWakeBitset = 10;
constexpr uint32_t kFutexPrivateFlag = 128;
constexpr uint32_t kFutexClockRealtime = 256;
constexpr uint32_t kFutexCmdMask = ~(kFutexPrivateFlag | kFutexClockRealtime);

// A struct timespec in guest memory: tv_sec at +0, tv_nsec at +8.
bool readTimespec(RuntimeMemory &memory, uint64_t address, int64_t *seconds, int64_t *nanoseconds)
{
	return memory.read(GuestVA{ address }, seconds, sizeof(*seconds)).ok() &&
	       memory.read(GuestVA{ address + 8 }, nanoseconds, sizeof(*nanoseconds)).ok();
}

// Converts a futex timeout argument to relative microseconds. FUTEX_WAIT
// carries a relative timespec; FUTEX_WAIT_BITSET an absolute one on the
// monotonic clock, or on the realtime clock when FUTEX_CLOCK_REALTIME is set.
// A deadline already in the past yields 1 us rather than "no deadline".
std::optional<uint64_t> futexTimeoutUs(SyscallContext &ctx,
	uint32_t raw_operation,
	uint64_t timeout_address,
	int64_t *error)
{
	if (timeout_address == 0)
	{
		return uint64_t{ 0 };
	}
	int64_t seconds = 0;
	int64_t nanoseconds = 0;
	if (!readTimespec(ctx.memory, timeout_address, &seconds, &nanoseconds))
	{
		*error = kEfault;
		return std::nullopt;
	}
	if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000LL)
	{
		*error = kEinval;
		return std::nullopt;
	}
	const uint64_t as_ns = static_cast<uint64_t>(seconds) * 1000000000ULL +
			       static_cast<uint64_t>(nanoseconds);
	uint64_t wait_ns = as_ns;
	if ((raw_operation & kFutexCmdMask) == kFutexWaitBitset)
	{
		const bool realtime = (raw_operation & kFutexClockRealtime) != 0;
		const uint64_t now_ns =
			realtime ? ctx.clock.nextRealtimeNs() : ctx.clock.nextMonotonicNs();
		wait_ns = as_ns > now_ns ? as_ns - now_ns : 0;
	}
	// Rounded up: a sub-microsecond wait still expires.
	uint64_t timeout_us = (wait_ns + 999) / 1000;
	if (timeout_us == 0)
	{
		timeout_us = 1;
	}
	return timeout_us;
}

int64_t sysClone(SyscallContext &ctx)
{
	threads::CloneRequest request;
	request.flags = ctx.args[0];
	request.stack_top = GuestVA{ ctx.args[1] };
	request.parent_tid = GuestVA{ ctx.args[2] };
	// CONFIG_CLONE_BACKWARDS: tls precedes child_tid on arm64.
	request.tls = GuestVA{ ctx.args[3] };
	request.child_tid = GuestVA{ ctx.args[4] };
	return ctx.threads.cloneThread(ctx.thread, request);
}

std::optional<int64_t> sysFutex(SyscallContext &ctx)
{
	const GuestVA uaddr{ ctx.args[0] };
	const uint32_t raw_operation = static_cast<uint32_t>(ctx.args[1]);
	const uint32_t operation = raw_operation & kFutexCmdMask;
	const uint32_t value = static_cast<uint32_t>(ctx.args[2]);

	switch (operation)
	{
	case kFutexWait:
	case kFutexWaitBitset:
	{
		// WAIT_BITSET's val3 is the bit mask. There is one wait queue per
		// address and every waiter matches every bit, so only the zero-mask
		// error case needs the argument.
		if (operation == kFutexWaitBitset && static_cast<uint32_t>(ctx.args[5]) == 0)
		{
			return kError(kEinval);
		}
		int64_t error = 0;
		auto timeout_us = futexTimeoutUs(ctx, raw_operation, ctx.args[3], &error);
		if (!timeout_us.has_value())
		{
			return error;
		}
		return ctx.threads.futexWait(ctx.thread, uaddr, value, timeout_us.value());
	}
	case kFutexWake:
	case kFutexWakeBitset:
		if (operation == kFutexWakeBitset && static_cast<uint32_t>(ctx.args[5]) == 0)
		{
			return kError(kEinval);
		}
		return ctx.threads.futexWake(uaddr, static_cast<int32_t>(value));
	default:
		// PI, requeue and the *_OP variants are not implemented.
		return std::nullopt;
	}
}

int64_t sysSetTidAddress(SyscallContext &ctx)
{
	ctx.threads.setClearChildTid(ctx.thread, GuestVA{ ctx.args[0] });
	return static_cast<int64_t>(ctx.tid);
}

int64_t sysExit(SyscallContext &ctx)
{
	ctx.threads.exitThread(ctx.thread, static_cast<int32_t>(ctx.args[0]));
	return 0;
}

int64_t sysExitGroup(SyscallContext &ctx)
{
	ctx.threads.exitGroup(ctx.thread, static_cast<int32_t>(ctx.args[0]));
	return 0;
}

} // namespace

std::optional<int64_t> handleThreadSyscall(SyscallContext &ctx, uint32_t number)
{
	switch (number)
	{
	case kSysClone:
		return sysClone(ctx);
	case kSysFutex:
		return sysFutex(ctx);
	case kSysSetTidAddress:
		return sysSetTidAddress(ctx);
	case kSysExit:
		return sysExit(ctx);
	case kSysExitGroup:
		return sysExitGroup(ctx);
	default:
		return std::nullopt;
	}
}

} // namespace duckemu::runtime::svc
