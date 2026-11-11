#pragma once
#include "online_boot.h"
#include "online_checkpoint.h"
#include <atomic>
#include <memory>
#include <stop_token>
namespace em2::net {
struct PreparedOnlineBoot {
    Identity identity;
    std::shared_ptr<const CheckpointBundle> bundle;
};
// Shared worker implementation used by legacy Session and Room bootstraps.
// Performs the same verified identity, retained-source and CREATE_NEW checks;
// creates only a new private destination and installs the host's frozen save.
PreparedOnlineBoot PrepareOnlineBoot(const BootPaths&,const std::filesystem::path& frozen_source,
    bool host,uint8_t delay,InitializationPolicy,const std::array<uint8_t,16>& nonce,
    const std::shared_ptr<std::atomic<PreparationStep>>&,std::stop_token);
}
