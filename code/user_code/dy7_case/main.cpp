// SPDX-License-Identifier: GPL-2.0-only
// dy7_case: 抖音 40.8.0 libmetasec_ml.so seven-header signer.
//
//   ./duckemu_dy7_case
//   DY_REPEAT=2 ./duckemu_dy7_case
//   ./duckemu_dy7_case --wall-time-ns 1792886400000000000 --print-headers
//   ./duckemu_dy7_case --trace DIR
//
// The Java side (MS/MS0/k3 and Context) is dy7_env.cpp; main installs it
// before loading anything. The target .so and the device property picture
// live in assets/.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "dy7_case/dy7_case.hpp"
#include "dy7_case/dy7_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"

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
using namespace duckemu::runtime;
using namespace duckemu::user;

namespace
{

// metasec's setup and the v5 signature path are slower than the runtime's
// 5 s default; the reference runner also allowed 120 s per call.
constexpr uint64_t kCallTimeoutUs = 120ULL * 1000 * 1000;
// DuckDroid parity: the reference profile pins the monotonic clock at 9 s.
constexpr uint64_t kDefaultMonotonicNs = 9000000000ULL;

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

} // namespace

int main(int argc, char **argv)
{
	std::string trace_path;
	std::optional<uint64_t> wall_time_ns;
	std::optional<uint64_t> monotonic_time_ns;
	std::optional<uint64_t> random_seed;
	std::optional<uint64_t> repeat_count;
	bool print_headers = std::getenv("DY_PRINT_HEADERS") != nullptr;

	if (const char *repeat = std::getenv("DY_REPEAT"))
	{
		repeat_count = std::strtoull(repeat, nullptr, 10);
	}
	// DuckDroid parity: DY_WALL_TIME_MS may inject a replay base instead of
	// the explicit flag.
	if (const char *wall = std::getenv("DY_WALL_TIME_MS"))
	{
		wall_time_ns = std::strtoull(wall, nullptr, 10) * 1000000ULL;
	}

	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		const auto needValue = [&](const char *flag) -> const char * {
			if (index + 1 >= argc)
			{
				std::fprintf(stderr, "%s needs a value\n", flag);
				return nullptr;
			}
			return argv[++index];
		};
		if (argument == "--trace")
		{
			const char *value = needValue("--trace");
			if (value == nullptr)
			{
				return 2;
			}
			trace_path = value;
			continue;
		}
		if (argument == "--repeat")
		{
			const char *value = needValue("--repeat");
			if (value == nullptr)
			{
				return 2;
			}
			repeat_count = std::strtoull(value, nullptr, 10);
			continue;
		}
		if (argument == "--wall-time-ns" || argument == "--monotonic-time-ns" ||
			argument == "--random-seed")
		{
			const char *value = needValue(argument.c_str());
			if (value == nullptr)
			{
				return 2;
			}
			const uint64_t parsed = std::strtoull(value, nullptr, 10);
			if (argument == "--wall-time-ns")
			{
				wall_time_ns = parsed;
			}
			else if (argument == "--monotonic-time-ns")
			{
				monotonic_time_ns = parsed;
			}
			else
			{
				random_seed = parsed;
			}
			continue;
		}
		if (argument == "--print-headers")
		{
			print_headers = true;
			continue;
		}
		std::fprintf(stderr,
			"usage: %s [--trace DIR] [--repeat N] [--print-headers]\n"
			"\t[--wall-time-ns N] [--monotonic-time-ns N] [--random-seed N]\n",
			argv[0]);
		return 2;
	}

	if (!trace_path.empty())
	{
		std::error_code directory_error;
		std::filesystem::create_directories(trace_path, directory_error);
		if (directory_error)
		{
			std::fprintf(stderr, "cannot create trace directory %s: %s\n",
				trace_path.c_str(), directory_error.message().c_str());
			return 1;
		}
	}

	RuntimeConfig config;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	// The device picture the pack was validated against (Mi 8, SDK 32).
	// DY_PROPS_FILE replays a full `adb shell getprop` dump instead.
	const char *props = std::getenv("DY_PROPS_FILE");
	config.property_file = props != nullptr ? props : DUCKEMU_DY7_PROPERTIES;
	config.trace_path = trace_path.empty() ? std::string{} : trace_path + "/svc.log";
	config.default_timeout_us = kCallTimeoutUs;
	config.random_seed = random_seed;

	auto created = RuntimeInstance::create(config);
	if (!created.ok())
	{
		std::fprintf(stderr, "RuntimeInstance::create failed: %s\n",
			created.error().message.c_str());
		return 1;
	}
	RuntimeInstance &instance = *created.value();

	if (wall_time_ns.has_value())
	{
		instance.setWallTimeNs(wall_time_ns.value());
	}
	instance.setMonotonicTimeNs(
		monotonic_time_ns.has_value() ? monotonic_time_ns.value() : kDefaultMonotonicNs);

	// The project's Java side, before any module load.
	auto installed = instance.installJavaEnvironment(&dy7::installEnvironment);
	if (!installed.ok())
	{
		std::fprintf(stderr, "install Java environment failed: %s\n",
			installed.error().message.c_str());
		return 1;
	}

	// Strict Java world: anything libmetasec asks for and the packs did not
	// declare shows up here instead of silently returning a default.
	instance.addJniListener([](const observer::JniEvent &event) {
		std::fprintf(stderr, "jni %s class=%s name=%s sig=%s slot=%u tid=%u\n",
			std::string(observer::jniEventKindName(event.kind)).c_str(),
			event.class_descriptor.c_str(), event.name.c_str(),
			event.signature.c_str(), event.slot, event.tid);
	});

	auto libc = instance.loadLibrary(romLibc());
	if (!libc.ok())
	{
		std::fprintf(stderr, "load libc failed: %s\n", libc.error().message.c_str());
		return 1;
	}
	auto module = instance.loadLibrary(DUCKEMU_DY7_SO);
	if (!module.ok())
	{
		std::fprintf(stderr, "load libmetasec_ml failed: %s\n",
			module.error().message.c_str());
		return 1;
	}

	dy7::Case app;
	auto initialized = app.init(instance, module.value());
	if (!initialized.ok())
	{
		std::fprintf(stderr, "metasec init failed: %s\n",
			initialized.error().message.c_str());
		return 1;
	}

	if (!trace_path.empty())
	{
		// Trace only the signing call, not the one-time metasec setup. The
		// range is the loaded target module, computed from its handle in code.
		auto range = instance.moduleRange(module.value());
		if (!range.ok())
		{
			std::fprintf(stderr, "module range for trace failed: %s\n",
				range.error().message.c_str());
			return 1;
		}
		observer::InsnTraceOptions options;
		options.directory = trace_path + "/insn";
		options.begin = range.value().base;
		options.size = range.value().span;
		options.stop_at_return = true;
		auto started = instance.startInsnTrace(options);
		if (!started.ok())
		{
			std::fprintf(stderr, "start instruction trace failed: %s\n",
				started.error().message.c_str());
			return 1;
		}
	}

	dy7::SignParams params = dy7::signParamsFromEnvironment();
	// The signer re-reads the wall clock per call, so a CLI-injected base has
	// to reach the params; DY_WALL_TIME_MS (read above) still wins.
	if (params.wall_time_ms == 0 && wall_time_ns.has_value())
	{
		params.wall_time_ms = wall_time_ns.value() / 1000000ULL;
	}
	const uint64_t repeat = repeat_count.has_value() ? repeat_count.value() : 1;
	for (uint64_t iteration = 0; iteration < repeat; ++iteration)
	{
		auto output = app.sign(instance, params);
		if (!output.ok())
		{
			std::fprintf(stderr, "sign failed: %s\n", output.error().message.c_str());
			instance.stopInsnTrace();
			return 1;
		}
		std::printf("%s\n", output.value().sign.c_str());
		if (print_headers)
		{
			for (const auto &header : output.value().headers)
			{
				std::printf("%s: %s\n", header.first.c_str(), header.second.c_str());
			}
		}
	}

	instance.stopInsnTrace();
	return 0;
}
