// SPDX-License-Identifier: GPL-2.0-only
#include "dy7_case/dy7_env.hpp"

#include <cstdint>
#include <utility>

#include "common/id/ids.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_binder.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::user::dy7
{
namespace
{

using runtime::jni::ArrayElementKind;
using runtime::jni::InvokeContext;
using runtime::jni::JavaBinder;
using runtime::jni::JavaWorld;
using runtime::jni::ShimHandler;

// The native asks the app for Android values and calls the Java-side k3.b
// wrapper; every other Java method it uses is a platform pack.
JavaWorld *g_world = nullptr;
uint64_t g_context_class = 0;

ShimHandler returnNull()
{
	return [](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; };
}

ShimHandler returnString(std::string value)
{
	return [value = std::move(value)](JavaWorld &world, InvokeContext &) -> Result<uint64_t> {
		return world.newString(value);
	};
}

Result<void> checkWorld()
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "dy7: installEnvironment has not run" };
	}
	return {};
}

} // namespace

const char *const kEntryClass = "com/bytedance/mobsec/metasec/ml/MS";
const char *const kCoreClass = "com/bytedance/mobsec/metasec/core/MS0";
const char *const kNativeClass = "ms/bd/c/k3";
const char *const kNativeSignature = "(IIJLjava/lang/String;Ljava/lang/Object;)Ljava/lang/Object;";
const char *const kContextClass = "android/content/Context";
const char *const kPackage = "com.ss.android.ugc.aweme";

Result<void> installEnvironment(JavaWorld &world)
{
	JavaBinder binder(world);

	// libmetasec loads MS, walks GetSuperclass twice (MS0, k3) and calls
	// RegisterNatives on k3. The hierarchy is declared leaf-last because
	// declareClass requires the superclass to exist.
	uint64_t entry_class = 0;
	uint64_t core_class = 0;
	uint64_t native_class = 0;
	uint64_t context_class = 0;
	binder.declareClass(kNativeClass, &native_class)
		.declareClass(kCoreClass, &core_class, kNativeClass)
		.declareClass(kEntryClass, &entry_class, kCoreClass)
		.declareClass(kContextClass, &context_class);

	// k3.a is the registered dispatcher the runner calls. k3.c is its sibling
	// in the dex; declared so RegisterNatives binds it like on a device.
	binder.declareMethod(native_class, "a", kNativeSignature, /*is_static=*/true, {})
		.declareMethod(native_class, "c", kNativeSignature, /*is_static=*/true, {})
		// k3.b is a Java static wrapper the native calls during setup. Its real
		// body delegates to the obfuscated d3.LIZ; the signer works when it
		// answers null, which is what the reference case proved.
		.declareMethod(native_class, "b", kNativeSignature, /*is_static=*/true, returnNull());

	// android.content.Context: the app object the native is handed.
	binder.declareMethod(context_class, "getPackageName", "()Ljava/lang/String;", false,
			returnString(kPackage))
		.declareMethod(context_class, "getApplicationContext", "()Landroid/content/Context;",
			false,
			[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
				return call.receiver;
			})
		// A writable files dir does not exist yet; the signer continues when
		// the lookup answers null, same as the reference runner.
		.declareMethod(context_class, "getFilesDir", "()Ljava/io/File;", false, returnNull());

	auto done = std::move(binder).done();
	if (!done.ok())
	{
		return done;
	}
	g_world = &world;
	g_context_class = context_class;
	return {};
}

Result<uint64_t> makeString(std::string_view text)
{
	auto ready = checkWorld();
	if (!ready.ok())
	{
		return ready.error();
	}
	return g_world->newString(text);
}

Result<uint64_t> makeByteArray(const void *data, uint64_t size)
{
	auto ready = checkWorld();
	if (!ready.ok())
	{
		return ready.error();
	}
	auto array = g_world->newArray(ArrayElementKind::Byte, size);
	if (!array.ok())
	{
		return array.error();
	}
	auto view = g_world->arrayView(array.value());
	if (!view.ok())
	{
		return view.error();
	}
	if (size != 0)
	{
		auto written = g_world->memory().write(GuestVA{ view.value().storage }, data, size);
		if (!written.ok())
		{
			return written.error();
		}
	}
	return array.value();
}

Result<uint64_t> newContextObject()
{
	auto ready = checkWorld();
	if (!ready.ok())
	{
		return ready.error();
	}
	return g_world->allocateObject(g_context_class);
}

Result<std::string> readString(uint64_t object)
{
	auto ready = checkWorld();
	if (!ready.ok())
	{
		return ready.error();
	}
	return g_world->stringUtf8(object);
}

Result<bool> readBooleanBox(uint64_t object)
{
	auto ready = checkWorld();
	if (!ready.ok())
	{
		return ready.error();
	}
	return runtime::jni::envs::booleanValue(*g_world, object);
}

Result<uint64_t> readLongBox(uint64_t object)
{
	auto ready = checkWorld();
	if (!ready.ok())
	{
		return ready.error();
	}
	return runtime::jni::envs::longValue(*g_world, object);
}

} // namespace duckemu::user::dy7
