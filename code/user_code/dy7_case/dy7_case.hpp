// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "common/error/result.hpp"

namespace duckemu::runtime
{
class RuntimeInstance;
}

namespace duckemu::user::dy7
{

// libmetasec_ml.so-relative address of god7, the seven-header signer entry.
inline constexpr uint64_t kSignOffset = 0x2e01c0;

// Parameters of one seven-header signer call.
//
// libmetasec_ml's internal entry is char *(char *url, char *headers), where
// |headers| is the whole device-log block ("key\r\nvalue\r\n..."). When the
// caller has no block, |cookie| is wrapped into "cookie\r\n<value>\r\n".
struct SignParams
{
	std::string url;
	std::string headers;
	bool headers_given = false;
	std::string cookie;
	uint64_t wall_time_ms = 0; // 0 = host wall clock at call time
};

struct SignOutput
{
	std::string sign; // the raw CRLF block returned by the signer
	std::map<std::string, std::string> headers; // parsed header name -> value
};

// The captured URL/cookie defaults, with DY_* environment overrides applied.
SignParams signParamsFromEnvironment();

// Douyin 40.8.0 libmetasec_ml.so. init() reproduces the app's metasec setup
// (device and install id, config-center blob, native handle); sign() calls the
// internal seven-header entry (libmetasec_ml.so + kSignOffset).
//
// The Java side is installed separately through installEnvironment(); init()
// only talks to the already-loaded module.
class Case
{
    public:
	Result<void> init(runtime::RuntimeInstance &instance, uint64_t module_handle);
	Result<SignOutput> sign(runtime::RuntimeInstance &instance,
		const SignParams &params);

    private:
	uint64_t module_base_ = 0;
	bool initialized_ = false;
};

} // namespace duckemu::user::dy7
