// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::user::dy7
{

// 抖音 40.8.0 (com.ss.android.ugc.aweme) libmetasec_ml.so — the Java side the
// native walks during load and initialization.
//
// libmetasec walks MS -> MS0 -> ms/bd/c/k3 to reach the class carrying the
// RegisterNatives table, then calls back into the app for the package name
// and the files directory. The shims here only supply target-specific values;
// String/System/Thread and the box classes come from the platform packs.
extern const char *const kEntryClass;
extern const char *const kCoreClass;
extern const char *const kNativeClass;
extern const char *const kNativeSignature;
extern const char *const kContextClass;
extern const char *const kPackage;

Result<void> installEnvironment(runtime::jni::JavaWorld &world);

// Host-side access to the installed world. The runner and the worker plugin
// build the metasec call arguments (strings, byte arrays, the Context object)
// and read boxed results through these; one install per instance process.
Result<uint64_t> makeString(std::string_view text);
Result<uint64_t> makeByteArray(const void *data, uint64_t size);
Result<uint64_t> newContextObject();
Result<std::string> readString(uint64_t object);
Result<bool> readBooleanBox(uint64_t object);
Result<uint64_t> readLongBox(uint64_t object);

} // namespace duckemu::user::dy7
