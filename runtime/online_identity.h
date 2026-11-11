#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace em2::net {
struct InstallationIdentity {
    std::array<uint8_t,32> build{},content{},checkpoint{};
    uint64_t files=0,bytes=0;
};
struct IdentityResult {std::optional<InstallationIdentity> value;std::string error;};
// Blocking worker operation; never call on the render/simulation thread.
// The checkpoint directory must be a frozen copy, not live autosave files.
// These are file identities, not a serialization of the running world.
IdentityResult FingerprintInstallation(const std::filesystem::path& executable,
    const std::filesystem::path& staged_content,const std::filesystem::path& frozen_checkpoint,
    std::stop_token cancel={});
// Client preflight does not require a local save. Its checkpoint field is zero
// until the explicit download handshake negotiates the host's checkpoint.
IdentityResult FingerprintProgram(const std::filesystem::path& executable,
    const std::filesystem::path& staged_content,std::stop_token cancel={});
bool TestInstallationIdentity(const std::filesystem::path& fixture);
}
