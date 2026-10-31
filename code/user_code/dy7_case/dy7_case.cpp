// SPDX-License-Identifier: GPL-2.0-only
#include "dy7_case/dy7_case.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "common/id/ids.hpp"
#include "dy7_case/dy7_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"

namespace duckemu::user::dy7
{
namespace
{

// ---------------------------------------------------------------------------
// 按需修改这里，或使用 DY_* 环境变量覆盖。
// ---------------------------------------------------------------------------
// 设备身份（换设备时和下面的 URL / cookie 一起换）。
constexpr const char *kDeviceId = "3951009026851867";
constexpr const char *kInstallId = "3489221505207928";
// 请求参数：URL + cookie。header blob 也可由 DY_HEADERS 整段覆盖。
constexpr const char *kDefaultUrl =
	"https://log0-misc-lf.amemv.com/service/2/app_log/"
	"?version_code=400800&device_platform=android&device_id=3951009026851867"
	"&aid=1128&iid=3489221505207928&tt_data=a";
constexpr const char *kDefaultCookie =
	"store-region-src=did; odin_tt=f6561e4976f49fba3a0e58569effb66e5bac1f85ae38ab95c3771a70a2078fbe2f50f4f4c13332705443edcae1bcd11056bb0217066416a3a7789b749125abc2c362bb9da8fdb561b3b58f41c7c6ee20; "
	"ttreq=1$df343d2b353e6df23c254e3bf6519c336fc370a3; store-region=cn-he; install_id=3489221505207928";
// ---------------------------------------------------------------------------

// The native decrypts this server-side switch blob; it carries
// {"d_httpdns_ctrl":true,"d_signv5_ctrl":true,"d_navi_ctrl":false}.
constexpr const char *kConfigCenter =
	"fidhWm1xcXVha3ZaZnF3aSc/cXdwYCknYVp2bGJrczBaZnF3aSc/cXdwYCknYVprZHNsWmZxd2knP2NkaXZgeA==";
constexpr const char *kConfigBlob =
	"bo95dJizD1WFcV03zOuLzN5Pn1sFtVa3szqiVQmflMJTNW0p0Kpqfw8D4i0zUlfrou4kuYt/i0521YRygM83dwv/wn3DD+TMJF+QFzW9wb8Qq2/1B4jPMbObrDNdyMMukpAYqy1fLWtbLGVIPxsFsZegwQy5lsRX9h49PH/Qx8MwgYvWvH7ZTFLV28LwTWZiljQyBPaBE+TsyumEu0Y+JRkeidHFEYcVs0yRoa+xC004hugQhdPupIt6dBiWA4phsB3fNJZjFTAKGE1lPB4gzt6Qf+FmlgZBbRvT8zekxTV2HZ5dUvSutB2/0QpbHKAvWL4DRA==";
constexpr const char *kDefaultSdkVersion = "v04.10.04.01-bugfix";

// The dispatcher selectors the app uses to set metasec up.
constexpr uint64_t kOpSetContext = 0x1000003;
constexpr uint64_t kOpDecodeSdkVersion = 0x1000001;
constexpr uint64_t kOpRegisterConfig = 0x4000001;
constexpr uint64_t kOpCreateHandle = 0x4000002;
constexpr uint64_t kOpConfigCenter = 0x200000f;
constexpr uint64_t kOpSetDeviceId = 0x2000002;
constexpr uint64_t kOpSetInstallId = 0x2000003;

const char *EnvOr(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return value != nullptr ? value : fallback;
}

uint64_t nowMs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch())
					     .count());
}

// Calls the RegisterNatives entry k3.a(op, arg2, j, string, object) and
// returns the object result as an object address (0 for null).
Result<uint64_t> callDispatcher(runtime::RuntimeInstance &instance,
	const char *what,
	uint64_t op,
	uint64_t arg2,
	uint64_t j,
	uint64_t string_object,
	uint64_t object)
{
	const std::array<uint64_t, 8> arguments = { op, arg2, j, string_object, object };
	auto called = instance.callRegisteredNative(
		kNativeClass, "a", kNativeSignature, arguments);
	if (!called.ok())
	{
		return Error{ called.error().code,
			std::string(what) + ": " + called.error().message };
	}
	if (called.value() == 0)
	{
		return uint64_t{ 0 };
	}
	auto resolved = instance.jniResolveHandle(called.value());
	if (!resolved.ok())
	{
		return Error{ resolved.error().code,
			std::string(what) + ": " + resolved.error().message };
	}
	return resolved.value();
}

Result<uint64_t> allocateCString(runtime::RuntimeInstance &instance,
	std::string_view text,
	std::string_view name)
{
	const uint64_t size = text.size() + 1;
	auto mapped = instance.mapMemory(size, runtime::PermReadWrite, name);
	if (!mapped.ok())
	{
		return mapped.error();
	}
	std::string buffer(text);
	buffer.push_back('\0');
	auto written = instance.writeMemory(mapped.value(), buffer.data(), buffer.size());
	if (!written.ok())
	{
		return written.error();
	}
	return mapped.value().value();
}

Result<std::string> readCString(runtime::RuntimeInstance &instance, GuestVA address)
{
	// The signer returns a NUL-terminated CRLF block; read it in chunks so the
	// guest address does not have to name a known length. A chunk that runs off
	// the end of the mapping shrinks instead of failing: the string may end
	// exactly at a page boundary.
	constexpr size_t kMaxChunk = 256;
	constexpr size_t kLimit = 1024 * 1024;
	std::string text;
	size_t offset = 0;
	size_t chunk_size = kMaxChunk;
	while (text.size() < kLimit)
	{
		char chunk[kMaxChunk];
		auto read = instance.readMemory(address + offset, chunk, chunk_size);
		if (!read.ok())
		{
			if (chunk_size == 1)
			{
				return read.error();
			}
			chunk_size /= 2;
			continue;
		}
		const void *terminator = std::memchr(chunk, '\0', chunk_size);
		if (terminator != nullptr)
		{
			text.append(chunk,
				static_cast<size_t>(static_cast<const char *>(terminator) - chunk));
			return text;
		}
		text.append(chunk, chunk_size);
		offset += chunk_size;
		chunk_size = kMaxChunk;
	}
	return Error{ ErrorCode::InvalidArgument, "the signer result is not NUL-terminated" };
}

void parseHeaderBlock(const std::string &text, std::map<std::string, std::string> *headers)
{
	size_t position = 0;
	while (position < text.size())
	{
		const size_t name_end = text.find("\r\n", position);
		if (name_end == std::string::npos)
		{
			break;
		}
		const size_t value_start = name_end + 2;
		const size_t value_end = text.find("\r\n", value_start);
		const std::string name = text.substr(position, name_end - position);
		const size_t end = value_end == std::string::npos ? text.size() : value_end;
		if (!name.empty())
		{
			(*headers)[name] = text.substr(value_start, end - value_start);
		}
		if (value_end == std::string::npos)
		{
			break;
		}
		position = value_end + 2;
	}
}

} // namespace

SignParams signParamsFromEnvironment()
{
	SignParams params;
	params.url = EnvOr("DY_URL", kDefaultUrl);
	if (const char *headers = std::getenv("DY_HEADERS"))
	{
		params.headers = headers;
		params.headers_given = true;
	}
	else
	{
		params.cookie = EnvOr("DY_COOKIE", kDefaultCookie);
	}
	if (const char *wall_time = std::getenv("DY_WALL_TIME_MS"))
	{
		params.wall_time_ms = std::strtoull(wall_time, nullptr, 10);
	}
	return params;
}

Result<void> Case::init(runtime::RuntimeInstance &instance, uint64_t module_handle)
{
	if (initialized_)
	{
		return {};
	}
	auto range = instance.moduleRange(module_handle);
	if (!range.ok())
	{
		return range.error();
	}

	auto context = newContextObject();
	if (!context.ok())
	{
		return context.error();
	}

	auto configured = callDispatcher(
		instance, "set-context", kOpSetContext, 0, 0, 0, context.value());
	if (!configured.ok())
	{
		return configured.error();
	}

	// Decode s4.LIZ, the SDK version used in the config JSON. A failed decode
	// keeps the captured default, exactly like the app falls back.
	std::string sdk_version = kDefaultSdkVersion;
	{
		static const uint8_t kSdkVersionKey[] = { 51, 102, 20, 15, 90, 126, 121, 69, 54, 43,
			117, 103, 13, 67, 30, 41, 49, 28, 122 };
		auto array = makeByteArray(kSdkVersionKey, sizeof(kSdkVersionKey));
		auto key = makeString("443549");
		if (array.ok() && key.ok())
		{
			auto decoded = callDispatcher(instance,
				"decode-sdk-version",
				kOpDecodeSdkVersion,
				0,
				0,
				key.value(),
				array.value());
			if (decoded.ok() && decoded.value() != 0)
			{
				auto text = readString(decoded.value());
				if (text.ok())
				{
					sdk_version = text.value();
				}
			}
		}
	}

	// u2.LIZ(): the config JSON. The encrypted field and the 15 positional
	// fields are required; without register-config the handle stays null.
	const std::string config = std::string("[\"1128\",\"\",\"\",\"") + kConfigBlob + "\",\"" +
		sdk_version + "\",\"\",\"" + kDeviceId + "\",\"\",\"" + kInstallId +
		"\",\"\",\"0\",\"-1\",\"810\",\"" + kDeviceId + "\",\"1\",[],[\"tk_key\",\"douyin\"]]";
	auto config_string = makeString(config);
	if (!config_string.ok())
	{
		return config_string.error();
	}
	auto registered =
		callDispatcher(instance, "register-config", kOpRegisterConfig, 0, 0,
			config_string.value(), 0);
	if (!registered.ok())
	{
		return registered.error();
	}
	if (registered.value() == 0)
	{
		return Error{ ErrorCode::Unsupported, "register-config returned null" };
	}
	auto registered_value = readBooleanBox(registered.value());
	if (!registered_value.ok())
	{
		return Error{ registered_value.error().code,
			"cannot read register-config: " + registered_value.error().message };
	}
	if (!registered_value.value())
	{
		return Error{ ErrorCode::Unsupported, "register-config did not return true" };
	}

	auto handle_object = callDispatcher(instance, "create-handle", kOpCreateHandle, 0, 0, 0, 0);
	if (!handle_object.ok())
	{
		return handle_object.error();
	}
	if (handle_object.value() == 0)
	{
		return Error{ ErrorCode::Unsupported, "create-handle returned null" };
	}
	auto handle = readLongBox(handle_object.value());
	if (!handle.ok())
	{
		return Error{ handle.error().code,
			"cannot read the native handle: " + handle.error().message };
	}
	if (handle.value() == 0)
	{
		return Error{ ErrorCode::Unsupported, "create-handle returned 0" };
	}

	auto center = makeString(kConfigCenter);
	auto device_id = makeString(kDeviceId);
	auto install_id = makeString(kInstallId);
	if (!center.ok() || !device_id.ok() || !install_id.ok())
	{
		return Error{ ErrorCode::Internal, "cannot allocate the metasec setup strings" };
	}

	const struct
	{
		const char *what;
		uint64_t op;
		uint64_t string_object;
	} setup[] = {
		{ "config-center", kOpConfigCenter, center.value() },
		{ "set-device-id", kOpSetDeviceId, device_id.value() },
		{ "set-install-id", kOpSetInstallId, install_id.value() },
	};
	for (const auto &step : setup)
	{
		auto called = callDispatcher(instance, step.what, step.op, 0, handle.value(),
			step.string_object, context.value());
		if (!called.ok())
		{
			return called.error();
		}
	}

	module_base_ = range.value().base.value();
	initialized_ = true;
	return {};
}

Result<SignOutput> Case::sign(runtime::RuntimeInstance &instance, const SignParams &params)
{
	if (!initialized_)
	{
		return Error{ ErrorCode::InvalidArgument, "dy7: sign before init" };
	}

	const uint64_t wall_time_ms = params.wall_time_ms != 0 ? params.wall_time_ms : nowMs();
	instance.setWallTimeNs(wall_time_ms * 1000000ULL);

	const std::string url = params.url.empty() ? std::string(kDefaultUrl) : params.url;
	const std::string headers =
		params.headers_given ? params.headers : "cookie\r\n" + params.cookie + "\r\n";

	auto url_address = allocateCString(instance, url, "dy7-url");
	auto header_address = allocateCString(instance, headers, "dy7-headers");
	if (!url_address.ok() || !header_address.ok())
	{
		return Error{ ErrorCode::Internal, "cannot allocate the signer arguments" };
	}

	const std::array<uint64_t, 8> arguments = { url_address.value(), header_address.value() };
	auto called = instance.callFunction(GuestVA{ module_base_ + kSignOffset }, arguments);
	if (!called.ok())
	{
		return called.error();
	}
	if (called.value().exit_code.has_value())
	{
		return Error{ ErrorCode::Unsupported, "the seven-header signer called exit_group" };
	}
	if (called.value().return_value == 0)
	{
		return Error{ ErrorCode::Unsupported, "the seven-header signer returned null" };
	}
	auto text = readCString(instance, GuestVA{ called.value().return_value });
	if (!text.ok())
	{
		return text.error();
	}

	SignOutput output;
	output.sign = std::move(text.value());
	parseHeaderBlock(output.sign, &output.headers);
	return output;
}

} // namespace duckemu::user::dy7
