// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/jni/refs/ref_table.hpp"

#include <algorithm>

#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::jni
{

RefTable::RefTable(RuntimeMemory &memory, JniArena &arena)
	: memory_(memory)
	, arena_(arena)
{
}

Result<uint64_t> RefTable::store(uint64_t object)
{
	auto cell = arena_.allocate(sizeof(uint64_t), 8);
	if (!cell.ok())
	{
		return cell.error();
	}
	auto written = memory_.write(GuestVA{ cell.value() }, &object, sizeof(object));
	if (!written.ok())
	{
		return written.error();
	}
	return cell.value();
}

Result<uint64_t> RefTable::newLocal(uint64_t thread_key, uint64_t object)
{
	if (object == 0)
	{
		return uint64_t{ 0 };
	}
	auto handle = store(object);
	if (!handle.ok())
	{
		return handle.error();
	}
	threads_[thread_key].handles.push_back(handle.value());
	return handle.value();
}

Result<uint64_t> RefTable::newGlobal(uint64_t object)
{
	if (object == 0)
	{
		return uint64_t{ 0 };
	}
	auto handle = store(object);
	if (!handle.ok())
	{
		return handle.error();
	}
	globals_.push_back(handle.value());
	return handle.value();
}

uint64_t RefTable::resolve(uint64_t handle) const
{
	if (handle == 0)
	{
		return 0;
	}
	uint64_t object = 0;
	auto read = memory_.read(GuestVA{ handle }, &object, sizeof(object));
	return read.ok() ? object : 0;
}

RefType RefTable::typeOf(uint64_t handle) const
{
	if (resolve(handle) == 0)
	{
		return RefType::Invalid;
	}
	if (std::find(globals_.begin(), globals_.end(), handle) != globals_.end())
	{
		return RefType::Global;
	}
	for (const auto &entry : threads_)
	{
		if (std::find(entry.second.handles.begin(), entry.second.handles.end(), handle) !=
			entry.second.handles.end())
		{
			return RefType::Local;
		}
	}
	return RefType::Invalid;
}

bool RefTable::deleteLocal(uint64_t thread_key, uint64_t handle)
{
	if (handle == 0)
	{
		return true;
	}
	const uint64_t zero = 0;
	auto written = memory_.write(GuestVA{ handle }, &zero, sizeof(zero));
	if (!written.ok())
	{
		return false;
	}
	const auto found = threads_.find(thread_key);
	if (found != threads_.end())
	{
		// The slot stays in the vector so frame marks remain valid; a second
		// DeleteLocalRef is a no-op.
	}
	return true;
}

bool RefTable::deleteGlobal(uint64_t handle)
{
	if (handle == 0)
	{
		return true;
	}
	const uint64_t zero = 0;
	auto written = memory_.write(GuestVA{ handle }, &zero, sizeof(zero));
	return written.ok();
}

void RefTable::pushFrame(uint64_t thread_key)
{
	threads_[thread_key].frames.push_back(threads_[thread_key].handles.size());
}

Result<uint64_t> RefTable::popFrame(uint64_t thread_key, uint64_t result)
{
	ThreadState &state = threads_[thread_key];
	if (state.frames.empty())
	{
		return Error{ ErrorCode::InvalidArgument, "PopLocalFrame without PushLocalFrame" };
	}
	const size_t mark = state.frames.back();
	state.frames.pop_back();

	uint64_t moved_object = 0;
	if (result != 0)
	{
		moved_object = resolve(result);
	}
	for (size_t index = mark; index < state.handles.size(); ++index)
	{
		const uint64_t zero = 0;
		(void)memory_.write(GuestVA{ state.handles[index] }, &zero, sizeof(zero));
	}
	state.handles.resize(mark);

	if (result == 0)
	{
		return uint64_t{ 0 };
	}
	auto handle = store(moved_object);
	if (!handle.ok())
	{
		return handle.error();
	}
	state.handles.push_back(handle.value());
	return handle.value();
}

} // namespace duckemu::runtime::jni
