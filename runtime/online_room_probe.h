#pragma once
#include <filesystem>
namespace em2::net {
// Private loopback diagnostics only. No role means a successful no-op.
// Call before Runtime construction; user_data changes only after real verified
// checkpoint installation and frozen Room adoption succeed.
bool PreparePrivateRoomProbe(const std::filesystem::path& executable,
    const std::filesystem::path& content,std::filesystem::path& user_data);
// Call after BindNativeRoomThread. The installed completed-tick observer ALWAYS
// votes false: this probe is never an admitted gameplay adapter.
bool InstallPrivateRoomProbe();
}
