#pragma once
#include "online_local_movement.h"
#include "online.h"
#include <array>
#include <memory>
#include <string>
#include <vector>
namespace em2::net {
inline constexpr unsigned kNativeTriggerActions=256;
enum class NativeButtonScope {TriggersOnly,DigitalAndTriggers};
struct NativeTriggerAction {
    uint32_t event=0,action=0,ordinal=0,context=0,uid=0,kind=0,key=0;
    std::string name;
    std::array<uint8_t,436> shape{}; // Intraprocess schema; mutable state normalized.
    bool operator==(const NativeTriggerAction&)const=default;
};
struct NativeTriggerExclusion {
    uint32_t event=0,action=0,ordinal=0,kind=0,condition=0,curve=0;
    std::string reason;
    bool operator==(const NativeTriggerExclusion&)const=default;
};
struct NativeTriggerContext {
    uint32_t event=0;bool retail_listener=false;
    std::array<uint32_t,6> header{};
    std::vector<uint32_t> listeners,action_order;
    bool operator==(const NativeTriggerContext&)const=default;
};
struct NativeTriggerBinding {
    NativeLocalMovementBinding parent;
    NativeButtonScope scope=NativeButtonScope::TriggersOnly;
    std::string active_context_name;
    uint32_t threshold=0,buttons=0,button_count=0;
    std::vector<NativeTriggerAction> actions;
    std::vector<NativeTriggerExclusion> excluded;
    std::vector<NativeTriggerContext> contexts;
    bool operator==(const NativeTriggerBinding&)const=default;
};
struct NativeTriggerResult {
    uint16_t descriptor=0;bool dispatch=false;float x=0,y=0;
    bool operator==(const NativeTriggerResult&)const=default;
};
struct NativeTriggerBatch {
    uint64_t source_epoch=0,source_tick=0;
    unsigned count=0;
    std::array<NativeTriggerResult,kNativeTriggerActions> actions{};
    bool operator==(const NativeTriggerBatch&)const=default;
};
class NativeTriggerSource {
public:
    NativeTriggerSource();~NativeTriggerSource();
    NativeTriggerSource(const NativeTriggerSource&)=delete;
    NativeTriggerSource& operator=(const NativeTriggerSource&)=delete;
private:
    struct Impl;std::unique_ptr<Impl> impl_;friend struct NativeTriggerAccess;
};
// Partial original-math slice, never action dispatch or complete controls.
// Inventories LT/RT, or all digital/trigger records, in default/current order;
// excluded kind4/condition/combination shapes are reported explicitly.
bool DiscoverNativeTriggerBinding(const NativeLocalMovementBinding&,PPCContext&,uint8_t*,NativeTriggerBinding&,
    NativeButtonScope scope=NativeButtonScope::TriggersOnly);
bool ArmNativeTriggers(NativeTriggerSource&,const NativeTriggerBinding&,uint64_t source_epoch,uint64_t first_source_tick,PPCContext&,uint8_t*);
bool ResetNativeTriggers(NativeTriggerSource&);
// Read-only receiver snapshot check. Does not require/rebind the one local
// physical producer lease and never grants native callback permission.
bool ValidateNativeButtonDefinitionSnapshot(const NativeTriggerBinding&,uint8_t*);
// Strict provenance: sampled_native_user is the actual selected native source,
// not a room peer ID. No physical0->native1 shortcut is provided here.
bool ProduceNativeTriggers(NativeTriggerSource&,const NativeTriggerBinding&,const Pad&,unsigned sampled_native_user,
    uint64_t source_epoch,uint64_t source_tick,PPCContext&,uint8_t*,NativeTriggerBatch&);
// Room-owned physical-to-native routing requires the original sealed sample;
// neither a peer ID nor a caller-supplied Pad can authorize that conversion.
bool ProduceNativeTriggers(const Room&,NativeTriggerSource&,const NativeTriggerBinding&,const NativeRoutedPadSample&,
    uint64_t source_epoch,PPCContext&,uint8_t*,NativeTriggerBatch&);
bool TestNativeTriggers();
}
