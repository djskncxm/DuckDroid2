// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/jni/env/jni_state.hpp"

#include <cstdio>
#include <utility>

#include "common/log/logger.hpp"
#include "runtime/jni/abi/abi_adapter.hpp"
#include "runtime/jni/jni_layout.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/threads/scheduler.hpp"

namespace duckemu::runtime::jni
{
namespace
{

constexpr uint32_t kCallGroupSize = 30; // ten result kinds, three variants each

// JNI result kind by type index inside a Call group: Object, Boolean, Byte,
// Char, Short, Int, Long, Float, Double, Void.
constexpr ValueKind kCallKinds[] = {
	ValueKind::Object,
	ValueKind::Boolean,
	ValueKind::Byte,
	ValueKind::Char,
	ValueKind::Short,
	ValueKind::Int,
	ValueKind::Long,
	ValueKind::Float,
	ValueKind::Double,
	ValueKind::Void,
};

// Field access group: Object, Boolean, Byte, Char, Short, Int, Long, Float,
// Double -- nine kinds, no void.
constexpr ValueKind kFieldKinds[] = {
	ValueKind::Object,
	ValueKind::Boolean,
	ValueKind::Byte,
	ValueKind::Char,
	ValueKind::Short,
	ValueKind::Int,
	ValueKind::Long,
	ValueKind::Float,
	ValueKind::Double,
};

// Array slot groups repeat the same eight primitive kinds.
constexpr ArrayElementKind kArrayKinds[] = {
	ArrayElementKind::Boolean,
	ArrayElementKind::Byte,
	ArrayElementKind::Char,
	ArrayElementKind::Short,
	ArrayElementKind::Int,
	ArrayElementKind::Long,
	ArrayElementKind::Float,
	ArrayElementKind::Double,
};

bool isSupportedJniVersion(int64_t version)
{
	return version == kJniVersion1_1 || version == kJniVersion1_2 || version == kJniVersion1_4 ||
	       version == kJniVersion1_6;
}

struct CallSlot
{
	bool valid = false;
	bool is_static = false;
	ValueKind result = ValueKind::Void;
	CallVariant variant = CallVariant::Plain;
};

CallSlot decodeCallSlot(uint32_t slot)
{
	CallSlot decoded;
	if (slot >= JniSlotCallStaticObjectMethod && slot < JniSlotCallStaticObjectMethod + kCallGroupSize)
	{
		decoded.valid = true;
		decoded.is_static = true;
		const uint32_t offset = slot - JniSlotCallStaticObjectMethod;
		decoded.result = kCallKinds[offset / 3];
		decoded.variant = offset % 3 == 0 ? CallVariant::Plain
						  : (offset % 3 == 1 ? CallVariant::VaList : CallVariant::Array);
	}
	else if (slot >= JniSlotCallObjectMethod && slot < JniSlotCallObjectMethod + kCallGroupSize)
	{
		decoded.valid = true;
		decoded.is_static = false;
		const uint32_t offset = slot - JniSlotCallObjectMethod;
		decoded.result = kCallKinds[offset / 3];
		decoded.variant = offset % 3 == 0 ? CallVariant::Plain
						  : (offset % 3 == 1 ? CallVariant::VaList : CallVariant::Array);
	}
	return decoded;
}

struct FieldSlot
{
	bool valid = false;
	bool is_static = false;
	bool is_set = false;
	ValueKind kind = ValueKind::Object;
};

FieldSlot decodeFieldSlot(uint32_t slot)
{
	FieldSlot decoded;
	if (slot >= JniSlotGetObjectField && slot < JniSlotGetObjectField + 9)
	{
		decoded.valid = true;
		decoded.kind = kFieldKinds[slot - JniSlotGetObjectField];
	}
	else if (slot >= JniSlotSetObjectField && slot < JniSlotSetObjectField + 9)
	{
		decoded.valid = true;
		decoded.is_set = true;
		decoded.kind = kFieldKinds[slot - JniSlotSetObjectField];
	}
	else if (slot >= JniSlotGetStaticObjectField && slot < JniSlotGetStaticObjectField + 9)
	{
		decoded.valid = true;
		decoded.is_static = true;
		decoded.kind = kFieldKinds[slot - JniSlotGetStaticObjectField];
	}
	else if (slot >= JniSlotSetStaticObjectField && slot < JniSlotSetStaticObjectField + 9)
	{
		decoded.valid = true;
		decoded.is_static = true;
		decoded.is_set = true;
		decoded.kind = kFieldKinds[slot - JniSlotSetStaticObjectField];
	}
	return decoded;
}

} // namespace

JniState::JniState(RuntimeMemory &memory,
	ThunkTable &thunks,
	observer::EventBus *events,
	std::unique_ptr<JavaWorld> world)
	: memory_(memory)
	, thunks_(thunks)
	, events_(events)
	, world_(std::move(world))
	, refs_(memory, world_->arena())
{
}

JniState::~JniState() = default;

Result<std::unique_ptr<JniState> > JniState::create(RuntimeMemory &memory,
	ThunkTable &thunks,
	observer::EventBus *events,
	std::unique_ptr<JavaWorld> world)
{
	if (world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "JniState requires a Java world" };
	}
	std::unique_ptr<JniState> state(new JniState(memory, thunks, events, std::move(world)));
	if (auto created = state->createTables(); !created.ok())
	{
		return created.error();
	}
	return state;
}

Result<void> JniState::createTables()
{
	auto env_table = memory_.map(kGuestPageSize, PermReadWrite, "jni-env-table");
	if (!env_table.ok())
	{
		return env_table.error();
	}
	env_table_ = env_table.value();

	auto vm_table = memory_.map(kGuestPageSize, PermReadWrite, "jni-vm-table");
	if (!vm_table.ok())
	{
		return vm_table.error();
	}
	vm_table_ = vm_table.value();

	auto vm_slot = memory_.map(kGuestPageSize, PermReadWrite, "jni-java-vm");
	if (!vm_slot.ok())
	{
		return vm_slot.error();
	}
	vm_value_ = vm_slot.value();

	for (uint32_t slot = 0; slot < JniSlotCount; ++slot)
	{
		auto entry = thunks_.add(std::string("JNIEnv::") + jniSlotName(slot),
			[this, slot](HostCallArgs &args) -> int64_t {
				return invokeEnv(slot, args);
			});
		if (!entry.ok())
		{
			return entry.error();
		}
		const uint64_t address = entry.value().value();
		auto written = memory_.write(GuestVA{ env_table_.value() + slot * 8 }, &address, sizeof(address));
		if (!written.ok())
		{
			return written.error();
		}
	}

	for (uint32_t slot = 0; slot < JvmSlotCount; ++slot)
	{
		auto entry = thunks_.add(std::string("JavaVM::") + jvmSlotName(slot),
			[this, slot](HostCallArgs &args) -> int64_t {
				return invokeVm(slot, args);
			});
		if (!entry.ok())
		{
			return entry.error();
		}
		const uint64_t address = entry.value().value();
		auto written = memory_.write(GuestVA{ vm_table_.value() + slot * 8 }, &address, sizeof(address));
		if (!written.ok())
		{
			return written.error();
		}
	}

	const uint64_t table = vm_table_.value();
	if (auto written = memory_.write(vm_value_, &table, sizeof(table)); !written.ok())
	{
		return written.error();
	}

	// The tables are immutable once published. Backends created later replay
	// the region with these permissions.
	if (auto protected_table = memory_.protect(env_table_, kGuestPageSize, PermRead); !protected_table.ok())
	{
		return protected_table.error();
	}
	return memory_.protect(vm_table_, kGuestPageSize, PermRead);
}

Result<uint64_t> JniState::ensureEnvSlot(uint64_t thread_key)
{
	const auto found = env_slots_.find(thread_key);
	if (found != env_slots_.end())
	{
		return found->second;
	}
	auto cell = world_->arena().allocate(sizeof(uint64_t), 8);
	if (!cell.ok())
	{
		return cell.error();
	}
	const uint64_t table = env_table_.value();
	auto written = memory_.write(GuestVA{ cell.value() }, &table, sizeof(table));
	if (!written.ok())
	{
		return written.error();
	}
	env_slots_[thread_key] = cell.value();
	return cell.value();
}

Result<uint64_t> JniState::newLocal(uint64_t thread_key, uint64_t object)
{
	return refs_.newLocal(thread_key, object);
}

Result<std::string> JniState::readUtf8(uint64_t address)
{
	if (address == 0)
	{
		return Error{ ErrorCode::InvalidArgument, "null JNI string" };
	}
	std::string text;
	text.reserve(32);
	for (uint64_t offset = 0; offset < 4096; ++offset)
	{
		char character = 0;
		auto read = memory_.read(GuestVA{ address + offset }, &character, 1);
		if (!read.ok())
		{
			return read.error();
		}
		if (character == '\0')
		{
			return text;
		}
		text.push_back(character);
	}
	return Error{ ErrorCode::InvalidArgument, "unterminated JNI string" };
}

void JniState::publishMissing(observer::JniEventKind kind,
	std::string class_descriptor,
	std::string name,
	std::string signature,
	uint32_t slot,
	uint32_t tid)
{
	DUCKEMU_WARN("jni %s class=%s name=%s sig=%s slot=%s tid=%u",
		std::string(observer::jniEventKindName(kind)).c_str(),
		class_descriptor.c_str(),
		name.c_str(),
		signature.c_str(),
		jniSlotName(slot),
		tid);
	if (events_ == nullptr)
	{
		return;
	}
	observer::JniEvent event;
	event.kind = kind;
	event.class_descriptor = std::move(class_descriptor);
	event.name = std::move(name);
	event.signature = std::move(signature);
	event.slot = slot;
	event.tid = tid;
	events_->publishJni(std::move(event));
}

void JniState::publishUnsupported(uint32_t slot, uint32_t tid)
{
	publishMissing(observer::JniEventKind::UnsupportedFunction, {}, {}, {}, slot, tid);
}

int64_t JniState::invokeEnv(uint32_t slot, HostCallArgs &args)
{
	const uint64_t thread_key = args.thread_pointer;
	const uint32_t tid = args.tid;
	auto local = [&](uint64_t object) -> int64_t {
		if (object == 0)
		{
			return 0;
		}
		auto handle = newLocal(thread_key, object);
		if (!handle.ok())
		{
			DUCKEMU_WARN("jni: cannot allocate a local reference: %s", handle.error().message.c_str());
			return 0;
		}
		return static_cast<int64_t>(handle.value());
	};

	switch (slot)
	{
	case JniSlotGetVersion:
		return kJniVersion1_6;

	case JniSlotFindClass:
	{
		auto name = readUtf8(args.x[1]);
		if (!name.ok())
		{
			publishMissing(observer::JniEventKind::MissingClass, {}, {}, {}, slot, tid);
			return 0;
		}
		if (!name.value().empty() && name.value().front() == '[')
		{
			publishUnsupported(slot, tid);
			return 0;
		}
		const uint64_t klass = world_->findClass(name.value());
		if (klass == 0)
		{
			publishMissing(observer::JniEventKind::MissingClass, name.value(), {}, {}, slot, tid);
			return 0;
		}
		return local(klass);
	}

	case JniSlotGetSuperclass:
	{
		const uint64_t klass = refs_.resolve(args.x[1]);
		return world_->isClassObject(klass) ? local(world_->superclassOf(klass)) : 0;
	}

	case JniSlotIsAssignableFrom:
	{
		const uint64_t target = refs_.resolve(args.x[1]);
		const uint64_t source = refs_.resolve(args.x[2]);
		if (!world_->isClassObject(target) || !world_->isClassObject(source))
		{
			return 0;
		}
		return world_->isAssignableFrom(target, source) ? 1 : 0;
	}

	case JniSlotThrow:
		world_->setPendingException(refs_.resolve(args.x[1]));
		return 0;

	case JniSlotThrowNew:
	{
		const uint64_t klass = refs_.resolve(args.x[1]);
		if (klass != 0)
		{
			auto object = world_->allocateObject(klass);
			if (object.ok())
			{
				world_->setPendingException(object.value());
			}
		}
		return 0;
	}

	case JniSlotExceptionOccurred:
		return local(world_->pendingException());

	case JniSlotExceptionDescribe:
		if (world_->pendingException() != 0)
		{
			DUCKEMU_WARN("jni: pending exception class=%s",
				std::string(world_->descriptorOfClass(world_->classOf(world_->pendingException()))).c_str());
		}
		return 0;

	case JniSlotExceptionClear:
		world_->clearPendingException();
		return 0;

	case JniSlotExceptionCheck:
		return world_->pendingException() != 0 ? 1 : 0;

	case JniSlotPushLocalFrame:
		refs_.pushFrame(thread_key);
		return kJniOk;

	case JniSlotPopLocalFrame:
	{
		auto popped = refs_.popFrame(thread_key, args.x[1]);
		return popped.ok() ? static_cast<int64_t>(popped.value()) : 0;
	}

	case JniSlotNewGlobalRef:
	{
		auto handle = refs_.newGlobal(refs_.resolve(args.x[1]));
		return handle.ok() ? static_cast<int64_t>(handle.value()) : 0;
	}

	case JniSlotDeleteGlobalRef:
		refs_.deleteGlobal(args.x[1]);
		return 0;

	case JniSlotDeleteLocalRef:
		refs_.deleteLocal(thread_key, args.x[1]);
		return 0;

	case JniSlotIsSameObject:
		return refs_.resolve(args.x[1]) == refs_.resolve(args.x[2]) ? 1 : 0;

	case JniSlotNewLocalRef:
		return local(refs_.resolve(args.x[1]));

	case JniSlotGetObjectRefType:
		return static_cast<int64_t>(refs_.typeOf(args.x[1]));

	case JniSlotEnsureLocalCapacity:
		return kJniOk;

	case JniSlotAllocObject:
	{
		const uint64_t klass = refs_.resolve(args.x[1]);
		if (!world_->isClassObject(klass))
		{
			return 0;
		}
		auto object = world_->allocateObject(klass);
		return object.ok() ? local(object.value()) : 0;
	}

	case JniSlotGetObjectClass:
	{
		const uint64_t object = refs_.resolve(args.x[1]);
		return object == 0 ? 0 : local(world_->classOf(object));
	}

	case JniSlotIsInstanceOf:
	{
		const uint64_t object = refs_.resolve(args.x[1]);
		const uint64_t klass = refs_.resolve(args.x[2]);
		return object != 0 && world_->isClassObject(klass) && world_->isInstanceOf(object, klass) ? 1 : 0;
	}

	case JniSlotGetMethodID:
	case JniSlotGetStaticMethodID:
	{
		const bool is_static = slot == JniSlotGetStaticMethodID;
		const uint64_t klass = refs_.resolve(args.x[1]);
		auto name = readUtf8(args.x[2]);
		auto signature = readUtf8(args.x[3]);
		if (!world_->isClassObject(klass))
		{
			publishMissing(observer::JniEventKind::MissingClass, {}, name.ok() ? name.value() : "",
				signature.ok() ? signature.value() : "", slot, tid);
			return 0;
		}
		if (!name.ok() || !signature.ok())
		{
			publishMissing(observer::JniEventKind::MissingMethod,
				std::string(world_->descriptorOfClass(klass)), {}, {}, slot, tid);
			return 0;
		}
		const uint64_t method = world_->findMethod(klass, name.value(), signature.value(), is_static);
		if (method == 0)
		{
			publishMissing(observer::JniEventKind::MissingMethod,
				std::string(world_->descriptorOfClass(klass)), name.value(), signature.value(), slot, tid);
			return 0;
		}
		return static_cast<int64_t>(method);
	}

	case JniSlotGetFieldID:
	case JniSlotGetStaticFieldID:
	{
		const bool is_static = slot == JniSlotGetStaticFieldID;
		const uint64_t klass = refs_.resolve(args.x[1]);
		auto name = readUtf8(args.x[2]);
		auto signature = readUtf8(args.x[3]);
		if (!world_->isClassObject(klass))
		{
			publishMissing(observer::JniEventKind::MissingClass, {}, {}, {}, slot, tid);
			return 0;
		}
		if (!name.ok() || !signature.ok())
		{
			publishMissing(observer::JniEventKind::MissingField,
				std::string(world_->descriptorOfClass(klass)), {}, {}, slot, tid);
			return 0;
		}
		const uint64_t field = world_->findField(klass, name.value(), signature.value(), is_static);
		if (field == 0)
		{
			publishMissing(observer::JniEventKind::MissingField,
				std::string(world_->descriptorOfClass(klass)), name.value(), signature.value(), slot, tid);
			return 0;
		}
		return static_cast<int64_t>(field);
	}

	case JniSlotNewStringUTF:
	{
		auto text = readUtf8(args.x[1]);
		if (!text.ok())
		{
			return 0;
		}
		auto object = world_->newString(text.value());
		return object.ok() ? local(object.value()) : 0;
	}

	case JniSlotGetStringLength:
	{
		const uint64_t object = refs_.resolve(args.x[1]);
		auto length = world_->stringLength(object);
		return length.ok() ? static_cast<int64_t>(length.value()) : 0;
	}

	case JniSlotGetStringUTFLength:
	{
		const uint64_t object = refs_.resolve(args.x[1]);
		auto text = world_->stringUtf8(object);
		return text.ok() ? static_cast<int64_t>(text.value().size()) : 0;
	}

	case JniSlotGetStringUTFChars:
	{
		const uint64_t object = refs_.resolve(args.x[1]);
		auto text = world_->stringUtf8(object);
		if (!text.ok())
		{
			return 0;
		}
		auto buffer = world_->arena().allocate(text.value().size() + 1, 1);
		if (!buffer.ok())
		{
			return 0;
		}
		if (auto written = memory_.write(GuestVA{ buffer.value() }, text.value().c_str(),
				text.value().size() + 1);
			!written.ok())
		{
			return 0;
		}
		if (args.x[2] != 0)
		{
			const uint8_t is_copy = 1;
			(void)memory_.write(GuestVA{ args.x[2] }, &is_copy, sizeof(is_copy));
		}
		return static_cast<int64_t>(buffer.value());
	}

	case JniSlotReleaseStringUTFChars:
		// The arena is instance-scoped; M5 does not recycle individual buffers.
		return 0;

	case JniSlotMonitorEnter:
	case JniSlotMonitorExit:
		return kJniOk;

	case JniSlotGetJavaVM:
	{
		if (args.x[1] == 0)
		{
			return kJniEInval;
		}
		const uint64_t vm = vm_value_.value();
		auto written = memory_.write(GuestVA{ args.x[1] }, &vm, sizeof(vm));
		return written.ok() ? kJniOk : kJniEInval;
	}

	case JniSlotRegisterNatives:
	{
		const uint64_t klass = refs_.resolve(args.x[1]);
		if (!world_->isClassObject(klass))
		{
			publishMissing(observer::JniEventKind::MissingClass, {}, {}, {}, slot, tid);
			return kJniErr;
		}
		const std::string class_name(world_->descriptorOfClass(klass));
		const uint64_t methods = args.x[2];
		const int32_t count = static_cast<int32_t>(args.x[3]);
		if (count < 0 || (count > 0 && methods == 0))
		{
			return kJniEInval;
		}
		for (int32_t index = 0; index < count; ++index)
		{
			uint64_t fields[3] = { 0, 0, 0 };
			auto read = memory_.read(GuestVA{ methods + static_cast<uint64_t>(index) * 24 },
				fields,
				sizeof(fields));
			if (!read.ok())
			{
				return kJniErr;
			}
			auto name = readUtf8(fields[0]);
			auto signature = readUtf8(fields[1]);
			if (!name.ok() || !signature.ok())
			{
				publishMissing(observer::JniEventKind::MissingMethod, class_name, {}, {}, slot, tid);
				return kJniErr;
			}
			auto bound = world_->registerNative(klass, name.value(), signature.value(), fields[2]);
			if (!bound.ok())
			{
				publishMissing(observer::JniEventKind::MissingMethod, class_name, name.value(),
					signature.value(), slot, tid);
				return kJniErr;
			}
		}
		return kJniOk;
	}

	case JniSlotNewObject:
	case JniSlotNewObjectV:
	case JniSlotNewObjectA:
		return newObject(slot, args);

	default:
		break;
	}

	if (decodeCallSlot(slot).valid || (slot >= JniSlotCallNonvirtualObjectMethod &&
										  slot < JniSlotCallNonvirtualObjectMethod + kCallGroupSize))
	{
		return callMethod(slot, args);
	}
	if (decodeFieldSlot(slot).valid)
	{
		return accessField(slot, args);
	}
	// GetArrayLength, New*Array, *ArrayElements, *ArrayRegion.
	if (slot >= JniSlotGetArrayLength && slot <= JniSlotSetDoubleArrayRegion)
	{
		return accessArray(slot, args);
	}

	publishUnsupported(slot, tid);
	return 0;
}

int64_t JniState::callMethod(uint32_t slot, HostCallArgs &args)
{
	const uint32_t nonvirtual_base = JniSlotCallNonvirtualObjectMethod;
	if (slot >= nonvirtual_base && slot < nonvirtual_base + kCallGroupSize)
	{
		publishUnsupported(slot, args.tid);
		return 0;
	}

	const CallSlot decoded = decodeCallSlot(slot);
	if (!decoded.valid)
	{
		publishUnsupported(slot, args.tid);
		return 0;
	}
	if (decoded.result == ValueKind::Float || decoded.result == ValueKind::Double)
	{
		// The adapter does not return floating-point results in v0.
		publishUnsupported(slot, args.tid);
		return 0;
	}

	const MethodRecord *record = world_->method(args.x[2]);
	if (record == nullptr || record->is_static != decoded.is_static || record->parsed.result != decoded.result)
	{
		publishMissing(observer::JniEventKind::MissingMethod, {}, {}, {}, slot, args.tid);
		return 0;
	}

	auto raw_args = readCallArguments(record->parsed, args.x, decoded.variant, memory_);
	if (!raw_args.ok())
	{
		publishUnsupported(slot, args.tid);
		return 0;
	}

	std::vector<uint64_t> resolved;
	resolved.reserve(raw_args.value().size());
	for (size_t index = 0; index < raw_args.value().size(); ++index)
	{
		const ValueKind kind = record->parsed.args[index];
		resolved.push_back(kind == ValueKind::Object ? refs_.resolve(raw_args.value()[index])
													 : raw_args.value()[index]);
	}

	const uint64_t receiver_handle = args.x[1];
	const uint64_t receiver_object = refs_.resolve(receiver_handle);
	if (!decoded.is_static && receiver_object == 0)
	{
		publishMissing(observer::JniEventKind::MissingMethod, {}, record->name, record->signature,
			slot, args.tid);
		return 0;
	}

	if (record->native_address != 0)
	{
		if (!args.call_guest)
		{
			publishUnsupported(slot, args.tid);
			return 0;
		}
		auto env = ensureEnvSlot(args.thread_pointer);
		if (!env.ok())
		{
			return 0;
		}
		std::array<uint64_t, 8> guest_args{};
		guest_args[0] = env.value();
		guest_args[1] = receiver_handle;
		for (size_t index = 0; index < raw_args.value().size() && index + 2 < guest_args.size(); ++index)
		{
			guest_args[index + 2] = raw_args.value()[index];
		}
		auto called = args.call_guest(GuestVA{ record->native_address }, guest_args);
		if (!called.ok())
		{
			DUCKEMU_WARN("jni: nested call to %s%s failed: %s", record->name.c_str(),
				record->signature.c_str(), called.error().message.c_str());
			return 0;
		}
		return called.value();
	}

	if (!record->shim)
	{
		publishMissing(observer::JniEventKind::MissingMethod,
			std::string(world_->descriptorOfClass(record->class_object)), record->name,
			record->signature, slot, args.tid);
		return 0;
	}

	InvokeContext call;
	call.receiver = receiver_object;
	call.args = std::move(resolved);
	call.tid = args.tid;
	auto called = record->shim(*world_, call);
	if (!called.ok())
	{
		DUCKEMU_WARN("jni: %s%s shim failed: %s", record->name.c_str(), record->signature.c_str(),
			called.error().message.c_str());
		return 0;
	}
	uint64_t result = called.value();
	if (decoded.result == ValueKind::Object && result != 0)
	{
		auto handle = newLocal(args.thread_pointer, result);
		if (!handle.ok())
		{
			return 0;
		}
		result = handle.value();
	}
	return static_cast<int64_t>(result);
}

int64_t JniState::accessField(uint32_t slot, HostCallArgs &args)
{
	const FieldSlot decoded = decodeFieldSlot(slot);
	if (!decoded.valid || decoded.kind == ValueKind::Float || decoded.kind == ValueKind::Double)
	{
		publishUnsupported(slot, args.tid);
		return 0;
	}

	const FieldRecord *record = world_->field(args.x[2]);
	const uint64_t target_class = decoded.is_static ? refs_.resolve(args.x[1])
													: world_->classOf(refs_.resolve(args.x[1]));
	if (record == nullptr || record->is_static != decoded.is_static || record->kind != decoded.kind)
	{
		publishMissing(observer::JniEventKind::MissingField,
			target_class == 0 ? std::string{} : std::string(world_->descriptorOfClass(target_class)),
			{}, {}, slot, args.tid);
		return 0;
	}

	if (decoded.is_set)
	{
		const uint64_t value = decoded.kind == ValueKind::Object ? refs_.resolve(args.x[3]) : args.x[3];
		const Result<void> written = decoded.is_static
										 ? world_->writeStaticField(target_class, record->id, value)
										 : world_->writeInstanceField(refs_.resolve(args.x[1]), record->id, value);
		if (!written.ok())
		{
			DUCKEMU_WARN("jni: field write failed: %s", written.error().message.c_str());
		}
		return 0;
	}

	Result<uint64_t> value = decoded.is_static
								 ? world_->readStaticField(target_class, record->id)
								 : world_->readInstanceField(refs_.resolve(args.x[1]), record->id);
	if (!value.ok())
	{
		return 0;
	}
	if (decoded.kind == ValueKind::Object)
	{
		auto handle = newLocal(args.thread_pointer, value.value());
		return handle.ok() ? static_cast<int64_t>(handle.value()) : 0;
	}
	return static_cast<int64_t>(value.value());
}

int64_t JniState::newObject(uint32_t slot, HostCallArgs &args)
{
	const uint64_t klass = refs_.resolve(args.x[1]);
	if (!world_->isClassObject(klass))
	{
		publishMissing(observer::JniEventKind::MissingClass, {}, {}, {}, slot, args.tid);
		return 0;
	}
	const MethodRecord *record = world_->method(args.x[2]);
	if (record == nullptr || record->is_static || record->class_object != klass)
	{
		publishMissing(observer::JniEventKind::MissingMethod,
			std::string(world_->descriptorOfClass(klass)), {}, {}, slot, args.tid);
		return 0;
	}

	const CallVariant variant = slot == JniSlotNewObjectV
					    ? CallVariant::VaList
					    : (slot == JniSlotNewObjectA ? CallVariant::Array : CallVariant::Plain);
	auto raw_args = readCallArguments(record->parsed, args.x, variant, memory_);
	if (!raw_args.ok())
	{
		publishUnsupported(slot, args.tid);
		return 0;
	}
	std::vector<uint64_t> resolved;
	resolved.reserve(raw_args.value().size());
	for (size_t index = 0; index < raw_args.value().size(); ++index)
	{
		resolved.push_back(record->parsed.args[index] == ValueKind::Object
					   ? refs_.resolve(raw_args.value()[index])
					   : raw_args.value()[index]);
	}

	auto object = world_->allocateObject(klass);
	if (!object.ok())
	{
		DUCKEMU_WARN("jni: NewObject failed: %s", object.error().message.c_str());
		return 0;
	}

	if (record->native_address != 0)
	{
		if (!args.call_guest)
		{
			publishUnsupported(slot, args.tid);
			return 0;
		}
		auto env = ensureEnvSlot(args.thread_pointer);
		auto receiver = newLocal(args.thread_pointer, object.value());
		if (!env.ok() || !receiver.ok())
		{
			return 0;
		}
		std::array<uint64_t, 8> guest_args{};
		guest_args[0] = env.value();
		guest_args[1] = receiver.value();
		for (size_t index = 0; index < raw_args.value().size() && index + 2 < guest_args.size();
			++index)
		{
			guest_args[index + 2] = raw_args.value()[index];
		}
		auto called = args.call_guest(GuestVA{ record->native_address }, guest_args);
		if (!called.ok())
		{
			DUCKEMU_WARN("jni: nested constructor %s%s failed: %s", record->name.c_str(),
				record->signature.c_str(), called.error().message.c_str());
			return 0;
		}
	}
	else if (record->shim)
	{
		InvokeContext call;
		call.receiver = object.value();
		call.args = std::move(resolved);
		call.tid = args.tid;
		auto called = record->shim(*world_, call);
		if (!called.ok())
		{
			DUCKEMU_WARN("jni: %s%s shim failed: %s", record->name.c_str(),
				record->signature.c_str(), called.error().message.c_str());
			return 0;
		}
	}
	else
	{
		publishMissing(observer::JniEventKind::MissingMethod,
			std::string(world_->descriptorOfClass(klass)), record->name, record->signature, slot,
			args.tid);
		return 0;
	}

	auto handle = newLocal(args.thread_pointer, object.value());
	return handle.ok() ? static_cast<int64_t>(handle.value()) : 0;
}

int64_t JniState::accessArray(uint32_t slot, HostCallArgs &args)
{
	const auto local = [&](uint64_t object) -> int64_t {
		if (object == 0)
		{
			return 0;
		}
		auto handle = newLocal(args.thread_pointer, object);
		return handle.ok() ? static_cast<int64_t>(handle.value()) : 0;
	};

	if (slot == JniSlotGetArrayLength)
	{
		auto view = world_->arrayView(refs_.resolve(args.x[1]));
		return view.ok() ? static_cast<int64_t>(view.value().length) : 0;
	}
	if (slot == JniSlotNewObjectArray)
	{
		auto array = world_->newArray(ArrayElementKind::Object,
			args.x[1],
			refs_.resolve(args.x[2]),
			refs_.resolve(args.x[3]));
		if (!array.ok())
		{
			DUCKEMU_WARN("jni: NewObjectArray failed: %s", array.error().message.c_str());
			return 0;
		}
		return local(array.value());
	}
	if (slot == JniSlotGetObjectArrayElement)
	{
		auto element = world_->readArrayElement(refs_.resolve(args.x[1]), args.x[2]);
		return element.ok() ? local(element.value()) : 0;
	}
	if (slot == JniSlotSetObjectArrayElement)
	{
		auto written = world_->writeArrayElement(refs_.resolve(args.x[1]),
			args.x[2],
			refs_.resolve(args.x[3]));
		if (!written.ok())
		{
			DUCKEMU_WARN("jni: SetObjectArrayElement failed: %s", written.error().message.c_str());
		}
		return 0;
	}

	if (slot >= JniSlotNewBooleanArray && slot <= JniSlotNewDoubleArray)
	{
		const ArrayElementKind kind = kArrayKinds[slot - JniSlotNewBooleanArray];
		auto array = world_->newArray(kind, args.x[1]);
		if (!array.ok())
		{
			DUCKEMU_WARN("jni: New*Array failed: %s", array.error().message.c_str());
			return 0;
		}
		return local(array.value());
	}

	if (slot >= JniSlotGetBooleanArrayElements && slot <= JniSlotReleaseDoubleArrayElements)
	{
		// Element storage lives in guest memory and never moves, so Get* answers
		// the real buffer (isCopy = false) and Release* is a no-op.
		if (slot >= JniSlotReleaseBooleanArrayElements)
		{
			return 0;
		}
		const ArrayElementKind kind = kArrayKinds[slot - JniSlotGetBooleanArrayElements];
		auto view = world_->arrayView(refs_.resolve(args.x[1]));
		if (!view.ok() || view.value().kind != kind)
		{
			return 0;
		}
		if (args.x[2] != 0)
		{
			const uint8_t is_copy = 0;
			(void)memory_.write(GuestVA{ args.x[2] }, &is_copy, sizeof(is_copy));
		}
		return static_cast<int64_t>(view.value().storage);
	}

	const uint32_t region_base = slot >= JniSlotSetBooleanArrayRegion
					     ? JniSlotSetBooleanArrayRegion
					     : JniSlotGetBooleanArrayRegion;
	if (slot >= region_base && slot < region_base + 8)
	{
		const bool set = slot >= JniSlotSetBooleanArrayRegion;
		const ArrayElementKind kind = kArrayKinds[slot - region_base];
		auto view = world_->arrayView(refs_.resolve(args.x[1]));
		if (!view.ok() || view.value().kind != kind)
		{
			return 0;
		}
		const int64_t start = static_cast<int32_t>(args.x[2]);
		const int64_t count = static_cast<int32_t>(args.x[3]);
		const uint64_t buffer = args.x[4];
		if (start < 0 || count < 0 ||
			static_cast<uint64_t>(start) + static_cast<uint64_t>(count) > view.value().length)
		{
			return 0;
		}
		const uint64_t element = arrayElementSize(kind);
		const uint64_t bytes = static_cast<uint64_t>(count) * element;
		const uint64_t array_address =
			view.value().storage + static_cast<uint64_t>(start) * element;
		if (bytes != 0 && buffer != 0)
		{
			std::vector<uint8_t> temporary(static_cast<size_t>(bytes));
			Result<void> done = set ? memory_.read(GuestVA{ buffer }, temporary.data(), bytes) :
						  memory_.read(GuestVA{ array_address }, temporary.data(), bytes);
			if (done.ok())
			{
				done = set ? memory_.write(GuestVA{ array_address }, temporary.data(), bytes) :
					     memory_.write(GuestVA{ buffer }, temporary.data(), bytes);
			}
			if (!done.ok())
			{
				DUCKEMU_WARN("jni: *ArrayRegion failed: %s", done.error().message.c_str());
			}
		}
		return 0;
	}

	publishUnsupported(slot, args.tid);
	return 0;
}

int64_t JniState::invokeVm(uint32_t slot, HostCallArgs &args)
{
	switch (slot)
	{
	case JvmSlotGetEnv:
	case JvmSlotAttachCurrentThread:
	case JvmSlotAttachCurrentThreadAsDaemon:
	{
		if (slot == JvmSlotGetEnv && !isSupportedJniVersion(static_cast<int64_t>(args.x[2])))
		{
			return kJniEVersion;
		}
		if (args.x[1] == 0)
		{
			return kJniEInval;
		}
		auto env = ensureEnvSlot(args.thread_pointer);
		if (!env.ok())
		{
			return kJniENoMem;
		}
		const uint64_t value = env.value();
		auto written = memory_.write(GuestVA{ args.x[1] }, &value, sizeof(value));
		return written.ok() ? kJniOk : kJniEInval;
	}

	case JvmSlotDetachCurrentThread:
		return kJniOk;

	case JvmSlotDestroyJavaVM:
		// The instance outlives the guest; destroying the VM is not supported.
		return kJniErr;

	default:
		publishMissing(observer::JniEventKind::UnsupportedFunction, {}, {}, {}, slot, args.tid);
		return kJniErr;
	}
}

Result<uint64_t> JniState::callRegisteredNative(threads::ThreadScheduler &scheduler,
	std::string_view class_descriptor,
	std::string_view name,
	std::string_view signature,
	const std::array<uint64_t, 8> &args)
{
	MethodSignature parsed;
	if (!parseMethodSignature(signature, &parsed))
	{
		return Error{ ErrorCode::InvalidArgument, "malformed native signature" };
	}

	uint64_t native_address = 0;
	uint64_t jclass = 0;
	uint64_t env = 0;
	std::array<uint64_t, 8> call_args{};
	{
		// Resolving and allocating handles writes guest memory; keep it
		// serialized with any running guest thread.
		auto execution = scheduler.lockExecution();
		const uint64_t klass = world_->findClass(class_descriptor);
		if (klass == 0)
		{
			return Error{ ErrorCode::NotFound,
				"class " + std::string(class_descriptor) + " is not declared" };
		}
		uint64_t method_id = world_->findMethod(klass, name, signature, true);
		if (method_id == 0)
		{
			method_id = world_->findMethod(klass, name, signature, false);
		}
		const MethodRecord *record = world_->method(method_id);
		if (record == nullptr || record->native_address == 0)
		{
			return Error{ ErrorCode::NotFound,
				"native method " + std::string(name) + std::string(signature) + " is not registered" };
		}
		native_address = record->native_address;
		// Everything this call creates belongs to a local frame; only an object
		// result is promoted back to the caller's frame.
		refs_.pushFrame(0);
		auto class_handle = refs_.newLocal(0, klass);
		if (!class_handle.ok())
		{
			refs_.popFrame(0, 0);
			return class_handle.error();
		}
		jclass = class_handle.value();
		auto env_slot = ensureEnvSlot(0);
		if (!env_slot.ok())
		{
			refs_.popFrame(0, 0);
			return env_slot.error();
		}
		env = env_slot.value();

		call_args[0] = env;
		call_args[1] = jclass;
		for (size_t index = 0; index < 6; ++index)
		{
			uint64_t value = args[index];
			if (index < parsed.args.size() && parsed.args[index] == ValueKind::Object && value != 0)
			{
				auto handle = refs_.newLocal(0, value);
				if (!handle.ok())
				{
					refs_.popFrame(0, 0);
					return handle.error();
				}
				value = handle.value();
			}
			call_args[index + 2] = value;
		}
	}

	auto called = scheduler.callFunction(GuestVA{ native_address }, call_args);
	uint64_t returned = 0;
	{
		auto execution = scheduler.lockExecution();
		if (!called.ok())
		{
			refs_.popFrame(0, 0);
			return called.error();
		}
		if (called.value().exit_code.has_value())
		{
			refs_.popFrame(0, 0);
			return Error{ ErrorCode::Unsupported, "a registered native called exit_group" };
		}
		returned = called.value().return_value;
		if (parsed.result == ValueKind::Object && returned != 0)
		{
			auto promoted = refs_.popFrame(0, returned);
			if (!promoted.ok())
			{
				return promoted.error();
			}
			returned = promoted.value();
		}
		else
		{
			refs_.popFrame(0, 0);
		}
	}
	return returned;
}

Result<GuestVA> JniState::lookupRegisteredNative(std::string_view class_descriptor,
	std::string_view name,
	std::string_view signature) const
{
	const uint64_t klass = world_->findClass(class_descriptor);
	if (klass == 0)
	{
		return Error{ ErrorCode::NotFound,
			"class " + std::string(class_descriptor) + " is not declared" };
	}
	uint64_t method_id = world_->findMethod(klass, name, signature, true);
	if (method_id == 0)
	{
		method_id = world_->findMethod(klass, name, signature, false);
	}
	const MethodRecord *record = world_->method(method_id);
	if (record == nullptr || record->native_address == 0)
	{
		return Error{ ErrorCode::NotFound,
			"native method " + std::string(name) + std::string(signature) + " is not registered" };
	}
	return GuestVA{ record->native_address };
}

Result<std::string> JniState::stringUtf8(uint64_t handle) const
{
	const uint64_t object = refs_.resolve(handle);
	if (object == 0)
	{
		return Error{ ErrorCode::InvalidArgument, "null jstring handle" };
	}
	return world_->stringUtf8(object);
}

Result<uint64_t> JniState::resolveHandle(uint64_t handle) const
{
	if (handle == 0)
	{
		return uint64_t{ 0 };
	}
	const uint64_t object = refs_.resolve(handle);
	if (object == 0)
	{
		return Error{ ErrorCode::InvalidArgument, "handle does not name a live reference" };
	}
	return object;
}

} // namespace duckemu::runtime::jni
