#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace em2::net {
struct InstallationIdentity {
    // Packaged build compatibility hashes the declared release version, not the
    // executable. Unversioned development trees use an executable fingerprint.
    std::array<uint8_t,32> build{},content{},checkpoint{};
    uint64_t files=0,bytes=0;
    // Physical verification reads (including build/config key scans); logical
    // files/bytes above retain their existing protocol-independent semantics.
    uint64_t read_bytes=0,reused_files=0;
    bool reused_content_digest=false;
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
    const std::filesystem::path& staged_content,std::stop_token cancel={},
    const std::filesystem::path& content_cache={});
bool TestInstallationIdentity(const std::filesystem::path& fixture);
}
