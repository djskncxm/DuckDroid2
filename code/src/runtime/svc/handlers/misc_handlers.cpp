// SPDX-License-Identifier: GPL-2.0-only
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include "runtime/clock/runtime_clock.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/random/random_source.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"

namespace duckemu::runtime::svc
{
namespace
{

constexpr uint64_t kMaxRandomBytes = 1ULL << 20;

int64_t sysGetpid(SyscallContext &ctx)
{
	return static_cast<int64_t>(ctx.pid);
}

int64_t sysGettid(SyscallContext &ctx)
{
	return static_cast<int64_t>(ctx.tid);
}

int64_t sysGetppid(SyscallContext &)
{
	return 0;
}

int64_t sysGetUid(SyscallContext &ctx)
{
	return static_cast<int64_t>(ctx.device.uid);
}

int64_t sysGetGid(SyscallContext &ctx)
{
	return static_cast<int64_t>(ctx.device.gid);
}

int64_t writeArray(SyscallContext &ctx, uint64_t address, const uint64_t *values, size_t count)
{
	if (address == 0)
	{
		return 0;
	}
	auto written = ctx.memory.write(GuestVA{ address }, values, count * sizeof(uint64_t));
	return written.ok() ? 0 : -kEfault;
}

int64_t sysPrctl(SyscallContext &ctx)
{
	constexpr uint64_t kPrGetName = 16;
	constexpr uint64_t kPrSetTaggedAddrCtrl = 55;
	constexpr uint64_t kPrGetTaggedAddrCtrl = 56;

	const uint64_t option = ctx.args[0];
	if (option == kPrSetTaggedAddrCtrl || option == kPrGetTaggedAddrCtrl)
	{
		// The guest has no MTE; bionic checks for ENOTSUP via EINVAL.
		return -kEinval;
	}
	if (option == kPrGetName)
	{
		char name[16] = {};
		const std::string &process = ctx.device.process_name;
		const size_t copy = process.size() < sizeof(name) - 1 ? process.size() : sizeof(name) - 1;
		std::memcpy(name, process.data(), copy);
		auto written = ctx.memory.write(GuestVA{ ctx.args[1] }, name, sizeof(name));
		return written.ok() ? 0 : -kEfault;
	}
	return 0;
}

int64_t sysSchedSetaffinity(SyscallContext &ctx)
{
	const uint64_t pid = ctx.args[0];
	return (pid == 0 || pid == ctx.pid) ? 0 : -kEsrch;
}

int64_t sysSchedGetaffinity(SyscallContext &ctx)
{
	const uint64_t pid = ctx.args[0];
	const uint64_t size = ctx.args[1];
	const uint64_t mask_address = ctx.args[2];
	if (pid != 0 && pid != ctx.pid)
	{
		return -kEsrch;
	}
	if (size < sizeof(uint64_t))
	{
		return -kEinval;
	}
	const uint64_t mask = 1; // one CPU
	auto written = ctx.memory.write(GuestVA{ mask_address }, &mask, sizeof(mask));
	return written.ok() ? static_cast<int64_t>(sizeof(mask)) : -kEfault;
}

int64_t sysSigaltstack(SyscallContext &ctx)
{
	const uint64_t old_stack = ctx.args[1];
	const uint64_t empty[3] = { 0, 0, 0 };
	return writeArray(ctx, old_stack, empty, 3);
}

int64_t sysRtSigprocmask(SyscallContext &ctx)
{
	const uint64_t old_set = ctx.args[2];
	const uint64_t empty = 0;
	return writeArray(ctx, old_set, &empty, 1);
}

int64_t sysGetrlimit(SyscallContext &ctx)
{
	const uint64_t limits[2] = { ~0ULL, ~0ULL };
	return writeArray(ctx, ctx.args[1], limits, 2);
}

int64_t sysPrlimit64(SyscallContext &ctx)
{
	const uint64_t pid = ctx.args[0];
	if (pid != 0 && pid != ctx.pid)
	{
		return -kEsrch;
	}
	const uint64_t limits[2] = { ~0ULL, ~0ULL };
	return writeArray(ctx, ctx.args[3], limits, 2);
}

int64_t sysGetrandom(SyscallContext &ctx)
{
	uint64_t address = ctx.args[0];
	const uint64_t count = ctx.args[1];
	if (count > kMaxRandomBytes)
	{
		return -kEinval;
	}
	if (count == 0)
	{
		return 0;
	}

	// One stream per instance, shared with /dev/{u,}random so both surfaces
	// agree; a replay can pin it when RandomSource takes a seed.
	std::array<uint8_t, 256> chunk{};
	uint64_t remaining = count;
	while (remaining > 0)
	{
		const uint64_t take = remaining < chunk.size() ? remaining : chunk.size();
		ctx.random.fill(chunk.data(), take);
		auto written = ctx.memory.write(GuestVA{ address }, chunk.data(), take);
		if (!written.ok())
		{
			return -kEfault;
		}
		address += take;
		remaining -= take;
	}
	return static_cast<int64_t>(count);
}

int64_t sysClockGettime(SyscallContext &ctx)
{
	const uint64_t clock_id = ctx.args[0];
	const uint64_t timespec = ctx.args[1];

	constexpr uint64_t kClockRealtime = 0;
	constexpr uint64_t kClockMonotonic = 1;
	constexpr uint64_t kClockProcessCputime = 2;
	constexpr uint64_t kClockThreadCputime = 3;
	constexpr uint64_t kClockMonotonicRaw = 4;
	constexpr uint64_t kClockRealtimeCoarse = 5;
	constexpr uint64_t kClockMonotonicCoarse = 6;
	constexpr uint64_t kClockBoottime = 7;

	uint64_t ns = 0;
	switch (clock_id)
	{
	case kClockRealtime:
	case kClockRealtimeCoarse:
		ns = ctx.clock.nextRealtimeNs();
		break;
	case kClockMonotonic:
	case kClockMonotonicRaw:
	case kClockMonotonicCoarse:
	case kClockBoottime:
		ns = ctx.clock.nextMonotonicNs();
		break;
	// DuckDroid parity: CPU-time clocks are approximated by monotonic time
	// instead of returning a fabricated zero. When a target actually measures
	// CPU time, replace these with a real CPU clock.
	case kClockProcessCputime:
	case kClockThreadCputime:
		ns = ctx.clock.nextMonotonicNs();
		break;
	default:
		return -kEinval;
	}

	const int64_t values[2] = { static_cast<int64_t>(ns / 1000000000ULL),
		static_cast<int64_t>(ns % 1000000000ULL) };
	auto written = ctx.memory.write(GuestVA{ timespec }, values, sizeof(values));
	return written.ok() ? 0 : -kEfault;
}

int64_t sysGettimeofday(SyscallContext &ctx)
{
	const uint64_t timeval = ctx.args[0];
	const uint64_t timezone = ctx.args[1];
	if (timeval != 0)
	{
		const uint64_t ns = ctx.clock.nextRealtimeNs();
		const uint64_t us = ns / 1000ULL;
		const int64_t values[2] = { static_cast<int64_t>(us / 1000000ULL),
			static_cast<int64_t>(us % 1000000ULL) };
		auto written = ctx.memory.write(GuestVA{ timeval }, values, sizeof(values));
		if (!written.ok())
		{
			return -kEfault;
		}
	}
	const uint64_t zero[2] = { 0, 0 };
	return writeArray(ctx, timezone, zero, 2);
}

// A struct timespec in guest memory: tv_sec at +0, tv_nsec at +8.
bool readTimespec(SyscallContext &ctx, uint64_t address, int64_t *seconds, int64_t *nanoseconds)
{
	return ctx.memory.read(GuestVA{ address }, seconds, sizeof(*seconds)).ok() &&
	       ctx.memory.read(GuestVA{ address + 8 }, nanoseconds, sizeof(*nanoseconds)).ok();
}

// Negative or out-of-range fields are EINVAL, and the seconds-to-nanoseconds
// multiply must not wrap (same parsing as DuckDroid's nanosleep).
std::optional<uint64_t> timespecToNanoseconds(int64_t seconds, int64_t nanoseconds)
{
	constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;
	if (seconds < 0 || nanoseconds < 0 || nanoseconds >= static_cast<int64_t>(kNanosecondsPerSecond))
	{
		return std::nullopt;
	}
	const uint64_t whole_seconds = static_cast<uint64_t>(seconds);
	if (whole_seconds > std::numeric_limits<uint64_t>::max() / kNanosecondsPerSecond)
	{
		return std::nullopt;
	}
	const uint64_t seconds_ns = whole_seconds * kNanosecondsPerSecond;
	const uint64_t total = seconds_ns + static_cast<uint64_t>(nanoseconds);
	if (total < seconds_ns)
	{
		return std::nullopt;
	}
	return total;
}

int64_t clearRemainder(SyscallContext &ctx, uint64_t address)
{
	if (address == 0)
	{
		return 0;
	}
	const int64_t zero[2] = { 0, 0 };
	auto written = ctx.memory.write(GuestVA{ address }, zero, sizeof(zero));
	return written.ok() ? 0 : -kEfault;
}

// The guest sleep is virtual (DuckDroid parity): the clock moves forward by
// the requested duration and the thread keeps running. Under an injected
// clock this keeps a replay reproducible without real waits.
int64_t sysNanosleep(SyscallContext &ctx)
{
	int64_t seconds = 0;
	int64_t nanoseconds = 0;
	if (!readTimespec(ctx, ctx.args[0], &seconds, &nanoseconds))
	{
		return -kEfault;
	}
	const auto duration = timespecToNanoseconds(seconds, nanoseconds);
	if (!duration.has_value())
	{
		return -kEinval;
	}
	const int64_t cleared = clearRemainder(ctx, ctx.args[1]);
	if (cleared != 0)
	{
		return cleared;
	}
	ctx.clock.advanceSleep(duration.value());
	return 0;
}

// DuckDroid treats clock_nanosleep as a relative sleep: clockid and
// TIMER_ABSTIME are ignored. No target in this tree uses the absolute form.
int64_t sysClockNanosleep(SyscallContext &ctx)
{
	int64_t seconds = 0;
	int64_t nanoseconds = 0;
	if (!readTimespec(ctx, ctx.args[2], &seconds, &nanoseconds))
	{
		return -kEfault;
	}
	const auto duration = timespecToNanoseconds(seconds, nanoseconds);
	if (!duration.has_value())
	{
		return -kEinval;
	}
	const int64_t cleared = clearRemainder(ctx, ctx.args[3]);
	if (cleared != 0)
	{
		return cleared;
	}
	ctx.clock.advanceSleep(duration.value());
	return 0;
}

// No signal delivery: sigaction/sigprocmask are accepted and forgotten, so a
// raised signal cannot reach a handler. DuckDroid answers these with 0;
// metasec probes tgkill(getpid(), gettid(), SIGRTMAX) and takes a fallback
// path when the call fails.
int64_t sysRaiseSignal(SyscallContext &)
{
	return 0;
}

int64_t sysUname(SyscallContext &ctx)
{
	constexpr uint64_t kFieldSize = 65;
	constexpr uint64_t kFieldCount = 6;
	const std::string_view fields[kFieldCount] = {
		ctx.device.sysname,
		ctx.device.nodename,
		ctx.device.kernel_release,
		ctx.device.kernel_version,
		ctx.device.machine,
		"",
	};
	std::array<char, kFieldSize * kFieldCount> buffer{};
	for (uint64_t i = 0; i < kFieldCount; ++i)
	{
		const uint64_t copy = fields[i].size() < kFieldSize - 1 ? fields[i].size() : kFieldSize - 1;
		std::memcpy(buffer.data() + i * kFieldSize, fields[i].data(), copy);
	}
	auto written = ctx.memory.write(GuestVA{ ctx.args[0] }, buffer.data(), buffer.size());
	return written.ok() ? 0 : -kEfault;
}

int64_t sysGetcwd(SyscallContext &ctx)
{
	const uint64_t buffer = ctx.args[0];
	const uint64_t size = ctx.args[1];
	if (size < 2)
	{
		return -kEinval;
	}
	const char root[2] = { '/', '\0' };
	auto written = ctx.memory.write(GuestVA{ buffer }, root, sizeof(root));
	return written.ok() ? static_cast<int64_t>(buffer) : -kEfault;
}

} // namespace

// exit/exit_group moved to thread_handlers.cpp: they act on the calling
// RuntimeThread and, for exit_group, on the whole instance.
std::optional<int64_t> handleMiscSyscall(SyscallContext &ctx, uint32_t number)
{
	switch (number)
	{
	case kSysGetpid:
		return sysGetpid(ctx);
	case kSysGettid:
		return sysGettid(ctx);
	case kSysGetppid:
		return sysGetppid(ctx);
	case kSysGetuid:
	case kSysGeteuid:
		return sysGetUid(ctx);
	case kSysGetgid:
	case kSysGetegid:
		return sysGetGid(ctx);
	case kSysPrctl:
		return sysPrctl(ctx);
	case kSysSchedSetaffinity:
		return sysSchedSetaffinity(ctx);
	case kSysSchedGetaffinity:
		return sysSchedGetaffinity(ctx);
	case kSysSchedYield:
		return 0;
	case kSysSigaltstack:
		return sysSigaltstack(ctx);
	case kSysRtSigaction:
		return 0;
	case kSysRtSigprocmask:
		return sysRtSigprocmask(ctx);
	case kSysGetrlimit:
		return sysGetrlimit(ctx);
	case kSysPrlimit64:
		return sysPrlimit64(ctx);
	case kSysSetRobustList:
	case kSysMembarrier:
		return 0;
	case kSysGetrandom:
		return sysGetrandom(ctx);
	case kSysClockGettime:
		return sysClockGettime(ctx);
	case kSysNanosleep:
		return sysNanosleep(ctx);
	case kSysClockNanosleep:
		return sysClockNanosleep(ctx);
	case kSysKill:
	case kSysTkill:
	case kSysTgkill:
		return sysRaiseSignal(ctx);
	case kSysGettimeofday:
		return sysGettimeofday(ctx);
	case kSysUname:
		return sysUname(ctx);
	case kSysGetcwd:
		return sysGetcwd(ctx);
	default:
		return std::nullopt;
	}
}

} // namespace duckemu::runtime::svc
