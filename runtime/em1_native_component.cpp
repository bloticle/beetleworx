#include "em1_native_component.h"
#include "em1_ammo_change.h"
#include "em1_native_item_manager.h"
#include "em1_parser_probe.h"
#include "em1_native_navigation.h"
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <bit>
#include <array>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <rex/logging.h>
PPC_EXTERN_FUNC(__imp__sub_826550C8);
PPC_EXTERN_FUNC(__imp__sub_82655058);
PPC_EXTERN_FUNC(__imp__sub_825430D8);
PPC_EXTERN_FUNC(__imp__sub_82504410);
PPC_EXTERN_FUNC(__imp__sub_822F1C10);
PPC_EXTERN_FUNC(__imp__sub_82792878);
PPC_EXTERN_FUNC(__imp__sub_82757C50);
PPC_EXTERN_FUNC(__imp__sub_82551F78);
PPC_EXTERN_FUNC(__imp__sub_82510578);
PPC_EXTERN_FUNC(__imp__sub_831BEE80);
PPC_EXTERN_FUNC(__imp__sub_827B9570);
PPC_EXTERN_FUNC(__imp__sub_827B8628);
PPC_EXTERN_FUNC(__imp__sub_82759898);
PPC_EXTERN_FUNC(__imp__sub_827584E0);
PPC_EXTERN_FUNC(__imp__sub_825052B0);
PPC_EXTERN_FUNC(__imp__sub_8256C338);
PPC_EXTERN_FUNC(__imp__sub_82510678);
PPC_EXTERN_FUNC(__imp__sub_82552238);
PPC_EXTERN_FUNC(__imp__sub_831BEFD8);
PPC_EXTERN_FUNC(__imp__sub_827B8450);
PPC_EXTERN_FUNC(__imp__sub_827CCF40);
PPC_EXTERN_FUNC(__imp__sub_827CD028);
namespace em2::em1 {
namespace {
struct Context {PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();Context(PPCContext& c):ctx(c),saved(c){}~Context(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}};
constexpr uint32_t object_size=0x48,table_size=0x6C,count_at=0x6C,type_at=0x70,node_at=0x80,array_at=0x94,name_ref=0xCC,live_at=0xD0,text_at=0xD4;
constexpr char class_name[]="AmmoRechargeComponent";
constexpr uint32_t native_mode_at=0x44,face_table_at=0xF0,face_table_size=0x190;
constexpr uint32_t descriptor_data=face_table_at+face_table_size;
constexpr uint32_t metadata_size=descriptor_data+17*4;
constexpr uint32_t shared_class_at=0xEC;
constexpr uint32_t mask_index_at=0xC8;
thread_local uint32_t creation_metadata=0;
std::atomic<uint32_t> installed_metadata{0};
NativeFloatSchema installed_schema;
std::atomic<uint32_t> installed_inventory_metadata{0};
NativeFloatSchema installed_inventory_schema;
std::atomic<uint32_t> installed_navigation_metadata{0};
NativeFloatSchema installed_navigation_schema;
std::atomic<uint32_t> installed_branch_metadata{0},installed_relay_metadata{0};
NativeFloatSchema installed_branch_schema,installed_relay_schema;
std::atomic<uint32_t> installed_seed_metadata{0};NativeFloatSchema installed_seed_schema;
std::atomic<uint32_t> installed_tracking_metadata{0};NativeFloatSchema installed_tracking_schema;
bool ReadNativeText(uint8_t*,uint32_t,std::string&);
struct NavigationComponentState {NativeNavigationProfile profile;NativeNavigationAsset asset;std::string error;bool ready=false;};
std::mutex navigation_mutex;
std::shared_ptr<const std::vector<NavigationDatabase>> navigation_sources;
std::map<uint32_t,std::unique_ptr<NavigationComponentState>> navigation_components;
enum class FacadeKind {Ammo,Inventory,Navigation,Branch,Relay,Seed,Tracking};
void ChangeSharedLiveCount(uint8_t* base,uint32_t metadata,int delta){
  std::atomic_ref<uint32_t> counter(*reinterpret_cast<uint32_t*>(base+metadata+live_at));
  auto raw=counter.load(std::memory_order_relaxed);
  while(!counter.compare_exchange_weak(raw,_byteswap_ulong(_byteswap_ulong(raw)+delta),std::memory_order_relaxed)){}
}
uint32_t Metadata(uint8_t* base,uint32_t object){return PPC_LOAD_U32(object+0x40);}
uint32_t Allocate(PPCContext& ctx,uint8_t* base,uint32_t bytes){ctx.r3.u64=bytes;ctx.r4.u64=0;__imp__sub_826550C8(ctx,base);return ctx.r3.u32;}
void Free(PPCContext& ctx,uint8_t* base,uint32_t at,uint32_t size){if(at){ctx.r3.u64=at;ctx.r4.u64=size;__imp__sub_82655058(ctx,base);}}
void InitializeAmmoMetadata(PPCContext& ctx,uint8_t* base,uint32_t metadata,const NativeFloatSchema& schema,std::string_view name=class_name){
  if(schema.descriptors.size()>17 || name.size()>=shared_class_at-text_at)throw std::runtime_error("Native item facade metadata bounds exceeded");
  std::memset(base+metadata,0,metadata_size);std::memcpy(base+metadata,base+0x82075CC0,table_size);
  PPC_STORE_U32(metadata+mask_index_at,UINT32_MAX);
  std::memcpy(base+metadata+face_table_at,base+0x82075B30,face_table_size);
  PPC_STORE_U32(metadata+face_table_at+0xD4,kAmmoNativeRead);PPC_STORE_U32(metadata+face_table_at+0xFC,kAmmoNativeWrite);
  for(const auto [slot,function]:{std::pair<uint32_t,uint32_t>{0,kAmmoPrototypeDestroy},{8,kAmmoPrototypeType},{12,kAmmoPrototypeNode},{0x5C,kAmmoPrototypeCount},{0x60,kAmmoPrototypeDescriptor},{0x64,kAmmoPrototypeNames}})PPC_STORE_U32(metadata+slot,function);
  PPC_STORE_U32(metadata+count_at,uint32_t(schema.descriptors.size()));
  PPC_STORE_U32(metadata+array_at,metadata+descriptor_data);
  for(size_t i=0;i<schema.descriptors.size();++i)PPC_STORE_U32(metadata+descriptor_data+uint32_t(i)*4,schema.descriptors[i]);
  std::memcpy(base+metadata+text_at,name.data(),name.size());
  ctx.r3.u64=metadata+type_at;ctx.r4.u64=metadata+text_at;ctx.r5.u64=0x833FCA9C;__imp__sub_82551F78(ctx,base);
  PPC_STORE_U32(metadata+node_at+8,metadata+type_at);
  ctx.r3.u64=metadata+text_at;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(metadata+name_ref,ctx.r3.u32);
  // Entity type chains and their lookup map use the same interned name handle.
  PPC_STORE_U32(metadata+type_at,ctx.r3.u32);
}
void CreateItemFacade(PPCContext& ctx,uint8_t* base,uint32_t metadata){
  uint32_t object=0;
  if(metadata){Context restore(ctx);object=Allocate(ctx,base,object_size);
    if(object){std::memset(base+object,0,object_size);ctx.r3.u64=object;ctx.r4.u64=metadata+name_ref;__imp__sub_82792878(ctx,base);
      PPC_STORE_U32(object,metadata);PPC_STORE_U32(object+8,metadata+face_table_at);PPC_STORE_U32(object+0x40,metadata);
      ChangeSharedLiveCount(base,metadata,1);
    }
  }
  ctx.r3.u64=object?object+8:0;
}
bool RegisterItemFacade(PPCContext& ctx,uint8_t* base,std::string_view name,uint32_t creator,std::atomic<uint32_t>& installed,NativeFloatSchema& schema,FacadeKind kind,std::string& error){
  Context restore(ctx);std::map<std::string,uint32_t> factories;
  if(!ReadNativeComponentFactories(base,factories,error))return false;
  if(const auto found=factories.find(std::string(name));found!=factories.end()){
    if(found->second!=creator){error="Existing item facade class has a different native implementation";return false;}
    error.clear();return true;
  }
  auto metadata=installed.load(std::memory_order_acquire);
  if(!metadata){
    const NativePropertyDeclaration logic[]={{kind==FacadeKind::Branch?"BranchValue":"RelayDisabled",NativePropertyShape::Boolean}};
    const NativePropertyDeclaration seed[]={{"AI Size 1",NativePropertyShape::Integer},{"AI Size 2",NativePropertyShape::Integer}};
    const NativePropertyDeclaration tracking[]={{"TrackingType",NativePropertyShape::Integer},{"TrackingStimType",NativePropertyShape::Integer},{"Collectible",NativePropertyShape::StringArray}};
    const bool built=kind==FacadeKind::Inventory?BuildNativeInventorySchema(ctx,base,schema,error):
      kind==FacadeKind::Navigation?BuildNativePropertySchema(ctx,base,NativeNavigationDeclarations(),schema,error):
      kind==FacadeKind::Seed?BuildNativePropertySchema(ctx,base,seed,schema,error):
      kind==FacadeKind::Tracking?BuildNativePropertySchema(ctx,base,tracking,schema,error):
      (kind==FacadeKind::Branch || kind==FacadeKind::Relay)?BuildNativePropertySchema(ctx,base,logic,schema,error):BuildNativeAmmoSchema(ctx,base,schema,error);
    if(!built)return false;
    metadata=Allocate(ctx,base,metadata_size);
    if(!metadata){ReleaseNativeFloatSchema(ctx,base,schema);error="Native item class metadata allocation failed";return false;}
    InitializeAmmoMetadata(ctx,base,metadata,schema,name);
    if(kind==FacadeKind::Branch || kind==FacadeKind::Relay || kind==FacadeKind::Seed || kind==FacadeKind::Tracking){
      for(uint32_t offset:{0xD4u,0xFCu})PPC_STORE_U32(metadata+face_table_at+offset,PPC_LOAD_U32(0x82075B30+offset));
    }
    if(kind==FacadeKind::Navigation){
      PPC_STORE_U32(metadata,kNavigationNativeDestroy);PPC_STORE_U32(metadata+0x14,kNavigationNativeDefinition);
      for(uint32_t offset:{0xD4u,0xFCu})PPC_STORE_U32(metadata+face_table_at+offset,PPC_LOAD_U32(0x82075B30+offset));
    }
    if(PPC_LOAD_U32(metadata+name_ref)<4096){Free(ctx,base,metadata,metadata_size);ReleaseNativeFloatSchema(ctx,base,schema);error="Native item class name registration failed";return false;}
    PPC_STORE_U32(metadata+shared_class_at,1);
    ctx.r3.u64=metadata+node_at;ctx.r4.u64=metadata+type_at;__imp__sub_82552238(ctx,base);
    installed.store(metadata,std::memory_order_release);
  }
  const auto factory=PPC_LOAD_U32(0x833CD7A0);
  if(PPC_LOAD_U32(metadata+mask_index_at)==UINT32_MAX){
    const auto scratch=Allocate(ctx,base,16);
    if(!scratch){error="Component type lookup scratch allocation failed";return false;}
    ctx.r3.u64=scratch;ctx.r4.u64=PPC_LOAD_U32(0x833DBAF4);ctx.r5.u64=PPC_LOAD_U32(metadata+name_ref);__imp__sub_827CD028(ctx,base);
    const auto existing=PPC_LOAD_U32(scratch+8);Free(ctx,base,scratch,16);uint32_t index=UINT32_MAX;
    if(existing)index=PPC_LOAD_U8(existing+4);
    else{
      if(PPC_LOAD_U8(0x833DBAF0)>=PPC_LOAD_U8(0x833DBAF1)){error="Native component type capacity was not reserved before entity creation";return false;}
      ctx.r3.u64=metadata+name_ref;__imp__sub_827CCF40(ctx,base);index=ctx.r3.u32;
    }
    if(index>=PPC_LOAD_U8(0x833DBAF1)){error="Native component type registration returned an invalid index";return false;}
    PPC_STORE_U32(metadata+mask_index_at,index);
    REXLOG_INFO("[em1] native component type registered: class='{}' index={} assigned={} capacity={}",name,index,PPC_LOAD_U8(0x833DBAF0),PPC_LOAD_U8(0x833DBAF1));
  }
  ctx.r3.u64=PPC_LOAD_U32(factory+8);ctx.r4.u64=metadata+name_ref;ctx.r5.u64=creator;__imp__sub_8256C338(ctx,base);
  factories.clear();if(!ReadNativeComponentFactories(base,factories,error))return false;
  const auto found=factories.find(std::string(name));
  if(found==factories.end() || found->second!=creator){error="Native item class factory registration did not persist";return false;}
  error.clear();return true;
}
}
void CreateAmmoFacade(PPCContext& ctx,uint8_t* base){CreateItemFacade(ctx,base,creation_metadata?creation_metadata:installed_metadata.load(std::memory_order_acquire));}
void CreateInventoryFacade(PPCContext& ctx,uint8_t* base){CreateItemFacade(ctx,base,creation_metadata?creation_metadata:installed_inventory_metadata.load(std::memory_order_acquire));}
bool RegisterNativeAmmoFacade(PPCContext& ctx,uint8_t* base,std::string& error){return RegisterItemFacade(ctx,base,class_name,kAmmoNativeCreate,installed_metadata,installed_schema,FacadeKind::Ammo,error);}
bool RegisterNativeInventoryFacade(PPCContext& ctx,uint8_t* base,std::string& error){return RegisterItemFacade(ctx,base,"InventoryComponent",kInventoryNativeCreate,installed_inventory_metadata,installed_inventory_schema,FacadeKind::Inventory,error);}
void CreateNativeBranchComponent(PPCContext& ctx,uint8_t* base){CreateItemFacade(ctx,base,installed_branch_metadata.load(std::memory_order_acquire));}
void CreateNativeRelayComponent(PPCContext& ctx,uint8_t* base){CreateItemFacade(ctx,base,installed_relay_metadata.load(std::memory_order_acquire));}
bool RegisterNativeLogicFacades(PPCContext& ctx,uint8_t* base,std::string& error){
  return RegisterItemFacade(ctx,base,"LogicBranchComponent",kBranchNativeCreate,installed_branch_metadata,installed_branch_schema,FacadeKind::Branch,error) &&
    RegisterItemFacade(ctx,base,"LogicRelayComponent",kRelayNativeCreate,installed_relay_metadata,installed_relay_schema,FacadeKind::Relay,error);
}
void CreateNativeSeedComponent(PPCContext& ctx,uint8_t* base){CreateItemFacade(ctx,base,installed_seed_metadata.load(std::memory_order_acquire));}
bool RegisterNativeSeedFacade(PPCContext& ctx,uint8_t* base,std::string& error){
  return RegisterItemFacade(ctx,base,"SeedPointComponent",kSeedNativeCreate,installed_seed_metadata,installed_seed_schema,FacadeKind::Seed,error);
}
void CreateNativeTrackingComponent(PPCContext& ctx,uint8_t* base){CreateItemFacade(ctx,base,installed_tracking_metadata.load(std::memory_order_acquire));}
bool RegisterNativeTrackingFacade(PPCContext& ctx,uint8_t* base,std::string& error){
  return RegisterItemFacade(ctx,base,"TrackingComponent",kTrackingNativeCreate,installed_tracking_metadata,installed_tracking_schema,FacadeKind::Tracking,error);
}
bool ReadNativeTrackingProfile(PPCContext& ctx,uint8_t* base,uint32_t object,NativeTrackingProfile& output,std::string& error){
  const auto metadata=installed_tracking_metadata.load(std::memory_order_acquire);
  if(!base || !metadata || object<4096 || PPC_LOAD_U32(object)!=metadata || !PPC_LOAD_U32(object+0x3C)){
    error="Tracking component is not initialized";return false;
  }
  Context restore(ctx);NativeTrackingProfile profile;profile.retired=base[object+native_mode_at]!=0;
  for(unsigned p=0;p<3;++p){
    ctx.r3.u64=object;ctx.r4.u64=installed_tracking_schema.descriptors[p]+4;__imp__sub_831BEFD8(ctx,base);const auto box=ctx.r3.u32;
    if(!box){error="Native tracking property could not be resolved";return false;}
    if(p<2){(p==0?profile.type:profile.stimulus)=int32_t(PPC_LOAD_U32(box+4));continue;}
    ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)+8));const auto count=ctx.r3.u32,data=PPC_LOAD_U32(box+4);
    if(count>65536 || (count && data<4096)){error="Invalid native collectible array";return false;}
    for(uint32_t i=0;i<count;++i){std::string name;
      if(!ReadNativeText(base,PPC_LOAD_U32(data+i*4),name)){error="Invalid native collectible string";return false;}
      profile.collectibles.push_back(std::move(name));
    }
  }
  output=std::move(profile);error.clear();return true;
}
bool ProcessNativeTrackingInput(PPCContext& ctx,uint8_t* base,uint32_t object,uint32_t owner,
    uint16_t event,uint32_t subject,uint32_t instigator,uint32_t player,float amount,
    std::span<const int32_t> stimulus_types,NativeTrackingSink sink,void* sink_context,std::string& error){
  Context restore(ctx);NativeTrackingProfile profile;
  if(!ReadNativeTrackingProfile(ctx,base,object,profile,error))return false;
  if(event!=0x10 && event!=0x11){error.clear();return true;}
  if(subject!=owner || (event==0x10 && instigator!=player) || (event==0x11 && profile.type!=0)){error.clear();return true;}
  if(!sink){error="Original tracking world/manager receiver is unavailable";return false;}
  NativeTrackingAction action;action.subject=subject;action.instigator=instigator;action.amount=amount;
  if(event==0x11)action.kind=NativeTrackingAction::Kind::Retire;
  else{
    const auto contains=[&](int32_t type){return std::find(stimulus_types.begin(),stimulus_types.end(),type)!=stimulus_types.end();};
    // Original A1E4C..A1F10: mode0 accepts both, mode1 prioritizes stimulus1,
    // mode2 accepts stimulus0. The unsigned <=1 comparison matters for negatives.
    if(uint32_t(profile.stimulus)<=1 && contains(1))action.classification=0;
    else if((profile.stimulus==0 || profile.stimulus==2) && contains(0))action.classification=1;
  }
  if(!sink(sink_context,object,action)){error="Original tracking receiver rejected the event";return false;}
  if(event==0x11)base[object+native_mode_at]=1;
  error.clear();return true;
}
bool ProbeRegisteredTrackingComponents(PPCContext& ctx,uint8_t* base,std::span<const uint32_t> definitions,std::string& error){
  Context restore(ctx);const auto metadata=installed_tracking_metadata.load(std::memory_order_acquire);size_t checked=0;
  if(!metadata){error="Original tracking class is not registered";return false;}
  struct Receiver {std::vector<NativeTrackingAction> actions;bool accept=true;} receiver;
  const auto sink=+[](void* context,uint32_t,const NativeTrackingAction& action){auto& receiver=*static_cast<Receiver*>(context);
    if(!receiver.accept)return false;receiver.actions.push_back(action);return true;};
  const auto scratch=Allocate(ctx,base,4);if(!scratch){error="Tracking probe scratch allocation failed";return false;}
  bool all_valid=true;
  for(auto definition:definitions){
    if(std::strcmp(reinterpret_cast<const char*>(base+PPC_LOAD_U32(definition)),"TrackingComponent"))continue;
    const auto before=PPC_LOAD_U32(metadata+live_at);std::array<uint32_t,2> faces{};std::array<NativeTrackingProfile,2> profiles{};bool valid=true;
    for(unsigned i=0;i<2 && valid;++i){
      ctx.r3.u64=PPC_LOAD_U32(0x833CD7A0);ctx.r4.u64=metadata+name_ref;ctx.r5.u64=0;__imp__sub_82510678(ctx,base);faces[i]=ctx.r3.u32;
      if(!faces[i]){valid=false;break;}
      ctx.r3.u64=faces[i];PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(faces[i])));
      const auto object=faces[i]-8;ctx.r3.u64=object;ctx.r4.u64=definition;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
      ctx.r3.u64=object;__imp__sub_831BEE80(ctx,base);valid=ReadNativeTrackingProfile(ctx,base,object,profiles[i],error);
    }
    if(valid){
      const auto object=faces[0]-8;valid=profiles[0]==profiles[1] && !profiles[0].retired && PPC_LOAD_U32(metadata+live_at)==before+2;
      const auto write=[&](unsigned field,int32_t value){PPC_STORE_U32(scratch,uint32_t(value));ctx.r3.u64=faces[0];ctx.r4.u64=installed_tracking_schema.descriptors[field]+4;ctx.r5.u64=scratch;ctx.r6.u64=0;
        PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(faces[0])+0xF0));return ctx.r3.u32!=0;};
      const std::vector<int32_t> stimuli[]={{},{0},{1},{0,1},{2}};
      const int32_t modes[]={0,1,2,3,-1};const int32_t expected[5][5]={{2,1,0,0,2},{2,2,0,0,2},{2,1,2,1,2},{2,2,2,2,2},{2,2,2,2,2}};
      for(unsigned mode=0;mode<5 && valid;++mode){valid=write(1,modes[mode]);
        for(unsigned types=0;types<5 && valid;++types){receiver.actions.clear();receiver.accept=true;
          valid=ProcessNativeTrackingInput(ctx,base,object,17,0x10,17,23,23,17.25f,stimuli[types],sink,&receiver,error) && receiver.actions.size()==1;
          if(valid){const auto& action=receiver.actions.front();valid=action.kind==NativeTrackingAction::Kind::PublishStimulus && action.subject==17 && action.instigator==23 && action.amount==17.25f && action.classification==expected[mode][types];}
        }
      }
      receiver.actions.clear();
      valid=valid && ProcessNativeTrackingInput(ctx,base,object,17,0x10,19,23,23,1,{},nullptr,nullptr,error) &&
        ProcessNativeTrackingInput(ctx,base,object,17,0x10,17,29,23,1,{},nullptr,nullptr,error) &&
        ProcessNativeTrackingInput(ctx,base,object,17,0x12,17,23,23,1,{},nullptr,nullptr,error);
      valid=valid && !ProcessNativeTrackingInput(ctx,base,object,17,0x10,17,23,23,1,{},nullptr,nullptr,error);
      valid=valid && write(0,1) && ProcessNativeTrackingInput(ctx,base,object,17,0x11,17,23,23,1,{},nullptr,nullptr,error) && write(0,0);
      receiver.accept=false;valid=valid && !ProcessNativeTrackingInput(ctx,base,object,17,0x11,17,23,23,1,{},sink,&receiver,error) && !base[object+native_mode_at];
      receiver.accept=true;valid=valid && ProcessNativeTrackingInput(ctx,base,object,17,0x11,17,23,23,1,{},sink,&receiver,error) && base[object+native_mode_at] && receiver.actions.size()==1 && receiver.actions.front().kind==NativeTrackingAction::Kind::Retire;
      NativeTrackingProfile peer;valid=valid && ReadNativeTrackingProfile(ctx,base,faces[1]-8,peer,error) && peer==profiles[1];
      REXLOG_INFO("[em1] original tracking component: definition={:08X} own_properties={} inherited={} type={} stimulus={} collectibles={} success={}; native fields and original typed event semantics; capture receiver only",definition,PPC_LOAD_U32(definition+0x28),PPC_LOAD_U32(definition+0x30)!=0,profiles[0].type,profiles[0].stimulus,profiles[0].collectibles.size(),valid);
      for(size_t i=0;i<profiles[0].collectibles.size();++i)REXLOG_INFO("[em1] original tracking collectible: definition={:08X} index={} value='{}'",definition,i,profiles[0].collectibles[i]);
    }
    for(auto face:faces)if(face){ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)+4));}
    if(!valid || PPC_LOAD_U32(metadata+live_at)!=before){all_valid=false;if(error.empty())error="Original tracking source field, input/output or lifecycle validation failed";break;}++checked;
  }
  Free(ctx,base,scratch,4);if(!all_valid)return false;if(!checked){error="Original scene has no tracking definitions";return false;}error.clear();return true;
}
bool ProbeRegisteredSeedComponents(PPCContext& ctx,uint8_t* base,std::span<const uint32_t> definitions,std::string& error){
  Context restore(ctx);const auto metadata=installed_seed_metadata.load(std::memory_order_acquire);size_t checked=0;
  if(!metadata){error="Original seed class is not registered";return false;}
  for(auto definition:definitions){
    if(std::strcmp(reinterpret_cast<const char*>(base+PPC_LOAD_U32(definition)),"SeedPointComponent"))continue;
    const auto before=PPC_LOAD_U32(metadata+live_at);std::array<uint32_t,2> faces{};std::array<std::array<int32_t,2>,2> values{};bool valid=true;
    for(unsigned i=0;i<2 && valid;++i){
      ctx.r3.u64=PPC_LOAD_U32(0x833CD7A0);ctx.r4.u64=metadata+name_ref;ctx.r5.u64=0;__imp__sub_82510678(ctx,base);faces[i]=ctx.r3.u32;
      if(!faces[i]){valid=false;break;}
      ctx.r3.u64=faces[i];PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(faces[i])));
      const auto object=faces[i]-8;ctx.r3.u64=object;ctx.r4.u64=definition;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
      ctx.r3.u64=object;__imp__sub_831BEE80(ctx,base);valid=base[object+native_mode_at]==0;
      for(unsigned p=0;p<2 && valid;++p){
        ctx.r3.u64=object;ctx.r4.u64=installed_seed_schema.descriptors[p]+4;__imp__sub_831BEFD8(ctx,base);
        if(!ctx.r3.u32){valid=false;break;}values[i][p]=int32_t(PPC_LOAD_U32(ctx.r3.u32+4));
      }
    }
    valid=valid && values[0]==values[1] && PPC_LOAD_U32(metadata+live_at)==before+2;
    REXLOG_INFO("[em1] original seed component: definition={:08X} own_properties={} inherited={} sizes={},{} success={}; native Integer fields and source zero state; no actor AI-world integration",definition,PPC_LOAD_U32(definition+0x28),PPC_LOAD_U32(definition+0x30)!=0,values[0][0],values[0][1],valid);
    for(auto face:faces)if(face){ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)+4));}
    if(!valid || PPC_LOAD_U32(metadata+live_at)!=before){error="Original seed factory, field or lifetime validation failed";return false;}++checked;
  }
  if(!checked){error="Original scene has no seed component definitions";return false;}error.clear();return true;
}
bool InvokeNativeLogicInput(PPCContext& ctx,uint8_t* base,uint32_t object,NativeLogicInput input,bool value,
    NativeLogicSink sink,void* sink_context,std::string& error){
  if(!base || object<4096){error="Invalid logic component";return false;}
  if(unsigned(input)>unsigned(NativeLogicInput::RelayTrigger)){error="Unknown logic input";return false;}
  const auto metadata=PPC_LOAD_U32(object),branch=installed_branch_metadata.load(std::memory_order_acquire),relay=installed_relay_metadata.load(std::memory_order_acquire);
  const bool is_branch=branch && metadata==branch,is_relay=relay && metadata==relay;
  if((!is_branch && !is_relay) || !PPC_LOAD_U32(object+0x3C)){
    error="Logic input requires an initialized original logic component";return false;
  }
  if((is_branch && input!=NativeLogicInput::Test && input!=NativeLogicInput::SetBranchValue) ||
      (is_relay && (input==NativeLogicInput::Test || input==NativeLogicInput::SetBranchValue))){
    error="Input does not belong to this logic component";return false;
  }
  const bool emitting=input==NativeLogicInput::Test || input==NativeLogicInput::RelayTrigger;
  Context restore(ctx);const auto& schema=is_branch?installed_branch_schema:installed_relay_schema;
  const auto descriptor=schema.descriptors.front();
  ctx.r3.u64=object;ctx.r4.u64=descriptor+4;__imp__sub_831BEFD8(ctx,base);const auto box=ctx.r3.u32;
  if(!box){error="Native Boolean property could not be resolved";return false;}
  const bool current=base[box+4]!=0;
  if(emitting){
    if(is_relay && current){error.clear();return true;}
    if(!sink){error="Original logic event router is unavailable";return false;}
    const auto output=is_branch?(current?NativeLogicOutput::BranchTrue:NativeLogicOutput::BranchFalse):NativeLogicOutput::RelayTrigger;
    if(!sink(sink_context,object,output)){error="Original logic event router rejected the event";return false;}
  }else{
    const bool next=input==NativeLogicInput::RelayDisable || (input==NativeLogicInput::RelayToggle && !current) || (input==NativeLogicInput::SetBranchValue && value);
    const auto scratch=Allocate(ctx,base,4);if(!scratch){error="Native Boolean input allocation failed";return false;}
    base[scratch]=next;const auto face=object+8;ctx.r3.u64=face;ctx.r4.u64=descriptor+4;ctx.r5.u64=scratch;ctx.r6.u64=0;
    PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)+0xF8));const bool written=ctx.r3.u32!=0;Free(ctx,base,scratch,4);
    if(!written){error="Native Boolean property write failed";return false;}
  }
  error.clear();return true;
}
bool ProbeRegisteredLogicComponents(PPCContext& ctx,uint8_t* base,std::span<const uint32_t> definitions,std::string& error){
  Context restore(ctx);size_t branches=0,relays=0;
  struct Events {std::vector<std::pair<uint32_t,NativeLogicOutput>> values;bool accept=true;} events;
  const auto sink=+[](void* context,uint32_t object,NativeLogicOutput output){
    auto& events=*static_cast<Events*>(context);if(!events.accept)return false;
    events.values.emplace_back(object,output);return true;
  };
  for(auto definition:definitions){
    const std::string_view name(reinterpret_cast<const char*>(base+PPC_LOAD_U32(definition)));
    const bool branch=name=="LogicBranchComponent",relay=name=="LogicRelayComponent";
    if(!branch && !relay)continue;
    const auto metadata=(branch?installed_branch_metadata:installed_relay_metadata).load(std::memory_order_acquire);
    if(!metadata){error="Original logic class has not been registered";return false;}
    const auto descriptor=(branch?installed_branch_schema:installed_relay_schema).descriptors.front();
    const auto before=PPC_LOAD_U32(metadata+live_at);std::array<uint32_t,2> faces{},boxes{};bool valid=true;events.values.clear();events.accept=true;
    for(unsigned i=0;i<2 && valid;++i){
      ctx.r3.u64=PPC_LOAD_U32(0x833CD7A0);ctx.r4.u64=metadata+name_ref;ctx.r5.u64=0;__imp__sub_82510678(ctx,base);faces[i]=ctx.r3.u32;
      if(!faces[i]){valid=false;error="Registered logic factory returned no interface";break;}
      ctx.r3.u64=faces[i];PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(faces[i])));
      const auto object=faces[i]-8;ctx.r3.u64=object;ctx.r4.u64=definition;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
      ctx.r3.u64=object;__imp__sub_831BEE80(ctx,base);
      ctx.r3.u64=object;ctx.r4.u64=descriptor+4;__imp__sub_831BEFD8(ctx,base);boxes[i]=ctx.r3.u32;
      valid=boxes[i] && PPC_LOAD_U32(object+0x3C)==definition;
    }
    if(valid){
      const auto object=faces[0]-8;const bool authored=base[boxes[0]+4]!=0,peer=base[boxes[1]+4]!=0;
      valid=authored==peer && boxes[0]!=boxes[1] && PPC_LOAD_U32(metadata+live_at)==before+2;
      auto invoke=[&](NativeLogicInput input,bool value=false){return InvokeNativeLogicInput(ctx,base,object,input,value,sink,&events,error);};
      if(branch){
        valid=valid && invoke(NativeLogicInput::SetBranchValue,false) && invoke(NativeLogicInput::Test) &&
          invoke(NativeLogicInput::SetBranchValue,true) && invoke(NativeLogicInput::Test);
        valid=valid && events.values==std::vector<std::pair<uint32_t,NativeLogicOutput>>{{object,NativeLogicOutput::BranchFalse},{object,NativeLogicOutput::BranchTrue}};
        valid=valid && !invoke(NativeLogicInput::RelayDisable);
      }else{
        valid=valid && invoke(NativeLogicInput::RelayEnable) && invoke(NativeLogicInput::RelayTrigger) &&
          invoke(NativeLogicInput::RelayDisable) && invoke(NativeLogicInput::RelayTrigger) &&
          invoke(NativeLogicInput::RelayToggle) && invoke(NativeLogicInput::RelayTrigger) &&
          invoke(NativeLogicInput::RelayToggle) && invoke(NativeLogicInput::RelayTrigger);
        valid=valid && events.values==std::vector<std::pair<uint32_t,NativeLogicOutput>>{{object,NativeLogicOutput::RelayTrigger},{object,NativeLogicOutput::RelayTrigger}};
        valid=valid && InvokeNativeLogicInput(ctx,base,object,NativeLogicInput::RelayTrigger,false,nullptr,nullptr,error);
        valid=valid && !invoke(NativeLogicInput::Test) && invoke(NativeLogicInput::RelayEnable);
      }
      const auto emitting=branch?NativeLogicInput::Test:NativeLogicInput::RelayTrigger;
      events.accept=false;valid=valid && !invoke(emitting);events.accept=true;
      valid=valid && !InvokeNativeLogicInput(ctx,base,object,emitting,false,nullptr,nullptr,error);
      valid=valid && (base[boxes[1]+4]!=0)==peer && events.values.size()==2;
      REXLOG_INFO("[em1] original logic component: class='{}' definition={:08X} own_properties={} inherited={} authored={} success={}; real registered instances and original input/output semantics, router capture only",name,definition,PPC_LOAD_U32(definition+0x28),PPC_LOAD_U32(definition+0x30)!=0,authored,valid);
      if(branch)++branches;else ++relays;
    }
    for(auto face:faces)if(face){ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)+4));}
    valid=valid && PPC_LOAD_U32(metadata+live_at)==before;
    if(!valid){if(error.empty())error="Original logic factory, authored field, input/output or lifetime validation failed";return false;}
  }
  if(!branches || !relays){error="Original scene does not contain both logic component classes";return false;}
  error.clear();return true;
}
namespace {
std::string NavigationPathKey(std::string value){
  for(auto& ch:value){if(ch=='\\')ch='/';if(ch>='A' && ch<='Z')ch+=32;}
  if(value.starts_with("game:/"))value.erase(0,6);else if(value.starts_with("d:/"))value.erase(0,3);
  while(value.starts_with('/'))value.erase(0,1);return value;
}
bool ReadNativeText(uint8_t* base,uint32_t at,std::string& text){
  text.clear();if(!at)return true;
  for(unsigned i=0;i<1024;++i){char byte=0;SIZE_T read=0;
    if(at<4096 || uint64_t(at)+i>UINT32_MAX || !ReadProcessMemory(GetCurrentProcess(),base+at+i,&byte,1,&read) || read!=1)return false;
    if(!byte)return true;text+=byte;
  }return false;
}
bool ReadNavigationProfile(PPCContext& ctx,uint8_t* base,uint32_t object,bool from_definition,NativeNavigationProfile& output,std::string& error){
  if(!object || Metadata(base,object)!=installed_navigation_metadata.load(std::memory_order_acquire) || !PPC_LOAD_U32(object+0x3C)){
    error="Navigation component has no native definition";return false;
  }
  NativeNavigationProfile profile;const std::array<uint32_t,3> zero{};
  for(unsigned p=0;p<7;++p){const auto descriptor=installed_navigation_schema.descriptors[p];const uint8_t* value=nullptr;
    if(from_definition){
      ctx.r3.u64=PPC_LOAD_U32(object+0x3C);ctx.r4.u64=descriptor+4;__imp__sub_827B8450(ctx,base);const auto parameter=ctx.r3.u32;
      // During normal assembly the engine may assign a sparse definition
      // before inherited fields are linked. Match the declared native defaults;
      // the finalized instance is refreshed through its actual boxes below.
      const auto count=parameter?PPC_LOAD_U32(parameter+12):0,data=parameter?PPC_LOAD_U32(parameter+8):0;const auto type=parameter?base[parameter+4]:0;
      if(count && !data){error="Navigation property has no data buffer";return false;}
      const auto element=count?(type>0 && type<=7?data:PPC_LOAD_U32(data)):0;
      if(count && !element){error="Navigation property has no element buffer";return false;}
      value=count?base+element:reinterpret_cast<const uint8_t*>(zero.data());
    }else{
      ctx.r3.u64=object;ctx.r4.u64=descriptor+4;__imp__sub_831BEFD8(ctx,base);
      if(!ctx.r3.u32){error="Navigation component has no native property box";return false;}value=base+ctx.r3.u32+4;
    }
    auto word=[&](unsigned offset=0){uint32_t raw=0;std::memcpy(&raw,value+offset,4);return _byteswap_ulong(raw);};
    if(p==0){if(!ReadNativeText(base,word(),profile.filename)){error="Invalid native navigation filename";return false;}}
    if(p==1)profile.pitch=std::bit_cast<float>(word());
    if(p==2 || p==3)for(unsigned k=0;k<3;++k)(p==2?profile.maximum:profile.minimum)[k]=std::bit_cast<float>(word(k*4));
    if(p==4)profile.radius=int32_t(word());if(p==5)profile.height=int32_t(word());if(p==6)profile.movement=int32_t(word());
  }
  output=std::move(profile);error.clear();return true;
}
// Engine-thread synchronous ownership. No graph is published to actor jobs.
bool RefreshNavigationComponent(PPCContext& ctx,uint8_t* base,uint32_t object,bool from_definition,std::string& error){
  Context restore(ctx);std::lock_guard lock(navigation_mutex);
  const auto found=navigation_components.find(object);
  if(found==navigation_components.end()){error="Navigation component has no lifetime record";return false;}
  auto& state=*found->second;NativeNavigationProfile profile;
  if(!ReadNavigationProfile(ctx,base,object,from_definition,profile,error)){state.ready=false;state.error=error;return false;}
  std::shared_ptr<const NavigationDatabase> selected;
  if(!profile.filename.empty()){
    if(navigation_sources)for(const auto& database:*navigation_sources)if(NavigationPathKey(database.source_path)==NavigationPathKey(profile.filename)){
      if(selected){error="Navigation HPD has ambiguous source origins";state.ready=false;state.error=error;return false;}
      selected=std::shared_ptr<const NavigationDatabase>(navigation_sources,&database);
    }
    if(!selected){error="Navigation HPD absent from retained level: "+profile.filename;state.ready=false;state.error=error;return false;}
  }
  if(state.asset.source==selected){state.profile=std::move(profile);state.ready=true;state.error.clear();error.clear();return true;}
  NativeNavigationAsset candidate;
  if(selected && !BuildNativeNavigationAsset(ctx,base,std::move(selected),candidate,error)){state.ready=false;state.error=error;return false;}
  ReleaseNativeNavigationAsset(ctx,base,state.asset);
  state.asset.source=std::move(candidate.source);state.asset.graphs.swap(candidate.graphs);
  state.profile=std::move(profile);state.ready=true;state.error.clear();error.clear();return true;
}
}
bool RegisterNativeNavigationFacade(PPCContext& ctx,uint8_t* base,std::shared_ptr<const std::vector<NavigationDatabase>> source,std::string& error){
  if(!source){error="Native navigation registration requires retained level data";return false;}
  {std::lock_guard lock(navigation_mutex);navigation_sources=std::move(source);}
  return RegisterItemFacade(ctx,base,"AIPathDatabaseComponent",kNavigationNativeCreate,installed_navigation_metadata,installed_navigation_schema,FacadeKind::Navigation,error);
}
void CreateNativeNavigationComponent(PPCContext& ctx,uint8_t* base){
  CreateItemFacade(ctx,base,installed_navigation_metadata.load(std::memory_order_acquire));const auto face=ctx.r3.u32;
  if(face){
    try{std::lock_guard lock(navigation_mutex);navigation_components.emplace(face-8,std::make_unique<NavigationComponentState>());}
    catch(const std::bad_alloc&){{Context restore(ctx);ctx.r3.u64=face-8;ctx.r4.u64=1;DestroyAmmoPrototype(ctx,base);}ctx.r3.u64=0;return;}
  }
}
void DefineNativeNavigationComponent(PPCContext& ctx,uint8_t* base){
  const auto object=ctx.r3.u32;
  if(PPC_LOAD_U32(object+0x3C)!=ctx.r4.u32){Context restore(ctx);
    // Same boxed-value destruction as retail831BE930, retaining vector capacity
    // for the next definition. The stock82757CF0 setter only assigns a pointer.
    const auto data=PPC_LOAD_U32(object+0x24),count=PPC_LOAD_U32(object+0x28);
    for(uint32_t i=0;i<count;++i){const auto box=PPC_LOAD_U32(data+i*4);
      if(box){ctx.r3.u64=box;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(box)));}PPC_STORE_U32(data+i*4,0);
    }PPC_STORE_U32(object+0x28,0);
  }
  PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(0x82075CC0+0x14));
  Context restore(ctx);std::string error;
  if(!PPC_LOAD_U32(object+0x3C)){
    std::lock_guard lock(navigation_mutex);const auto found=navigation_components.find(object);
    if(found!=navigation_components.end()){auto& state=*found->second;ReleaseNativeNavigationAsset(ctx,base,state.asset);state.profile={};state.ready=false;state.error.clear();}
    return;
  }
  if(!RefreshNavigationComponent(ctx,base,object,true,error))REXLOG_ERROR("[em1] navigation component definition failed: {}",error);
}
void DestroyNativeNavigationComponent(PPCContext& ctx,uint8_t* base){
  {Context restore(ctx);std::unique_ptr<NavigationComponentState> state;
    {std::lock_guard lock(navigation_mutex);const auto at=navigation_components.find(ctx.r3.u32);if(at!=navigation_components.end()){state=std::move(at->second);navigation_components.erase(at);}}
    if(state)ReleaseNativeNavigationAsset(ctx,base,state->asset);
  }
  DestroyAmmoPrototype(ctx,base);
}
bool ReadNativeNavigationComponent(PPCContext& ctx,uint8_t* base,uint32_t object,NativeNavigationProfile& output,size_t& sectors,std::string& error){
  if(!RefreshNavigationComponent(ctx,base,object,false,error))return false;
  std::lock_guard lock(navigation_mutex);const auto& state=*navigation_components.at(object);output=state.profile;sectors=state.asset.graphs.size();return true;
}
bool IsNativeNavigationComponent(uint8_t* base,uint32_t object){
  const auto metadata=installed_navigation_metadata.load(std::memory_order_acquire);return metadata && object>=4096 && PPC_LOAD_U32(object)==metadata;
}
NativeNavigationComponentStatistics NativeNavigationComponents(){
  std::lock_guard lock(navigation_mutex);NativeNavigationComponentStatistics result;result.instances=navigation_components.size();
  for(const auto& [object,state]:navigation_components){result.graphs+=state->asset.graphs.size();if(!state->error.empty())++result.failed;}return result;
}
bool ProbeRegisteredNavigationLifecycle(PPCContext& ctx,uint8_t* base,std::span<const uint32_t> definitions,
    std::span<const NativeNavigationProfile> expected,std::string& error){
  if(definitions.size()!=expected.size() || definitions.size()<2){error="Navigation lifecycle requires original profile definitions";return false;}
  Context restore(ctx);const auto metadata=installed_navigation_metadata.load(std::memory_order_acquire);
  const auto before=NativeNavigationComponents();const auto live_before=metadata?PPC_LOAD_U32(metadata+live_at):0;
  if(!metadata){error="Navigation factory is not registered";return false;}
  std::array<uint32_t,2> faces{};bool valid=true;std::string detail;const auto scratch=Allocate(ctx,base,4);
  if(!scratch){error="Navigation lifecycle scratch allocation failed";return false;}
  auto define=[&](unsigned instance,unsigned index){const auto object=faces[instance]-8;
    ctx.r3.u64=object;ctx.r4.u64=definitions[index];PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
    ctx.r3.u64=object;__imp__sub_831BEE80(ctx,base);
    NativeNavigationProfile actual;size_t sectors=0;
    const bool success=ReadNativeNavigationComponent(ctx,base,object,actual,sectors,error) && actual==expected[index];
    if(!success && detail.empty())detail=error.empty()?"registered definition/profile mismatch":error;return success;
  };
  for(auto& face:faces){ctx.r3.u64=PPC_LOAD_U32(0x833CD7A0);ctx.r4.u64=metadata+name_ref;ctx.r5.u64=0;__imp__sub_82510678(ctx,base);face=ctx.r3.u32;
    if(!face){valid=false;detail="registered factory returned no interface";break;}
    ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)));
  }
  if(valid){valid=define(0,0) && define(1,1);}
  for(unsigned iteration=0;iteration<definitions.size()*2 && valid;++iteration){const auto index=(iteration+1)%definitions.size();
    valid=define(0,index);NativeNavigationProfile peer;size_t sectors=0;
    valid=ReadNativeNavigationComponent(ctx,base,faces[1]-8,peer,sectors,error) && peer==expected[1] && valid;
    if(valid && index==1){
      PPC_STORE_U32(scratch,std::bit_cast<uint32_t>(9.25f));valid=WriteNativeComponentFloat(ctx,base,faces[0]-8,installed_navigation_schema.descriptors[1]+4,scratch);
      NativeNavigationProfile changed;size_t changed_sectors=0;
      valid=ReadNativeNavigationComponent(ctx,base,faces[0]-8,changed,changed_sectors,error) && changed.pitch==9.25f && valid;
      valid=ReadNativeNavigationComponent(ctx,base,faces[1]-8,peer,sectors,error) && peer==expected[1] && valid;
    }
    if(!valid && detail.empty())detail=error.empty()?"registered property override/peer isolation failed":error;
  }
  if(valid){
    NativeNavigationProfile peer;size_t peer_sectors=0;
    valid=ReadNativeNavigationComponent(ctx,base,faces[1]-8,peer,peer_sectors,error) && peer==expected[1];
    ctx.r3.u64=faces[0]-8;ctx.r4.u64=0;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
    const auto cleared=NativeNavigationComponents();valid=valid && cleared.graphs==before.graphs+peer_sectors && cleared.failed==before.failed;
    if(!valid && detail.empty())detail="clearing a definition did not retire only its owned graphs";
  }
  for(auto face:faces)if(face){ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)+4));}
  Free(ctx,base,scratch,4);
  valid=NativeNavigationComponents()==before && PPC_LOAD_U32(metadata+live_at)==live_before && valid;
  if(!valid){error=detail.empty()?"registered navigation lifetime counts did not restore":detail;return false;}
  error.clear();return true;
}
bool ProbeNativeNavigationFactory(PPCContext& ctx,uint8_t* base,std::string& error,uint32_t definition,
    std::shared_ptr<const std::vector<NavigationDatabase>> navigation,size_t& bound_sectors,NativeNavigationProfile& output){
  if(creation_metadata || !definition || !navigation){error="Invalid private navigation factory context";return false;}
  Context restore(ctx);NativeFloatSchema schema;
  if(!BuildNativePropertySchema(ctx,base,NativeNavigationDeclarations(),schema,error))return false;
  const auto before=PPC_LOAD_U32(0x833CDD8C),metadata=Allocate(ctx,base,metadata_size);
  uint32_t factory=0;bool factory_built=false,valid=metadata!=0;size_t sectors=0;std::string detail;
  NativeNavigationProfile profile;
  struct Instance {uint32_t face=0;NativeNavigationAsset asset;};std::array<Instance,2> instances;
  auto string=[&](uint32_t at,std::string& text){
    text.clear();if(!at)return true; // Native interned string's empty handle.
    for(unsigned i=0;i<1024;++i){char byte=0;SIZE_T read=0;
      if(at<4096 || uint64_t(at)+i>UINT32_MAX || !ReadProcessMemory(GetCurrentProcess(),base+at+i,&byte,1,&read) || read!=1)return false;
      if(!byte)return true;text+=byte;
    }return false;
  };
  auto key=[](std::string value){
    for(auto& ch:value){if(ch=='\\')ch='/';if(ch>='A' && ch<='Z')ch+=32;}
    // The native blueprint reader qualifies filepath properties at the guest
    // disc root. These are virtual aliases, not host filesystem drive paths.
    if(value.starts_with("game:/"))value.erase(0,6);else if(value.starts_with("d:/"))value.erase(0,3);
    while(value.starts_with('/'))value.erase(0,1);return value;
  };
  if(metadata){
    InitializeAmmoMetadata(ctx,base,metadata,schema,"AIPathDatabaseComponent");PPC_STORE_U32(metadata+shared_class_at,1);
    factory=Allocate(ctx,base,0x14);valid=factory!=0;
    if(valid){
      ctx.r3.u64=factory;__imp__sub_825052B0(ctx,base);factory_built=true;
      ctx.r3.u64=PPC_LOAD_U32(factory+8);ctx.r4.u64=metadata+name_ref;ctx.r5.u64=kAmmoNativeCreate;__imp__sub_8256C338(ctx,base);
      struct Scope {Scope(uint32_t value){creation_metadata=value;}~Scope(){creation_metadata=0;}} scope(metadata);
      for(unsigned instance=0;instance<instances.size() && valid;++instance){
        auto& record=instances[instance];
        ctx.r3.u64=factory;ctx.r4.u64=metadata+name_ref;ctx.r5.u64=0;__imp__sub_82510678(ctx,base);record.face=ctx.r3.u32;
        if(!record.face){valid=false;detail="creation callback returned no interface";break;}
        const auto object=record.face-8;ctx.r3.u64=record.face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(record.face)));
        ctx.r3.u64=object;ctx.r4.u64=definition;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
        ctx.r3.u64=object;__imp__sub_831BEE80(ctx,base);
        valid=Metadata(base,object)==metadata && PPC_LOAD_U32(object+0x3C)==definition && PPC_LOAD_U32(metadata+count_at)==7;
        std::string filename;
        const auto declarations=NativeNavigationDeclarations();
        for(unsigned p=0;p<declarations.size() && valid;++p){const auto descriptor=schema.descriptors[p];
          ctx.r3.u64=definition;ctx.r4.u64=descriptor+4;__imp__sub_827B8450(ctx,base);const auto source=ctx.r3.u32;
          ctx.r3.u64=object;ctx.r4.u64=descriptor+4;__imp__sub_831BEFD8(ctx,base);const auto box=ctx.r3.u32;
          if(!source || !box){valid=false;detail="missing authored property "+declarations[p].name;break;}
          const auto data=PPC_LOAD_U32(source+8),count=PPC_LOAD_U32(source+12);
          if(count && !data){valid=false;detail="missing authored payload "+declarations[p].name;break;}
          // Retail827B82B8: type codes1..7 store scalar words directly;
          // aggregates (including String and Point3) store element pointers.
          const auto type=base[source+4];
          const auto value=count?(type>0 && type<=7?data:PPC_LOAD_U32(data)):0;
          if(count && !value){valid=false;detail="missing authored element "+declarations[p].name;break;}
          if(declarations[p].shape==NativePropertyShape::String){std::string expected,actual;
            const auto expected_ptr=count?PPC_LOAD_U32(value):0,actual_ptr=PPC_LOAD_U32(box+4);
            const bool source_read=!count || string(expected_ptr,expected),instance_read=string(actual_ptr,actual);
            valid=source_read && instance_read && expected==actual;filename=actual;profile.filename=actual;
            if(!valid)detail="String comparison: source_read="+std::to_string(source_read)+" instance_read="+std::to_string(instance_read)+" source_ptr="+std::to_string(expected_ptr)+" instance_ptr="+std::to_string(actual_ptr)+" count="+std::to_string(count)+" source_type="+std::to_string(PPC_LOAD_U32(source+4));
          }else{
            const unsigned words=declarations[p].shape==NativePropertyShape::Point3?3:1;
            const std::array<uint32_t,3> zero{};
            valid=std::memcmp(count?base+value:reinterpret_cast<const uint8_t*>(zero.data()),base+box+4,words*4)==0;
            if(p==1)profile.pitch=std::bit_cast<float>(PPC_LOAD_U32(box+4));
            if(p==2 || p==3)for(unsigned k=0;k<3;++k)(p==2?profile.maximum:profile.minimum)[k]=std::bit_cast<float>(PPC_LOAD_U32(box+4+k*4));
            if(p==4)profile.radius=int32_t(PPC_LOAD_U32(box+4));
            if(p==5)profile.height=int32_t(PPC_LOAD_U32(box+4));
            if(p==6)profile.movement=int32_t(PPC_LOAD_U32(box+4));
          }
          if(!valid && detail.empty())detail="authored value mismatch "+declarations[p].name;
        }
        if(valid && !filename.empty()){
          std::shared_ptr<const NavigationDatabase> selected;
          for(const auto& database:*navigation)if(key(database.source_path)==key(filename)){
            if(selected){valid=false;detail="ambiguous authored HPD origin";break;}
            selected=std::shared_ptr<const NavigationDatabase>(navigation,&database);
          }
          if(valid && !selected){valid=false;detail="authored HPD absent from retained level: "+filename;}
          if(valid){valid=BuildNativeNavigationAsset(ctx,base,std::move(selected),record.asset,error);if(!valid)detail=error;}
          if(valid){
            sectors+=record.asset.graphs.size();const auto& sector=record.asset.source->sectors.front();
            if(!sector.nodes.empty()){uint32_t node=0;NativeNavigationPath path;
              valid=FindNativeNavigationNode(ctx,base,record.asset.graphs.front(),sector.nodes.front().position,node,error) &&
                FindNativeNavigationPath(ctx,base,record.asset.graphs.front(),node,node,path,error) && path.nodes==std::vector<uint32_t>{node};
              if(!valid)detail=error.empty()?"owned HPD graph query failed":error;
            }
          }
        }
      }
      valid=valid && instances[0].face && instances[1].face && instances[0].face!=instances[1].face && PPC_LOAD_U32(metadata+live_at)==2;
      // Mutate one instance's pitch through its actual box. The second box and
      // retained source definition must keep their original value.
      if(valid){std::array<uint32_t,2> boxes{};const auto descriptor=schema.descriptors[1];
        for(unsigned i=0;i<2;++i){ctx.r3.u64=instances[i].face-8;ctx.r4.u64=descriptor+4;__imp__sub_831BEFD8(ctx,base);boxes[i]=ctx.r3.u32;}
        const auto original=boxes[1]?PPC_LOAD_U32(boxes[1]+4):0;
        ctx.r3.u64=definition;ctx.r4.u64=descriptor+4;__imp__sub_827B8450(ctx,base);const auto source=ctx.r3.u32;
        valid=boxes[0] && boxes[1] && boxes[0]!=boxes[1] && source;
        if(valid){const auto data=PPC_LOAD_U32(source+8),count=PPC_LOAD_U32(source+12);
          PPC_STORE_U32(boxes[0]+4,std::bit_cast<uint32_t>(9.25f));valid=PPC_LOAD_U32(boxes[1]+4)==original && (count?PPC_LOAD_U32(data):0)==original;}
        if(!valid)detail="instance pitch/source isolation failed";
      }
      for(auto& record:instances){
        ReleaseNativeNavigationAsset(ctx,base,record.asset);
        if(record.face){ctx.r3.u64=record.face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(record.face)+4));record.face=0;}
      }
      valid=valid && !PPC_LOAD_U32(metadata+live_at);
    }
    if(factory_built){ctx.r3.u64=factory;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(factory)));factory=0;}
    if(PPC_LOAD_U32(metadata+name_ref)){ctx.r3.u64=metadata+name_ref;__imp__sub_822F1C10(ctx,base);}
  }
  if(factory)Free(ctx,base,factory,0x14);Free(ctx,base,metadata,metadata_size);ReleaseNativeFloatSchema(ctx,base,schema);
  valid=valid && PPC_LOAD_U32(0x833CDD8C)==before && !creation_metadata;
  if(!valid){error=detail.empty()?"Native navigation component construction/ownership validation failed":detail;return false;}
  bound_sectors=sectors;output=std::move(profile);error.clear();return true;
}
namespace {
bool NativeRule(PPCContext& ctx,uint8_t* base,uint32_t object,std::string_view& property){
  const auto metadata=Metadata(base,object);if(!PPC_LOAD_U32(object+native_mode_at) || !ctx.r4.u32)return false;
  const auto name=PPC_LOAD_U32(ctx.r4.u32);
  if(PPC_LOAD_U32(metadata+count_at)==17){const auto declarations=NativeInventoryDeclarations();
    for(unsigned i=0;i<declarations.size();++i){const auto descriptor=PPC_LOAD_U32(PPC_LOAD_U32(metadata+array_at)+i*4);
      if(PPC_LOAD_U32(descriptor+4)==name && IsNativeInventoryProperty(declarations[i].name)){property=declarations[i].name;return true;}}
    return false;
  }
  for(unsigned i=4;i<14;++i){const auto descriptor=PPC_LOAD_U32(PPC_LOAD_U32(metadata+array_at)+i*4);
    if(PPC_LOAD_U32(descriptor+4)==name){property=LegacyAmmoProperties()[i].name;return true;}}
  return false;
}
}
void AmmoNativeRead(PPCContext& ctx,uint8_t* base){
  std::string_view property;
  if(!NativeRule(ctx,base,ctx.r3.u32-8,property)){__imp__sub_82759898(ctx,base);return;}
  const auto output=ctx.r5.u32,index=ctx.r6.u32;bool success=false;
  {Context restore(ctx);float value=0;std::string error;
    if(!index && output && (IsNativeInventoryProperty(property)?ReadNativeInventoryProperty(base,property,value,error):ReadLegacyAmmoRule(base,property,value,error))){
      PPC_STORE_U32(output,std::bit_cast<uint32_t>(value));success=true;
    }
  }
  ctx.r3.u64=success;
}
void AmmoNativeWrite(PPCContext& ctx,uint8_t* base){
  std::string_view property;
  if(!NativeRule(ctx,base,ctx.r3.u32-8,property)){__imp__sub_827584E0(ctx,base);return;}
  const auto input=ctx.r5.u32,index=ctx.r6.u32;bool success=false;
  {Context restore(ctx);std::string error;
    if(!index && input){const auto value=std::bit_cast<float>(PPC_LOAD_U32(input));
      success=IsNativeInventoryProperty(property)?WriteNativeInventoryProperty(ctx,base,property,value,error):WriteLegacyAmmoRule(base,property,value,error);}
  }
  ctx.r3.u64=success;
}
void DestroyAmmoPrototype(PPCContext& ctx,uint8_t* base){
  const auto object=ctx.r3.u32,flags=ctx.r4.u32,metadata=Metadata(base,object);
  {Context restore(ctx);ctx.r3.u64=object;ctx.r4.u64=0;__imp__sub_82757C50(ctx,base);
    if(PPC_LOAD_U32(metadata+shared_class_at))ChangeSharedLiveCount(base,metadata,-1);else PPC_STORE_U32(metadata+live_at,0);
    if(flags&1)Free(ctx,base,object,object_size);}
  ctx.r3.u64=object;
}
void AmmoPrototypeType(PPCContext& ctx,uint8_t* base){ctx.r3.u64=Metadata(base,ctx.r3.u32)+type_at;}
void AmmoPrototypeNode(PPCContext& ctx,uint8_t* base){ctx.r3.u64=Metadata(base,ctx.r3.u32)+node_at;}
void AmmoPrototypeCount(PPCContext& ctx,uint8_t* base){ctx.r3.u64=PPC_LOAD_U32(Metadata(base,ctx.r3.u32)+count_at);}
void AmmoPrototypeDescriptor(PPCContext& ctx,uint8_t* base){const auto metadata=Metadata(base,ctx.r3.u32),index=ctx.r4.u32;
  ctx.r3.u64=index<PPC_LOAD_U32(metadata+count_at)?PPC_LOAD_U32(PPC_LOAD_U32(metadata+array_at)+index*4):0;}
bool ReadNativeComponentFloat(PPCContext& ctx,uint8_t* base,uint32_t object,uint32_t name_reference,uint32_t value,uint32_t index){
  if(!base || object<4096 || name_reference<4096 || value<4096 || !PPC_LOAD_U32(object+0x3C))return false;
  Context restore(ctx);const auto face=object+8;ctx.r3.u64=face;ctx.r4.u64=name_reference;ctx.r5.u64=value;ctx.r6.u64=index;
  PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)+0xD4));return ctx.r3.u32!=0;
}
bool WriteNativeComponentFloat(PPCContext& ctx,uint8_t* base,uint32_t object,uint32_t name_reference,uint32_t value,uint32_t index){
  if(!base || object<4096 || name_reference<4096 || value<4096 || !PPC_LOAD_U32(object+0x3C))return false;
  Context restore(ctx);const auto face=object+8;ctx.r3.u64=face;ctx.r4.u64=name_reference;ctx.r5.u64=value;ctx.r6.u64=index;
  PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(face)+0xFC));return ctx.r3.u32!=0;
}
void AmmoPrototypeNames(PPCContext& ctx,uint8_t* base){
  const auto metadata=Metadata(base,ctx.r3.u32),output=ctx.r4.u32;Context restore(ctx);
  for(uint32_t i=0;i<PPC_LOAD_U32(metadata+count_at);++i){const auto descriptor=PPC_LOAD_U32(PPC_LOAD_U32(metadata+array_at)+i*4),name=PPC_LOAD_U32(descriptor+4);
    const auto count=PPC_LOAD_U32(output+8),capacity=PPC_LOAD_U32(output+4);
    if(count==capacity){ctx.r3.u64=output;ctx.r4.u64=capacity?capacity*2:1;__imp__sub_82510578(ctx,base);}
    if(PPC_LOAD_U32(output+4)<=count || !PPC_LOAD_U32(output))throw std::runtime_error("Native property-name vector allocation failed");
    const auto slot=PPC_LOAD_U32(output)+count*4;PPC_STORE_U32(output+8,count+1);
    if(PPC_LOAD_U32(slot)!=name){ctx.r3.u64=descriptor+4;__imp__sub_82504410(ctx,base);ctx.r3.u64=slot;__imp__sub_822F1C10(ctx,base);PPC_STORE_U32(slot,name);}
  }
}
bool ProbeNativeAmmoFactory(PPCContext& ctx,uint8_t* base,std::string& error,uint32_t original_definition){
  if(creation_metadata){error="Native ammo factory creation scope is already active";return false;}
  Context restore(ctx);NativeFloatSchema schema;if(!BuildNativeAmmoSchema(ctx,base,schema,error))return false;
  const auto before=PPC_LOAD_U32(0x833CDD8C),metadata=Allocate(ctx,base,metadata_size);
  uint32_t factory=0,scratch=0,definition=original_definition;std::array<uint32_t,2> faces{};bool factory_built=false,valid=metadata!=0;
  if(metadata){InitializeAmmoMetadata(ctx,base,metadata,schema);PPC_STORE_U32(metadata+shared_class_at,1);
    valid=valid && PPC_LOAD_U32(metadata+name_ref)>=4096;
    if(valid){factory=Allocate(ctx,base,0x14);scratch=Allocate(ctx,base,16);if(!definition)definition=Allocate(ctx,base,0x38);valid=factory && scratch && definition;}
    if(valid){if(!original_definition){ctx.r3.u64=definition;__imp__sub_827B9570(ctx,base);}
      ctx.r3.u64=factory;__imp__sub_825052B0(ctx,base);factory_built=true;
      ctx.r3.u64=PPC_LOAD_U32(factory+8);ctx.r4.u64=metadata+name_ref;ctx.r5.u64=kAmmoNativeCreate;__imp__sub_8256C338(ctx,base);
      struct CreationScope {explicit CreationScope(uint32_t m){creation_metadata=m;}~CreationScope(){creation_metadata=0;}} scope(metadata);
      for(unsigned i=0;i<faces.size();++i){
        ctx.r3.u64=factory;ctx.r4.u64=metadata+name_ref;ctx.r5.u64=0;__imp__sub_82510678(ctx,base);faces[i]=ctx.r3.u32;
        if(!faces[i]){valid=false;break;}
        const auto object=faces[i]-8,table=PPC_LOAD_U32(faces[i]);
        ctx.r3.u64=faces[i];PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table));
        ctx.r3.u64=object;ctx.r4.u64=definition;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
        ctx.r3.u64=object;__imp__sub_831BEE80(ctx,base);
        for(unsigned p=0;p<14;++p){uint32_t expected=std::bit_cast<uint32_t>(LegacyAmmoProperties()[p].default_value);
          if(original_definition){ctx.r3.u64=definition;ctx.r4.u64=schema.descriptors[p]+4;ctx.r5.u64=scratch+4;ctx.r6.u64=0;__imp__sub_827B8628(ctx,base);
            valid=ctx.r3.u32 && valid;expected=PPC_LOAD_U32(scratch+4);}
          valid=ReadNativeComponentFloat(ctx,base,object,schema.descriptors[p]+4,scratch) && valid;
          valid=valid && PPC_LOAD_U32(scratch)==expected;
        }
        PPC_STORE_U32(scratch,std::bit_cast<uint32_t>(float(7+i*2)));
        valid=WriteNativeComponentFloat(ctx,base,object,schema.descriptors[0]+4,scratch) && valid;
      }
      valid=valid && faces[0] && faces[1] && faces[0]!=faces[1] && PPC_LOAD_U32(metadata+live_at)==2;
      if(valid){
        for(unsigned i=0;i<2;++i){const auto object=faces[i]-8;
          valid=Metadata(base,object)==metadata && PPC_LOAD_U32(object)==metadata && PPC_LOAD_U32(object+4)==1 && valid;
          valid=ReadNativeComponentFloat(ctx,base,object,schema.descriptors[0]+4,scratch) && valid;
          valid=valid && PPC_LOAD_U32(scratch)==std::bit_cast<uint32_t>(float(7+i*2));
        }
        PPC_STORE_U32(scratch,std::bit_cast<uint32_t>(42.5f));
        valid=WriteNativeComponentFloat(ctx,base,faces[1]-8,schema.descriptors[4]+4,scratch) && valid;
        PPC_STORE_U32(faces[0]-8+native_mode_at,1);
        float native_rate=0;valid=ReadLegacyAmmoRule(base,"PaintRechargeRate",native_rate,error) && valid;
        valid=ReadNativeComponentFloat(ctx,base,faces[0]-8,schema.descriptors[4]+4,scratch) && valid;
        valid=valid && PPC_LOAD_U32(scratch)==std::bit_cast<uint32_t>(native_rate);
        valid=ReadNativeComponentFloat(ctx,base,faces[1]-8,schema.descriptors[4]+4,scratch) && valid;
        valid=valid && PPC_LOAD_U32(scratch)==std::bit_cast<uint32_t>(42.5f);
      }
    }
    for(auto& face:faces)if(face){const auto table=PPC_LOAD_U32(face);ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+4));face=0;}
    valid=valid && !PPC_LOAD_U32(metadata+live_at);
    if(factory_built){ctx.r3.u64=factory;ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(factory)));factory=0;}
    if(PPC_LOAD_U32(metadata+name_ref)){ctx.r3.u64=metadata+name_ref;__imp__sub_822F1C10(ctx,base);}
  }
  if(factory)Free(ctx,base,factory,0x14);
  if(!original_definition)Free(ctx,base,definition,0x38);
  Free(ctx,base,scratch,16);Free(ctx,base,metadata,metadata_size);ReleaseNativeFloatSchema(ctx,base,schema);
  valid=valid && PPC_LOAD_U32(0x833CDD8C)==before && !creation_metadata;
  if(!valid){if(error.empty())error="Native shared ammo factory creation/property/lifetime validation failed";return false;}
  error.clear();return true;
}
bool ProbeNativeInventoryFactory(PPCContext& ctx,uint8_t* base,std::string& error,uint32_t original_definition){
  if(creation_metadata){error="Inventory factory cannot run inside a private creation override";return false;}
  if(!RegisterNativeInventoryFacade(ctx,base,error))return false;
  Context restore(ctx);const auto metadata=installed_inventory_metadata.load(std::memory_order_acquire);
  const auto& schema=installed_inventory_schema;const auto declarations=NativeInventoryDeclarations();
  const auto before=PPC_LOAD_U32(0x833CDD8C),live_before=PPC_LOAD_U32(metadata+live_at);
  uint32_t scratch=Allocate(ctx,base,32),definition=original_definition;std::array<uint32_t,2> faces{};
  if(!definition)definition=Allocate(ctx,base,0x38);
  bool valid=scratch && definition;std::string detail;
  if(valid){std::memset(base+scratch,0,32);if(!original_definition){ctx.r3.u64=definition;__imp__sub_827B9570(ctx,base);}
    for(unsigned instance=0;instance<2;++instance){
      ctx.r3.u64=PPC_LOAD_U32(0x833CD7A0);ctx.r4.u64=metadata+name_ref;ctx.r5.u64=0;__imp__sub_82510678(ctx,base);faces[instance]=ctx.r3.u32;
      if(!faces[instance]){valid=false;detail="factory returned no interface";break;}
      const auto object=faces[instance]-8,table=PPC_LOAD_U32(faces[instance]);
      ctx.r3.u64=faces[instance];PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table));
      ctx.r3.u64=object;ctx.r4.u64=definition;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
      ctx.r3.u64=object;__imp__sub_831BEE80(ctx,base);
      valid=valid && PPC_LOAD_U32(metadata+count_at)==17 && Metadata(base,object)==metadata && PPC_LOAD_U32(object+4)==1;
      for(unsigned p=0;p<declarations.size();++p){const auto descriptor=schema.descriptors[p];
        if(declarations[p].shape==NativePropertyShape::Float){uint32_t expected=0;
          if(original_definition){ctx.r3.u64=definition;ctx.r4.u64=descriptor+4;ctx.r5.u64=scratch+4;ctx.r6.u64=0;__imp__sub_827B8628(ctx,base);
            valid=ctx.r3.u32 && valid;expected=PPC_LOAD_U32(scratch+4);}
          const bool read=ReadNativeComponentFloat(ctx,base,object,descriptor+4,scratch);
          if((!read || PPC_LOAD_U32(scratch)!=expected) && detail.empty())detail="scalar "+declarations[p].name;
          valid=read && PPC_LOAD_U32(scratch)==expected && valid;
        }else{
          uint32_t source=0,expected_count=0;
          if(original_definition){ctx.r3.u64=definition;ctx.r4.u64=descriptor+4;__imp__sub_827B8450(ctx,base);source=ctx.r3.u32;
            valid=source && valid;if(source)expected_count=PPC_LOAD_U32(source+12);}
          ctx.r3.u64=object;ctx.r4.u64=descriptor+4;__imp__sub_831BEFD8(ctx,base);const auto box=ctx.r3.u32;
          if(!box){valid=false;if(detail.empty())detail="array box "+declarations[p].name;continue;}
          const auto vtable=PPC_LOAD_U32(box);ctx.r3.u64=box;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(vtable+8));
          if(ctx.r3.u32!=expected_count && detail.empty())detail="array count "+declarations[p].name;
          valid=ctx.r3.u32==expected_count && valid;
          const auto data=PPC_LOAD_U32(box+4),source_data=source?PPC_LOAD_U32(source+8):0;
          for(uint32_t index=0;index<expected_count;++index){
            const auto value=PPC_LOAD_U32(data+index*4),expected=PPC_LOAD_U32(source_data+index*4);
            const bool same=declarations[p].shape==NativePropertyShape::StringArray?
              value>=4096 && expected>=4096 && std::strcmp(reinterpret_cast<const char*>(base+value),reinterpret_cast<const char*>(base+expected))==0:value==expected;
            if(!same && detail.empty())detail="array element "+declarations[p].name;
            valid=same && valid;
          }
        }
      }
      PPC_STORE_U32(scratch,std::bit_cast<uint32_t>(float(7+instance*2)));
      valid=WriteNativeComponentFloat(ctx,base,object,schema.descriptors[0]+4,scratch) && valid;
    }
    valid=faces[0] && faces[1] && faces[0]!=faces[1] && PPC_LOAD_U32(metadata+live_at)==live_before+2 && valid;
    if(faces[0] && faces[1]){
      for(unsigned instance=0;instance<2;++instance){
        valid=ReadNativeComponentFloat(ctx,base,faces[instance]-8,schema.descriptors[0]+4,scratch) && valid;
        valid=PPC_LOAD_U32(scratch)==std::bit_cast<uint32_t>(float(7+instance*2)) && valid;
      }
      // Only this private instance forwards to the real player manager. An
      // admitted world's player ownership/lifecycle must enable this in play.
      PPC_STORE_U32(faces[0]-8+native_mode_at,1);
      for(unsigned p=0;p<declarations.size();++p)if(IsNativeInventoryProperty(declarations[p].name)){
        float expected=0;valid=ReadNativeInventoryProperty(base,declarations[p].name,expected,error) && valid;
        valid=ReadNativeComponentFloat(ctx,base,faces[0]-8,schema.descriptors[p]+4,scratch) && valid;
        valid=PPC_LOAD_U32(scratch)==std::bit_cast<uint32_t>(expected) && valid;
      }
      PPC_STORE_U32(faces[0]-8+native_mode_at,0);
      valid=ReadNativeComponentFloat(ctx,base,faces[0]-8,schema.descriptors[0]+4,scratch) && valid;
      valid=PPC_LOAD_U32(scratch)==std::bit_cast<uint32_t>(7.f) && valid;
      valid=ReadNativeComponentFloat(ctx,base,faces[1]-8,schema.descriptors[0]+4,scratch) && valid;
      valid=PPC_LOAD_U32(scratch)==std::bit_cast<uint32_t>(9.f) && valid;
      // Arrays must be owned per instance, including retained strings. Resize
      // and override one copy; the other copy and original blueprint stay put.
      for(const unsigned p:{12u,14u}){std::array<uint32_t,2> boxes{};uint32_t second_count=0,second_value=0;
        for(unsigned i=0;i<2;++i){ctx.r3.u64=faces[i]-8;ctx.r4.u64=schema.descriptors[p]+4;__imp__sub_831BEFD8(ctx,base);boxes[i]=ctx.r3.u32;}
        if(!boxes[0] || !boxes[1] || boxes[0]==boxes[1]){valid=false;continue;}
        ctx.r3.u64=boxes[1];PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(boxes[1])+8));second_count=ctx.r3.u32;
        if(second_count)second_value=PPC_LOAD_U32(PPC_LOAD_U32(boxes[1]+4));
        ctx.r3.u64=boxes[0];ctx.r4.u64=1;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(boxes[0])+12));
        const auto wanted=p==12?PPC_LOAD_U32(schema.descriptors[0]+4):std::bit_cast<uint32_t>(42.5f);
        PPC_STORE_U32(scratch,wanted);ctx.r3.u64=faces[0]-8;ctx.r4.u64=boxes[0];ctx.r5.u64=scratch;ctx.r6.u64=0;
        PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(schema.descriptors[p]+20));
        valid=PPC_LOAD_U32(PPC_LOAD_U32(boxes[0]+4))==wanted && valid;
        ctx.r3.u64=boxes[1];PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(boxes[1])+8));valid=ctx.r3.u32==second_count && valid;
        if(second_count)valid=PPC_LOAD_U32(PPC_LOAD_U32(boxes[1]+4))==second_value && valid;
        if(original_definition){ctx.r3.u64=definition;ctx.r4.u64=schema.descriptors[p]+4;__imp__sub_827B8450(ctx,base);const auto property=ctx.r3.u32;
          valid=property && PPC_LOAD_U32(property+12)==second_count && valid;
          if(property && second_count)valid=PPC_LOAD_U32(PPC_LOAD_U32(property+8))==second_value && valid;
        }
      }
      std::memset(base+scratch,0,32);ctx.r3.u64=faces[0]-8;ctx.r4.u64=scratch;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x64));
      const auto names=PPC_LOAD_U32(scratch),count=PPC_LOAD_U32(scratch+8);valid=count==17 && valid;
      for(unsigned p=0;p<count && p<17;++p)valid=PPC_LOAD_U32(names+p*4)==PPC_LOAD_U32(schema.descriptors[p]+4) && valid;
      PPC_STORE_U32(scratch+8,0);ctx.r3.u64=scratch;ctx.r4.u64=0;__imp__sub_82510578(ctx,base);
    }
  }
  for(auto& face:faces)if(face){const auto table=PPC_LOAD_U32(face);ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+4));face=0;}
  if(!original_definition)Free(ctx,base,definition,0x38);Free(ctx,base,scratch,32);
  valid=PPC_LOAD_U32(metadata+live_at)==live_before && PPC_LOAD_U32(0x833CDD8C)==before && valid;
  if(!valid){error="Native Inventory factory/schema/source/lifetime validation failed: "+detail;return false;}
  error.clear();return true;
}
bool ProbeNativeAmmoComponent(PPCContext& ctx,uint8_t* base,std::string& error,uint32_t original_definition){
  Context restore(ctx);NativeFloatSchema schema;
  if(!BuildNativeAmmoSchema(ctx,base,schema,error))return false;
  const auto metadata=Allocate(ctx,base,metadata_size);uint32_t object=0,scratch=0,definition=original_definition;bool valid=metadata!=0,constructed=false;std::string detail;
  const auto before=PPC_LOAD_U32(0x833CDD8C);
  if(metadata){
    InitializeAmmoMetadata(ctx,base,metadata,schema);
    if(ctx.r3.u32>=4096){PPC_STORE_U32(metadata+name_ref,ctx.r3.u32);object=Allocate(ctx,base,object_size);scratch=Allocate(ctx,base,16);if(!definition)definition=Allocate(ctx,base,0x38);}
    valid=valid && object && scratch && definition;
    if(object && scratch && definition){std::memset(base+object,0,object_size);std::memset(base+scratch,0,16);
      // Empty native blueprint is a default-value fixture only. Actual GSA
      // definitions and master overrides must be supplied for level entities.
      if(!original_definition){ctx.r3.u64=definition;__imp__sub_827B9570(ctx,base);}
      ctx.r3.u64=object;ctx.r4.u64=metadata+name_ref;ctx.r5.u64=0;ctx.r6.u64=14;__imp__sub_82792878(ctx,base);constructed=true;
      PPC_STORE_U32(object+8,metadata+face_table_at);
      PPC_STORE_U32(object,metadata);PPC_STORE_U32(object+0x40,metadata);PPC_STORE_U32(metadata+live_at,object);
      ctx.r3.u64=object;ctx.r4.u64=definition;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x14));
      valid=valid && PPC_LOAD_U32(0x833CDD8C)==before+1 && !PPC_LOAD_U32(object+0x30) && PPC_LOAD_U32(object+0x3C)==definition;
      ctx.r3.u64=object;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+8));valid=valid && ctx.r3.u32==metadata+type_at;
      ctx.r3.u64=object;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+12));valid=valid && ctx.r3.u32==metadata+node_at;
      ctx.r3.u64=object;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x5C));valid=valid && ctx.r3.u32==14;
      for(uint32_t i=0;i<14;++i){ctx.r3.u64=object;ctx.r4.u64=i;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x60));valid=valid && ctx.r3.u32==schema.descriptors[i];}
      ctx.r3.u64=object;ctx.r4.u64=scratch;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(metadata+0x64));
      const auto names=PPC_LOAD_U32(scratch),count=PPC_LOAD_U32(scratch+8);valid=valid && count==14;
      for(uint32_t i=0;i<count;++i){const auto slot=names+i*4;if(i<14)valid=valid && PPC_LOAD_U32(slot)==PPC_LOAD_U32(schema.descriptors[i]+4);}
      // NiFixedString[] has a four-byte construction-count cookie BEFORE the
      // data. Native reserve(0), after clearing logical size, releases every
      // constructed string and deletes the original allocation with its own
      // allocator. A raw free of `names` corrupts the heap.
      PPC_STORE_U32(scratch+8,0);ctx.r3.u64=scratch;ctx.r4.u64=0;__imp__sub_82510578(ctx,base);
      const auto value=scratch+12;
      const auto properties=LegacyAmmoProperties();
      std::array<uint32_t,14> authored{};
      for(uint32_t i=0;i<14;++i){
        auto expected=std::bit_cast<uint32_t>(properties[i].default_value);
        if(original_definition){ctx.r3.u64=definition;ctx.r4.u64=schema.descriptors[i]+4;ctx.r5.u64=value;ctx.r6.u64=0;__imp__sub_827B8628(ctx,base);
          if(ctx.r3.u32)expected=PPC_LOAD_U32(value);else valid=false;}
        authored[i]=expected;
        const bool read=ReadNativeComponentFloat(ctx,base,object,schema.descriptors[i]+4,value);
        if((!read || PPC_LOAD_U32(value)!=expected) && detail.empty())detail="source/default "+std::string(properties[i].name)+" read="+std::to_string(read)+" bits="+std::to_string(PPC_LOAD_U32(value));
        valid=valid && read && PPC_LOAD_U32(value)==expected;
        PPC_STORE_U32(value,std::bit_cast<uint32_t>(float(i)+20.25f));
        const bool wrote=WriteNativeComponentFloat(ctx,base,object,schema.descriptors[i]+4,value);valid=valid && wrote;
        if(!wrote && detail.empty())detail="write "+std::string(properties[i].name);
      }
      for(uint32_t i=0;i<14;++i){const bool read=ReadNativeComponentFloat(ctx,base,object,schema.descriptors[i]+4,value);
        if((!read || PPC_LOAD_U32(value)!=std::bit_cast<uint32_t>(float(i)+20.25f)) && detail.empty())detail="readback "+std::string(properties[i].name)+" read="+std::to_string(read)+" bits="+std::to_string(PPC_LOAD_U32(value));
        valid=valid && read && PPC_LOAD_U32(value)==std::bit_cast<uint32_t>(float(i)+20.25f);
        if(original_definition){ctx.r3.u64=definition;ctx.r4.u64=schema.descriptors[i]+4;ctx.r5.u64=value;ctx.r6.u64=0;__imp__sub_827B8628(ctx,base);
          valid=valid && ctx.r3.u32 && PPC_LOAD_U32(value)==authored[i];}
      }
      NativeItemManagerSnapshot native_before,native_after;std::string facade_error;
      valid=ReadNativeItemManager(base,native_before,facade_error) && valid;
      // Private validation activation only. The real class must be bound to an
      // admitted EM1 world before enabling forwarding; ordinary loading leaves
      // this mode off and preserves authored/native instance property behavior.
      PPC_STORE_U32(object+native_mode_at,1);
      for(unsigned i=0;i<14;++i){float expected=float(i)+20.25f;
        if(i>=4)valid=ReadLegacyAmmoRule(base,properties[i].name,expected,facade_error) && valid;
        const bool read=ReadNativeComponentFloat(ctx,base,object,schema.descriptors[i]+4,value);
        valid=valid && read && PPC_LOAD_U32(value)==std::bit_cast<uint32_t>(expected);
        if(i>=4)valid=WriteNativeComponentFloat(ctx,base,object,schema.descriptors[i]+4,value) && valid;
      }
      PPC_STORE_U32(value,0xDEADBEEF);
      valid=!ReadNativeComponentFloat(ctx,base,object,schema.descriptors[4]+4,value,1) && PPC_LOAD_U32(value)==0xDEADBEEF && valid;
      PPC_STORE_U32(object+native_mode_at,0);
      for(unsigned i=0;i<14;++i){const bool read=ReadNativeComponentFloat(ctx,base,object,schema.descriptors[i]+4,value);
        valid=valid && read && PPC_LOAD_U32(value)==std::bit_cast<uint32_t>(float(i)+20.25f);
      }
      valid=ReadNativeItemManager(base,native_after,facade_error) && valid;
      static_assert(sizeof(NativeItemAmmo)==40);
      valid=valid && native_before.address==native_after.address &&
        !std::memcmp(native_before.ammo.data(),native_after.ammo.data(),sizeof(native_before.ammo));
      if(!valid && detail.empty())detail="native ItemManager property forwarding: "+facade_error;
      PPC_STORE_U32(value,0xDEADBEEF);const bool missing=ReadNativeComponentFloat(ctx,base,object,metadata+name_ref,value);
      valid=valid && !missing && PPC_LOAD_U32(value)==0xDEADBEEF;
      if((missing || PPC_LOAD_U32(value)!=0xDEADBEEF) && detail.empty())detail="unknown property modified output";
      ctx.r3.u64=object;__imp__sub_831BEE80(ctx,base);
      // Exercise the native interface's ordinary shared ownership, including
      // its virtual dispose path on the final release.
      const auto face=object+8,face_table=PPC_LOAD_U32(face);
      ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(face_table));
      valid=valid && PPC_LOAD_U32(object+4)==1;
      ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(face_table));
      valid=valid && PPC_LOAD_U32(object+4)==2;
      ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(face_table+4));
      valid=valid && PPC_LOAD_U32(object+4)==1 && PPC_LOAD_U32(metadata+live_at)==object;
      ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(face_table+4));
      valid=valid && !PPC_LOAD_U32(metadata+live_at) && PPC_LOAD_U32(0x833CDD8C)==before;
    }
    if(!constructed)Free(ctx,base,object,object_size);
    if(PPC_LOAD_U32(metadata+name_ref)){ctx.r3.u64=metadata+name_ref;__imp__sub_822F1C10(ctx,base);}
  }
  if(!original_definition)Free(ctx,base,definition,0x38);Free(ctx,base,scratch,16);Free(ctx,base,metadata,metadata_size);ReleaseNativeFloatSchema(ctx,base,schema);
  if(!valid){error="Native ammo object construction/reflection/finalization/lifetime failed: "+detail;return false;}error.clear();return true;
}
}
