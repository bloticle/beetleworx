#pragma once
#include "online.h"
#include "online_checkpoint.h"
#include <filesystem>
#include <stop_token>

namespace em2::net {
// Marker/lease files live outside 42560816 and never enter the campaign bundle.
// Only native-verified HOST sessions are published by the lifecycle consumer.
class HostCampaignLease {
public:
    explicit HostCampaignLease(void* handle):handle_(handle){}
    ~HostCampaignLease();
    HostCampaignLease(const HostCampaignLease&)=delete;
    HostCampaignLease& operator=(const HostCampaignLease&)=delete;
private:
    void* handle_;
};
struct HostCampaignRecord {
    std::unique_ptr<HostCampaignLease> lease;
    std::string error;
};
struct RetainedHostCampaign {
    std::filesystem::path user_data;
    std::string label;
    uint64_t saved_time=0;
};
struct HostCampaignCatalog {
    std::vector<RetainedHostCampaign> campaigns;
    std::string error;
    bool truncated=false;
};
// Call ONLY after original native archive decode and IsSavedGamePresent have
// succeeded, before releasing native Continue. This is not a native save parser.
HostCampaignRecord RecordValidatedNativeHostCampaign(const std::filesystem::path& normal_user_data,
    const std::filesystem::path& private_user_data,const Session& ready);
// Worker-only: bounded discovery of marked, idle host campaigns. Rejects links,
// active leases, unsupported/truncated headers and incomplete checkpoint trees.
// Actual native decode/summary validation remains mandatory when resuming.
HostCampaignCatalog FindRetainedHostCampaigns(const std::filesystem::path& normal_user_data,std::stop_token={});
CampaignSaveSourceResult ValidateRetainedHostCampaign(const std::filesystem::path& normal_user_data,
    const std::filesystem::path& private_user_data,std::stop_token={});
bool TestRetainedHostCampaigns(const std::filesystem::path& fixture);
}
