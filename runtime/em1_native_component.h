#pragma once
#include "em1_native_properties.h"
#include <memory>
#include <array>
namespace em2::em1 {
inline constexpr uint32_t kAmmoPrototypeDestroy=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+12);
inline constexpr uint32_t kAmmoPrototypeType=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+16);
inline constexpr uint32_t kAmmoPrototypeNode=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+20);
inline constexpr uint32_t kAmmoPrototypeCount=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+24);
inline constexpr uint32_t kAmmoPrototypeDescriptor=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+28);
inline constexpr uint32_t kAmmoPrototypeNames=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+32);
static_assert(kAmmoPrototypeDestroy==kFloatDefaultBuffer+4);
static_assert(kAmmoPrototypeNames+4==kFloatBoxDescriptor);
static_assert(kFloatBoxDescriptor+4==kAmmoNativeRead);
static_assert(kAmmoNativeWrite+4==kAmmoNativeCreate);
static_assert(kAmmoNativeCreate+4==kArrayDefaultBox);
static_assert(kArrayDefaultBox+4==kInventoryNativeCreate);
static_assert(kInventoryNativeCreate+4==kStringPropertyWrite);
static_assert(kStringPropertyWrite+4==kNavigationNativeCreate);
static_assert(kNavigationNativeDestroy+4==kBranchNativeCreate);
static_assert(kBranchNativeCreate+4==kRelayNativeCreate);
static_assert(kRelayNativeCreate+4==kSeedNativeCreate);
static_assert(kSeedNativeCreate+4==kTrackingNativeCreate);
static_assert(kTrackingNativeCreate+4==uint32_t(PPC_CODE_BASE+kPropertyCodeSize));
void CreateAmmoFacade(PPCContext&,uint8_t*);
void CreateInventoryFacade(PPCContext&,uint8_t*);
// Engine-thread installation in the actual component factory. Class metadata
// lives for the engine process; existing actors/types are not replaced.
bool RegisterNativeAmmoFacade(PPCContext&,uint8_t*,std::string&);
bool RegisterNativeInventoryFacade(PPCContext&,uint8_t*,std::string&);
void CreateNativeBranchComponent(PPCContext&,uint8_t*);
void CreateNativeRelayComponent(PPCContext&,uint8_t*);
bool RegisterNativeLogicFacades(PPCContext&,uint8_t*,std::string&);
void CreateNativeSeedComponent(PPCContext&,uint8_t*);
bool RegisterNativeSeedFacade(PPCContext&,uint8_t*,std::string&);
bool ProbeRegisteredSeedComponents(PPCContext&,uint8_t*,std::span<const uint32_t> component_definitions,std::string&);
void CreateNativeTrackingComponent(PPCContext&,uint8_t*);
bool RegisterNativeTrackingFacade(PPCContext&,uint8_t*,std::string&);
struct NativeTrackingProfile {
  int32_t type=0,stimulus=0;std::vector<std::string> collectibles;bool retired=false;
  bool operator==(const NativeTrackingProfile&)const=default;
};
struct NativeTrackingAction {
  enum class Kind {Retire,PublishStimulus};Kind kind=Kind::PublishStimulus;
  uint32_t subject=0,instigator=0;int32_t classification=2;float amount=0;
  // PublishStimulus uses ORIGINAL message8016. Retire is a tracker-manager
  // notification, not another death message to broadcast.
};
using NativeTrackingSink=bool(*)(void*,uint32_t component,const NativeTrackingAction&);
bool ReadNativeTrackingProfile(PPCContext&,uint8_t*,uint32_t,NativeTrackingProfile&,std::string&);
// Typed original-message boundary; requires a real world/manager receiver for
// accepted events. Does not publish original numeric IDs into EM2's event bus.
bool ProcessNativeTrackingInput(PPCContext&,uint8_t*,uint32_t component,uint32_t owner,
    uint16_t original_event,uint32_t subject,uint32_t instigator,uint32_t player,float amount,
    std::span<const int32_t> stimulus_types,NativeTrackingSink,void*,std::string&);
bool ProbeRegisteredTrackingComponents(PPCContext&,uint8_t*,std::span<const uint32_t>,std::string&);
// Original EM1 event IDs, scoped to an EM1 event router, never sent as EM2 IDs.
enum class NativeLogicOutput:uint16_t {BranchTrue=0x4C,BranchFalse=0x4D,RelayTrigger=0x4E};
enum class NativeLogicInput {Test,SetBranchValue,RelayEnable,RelayDisable,RelayToggle,RelayTrigger};
using NativeLogicSink=bool(*)(void*,uint32_t component,NativeLogicOutput);
// Engine thread only. Emitting inputs require an explicit receiving router.
// Property changes use the object's real native reflected Boolean storage.
bool InvokeNativeLogicInput(PPCContext&,uint8_t*,uint32_t component,NativeLogicInput,bool value,
    NativeLogicSink,void* sink_context,std::string&);
bool ProbeRegisteredLogicComponents(PPCContext&,uint8_t*,std::span<const uint32_t> component_definitions,std::string&);
void AmmoNativeRead(PPCContext&,uint8_t*);
void AmmoNativeWrite(PPCContext&,uint8_t*);
void DestroyAmmoPrototype(PPCContext&,uint8_t*);
void AmmoPrototypeType(PPCContext&,uint8_t*);
void AmmoPrototypeNode(PPCContext&,uint8_t*);
void AmmoPrototypeCount(PPCContext&,uint8_t*);
void AmmoPrototypeDescriptor(PPCContext&,uint8_t*);
void AmmoPrototypeNames(PPCContext&,uint8_t*);
// Caller supplies a live native object with its initialized blueprint,
// interned-name reference and writable
// four-byte guest value buffer, on the engine thread. Honor native overrides,
// blueprint inheritance/defaults, and instance property storage; preserve ctx.
bool ReadNativeComponentFloat(PPCContext&,uint8_t*,uint32_t object,uint32_t name_reference,uint32_t value,uint32_t index=0);
bool WriteNativeComponentFloat(PPCContext&,uint8_t*,uint32_t object,uint32_t name_reference,uint32_t value,uint32_t index=0);
// Isolated construction/reflection/lifetime only. No class registration,
// separate recharge simulation, inventory mutation or world admission.
bool ProbeNativeAmmoComponent(PPCContext&,uint8_t*,std::string&,uint32_t original_definition=0);
// Real retail private factory, shared class metadata and simultaneous objects.
// No publication in the global factory or world admission.
bool ProbeNativeAmmoFactory(PPCContext&,uint8_t*,std::string&,uint32_t original_definition=0);
// Actual registered Inventory factory, all17 declarations/native array boxes,
// independent instances and optional full source definition. No world binding.
bool ProbeNativeInventoryFactory(PPCContext&,uint8_t*,std::string&,uint32_t original_definition=0);
// Actual private retail factory and complete source definitions. Does not
// publish this class globally or claim its world lifecycle is implemented.
struct NavigationDatabase;
struct NativeNavigationProfile {
  std::string filename;
  float pitch=0;
  std::array<float,3> maximum{},minimum{};
  int32_t radius=0,height=0,movement=0;
  bool operator==(const NativeNavigationProfile&)const=default;
};
// Registered data component: owns original source data and native graph assets.
// Actor AI-world publication is a separate requirement for level admission.
bool RegisterNativeNavigationFacade(PPCContext&,uint8_t*,std::shared_ptr<const std::vector<NavigationDatabase>>,std::string&);
void CreateNativeNavigationComponent(PPCContext&,uint8_t*);
void DefineNativeNavigationComponent(PPCContext&,uint8_t*);
void DestroyNativeNavigationComponent(PPCContext&,uint8_t*);
bool ReadNativeNavigationComponent(PPCContext&,uint8_t*,uint32_t object,NativeNavigationProfile&,size_t& sectors,std::string&);
bool IsNativeNavigationComponent(uint8_t*,uint32_t object);
struct NativeNavigationComponentStatistics {size_t instances=0,graphs=0,failed=0;bool operator==(const NativeNavigationComponentStatistics&)const=default;};
NativeNavigationComponentStatistics NativeNavigationComponents();
bool ProbeRegisteredNavigationLifecycle(PPCContext&,uint8_t*,std::span<const uint32_t> definitions,
    std::span<const NativeNavigationProfile> expected,std::string&);
bool ProbeNativeNavigationFactory(PPCContext&,uint8_t*,std::string&,uint32_t original_definition,
    std::shared_ptr<const std::vector<NavigationDatabase>> navigation,size_t& bound_sectors,NativeNavigationProfile&);
}
