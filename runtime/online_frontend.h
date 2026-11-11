#pragma once
#include "online_boot.h"
namespace em2::net {
std::string PreferredOnlineHostAddress();
// Startup UI, before creating a guest runtime or opening any save files.
std::optional<BootResult> ShowOnlineFrontend(const std::filesystem::path& executable,
    const std::filesystem::path& content,const std::filesystem::path& normal_user_data);
}
