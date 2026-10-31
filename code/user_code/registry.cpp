// SPDX-License-Identifier: GPL-2.0-only
// Target-side plugin registry. The worker only knows app ids; everything that
// names a concrete Android app lives here (and in its project directory).

#include <memory>

#include "dy7_case/plugin.hpp"
#include "jdgs_case/plugin.hpp"
#include "libeleven_case/plugin.hpp"
#include "szstone_case/plugin.hpp"
#include "worker/app/app_plugin.hpp"

namespace duckemu::worker
{

Result<std::unique_ptr<AppPlugin> > make_app_plugin(const AppContext &context)
{
	if (context.app == "libeleven")
	{
		std::unique_ptr<AppPlugin> plugin =
			std::make_unique<user::libeleven::Plugin>(context);
		return Result<std::unique_ptr<AppPlugin> >(std::move(plugin));
	}
	if (context.app == "jdgs16")
	{
		std::unique_ptr<AppPlugin> plugin =
			std::make_unique<user::jdgs::Plugin>(context);
		return Result<std::unique_ptr<AppPlugin> >(std::move(plugin));
	}
	if (context.app == "szstone")
	{
		std::unique_ptr<AppPlugin> plugin =
			std::make_unique<user::szstone::Plugin>(context);
		return Result<std::unique_ptr<AppPlugin> >(std::move(plugin));
	}
	if (context.app == "dy7")
	{
		std::unique_ptr<AppPlugin> plugin =
			std::make_unique<user::dy7::Plugin>(context);
		return Result<std::unique_ptr<AppPlugin> >(std::move(plugin));
	}
	return Error{ ErrorCode::NotFound, "unknown app '" + context.app + "'" };
}

} // namespace duckemu::worker
