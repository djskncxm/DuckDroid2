// SPDX-License-Identifier: GPL-2.0-only
// duckemu_worker has two modes:
//
//   --instance      the RuntimeInstance host. Spawned by the supervisor; it
//                   creates the only engine in its process.
//   --supervisor    the pool/scheduler. It owns N instance processes and a
//                   frame channel to the System (fd inherited from the parent).
//
// The same binary serves both so a target plugin is linked exactly once and
// its world is identical no matter which process loads it.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include "common/log/logger.hpp"
#include "worker/instance_host.hpp"
#include "worker/worker_process.hpp"

namespace
{

void Usage()
{
	std::fprintf(stderr,
		"usage: duckemu_worker --instance --ipc-fd FD --instance-id ID\n"
		"                      --app ID --library PATH --rom-libc PATH\n"
		"                      [--library-path DIR]... [--filesystem-root DIR]...\n"
		"                      [--filesystem-write-root DIR] [--property-file PATH]\n"
		"                      [--trace DIR] [--wall-time-ns N] [--monotonic-time-ns N]\n"
		"   or: duckemu_worker --supervisor --ipc-fd FD\n"
		"                      --app ID --library PATH --rom-libc PATH\n"
		"                      [--instances N] [--instance-exec PATH]\n"
		"                      [--library-path DIR]... [--filesystem-root DIR]...\n"
		"                      [--filesystem-write-root DIR] [--property-file PATH]\n"
		"                      [--trace DIR] [--worker-id ID] [--init-timeout-us N]\n"
		"                      [--invoke-timeout-us N] [--backoff-ms N]\n"
		"                      [--max-requests-per-instance N]\n"
		"                      [--wall-time-ns N] [--monotonic-time-ns N]\n");
}

bool TakeValue(int argc, char **argv, int *index, const char *name, std::string *out)
{
	if (*index + 1 >= argc)
	{
		std::fprintf(stderr, "worker: %s needs a value\n", name);
		return false;
	}
	*out = argv[++(*index)];
	return true;
}

bool ParseU64(const std::string &text, uint64_t *out)
{
	errno = 0;
	char *end = nullptr;
	const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0')
	{
		return false;
	}
	*out = static_cast<uint64_t>(value);
	return true;
}

struct Arguments
{
	bool instance_mode = false;
	bool supervisor_mode = false;
	bool has_ipc_fd = false;
	int ipc_fd = -1;

	std::string instance_id;
	std::string worker_id;
	std::string app;
	std::string library;
	std::string rom_libc;
	std::vector<std::string> library_paths;
	std::vector<std::string> filesystem_roots;
	std::string filesystem_write_root;
	std::string property_file;
	bool has_random_seed = false;
	uint64_t random_seed = 0;
	std::string trace;
	std::string instance_exec;
	uint32_t instances = 1;
	uint64_t init_timeout_us = 120ull * 1000 * 1000;
	uint64_t invoke_timeout_us = 5ull * 1000 * 1000;
	uint64_t backoff_ms = 100;
	uint32_t max_requests_per_instance = 0;
	bool has_wall_time = false;
	uint64_t wall_time_ns = 0;
	bool has_monotonic_time = false;
	uint64_t monotonic_time_ns = 0;
};

bool ParseArguments(int argc, char **argv, Arguments *arguments)
{
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		std::string value;
		uint64_t number = 0;

		if (argument == "--instance")
		{
			arguments->instance_mode = true;
		}
		else if (argument == "--supervisor")
		{
			arguments->supervisor_mode = true;
		}
		else if (argument == "--ipc-fd")
		{
			if (!TakeValue(argc, argv, &index, "--ipc-fd", &value) || !ParseU64(value, &number) ||
				number > 1u << 20)
			{
				std::fprintf(stderr, "worker: --ipc-fd needs a file descriptor number\n");
				return false;
			}
			arguments->ipc_fd = static_cast<int>(number);
			arguments->has_ipc_fd = true;
		}
		else if (argument == "--instance-id")
		{
			if (!TakeValue(argc, argv, &index, "--instance-id", &arguments->instance_id))
			{
				return false;
			}
		}
		else if (argument == "--worker-id")
		{
			if (!TakeValue(argc, argv, &index, "--worker-id", &arguments->worker_id))
			{
				return false;
			}
		}
		else if (argument == "--app")
		{
			if (!TakeValue(argc, argv, &index, "--app", &arguments->app))
			{
				return false;
			}
		}
		else if (argument == "--library")
		{
			if (!TakeValue(argc, argv, &index, "--library", &arguments->library))
			{
				return false;
			}
		}
		else if (argument == "--rom-libc")
		{
			if (!TakeValue(argc, argv, &index, "--rom-libc", &arguments->rom_libc))
			{
				return false;
			}
		}
		else if (argument == "--instance-exec")
		{
			if (!TakeValue(argc, argv, &index, "--instance-exec", &arguments->instance_exec))
			{
				return false;
			}
		}
		else if (argument == "--library-path")
		{
			if (!TakeValue(argc, argv, &index, "--library-path", &value))
			{
				return false;
			}
			arguments->library_paths.push_back(value);
		}
		else if (argument == "--filesystem-root")
		{
			if (!TakeValue(argc, argv, &index, "--filesystem-root", &value))
			{
				return false;
			}
			arguments->filesystem_roots.push_back(value);
		}
		else if (argument == "--filesystem-write-root")
		{
			if (!TakeValue(argc, argv, &index, "--filesystem-write-root", &value))
			{
				return false;
			}
			arguments->filesystem_write_root = value;
		}
		else if (argument == "--property-file")
		{
			if (!TakeValue(argc, argv, &index, "--property-file", &arguments->property_file))
			{
				return false;
			}
		}
		else if (argument == "--random-seed")
		{
			if (!TakeValue(argc, argv, &index, "--random-seed", &value) ||
				!ParseU64(value, &arguments->random_seed))
			{
				std::fprintf(stderr, "worker: --random-seed needs a number\n");
				return false;
			}
			arguments->has_random_seed = true;
		}
		else if (argument == "--trace")
		{
			if (!TakeValue(argc, argv, &index, "--trace", &arguments->trace))
			{
				return false;
			}
		}
		else if (argument == "--instances")
		{
			if (!TakeValue(argc, argv, &index, "--instances", &value) || !ParseU64(value, &number) ||
				number == 0 || number > 1024)
			{
				std::fprintf(stderr, "worker: --instances needs a number in 1..1024\n");
				return false;
			}
			arguments->instances = static_cast<uint32_t>(number);
		}
		else if (argument == "--init-timeout-us")
		{
			if (!TakeValue(argc, argv, &index, "--init-timeout-us", &value) ||
				!ParseU64(value, &arguments->init_timeout_us))
			{
				std::fprintf(stderr, "worker: --init-timeout-us needs a number\n");
				return false;
			}
		}
		else if (argument == "--invoke-timeout-us")
		{
			if (!TakeValue(argc, argv, &index, "--invoke-timeout-us", &value) ||
				!ParseU64(value, &arguments->invoke_timeout_us))
			{
				std::fprintf(stderr, "worker: --invoke-timeout-us needs a number\n");
				return false;
			}
		}
		else if (argument == "--backoff-ms")
		{
			if (!TakeValue(argc, argv, &index, "--backoff-ms", &value) ||
				!ParseU64(value, &arguments->backoff_ms))
			{
				std::fprintf(stderr, "worker: --backoff-ms needs a number\n");
				return false;
			}
		}
		else if (argument == "--max-requests-per-instance")
		{
			if (!TakeValue(argc, argv, &index, "--max-requests-per-instance", &value) ||
				!ParseU64(value, &number) || number > 0xFFFFFFFFULL)
			{
				std::fprintf(stderr, "worker: --max-requests-per-instance needs a number\n");
				return false;
			}
			arguments->max_requests_per_instance = static_cast<uint32_t>(number);
		}
		else if (argument == "--wall-time-ns")
		{
			if (!TakeValue(argc, argv, &index, "--wall-time-ns", &value) ||
				!ParseU64(value, &arguments->wall_time_ns))
			{
				std::fprintf(stderr, "worker: --wall-time-ns needs a number\n");
				return false;
			}
			arguments->has_wall_time = true;
		}
		else if (argument == "--monotonic-time-ns")
		{
			if (!TakeValue(argc, argv, &index, "--monotonic-time-ns", &value) ||
				!ParseU64(value, &arguments->monotonic_time_ns))
			{
				std::fprintf(stderr, "worker: --monotonic-time-ns needs a number\n");
				return false;
			}
			arguments->has_monotonic_time = true;
		}
		else
		{
			std::fprintf(stderr, "worker: unknown argument %s\n", argument.c_str());
			return false;
		}
	}
	return true;
}

bool Validate(const Arguments &arguments)
{
	if (arguments.instance_mode == arguments.supervisor_mode)
	{
		std::fprintf(stderr, "worker: choose exactly one of --instance / --supervisor\n");
		return false;
	}
	if (!arguments.has_ipc_fd || arguments.app.empty() || arguments.library.empty() ||
		arguments.rom_libc.empty())
	{
		Usage();
		return false;
	}
	if (arguments.instance_mode && arguments.instance_id.empty())
	{
		std::fprintf(stderr, "worker: --instance needs --instance-id\n");
		return false;
	}
	return true;
}

int RunInstance(const Arguments &arguments)
{
	duckemu::worker::InstanceHostConfig config;
	config.ipc_fd = arguments.ipc_fd;
	config.instance_id = arguments.instance_id;
	config.app.app = arguments.app;
	config.app.library = arguments.library;
	config.app.rom_libc = arguments.rom_libc;
	config.app.library_search_paths = arguments.library_paths;
	config.app.filesystem_roots = arguments.filesystem_roots;
	config.app.filesystem_write_root = arguments.filesystem_write_root;
	config.app.property_file = arguments.property_file;
	if (arguments.has_random_seed)
	{
		config.app.random_seed = arguments.random_seed;
	}
	config.trace_path = arguments.trace;
	config.has_wall_time = arguments.has_wall_time;
	config.wall_time_ns = arguments.wall_time_ns;
	config.has_monotonic_time = arguments.has_monotonic_time;
	config.monotonic_time_ns = arguments.monotonic_time_ns;
	return duckemu::worker::run_instance_host(config);
}

int RunSupervisor(const Arguments &arguments)
{
	duckemu::worker::WorkerConfig config;
	config.worker_id = arguments.worker_id;
	if (config.worker_id.empty())
	{
		config.worker_id = "w" + std::to_string(::getpid());
	}
	config.app.app = arguments.app;
	config.app.library = arguments.library;
	config.app.rom_libc = arguments.rom_libc;
	config.app.library_search_paths = arguments.library_paths;
	config.app.filesystem_roots = arguments.filesystem_roots;
	config.app.filesystem_write_root = arguments.filesystem_write_root;
	config.app.property_file = arguments.property_file;
	if (arguments.has_random_seed)
	{
		config.app.random_seed = arguments.random_seed;
	}
	config.instance_executable = arguments.instance_exec;
	config.instances = arguments.instances;
	config.init_timeout_us = arguments.init_timeout_us;
	config.invoke_timeout_us = arguments.invoke_timeout_us;
	config.respawn_backoff_ms = arguments.backoff_ms;
	config.max_requests_per_instance = arguments.max_requests_per_instance;
	config.trace_path = arguments.trace;
	config.has_wall_time = arguments.has_wall_time;
	config.wall_time_ns = arguments.wall_time_ns;
	config.has_monotonic_time = arguments.has_monotonic_time;
	config.monotonic_time_ns = arguments.monotonic_time_ns;

	std::string error;
	auto worker = duckemu::worker::WorkerProcess::create(config, &error);
	if (!worker)
	{
		std::fprintf(stderr, "worker: create failed: %s\n", error.c_str());
		return 1;
	}
	if (!worker->run(arguments.ipc_fd, &error))
	{
		std::fprintf(stderr, "worker: run failed: %s\n", error.c_str());
		return 1;
	}
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	duckemu::log::set_level(duckemu::log::Level::Error);

	Arguments arguments;
	if (!ParseArguments(argc, argv, &arguments) || !Validate(arguments))
	{
		return 2;
	}
	if (arguments.instance_mode)
	{
		return RunInstance(arguments);
	}
	return RunSupervisor(arguments);
}
