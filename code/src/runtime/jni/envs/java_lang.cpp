// SPDX-License-Identifier: GPL-2.0-only
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "common/id/ids.hpp"
#include "runtime/clock/runtime_clock.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::jni::envs
{
namespace
{

uint64_t identityHash(uint64_t object)
{
	// Stable for the lifetime of the object, non-zero for non-null objects.
	return (object >> 4) & 0x7FFFFFFFu;
}

Result<void> declare(JavaWorld &world,
	uint64_t klass,
	const char *name,
	const char *signature,
	bool is_static,
	ShimHandler shim)
{
	auto declared = world.declareMethod(klass, name, signature, is_static, std::move(shim));
	if (!declared.ok())
	{
		return declared.error();
	}
	return {};
}

std::string objectToString(JavaWorld &world, uint64_t object)
{
	const uint64_t klass = world.classOf(object);
	std::string name = descriptorToClassName(world.descriptorOfClass(klass));
	name += '@';
	char address[32];
	std::snprintf(address, sizeof(address), "%llx", static_cast<unsigned long long>(object));
	name += address;
	return name;
}

Result<void> installObject(JavaWorld &world)
{
	const uint64_t object = world.findClass("java/lang/Object");
	if (object == 0)
	{
		return Error{ ErrorCode::Internal, "java/lang/Object is not bootstrapped" };
	}

	auto result = declare(world, object, "getClass", "()Ljava/lang/Class;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.classOf(call.receiver);
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, object, "hashCode", "()I", false,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return identityHash(call.receiver);
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, object, "equals", "(Ljava/lang/Object;)Z", false,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return call.args[0] == call.receiver ? uint64_t{ 1 } : uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, object, "toString", "()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.newString(objectToString(java, call.receiver));
		});
	return result;
}

Result<void> installClass(JavaWorld &world)
{
	const uint64_t klass = world.findClass("java/lang/Class");
	if (klass == 0)
	{
		return Error{ ErrorCode::Internal, "java/lang/Class is not bootstrapped" };
	}
	return declare(world, klass, "getName", "()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.newString(descriptorToClassName(java.descriptorOfClass(call.receiver)));
		});
}

Result<void> installString(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/String", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto result = declare(world, klass, "length", "()I", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.stringLength(call.receiver);
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "charAt", "(I)C", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.stringCharAt(call.receiver, call.args[0]);
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "equals", "(Ljava/lang/Object;)Z", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args[0] == 0)
			{
				return uint64_t{ 0 };
			}
			auto equal = java.stringEquals(call.receiver, call.args[0]);
			if (!equal.ok())
			{
				// equals(null) / equals(non-String) is false, not an error.
				return uint64_t{ 0 };
			}
			return equal.value() ? uint64_t{ 1 } : uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	// java.lang.String.hashCode: s[0]*31^(n-1) + ... + s[n-1] over UTF-16.
	result = declare(world, klass, "hashCode", "()I", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto length = java.stringLength(call.receiver);
			if (!length.ok())
			{
				return length.error();
			}
			uint32_t hash = 0;
			for (uint64_t index = 0; index < length.value(); ++index)
			{
				auto character = java.stringCharAt(call.receiver, index);
				if (!character.ok())
				{
					return character.error();
				}
				hash = hash * 31u + static_cast<uint32_t>(character.value());
			}
			return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(hash)));
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "toString", "()Ljava/lang/String;", false,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return call.receiver;
		});
	if (!result.ok())
	{
		return result;
	}
	// String([B, String charsetName)V: native code commonly assembles text by
	// passing the byte[] and a charset name. The served targets pass UTF-8, so
	// the name is accepted but not interpreted.
	result = declare(world, klass, "<init>", "([BLjava/lang/String;)V", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 2 || call.args[0] == 0)
			{
				return Error{ ErrorCode::InvalidArgument,
					"String([B,String) needs a byte array" };
			}
			auto view = java.arrayView(call.args[0]);
			if (!view.ok())
			{
				return view.error();
			}
			if (view.value().kind != ArrayElementKind::Byte)
			{
				return Error{ ErrorCode::InvalidArgument,
					"String([B,String) expects a byte[]" };
			}
			std::string bytes(static_cast<size_t>(view.value().length), '\0');
			if (!bytes.empty())
			{
				auto read = java.memory().read(GuestVA{ view.value().storage },
					bytes.data(),
					bytes.size());
				if (!read.ok())
				{
					return read.error();
				}
			}
			auto set = java.setStringContent(call.receiver, bytes);
			if (!set.ok())
			{
				return set.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	// String.getBytes(String charsetName)[B: the charset name is accepted but
	// the bytes are always UTF-8, matching String([B, String) above.
	return declare(world, klass, "getBytes", "(Ljava/lang/String;)[B", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto text = java.stringUtf8(call.receiver);
			if (!text.ok())
			{
				return text.error();
			}
			auto array = java.newArray(ArrayElementKind::Byte, text.value().size());
			if (!array.ok())
			{
				return array.error();
			}
			auto view = java.arrayView(array.value());
			if (!view.ok())
			{
				return view.error();
			}
			if (!text.value().empty())
			{
				auto written = java.memory().write(GuestVA{ view.value().storage },
					text.value().data(),
					text.value().size());
				if (!written.ok())
				{
					return written.error();
				}
			}
			return array.value();
		});
}

Result<void> installSystem(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/System", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto result = declare(world, klass, "currentTimeMillis", "()J", true,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.clock().nextRealtimeNs() / 1000000ULL;
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "nanoTime", "()J", true,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.clock().nextMonotonicNs();
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "identityHashCode", "(Ljava/lang/Object;)I", true,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return call.args[0] == 0 ? uint64_t{ 0 } : identityHash(call.args[0]);
		});
	if (!result.ok())
	{
		return result;
	}
	// System.getProperty serves the JVM properties the Android runtime seeds
	// itself (libcore System.initProperties); unknown keys are null, not "".
	// These are platform constants, not device values: ro.* belongs to the
	// property service and is deliberately not served here.
	return declare(world, klass, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;",
		true, [](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.empty() || call.args[0] == 0)
			{
				return uint64_t{ 0 };
			}
			auto key = java.stringUtf8(call.args[0]);
			if (!key.ok())
			{
				return key.error();
			}
			const char *value = nullptr;
			if (key.value() == "os.arch")
			{
				value = "aarch64";
			}
			else if (key.value() == "java.vm.version")
			{
				value = "2.1.0";
			}
			else if (key.value() == "java.vm.name")
			{
				value = "Dalvik";
			}
			else if (key.value() == "java.vendor")
			{
				value = "The Android Project";
			}
			else if (key.value() == "os.name")
			{
				value = "Linux";
			}
			if (value == nullptr)
			{
				return uint64_t{ 0 };
			}
			return java.newString(value);
		});
}

// java.lang.Thread: targets that label their signer output with the current
// thread need the object plus its name. One host thread serves the instance,
// so currentThread() always answers the same object.
Result<void> installThread(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Thread", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto element = world.declareClass("java/lang/StackTraceElement", "java/lang/Object");
	if (!element.ok())
	{
		return element.error();
	}
	const uint64_t element_class = element.value();

	auto thread = world.allocateObject(klass);
	if (!thread.ok())
	{
		return thread.error();
	}
	auto result = declare(world, klass, "<init>", "()V", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "currentThread", "()Ljava/lang/Thread;", true,
		[thread](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return thread.value();
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "getName", "()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newString("main");
		});
	if (!result.ok())
	{
		return result;
	}
	// The host thread has no managed frames, so the honest stack trace is the
	// empty array (the class is declared so the element type is real). Targets
	// use this for hook detection and accept an empty trace.
	return declare(world, klass, "getStackTrace", "()[Ljava/lang/StackTraceElement;",
		false, [element_class](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newArray(ArrayElementKind::Object, 0, element_class);
		});
}

// Boolean and Long are value holders like Integer: the native constructs them,
// stores a primitive through <init>, and reads it back through the getter or a
// host runner reads the field via booleanValue()/longValue() below.
Result<void> installBoolean(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Boolean", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();
	auto value = world.declareField(klass, "value", "Z", false);
	if (!value.ok())
	{
		return value.error();
	}
	const uint64_t value_field = value.value();

	auto result = declare(world, klass, "<init>", "(Z)V", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Boolean.<init> expects one boolean" };
			}
			auto written = java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "valueOf", "(Z)Ljava/lang/Boolean;", true,
		[klass, value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Boolean.valueOf expects one boolean" };
			}
			auto object = java.allocateObject(klass);
			if (!object.ok())
			{
				return object.error();
			}
			auto written = java.writeInstanceField(object.value(), value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "booleanValue", "()Z", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, value_field);
		});
}

Result<void> installLong(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Long", "java/lang/Number");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();
	auto value = world.declareField(klass, "value", "J", false);
	if (!value.ok())
	{
		return value.error();
	}
	const uint64_t value_field = value.value();

	auto result = declare(world, klass, "<init>", "(J)V", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Long.<init> expects one long" };
			}
			auto written = java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "valueOf", "(J)Ljava/lang/Long;", true,
		[klass, value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Long.valueOf expects one long" };
			}
			auto object = java.allocateObject(klass);
			if (!object.ok())
			{
				return object.error();
			}
			auto written = java.writeInstanceField(object.value(), value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "longValue", "()J", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, value_field);
		});
}

// java.lang.Number exists so Integer and BigInteger can name their real
// superclass instead of inventing one. No methods: nothing calls them.
Result<void> installNumber(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Number", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	return {};
}

// java.lang.Integer carries an int in the field the real class calls `value`.
// Targets that answer status codes with an Integer only need the constructor;
// host runners read it back through integerValue() below.
Result<void> installInteger(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Integer", "java/lang/Number");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto value = world.declareField(klass, "value", "I", false);
	if (!value.ok())
	{
		return value.error();
	}
	const uint64_t value_field = value.value();

	auto result = declare(world, klass, "<init>", "(I)V", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument,
					"Integer.<init> expects one int" };
			}
			auto written =
				java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "valueOf", "(I)Ljava/lang/Integer;", true,
		[klass, value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument,
					"Integer.valueOf expects one int" };
			}
			auto object = java.allocateObject(klass);
			if (!object.ok())
			{
				return object.error();
			}
			auto written =
				java.writeInstanceField(object.value(), value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "intValue", "()I", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, value_field);
		});
}

} // namespace

Result<int32_t> integerValue(JavaWorld &world, uint64_t object)
{
	const uint64_t klass = world.findClass("java/lang/Integer");
	const uint64_t field = klass == 0 ? 0 : world.findField(klass, "value", "I", false);
	if (field == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/Integer is not installed" };
	}
	if (!world.isInstanceOf(object, klass))
	{
		return Error{ ErrorCode::InvalidArgument, "the object is not a java.lang.Integer" };
	}
	auto value = world.readInstanceField(object, field);
	if (!value.ok())
	{
		return value.error();
	}
	return static_cast<int32_t>(value.value());
}

Result<bool> booleanValue(JavaWorld &world, uint64_t object)
{
	const uint64_t klass = world.findClass("java/lang/Boolean");
	const uint64_t field = klass == 0 ? 0 : world.findField(klass, "value", "Z", false);
	if (field == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/Boolean is not installed" };
	}
	if (!world.isInstanceOf(object, klass))
	{
		return Error{ ErrorCode::InvalidArgument, "the object is not a java.lang.Boolean" };
	}
	auto value = world.readInstanceField(object, field);
	if (!value.ok())
	{
		return value.error();
	}
	return value.value() != 0;
}

Result<uint64_t> longValue(JavaWorld &world, uint64_t object)
{
	const uint64_t klass = world.findClass("java/lang/Long");
	const uint64_t field = klass == 0 ? 0 : world.findField(klass, "value", "J", false);
	if (field == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/Long is not installed" };
	}
	if (!world.isInstanceOf(object, klass))
	{
		return Error{ ErrorCode::InvalidArgument, "the object is not a java.lang.Long" };
	}
	return world.readInstanceField(object, field);
}

Result<void> installJavaLang(JavaWorld &world)
{
	const auto installers = {
		installObject,
		installClass,
		installString,
		installSystem,
		installThread,
		installNumber,
		installInteger,
		installBoolean,
		installLong,
	};
	for (const auto installer : installers)
	{
		auto installed = installer(world);
		if (!installed.ok())
		{
			return installed.error();
		}
	}
	return {};
}

} // namespace duckemu::runtime::jni::envs
