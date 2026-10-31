// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

namespace duckemu::runtime
{

// Instance-level wall/monotonic clock, modelled on DuckDroid's OsState.
//
// Host mode (default): reads the host clock, so the guest sees real time.
// Injected mode: reads start at the injected base and advance exactly 1 ms per
// read. That is what makes a replay byte-identical: the same input sequence
// produces the same timestamps in every process.
//
// Reads are serialized by the caller (the scheduler's execution lock), so the
// counters need no internal locking.
class RuntimeClock
{
    public:
	RuntimeClock() = default;

	[[nodiscard]] uint64_t nextRealtimeNs();
	[[nodiscard]] uint64_t nextMonotonicNs();

	// DuckDroid's AdvanceSleep: a guest sleep is not a real sleep. Injected
	// clocks move forward by the requested duration; host clocks keep a
	// sleep offset so the guest never sees less time pass than it asked for.
	void advanceSleep(uint64_t ns);

	void setWallTimeNs(uint64_t ns);
	void setMonotonicTimeNs(uint64_t ns);
	void useHostWallClock();
	void useHostMonotonicClock();

	[[nodiscard]] bool wallTimeInjected() const
	{
		return wall_injected_;
	}
	[[nodiscard]] bool monotonicTimeInjected() const
	{
		return monotonic_injected_;
	}
	// Current stored value (the next one that will be returned when injected).
	[[nodiscard]] uint64_t wallTimeNs() const
	{
		return wall_ns_;
	}
	[[nodiscard]] uint64_t monotonicTimeNs() const
	{
		return monotonic_ns_;
	}

    private:
	static constexpr uint64_t kAdvanceNs = 1000000; // 1 ms per read

	[[nodiscard]] uint64_t hostRealtimeNs() const;
	[[nodiscard]] uint64_t hostMonotonicNs() const;

	bool wall_injected_ = false;
	bool monotonic_injected_ = false;
	uint64_t wall_ns_ = 0;
	uint64_t monotonic_ns_ = 0;
	// Only used in host mode; injected clocks absorb the sleep directly.
	uint64_t wall_sleep_offset_ns_ = 0;
	uint64_t monotonic_sleep_offset_ns_ = 0;
};

} // namespace duckemu::runtime
