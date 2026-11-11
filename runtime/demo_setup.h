#pragma once
#include <filesystem>
#include <string>
namespace em2 {
bool PublicDemoBuild();
bool Em1DemoSession();
bool PrepareDemoSession(const std::filesystem::path& executable_directory,
    std::filesystem::path& user_directory,bool explicit_user_directory,bool interactive,std::string& error);
void ApplyPublicDemoSettings();
}
