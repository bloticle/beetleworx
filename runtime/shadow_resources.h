#pragma once
#include <cstdint>
namespace em2 {
struct NativeShadowPreparation {uint32_t caller=0,owner=0,enabled=0,camera_count=0,first_camera=0,camera_type=0,cameras=0;uint32_t selected[3]{};bool operator==(const NativeShadowPreparation&)const=default;};
// Read-only diagnostics at the actual native preparation boundary. Keeps the
// current maps separate from allocated targets and character blob projectors.
void ObserveNativeShadowPreparation(uint32_t caller,uint32_t owner,uint32_t selected,uint32_t camera_count,uint32_t cameras,uint8_t* base);
struct AllocationTrace;
struct ShadowCreationTrace {
    bool active=false;
    uint32_t owner=0,slot=0,cached_size=0,requested_size=0;
    uint32_t requests=0,physical_requests=0,physical_completed=0,last_caller=0;
    uint64_t requested_bytes=0,physical_requested_bytes=0,physical_granted_bytes=0;
    uint32_t failed_allocation_size=0,failed_allocation_address=0;
    bool operator==(const ShadowCreationTrace&) const=default;
};
// Shared by startup construction and live staging, including nested probes.
class ShadowCreationAuditScope {
public:
    explicit ShadowCreationAuditScope(ShadowCreationTrace);
    ~ShadowCreationAuditScope();
    ShadowCreationAuditScope(const ShadowCreationAuditScope&)=delete;
    ShadowCreationAuditScope& operator=(const ShadowCreationAuditScope&)=delete;
private:
    ShadowCreationTrace previous_;
};
// POD TLS snapshots; safe to read from the exception filter without guest reads.
ShadowCreationTrace CurrentShadowCreation();
bool ShadowCreationActive();
void NoteShadowAllocationRequest(const AllocationTrace&);
void NoteShadowAllocationCompletion(const AllocationTrace&,uint32_t result,uint32_t granted);
// Reject null, null-page and wrapping metadata spans before native constructors
// or XG header copies can write them. Does not prove allocator/heap health.
bool ValidateShadowMetadataAllocation(uint32_t address,uint32_t bytes);
bool TestShadowMetadataAllocationFailure();
// Isolated diagnostic only; invokes the existing exception-filter probe.
void RunShadowCreationCrashProbe(bool physical);
bool TestShadowResourceCreation();
}
