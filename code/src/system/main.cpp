// SPDX-License-Identifier: GPL-2.0-only
// duckemu_system: the local gateway.
//
//   $ duckemu_system --socket-dir DIR --worker-exec PATH
//         --app id=libeleven,library=PATH,rom-libc=PATH,instances=4,workers=1
//
// Each --app creates <socket-dir>/<id>.sock and one WorkerPool. Clients speak
// the framed JSON protocol; the System only routes frames by listener.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/log/logger.hpp"
#include "system/system.hpp"

namespace
{

void Usage()
{
	std::fprintf(stderr,
		"usage: duckemu_system --socket-dir DIR --worker-exec PATH\n"
		"                      --app id=ID,library=PATH,rom-libc=PATH\n"
		"                            [,instances=N][,workers=N]\n"
		"                            [,library-path=P1:P2]\n"
		"                            [,filesystem-root=P1:P2]\n"
		"                            [,filesystem-write-root=DIR]\n"
		"                            [,property-file=PATH][,random-seed=N]\n"
		"                            [,init-timeout-us=N][,invoke-timeout-us=N]\n"
		"                            [,backoff-ms=N][,max-requests=N]\n"
		"                      [--wall-time-ns N] [--monotonic-time-ns N]\n"
		"                      [--trace DIR]\n");
}

bool TakeValue(int argc, char **argv, int *index, const char *name, std::string *out)
{
	if (*index + 1 >= argc)
	{
		std::fprintf(stderr, "system: %s needs a value\n", name);
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

std::vector<std::string> Split(const std::string &text, char separator)
{
	std::vector<std::string> parts;
	size_t start = 0;
	while (start <= text.size())
	{
		const size_t found = text.find(separator, start);
		if (found == std::string::npos)
		{
			parts.push_back(text.substr(start));
			break;
		}
		parts.push_back(text.substr(start, found - start));
		start = found + 1;
	}
	return parts;
}

bool ParseApp(const std::string &spec, duckemu::system::WorkerSpec *worker, std::string *error)
{
	for (const std::string &field : Split(spec, ','))
	{
		const size_t equals = field.find('=');
		if (equals == std::string::npos)
		{
			*error = "app field '" + field + "' is not key=value";
			return false;
		}
		const std::string key = field.substr(0, equals);
		const std::string value = field.substr(equals + 1);
		if (key == "id")
		{
			worker->app = value;
		}
		else if (key == "library")
		{
			worker->library = value;
		}
		else if (key == "rom-libc")
		{
			worker->rom_libc = value;
		}
		else if (key == "library-path")
		{
			for (const std::string &path : Split(value, ':'))
			{
				if (!path.empty())
				{
					worker->library_search_paths.push_back(path);
				}
			}
		}
		else if (key == "filesystem-root")
		{
			for (const std::string &root : Split(value, ':'))
			{
				if (!root.empty())
				{
					worker->filesystem_roots.push_back(root);
				}
			}
		}
		else if (key == "filesystem-write-root")
		{
			worker->filesystem_write_root = value;
		}
		else if (key == "property-file")
		{
			worker->property_file = value;
		}
		else if (key == "random-seed")
		{
			uint64_t number = 0;
			if (!ParseU64(value, &number))
			{
				*error = "app 'random-seed' must be a number";
				return false;
			}
			worker->has_random_seed = true;
			worker->random_seed = number;
		}
		else if (key == "instances")
		{
			uint64_t number = 0;
			if (!ParseU64(value, &number) || number == 0 || number > 1024)
			{
				*error = "app 'instances' must be 1..1024";
				return false;
			}
			worker->instances = static_cast<uint32_t>(number);
		}
		else if (key == "workers")
		{
			uint64_t number = 0;
			if (!ParseU64(value, &number) || number == 0 || number > 64)
			{
				*error = "app 'workers' must be 1..64";
				return false;
			}
			worker->workers = static_cast<uint32_t>(number);
		}
		else if (key == "init-timeout-us")
		{
			uint64_t number = 0;
			if (!ParseU64(value, &number))
			{
				*error = "app 'init-timeout-us' must be a number";
				return false;
			}
			worker->init_timeout_us = number;
		}
		else if (key == "invoke-timeout-us")
		{
			uint64_t number = 0;
			if (!ParseU64(value, &number))
			{
				*error = "app 'invoke-timeout-us' must be a number";
				return false;
			}
			worker->invoke_timeout_us = number;
		}
		else if (key == "backoff-ms")
		{
			uint64_t number = 0;
			if (!ParseU64(value, &number))
			{
				*error = "app 'backoff-ms' must be a number";
				return false;
			}
			worker->respawn_backoff_ms = number;
		}
		else if (key == "max-requests")
		{
			uint64_t number = 0;
			if (!ParseU64(value, &number) || number > 0xFFFFFFFFULL)
			{
				*error = "app 'max-requests' must be a number";
				return false;
			}
			worker->max_requests_per_instance = static_cast<uint32_t>(number);
		}
		else
		{
			*error = "unknown app field '" + key + "'";
			return false;
		}
	}
	if (worker->app.empty() || worker->library.empty() || worker->rom_libc.empty())
	{
		*error = "app needs id, library and rom-libc";
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	duckemu::log::set_level(duckemu::log::Level::Error);

	duckemu::system::SystemConfig config;
	std::string worker_executable;
	std::string trace;
	bool has_wall_time = false;
	uint64_t wall_time_ns = 0;
	bool has_monotonic_time = false;
	uint64_t monotonic_time_ns = 0;

	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		std::string value;
		if (argument == "--socket-dir")
		{
			if (!TakeValue(argc, argv, &index, "--socket-dir", &config.socket_dir))
			{
				return 2;
			}
		}
		else if (argument == "--worker-exec")
		{
			if (!TakeValue(argc, argv, &index, "--worker-exec", &worker_executable))
			{
				return 2;
			}
		}
		else if (argument == "--app")
		{
			if (!TakeValue(argc, argv, &index, "--app", &value))
			{
				return 2;
			}
			duckemu::system::AppConfig app;
			std::string error;
			if (!ParseApp(value, &app.worker, &error))
			{
				std::fprintf(stderr, "system: %s\n", error.c_str());
				return 2;
			}
			config.apps.push_back(std::move(app));
		}
		else if (argument == "--trace")
		{
			if (!TakeValue(argc, argv, &index, "--trace", &trace))
			{
				return 2;
			}
		}
		else if (argument == "--wall-time-ns")
		{
			if (!TakeValue(argc, argv, &index, "--wall-time-ns", &value) ||
				!ParseU64(value, &wall_time_ns))
			{
				std::fprintf(stderr, "system: --wall-time-ns needs a number\n");
				return 2;
			}
			has_wall_time = true;
		}
		else if (argument == "--monotonic-time-ns")
		{
			if (!TakeValue(argc, argv, &index, "--monotonic-time-ns", &value) ||
				!ParseU64(value, &monotonic_time_ns))
			{
				std::fprintf(stderr, "system: --monotonic-time-ns needs a number\n");
				return 2;
			}
			has_monotonic_time = true;
		}
		else
		{
			std::fprintf(stderr, "system: unknown argument %s\n", argument.c_str());
			return 2;
		}
	}

	if (config.apps.empty() || worker_executable.empty())
	{
		Usage();
		return 2;
	}
	for (auto &app : config.apps)
	{
		app.worker.worker_executable = worker_executable;
		app.worker.trace_path = trace;
		app.worker.has_wall_time = has_wall_time;
		app.worker.wall_time_ns = wall_time_ns;
		app.worker.has_monotonic_time = has_monotonic_time;
		app.worker.monotonic_time_ns = monotonic_time_ns;
	}

	std::string error;
	auto system = duckemu::system::System::create(config, &error);
	if (!system)
	{
		std::fprintf(stderr, "system: create failed: %s\n", error.c_str());
		return 1;
	}
	if (!system->run(&error))
	{
		std::fprintf(stderr, "system: run failed: %s\n", error.c_str());
		return 1;
	}
	return 0;
}
