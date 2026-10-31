// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

#include "dy7_case/dy7_case.hpp"
#include "worker/app/app_plugin.hpp"

namespace duckemu::user::dy7
{

// libmetasec_ml behind the worker's target contract: install the Java world,
// load the ROM libc and libmetasec_ml.so, run the metasec setup, then map
// request params onto the seven-header signer.
class Plugin final : public worker::AppPlugin
{
    public:
	explicit Plugin(worker::AppContext context);

	bool init(runtime::RuntimeInstance &instance, std::string *error) override;
	bool invoke(runtime::RuntimeInstance &instance,
		const worker::AppRequest &request,
		worker::AppResult *result,
		worker::AppError *error) override;

    private:
	worker::AppContext context_;
	Case case_;
};

} // namespace duckemu::user::dy7
