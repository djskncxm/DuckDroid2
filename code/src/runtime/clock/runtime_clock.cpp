// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/clock/runtime_clock.hpp"

#include <chrono>
#include <limits>

namespace duckemu::runtime
{
namespace
{

uint64_t saturatingAdd(uint64_t value, uint64_t increment)
{
	return value > std::numeric_limits<uint64_t>::max() - increment
		       ? std::numeric_limits<uint64_t>::max()
		       : value + increment;
}

} // namespace

uint64_t RuntimeClock::hostRealtimeNs() const
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					     std::chrono::system_clock::now().time_since_epoch())
					 .count());
}

uint64_t RuntimeClock::hostMonotonicNs() const
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					     std::chrono::steady_clock::now().time_since_epoch())
					 .count());
}

uint64_t RuntimeClock::nextRealtimeNs()
{
	if (!wall_injected_)
	{
		return saturatingAdd(hostRealtimeNs(), wall_sleep_offset_ns_);
	}
	const uint64_t value = wall_ns_;
	wall_ns_ = saturatingAdd(wall_ns_, kAdvanceNs);
	return value;
}

uint64_t RuntimeClock::nextMonotonicNs()
{
	if (!monotonic_injected_)
	{
		return saturatingAdd(hostMonotonicNs(), monotonic_sleep_offset_ns_);
	}
	const uint64_t value = monotonic_ns_;
	monotonic_ns_ = saturatingAdd(monotonic_ns_, kAdvanceNs);
	return value;
}

void RuntimeClock::advanceSleep(uint64_t ns)
{
	if (wall_injected_)
	{
		wall_ns_ = saturatingAdd(wall_ns_, ns);
	}
	else
	{
		wall_sleep_offset_ns_ = saturatingAdd(wall_sleep_offset_ns_, ns);
	}
	if (monotonic_injected_)
	{
		monotonic_ns_ = saturatingAdd(monotonic_ns_, ns);
	}
	else
	{
		monotonic_sleep_offset_ns_ = saturatingAdd(monotonic_sleep_offset_ns_, ns);
	}
}

void RuntimeClock::setWallTimeNs(uint64_t ns)
{
	wall_ns_ = ns;
	wall_injected_ = true;
	wall_sleep_offset_ns_ = 0;
}

void RuntimeClock::setMonotonicTimeNs(uint64_t ns)
{
	monotonic_ns_ = ns;
	monotonic_injected_ = true;
	monotonic_sleep_offset_ns_ = 0;
}

void RuntimeClock::useHostWallClock()
{
	wall_injected_ = false;
	wall_ns_ = 0;
	wall_sleep_offset_ns_ = 0;
}

void RuntimeClock::useHostMonotonicClock()
{
	monotonic_injected_ = false;
	monotonic_ns_ = 0;
	monotonic_sleep_offset_ns_ = 0;
}

} // namespace duckemu::runtime
