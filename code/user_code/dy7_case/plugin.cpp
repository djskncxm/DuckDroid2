// SPDX-License-Identifier: GPL-2.0-only
#include "dy7_case/plugin.hpp"

#include <cstdlib>
#include <string>
#include <utility>

#include "dy7_case/dy7_env.hpp"
#include "runtime/instance/runtime_instance.hpp"

namespace duckemu::user::dy7
{

Plugin::Plugin(worker::AppContext context) : context_(std::move(context)) {}

bool Plugin::init(runtime::RuntimeInstance &instance, std::string *error)
{
	auto installed = instance.installJavaEnvironment(&installEnvironment);
	if (!installed.ok())
	{
		*error = "install Java environment failed: " + installed.error().message;
		return false;
	}
	auto libc = instance.loadLibrary(context_.rom_libc);
	if (!libc.ok())
	{
		*error = "load libc failed: " + libc.error().message;
		return false;
	}
	auto module = instance.loadLibrary(context_.library);
	if (!module.ok())
	{
		*error = "load library failed: " + module.error().message;
		return false;
	}
	auto initialized = case_.init(instance, module.value());
	if (!initialized.ok())
	{
		*error = "metasec init failed: " + initialized.error().message;
		return false;
	}
	return true;
}

bool Plugin::invoke(runtime::RuntimeInstance &instance,
	const worker::AppRequest &request,
	worker::AppResult *result,
	worker::AppError *error)
{
	if (request.op != "sign")
	{
		error->code = "UNKNOWN_OP";
		error->message = "unsupported op '" + request.op + "'";
		return false;
	}

	SignParams params = signParamsFromEnvironment();
	const auto override_param = [&request](const char *name, std::string *value) {
		const auto found = request.params.find(name);
		if (found != request.params.end())
		{
			*value = found->second;
		}
	};
	override_param("url", &params.url);
	const auto headers = request.params.find("headers");
	if (headers != request.params.end())
	{
		params.headers = headers->second;
		params.headers_given = true;
	}
	else
	{
		override_param("cookie", &params.cookie);
	}
	const auto wall_time = request.params.find("wall_time_ms");
	if (wall_time != request.params.end())
	{
		params.wall_time_ms = std::strtoull(wall_time->second.c_str(), nullptr, 10);
	}

	auto output = case_.sign(instance, params);
	if (!output.ok())
	{
		error->code = "APP_ERROR";
		error->message = output.error().message;
		return false;
	}
	result->values["sign"] = output.value().sign;
	for (const auto &header : output.value().headers)
	{
		result->values[header.first] = header.second;
	}
	return true;
}

} // namespace duckemu::user::dy7
