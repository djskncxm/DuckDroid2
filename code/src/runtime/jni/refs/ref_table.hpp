// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "common/error/result.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
}

namespace duckemu::runtime::jni
{

class JniArena;

// GetObjectRefType's answer. The values are part of the JNI contract
// (jobjectRefType); invalid covers deleted and unknown handles.
enum class RefType : uint32_t
{
	Invalid = 0,
	Local = 1,
	Global = 2,
	WeakGlobal = 3,
};

// JNI references. A handle is the guest address of an 8-byte cell holding the
// object address; the cell is the guest-visible value, the object table stays
// host-side. Local references belong to one guest thread (keyed by TPIDR_EL0);
// global references live until the instance is destroyed.
//
// Callers hold the instance execution lock, so no internal locking here.
class RefTable
{
    public:
	RefTable(RuntimeMemory &memory, JniArena &arena);

	Result<uint64_t> newLocal(uint64_t thread_key, uint64_t object);
	Result<uint64_t> newGlobal(uint64_t object);

	// Object address behind a handle, or 0 for null/deleted/unknown handles.
	[[nodiscard]] uint64_t resolve(uint64_t handle) const;

	// Which kind of reference a live handle is (GetObjectRefType).
	[[nodiscard]] RefType typeOf(uint64_t handle) const;

	bool deleteLocal(uint64_t thread_key, uint64_t handle);
	bool deleteGlobal(uint64_t handle);

	// PushLocalFrame / PopLocalFrame. Pop with a non-null |result| re-creates
	// that reference in the enclosing frame and returns the new handle.
	void pushFrame(uint64_t thread_key);
	Result<uint64_t> popFrame(uint64_t thread_key, uint64_t result);

    private:
	struct ThreadState
	{
		std::vector<uint64_t> handles;
		std::vector<size_t> frames;
	};

	Result<uint64_t> store(uint64_t object);

	RuntimeMemory &memory_;
	JniArena &arena_;
	std::unordered_map<uint64_t, ThreadState> threads_;
	std::vector<uint64_t> globals_;
};

} // namespace duckemu::runtime::jni
