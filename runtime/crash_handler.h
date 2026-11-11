#pragma once
#include <filesystem>
void InstallCrashHandler(const std::filesystem::path& log_path);
bool TestStartupCallbackReports(const std::filesystem::path& directory);
