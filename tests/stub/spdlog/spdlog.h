// Minimal stand-in for spdlog so world_parser_v2.cpp can be unit-tested without
// the Conan dependency. All logging is swallowed; only fmt-style signatures that
// the parser uses are provided.
#pragma once
#include <string>

namespace spdlog {
template <typename... Args>
void info([[maybe_unused]] const char* fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void info([[maybe_unused]] const std::string& fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void warn([[maybe_unused]] const char* fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void warn([[maybe_unused]] const std::string& fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void debug([[maybe_unused]] const char* fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void debug([[maybe_unused]] const std::string& fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void error([[maybe_unused]] const char* fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void error([[maybe_unused]] const std::string& fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void trace([[maybe_unused]] const char* fmt, [[maybe_unused]] Args&&... args) {}
template <typename... Args>
void trace([[maybe_unused]] const std::string& fmt, [[maybe_unused]] Args&&... args) {}
}  // namespace spdlog
