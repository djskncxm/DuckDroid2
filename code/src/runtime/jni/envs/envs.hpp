// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string_view>

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{

class JavaWorld;

// Java environment supplements.
//
// Two layers, one direction of dependency:
//   src/runtime/jni/envs/   platform packs (java.lang, later android.*); no
//                           target-specific names are ever added here
//   code/user_code/<case>/  one target project; its main.cpp installs its own
//                           pack explicitly via
//                           RuntimeInstance::installJavaEnvironment()
//
// A pack is one function. It declares only the classes/methods/fields the
// target actually needs; nothing is created implicitly.
namespace envs
{

using Installer = Result<void> (*)(JavaWorld &world);

// Installs the built-in platform packs. Called once when the instance is
// created, before any module is loaded.
Result<void> installAll(JavaWorld &world);

// Platform packs. Every Java (java.* / javax.*) method a target may call
// belongs here, never in code/user_code/<case>/: a target pack only declares
// its own classes and the Android object graph with device values.
Result<void> installJavaLang(JavaWorld &world);
Result<void> installJavaIo(JavaWorld &world);
Result<void> installJavaMath(JavaWorld &world);
// Needs installJavaIo and installJavaMath to have run first.
Result<void> installJavaSecurity(JavaWorld &world);

// Reads the int a java.lang.Integer carries. Host runners that hold an object
// address use this instead of reaching for the pack's field layout.
Result<int32_t> integerValue(JavaWorld &world, uint64_t object);
// The same for the other box classes a native returns to its host caller.
Result<bool> booleanValue(JavaWorld &world, uint64_t object);
Result<uint64_t> longValue(JavaWorld &world, uint64_t object);

// Creates a java.io.File for an absolute path. Target packs use it when an
// Android method (Context.getFilesDir) has to hand the guest a File object.
Result<uint64_t> newFile(JavaWorld &world, std::string_view path);

} // namespace envs
} // namespace duckemu::runtime::jni
