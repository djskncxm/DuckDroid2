// SPDX-License-Identifier: GPL-2.0-only
// Worker seam for the Douyin target: the seven-header signer behind
// worker::make_app_plugin.
//
// The gate asserts the shape the case can actually guarantee:
//   - the strict Java world reports nothing except the one lookup the real APK
//     also misses (MS.a()V: the app has no such method, the native probes for
//     it and continues; the probe is asynchronous, so its appearance varies);
//   - at the injected clock X-Khronos is the timestamp and X-Argus is the
//     reference runner's byte-identical value;
//   - a second instance with the same clock produces the same stable headers.
//     X-Perseus / X-Gorgon / X-Ladon mix entropy from the target's own worker
//     thread and are not byte-reproducible even with a pinned random seed, so
//     the gate compares the headers the reference documents as stable.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"
#include "support/test_harness.hpp"
#include "worker/app/app_plugin.hpp"

#ifndef DUCKEMU_ROM_BOOTSTRAP
#error "DUCKEMU_ROM_BOOTSTRAP must point at the ROM bootstrap lib directory"
#endif
#ifndef DUCKEMU_ROM_LIB64
#error "DUCKEMU_ROM_LIB64 must point at the ROM system/lib64 directory"
#endif
#ifndef DUCKEMU_DY7_SO
#error "DUCKEMU_DY7_SO must point at libmetasec_ml.so"
#endif
#ifndef DUCKEMU_DY7_PROPERTIES
#error "DUCKEMU_DY7_PROPERTIES must point at the device property picture"
#endif

using namespace duckemu;

namespace
{

constexpr uint64_t kWallTimeNs = 1792886400000000000ULL;
constexpr uint64_t kMonotonicTimeNs = 9000000000ULL;
constexpr uint64_t kRandomSeed = 7;
constexpr const char *kWallTimeMs = "1792886400000";
constexpr const char *kExpectedKhronos = "1792886400";
// X-Argus only carries the timestamp and the device picture, so it is stable
// across machines; the other headers mix in random and environmental factors.
constexpr const char *kExpectedArgus = "gEbdag==";

struct JniEventShape
{
	runtime::observer::JniEventKind kind = runtime::observer::JniEventKind::MissingClass;
	std::string class_descriptor;
	std::string name;
	std::string signature;
};

struct Outcome
{
	std::string sign;
	std::string argus;
	std::string khronos;
	std::vector<JniEventShape> events;
};

// The APK has no MS.a()V; the native probes for it from its own worker thread,
// so whether the lookup lands before the sign call returns is a race. It is
// the only lookup allowed to fail: any other event means the packs are short.
bool onlyExpectedProbe(const Outcome &outcome)
{
	for (const JniEventShape &event : outcome.events)
	{
		if (event.kind != runtime::observer::JniEventKind::MissingMethod ||
			event.class_descriptor != "com/bytedance/mobsec/metasec/ml/MS" ||
			event.name != "a" || event.signature != "()V")
		{
			return false;
		}
	}
	return true;
}

bool runOnce(const worker::AppContext &context, Outcome *outcome, std::string *error)
{
	// Same configuration the worker's instance host builds: no timeout or
	// stack override, only the app context fields.
	runtime::RuntimeConfig config;
	config.library_search_paths = context.library_search_paths;
	config.filesystem_roots = context.filesystem_roots;
	config.property_file = context.property_file;
	config.random_seed = context.random_seed;

	auto created = runtime::RuntimeInstance::create(config);
	if (!created.ok())
	{
		*error = "RuntimeInstance::create failed: " + created.error().message;
		return false;
	}
	runtime::RuntimeInstance &instance = *created.value();
	instance.setWallTimeNs(kWallTimeNs);
	instance.setMonotonicTimeNs(kMonotonicTimeNs);

	instance.addJniListener([outcome](const runtime::observer::JniEvent &event) {
		outcome->events.push_back(
			JniEventShape{ event.kind, event.class_descriptor, event.name, event.signature });
	});

	auto plugin = worker::make_app_plugin(context);
	if (!plugin.ok())
	{
		*error = "make_app_plugin failed: " + plugin.error().message;
		return false;
	}
	if (!plugin.value()->init(instance, error))
	{
		return false;
	}

	// The signer reads the wall clock per call; the worker passes it through
	// the request when a replay base was configured.
	worker::AppRequest request;
	request.op = "sign";
	request.params["wall_time_ms"] = kWallTimeMs;
	worker::AppResult result;
	worker::AppError app_error;
	if (!plugin.value()->invoke(instance, request, &result, &app_error))
	{
		*error = "invoke failed: " + app_error.message;
		return false;
	}

	const auto sign = result.values.find("sign");
	const auto argus = result.values.find("X-Argus");
	const auto khronos = result.values.find("X-Khronos");
	if (sign == result.values.end() || argus == result.values.end() ||
		khronos == result.values.end())
	{
		*error = "result is missing sign/X-Argus/X-Khronos";
		return false;
	}
	outcome->sign = sign->second;
	outcome->argus = argus->second;
	outcome->khronos = khronos->second;
	return true;
}

} // namespace

int main()
{
	worker::AppContext context;
	context.app = "dy7";
	context.library = DUCKEMU_DY7_SO;
	context.rom_libc = std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
	context.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	context.property_file = DUCKEMU_DY7_PROPERTIES;
	context.random_seed = kRandomSeed;

	std::string error;
	Outcome first;
	if (!runOnce(context, &first, &error))
	{
		std::fprintf(stderr, "  %s\n", error.c_str());
		return test::finish("07g_dy7_plugin");
	}
	CHECK(!first.sign.empty());
	CHECK(first.khronos == kExpectedKhronos);
	CHECK(first.argus == kExpectedArgus);

	// The only strict-world lookup that fails is the optional MS.a()V probe;
	// the APK really has no such method. Anything else is a pack gap.
	CHECK(onlyExpectedProbe(first));
	for (const JniEventShape &event : first.events)
	{
		std::fprintf(stderr, "  event kind=%d class=%s name=%s sig=%s\n",
			static_cast<int>(event.kind), event.class_descriptor.c_str(),
			event.name.c_str(), event.signature.c_str());
	}

	// Same clock in a fresh instance: the stable headers must be identical.
	Outcome second;
	if (!runOnce(context, &second, &error))
	{
		std::fprintf(stderr, "  %s\n", error.c_str());
		return test::finish("07g_dy7_plugin");
	}
	CHECK(second.argus == first.argus);
	CHECK(second.khronos == first.khronos);
	CHECK(!second.sign.empty());
	CHECK(onlyExpectedProbe(second));

	return test::finish("07g_dy7_plugin");
}
