#include "em1_player_bootstrap.h"
#include "ppc_context.h"
#include "simulation.h"
#include "em1_havok_collision_world.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

PPC_EXTERN_FUNC(__imp__sub_82742640); // current global prefab definition lookup
PPC_EXTERN_FUNC(__imp__sub_8276B878); // player slot handle -> ScriptEntity
PPC_EXTERN_FUNC(__imp__sub_82769588); // stock player spawn-transform override
PPC_EXTERN_FUNC(sub_8276D6C8); // mode change, preserving existing audited hooks
PPC_EXTERN_FUNC(__imp__sub_8276A778); // enable slot, dispatches native factory
PPC_EXTERN_FUNC(sub_82A26348); // game factory wrapper + complete native replacement
PPC_EXTERN_FUNC(__imp__sub_8275C0C8); // entity position through transformation component
PPC_EXTERN_FUNC(__imp__sub_8275CDC8); // entity rotation + native physics notification
PPC_EXTERN_FUNC(__imp__sub_8275C160); // entity position getter
PPC_EXTERN_FUNC(__imp__sub_8275FAD0); // complete native respawn placement, no death invocation
PPC_EXTERN_FUNC(__imp__sub_8281BC28); // native spawn matrix -> padded rotation/translation
PPC_EXTERN_FUNC(sub_82A104F8); // attach/create real MickeyTracker camera proxy
PPC_EXTERN_FUNC(sub_82A166D8); // proxy handle -> native tracker component
PPC_EXTERN_FUNC(sub_82A28250); // player handle -> real player-input component
PPC_EXTERN_FUNC(sub_82A27328); // native controller/default input enable policy

namespace em2::em1 {
namespace {
bool Mapped(uint8_t* base,uint32_t address,size_t bytes,bool write=false){
  if(!base||address<4096||!bytes||uint64_t(address)+bytes>0x100000000ull)return false;
  auto at=reinterpret_cast<uintptr_t>(base+address);const auto end=at+bytes;
  while(at<end){MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto access=info.Protect&255;
    const bool writable=access==PAGE_READWRITE||access==PAGE_WRITECOPY||access==PAGE_EXECUTE_READWRITE||access==PAGE_EXECUTE_WRITECOPY;
    if(!writable&&(write||(access!=PAGE_READONLY&&access!=PAGE_EXECUTE_READ)))return false;
    const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;if(next<=at)return false;at=std::min(end,next);
  }return true;
}
struct Context {
  PPCContext& ctx;PPCContext saved;unsigned csr;DWORD error;
  explicit Context(PPCContext& c):ctx(c),saved(c),csr(_mm_getcsr()),error(GetLastError()){}
  void Reset(){ctx=saved;ctx.r1.u64=uint64_t(saved.r1.u32)-2048;}
  ~Context(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
struct Scratch {
  rex::memory::Memory& memory;uint32_t address;
  explicit Scratch(rex::memory::Memory& m):memory(m),address(m.SystemHeapAlloc(512,32)){}
  ~Scratch(){if(address)memory.SystemHeapFree(address);}
};
uint32_t Component(uint8_t* base,uint32_t entity,uint32_t global_type,size_t minimum){
  if(!Mapped(base,entity,96)||!Mapped(base,global_type,1))return 0;
  const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);const auto type=base[global_type];
  if(!Mapped(base,types,unsigned(type)+1))return 0;const auto slot=base[types+type];
  if(slot==255||!Mapped(base,components,unsigned(slot)*4+4))return 0;
  const auto face=PPC_LOAD_U32(components+unsigned(slot)*4);
  return face>=8&&!(face&3)&&Mapped(base,face-8,minimum)?face-8:0;
}
uint32_t Resolve(Context& scope,uint8_t* base,uint32_t manager){
  scope.Reset();scope.ctx.r3.u64=manager;scope.ctx.r4.u64=0;__imp__sub_8276B878(scope.ctx,base);
  const auto face=scope.ctx.r3.u32;return !(face&3)&&Mapped(base,face,96)?face:0;
}
uint32_t CharacterBody(uint8_t* base,uint32_t entity,uint32_t* input_out=nullptr){
  const auto motion=Component(base,entity,0x833daf54,976);if(!motion)return 0;
  const auto input=PPC_LOAD_U32(motion+80);if(!Mapped(base,input,180))return 0;
  const auto character=PPC_LOAD_U32(input+176);if(!Mapped(base,character,128))return 0;
  const auto body=PPC_LOAD_U32(character+16);
  if(!Mapped(base,body,544)||PPC_LOAD_U32(body)!=0x82030384)return 0;
  if(input_out)*input_out=input;return body;
}
void TraceCharacterBody(uint8_t* base,uint32_t entity,const char* phase){
  uint32_t input=0;const auto body=CharacterBody(base,entity,&input);if(!body){REXLOG_INFO("[em1] character physics {}: body unavailable",phase);return;}
  auto f=[&](uint32_t p){return std::bit_cast<float>(PPC_LOAD_U32(p));};
  const auto world=PPC_LOAD_U32(body+8),user=PPC_LOAD_U32(body+12);
  REXLOG_INFO("[em1] character physics {}: body={:08X} world={:08X} filter={:08X} userdata={:08X} quality={} response={} position={},{},{} actualVelocity={},{},{} inputOffset={}",
    phase,body,world,PPC_LOAD_U32(body+44),user,unsigned(base[body+42]),unsigned(base[body+136]),
    f(body+0x120),f(body+0x124),f(body+0x128),f(body+0x1b0),f(body+0x1b4),f(body+0x1b8),f(input+168));
  if(Mapped(base,user,112))REXLOG_INFO("[em1] character userdata {}: words0..28={:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X} word96={:08X}",phase,
    PPC_LOAD_U32(user),PPC_LOAD_U32(user+4),PPC_LOAD_U32(user+8),PPC_LOAD_U32(user+12),PPC_LOAD_U32(user+16),PPC_LOAD_U32(user+20),PPC_LOAD_U32(user+24),PPC_LOAD_U32(user+28),PPC_LOAD_U32(user+96));
}
}
NativePlayerBootstrapStatus BootstrapNativeCourtyardPlayer(PPCContext& ctx,uint8_t* base,
    NativePlayerBootstrap& state,std::string& diagnostic){
  Context scope(ctx);diagnostic.clear();
  auto pending=[&](const std::string& reason){diagnostic=reason;
    return ++state.attempts<=600?NativePlayerBootstrapStatus::Pending:NativePlayerBootstrapStatus::Failed;};
  auto failed=[&](const std::string& reason){diagnostic=reason;REXLOG_ERROR("[em1] native player bootstrap failed: {}",reason);return NativePlayerBootstrapStatus::Failed;};
  if(ctx.r1.u32<32768||!Mapped(base,ctx.r1.u32-32768,32768,true))return failed("Initialized game-thread stack unavailable");
  if(!Mapped(base,0x833dab60,4)||!Mapped(base,0x833da7e4,4))return pending("Native player/core globals unavailable");
  const auto manager=PPC_LOAD_U32(0x833dab60),core=PPC_LOAD_U32(0x833da7e4);
  if(!Mapped(base,manager,728,true)||!Mapped(base,core,232,true))return pending("Native PlayerManager/core not ready after source load");
  if(!Mapped(base,PPC_LOAD_U32(core+152),116,true))return pending("Native default entity group is not ready after source transition");
  if(!Mapped(base,0x833db67c,4)||!Mapped(base,PPC_LOAD_U32(0x833db67c),32))return pending("Native persistent template registry is not initialized");
  if(state.manager&&state.manager!=manager)state={};state.manager=manager;
  const auto camera=PPC_LOAD_U32(manager+28);
  if(!Mapped(base,camera,580,true)||PPC_LOAD_U32(camera)!=0x82112778)
    return pending("Normal 3D player interface/camera not installed by ordinary gameplay bootstrap");
  auto* kernel=rex::kernel::kernel_state();if(!kernel||!kernel->memory())return failed("Guest allocation context unavailable");
  Scratch work(*kernel->memory());if(!work.address||!Mapped(base,work.address,512,true))return failed("Native player bootstrap scratch allocation failed");
  std::memset(base+work.address,0,512);
  if(!state.prepared){
    // Definitions come from original _Dynamic.pak / palettes/_Dynamic.bin.
    // Do not pin rawentity+80: that descriptor setter performs no ownership.
    for(const auto& prefab:std::array<std::pair<uint32_t,const char*>,2>{{{0x8207877c,"Player"},{0x8211130c,"MickeyTracker"}}}){
      scope.Reset();ctx.r3.u64=core;ctx.r4.u64=prefab.first;__imp__sub_82742640(ctx,base);
      if(!Mapped(base,ctx.r3.u32,72))return pending(std::string("Original persistent prefab unavailable after source Begin: ")+prefab.second);
    }
    const std::array<float,16> spawn={1,0,0,0,0,1,0,0,0,0,1,0,51.41127014f,-.89998168f,5.21368694f,1};
    for(unsigned k=0;k<16;++k)PPC_STORE_U32(work.address+k*4,std::bit_cast<uint32_t>(spawn[k]));
    scope.Reset();ctx.r3.u64=manager;ctx.r4.u64=0;ctx.r5.u64=work.address;__imp__sub_82769588(ctx,base);
    const auto before=Resolve(scope,base,manager);
    if(PPC_LOAD_U32(manager+32)!=0){
      scope.Reset();ctx.r3.u64=manager;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=0;sub_8276D6C8(ctx,base);
    }
    if(!base[manager+37]){
      scope.Reset();ctx.r3.u64=manager;ctx.r4.u64=0;ctx.r5.u64=1;__imp__sub_8276A778(ctx,base);
    }
    auto face=Resolve(scope,base,manager);
    if(!face){
      REXLOG_INFO("[em1] recreating original Player through complete native PlayerManager factory");
      scope.Reset();ctx.r3.u64=manager;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=1;sub_82A26348(ctx,base);
      face=Resolve(scope,base,manager);
    }
    if(!face)return failed("Stock native Player factory returned no registered slot0 actor");
    state.created=before!=face;state.prepared=true;state.entity=face;
    state.handle=PPC_LOAD_U32(manager+20);state.camera=camera;
    REXLOG_INFO("[em1] original Player {}: handle={:08X} entity={:08X} camera={:08X}",state.created?"created":"retained",state.handle,face,camera);
  }
  if(PPC_LOAD_U32(manager+28)!=camera)return pending("Native player factory changed camera interface; rebinding on next tick");
  if(state.camera!=camera){state.camera=camera;state.camera_requested=false;state.controls_requested=false;}
  const auto face=Resolve(scope,base,manager);
  if(!face)return pending("Registered native player temporarily unavailable during component initialization");
  if(face!=state.entity){state.entity=face;state.handle=PPC_LOAD_U32(manager+20);state.placed=false;state.teleport_requested=false;state.camera_requested=false;state.controls_requested=false;}
  if(!Component(base,face,0x833dbc3c,16))return pending("Original player transformation component is not ready");
  if(!state.placed){
    if(!CharacterBody(base,face))return pending("Native character rigid body not ready for complete respawn placement");
    const std::array<float,16> spawn={1,0,0,0,0,1,0,0,0,0,1,0,51.41127014f,-.89998168f,5.21368694f,1};
    for(unsigned k=0;k<16;++k)PPC_STORE_U32(work.address+k*4,std::bit_cast<uint32_t>(spawn[k]));
    if(!state.teleport_requested){
      TraceCharacterBody(base,face,"before authored respawn placement");
      // Exactly the native respawn82A37518 sequence: convert the saved spawn,
      // then invoke complete ground-adjusted placement. The old visual-only
      // setters omitted8275DE68's physics/component and motion-state handling.
      scope.Reset();ctx.r3.u64=work.address;ctx.r4.u64=work.address+128;__imp__sub_8281BC28(ctx,base);
      scope.Reset();ctx.r3.u64=face;ctx.r4.u64=work.address+176;ctx.r5.u64=work.address+128;
      ctx.r6.u64=1;ctx.r7.u64=1;ctx.r8.u64=1;ctx.r9.u64=1;__imp__sub_8275FAD0(ctx,base);
      state.teleport_requested=true;TraceCharacterBody(base,face,"after authored respawn placement");
      return pending("Native respawn placement dispatched; waiting for visual/character-body agreement");
    }
    scope.Reset();ctx.r3.u64=face;ctx.r4.u64=work.address+96;__imp__sub_8275C160(ctx,base);
    const auto body=CharacterBody(base,face);if(!body)return pending("Native character body changed during respawn placement");
    for(unsigned k=0;k<3;++k){const auto visual=std::bit_cast<float>(PPC_LOAD_U32(work.address+96+k*4));
      const auto physical=std::bit_cast<float>(PPC_LOAD_U32(body+0x120+k*4));
      if(!std::isfinite(visual)||!std::isfinite(physical))return failed("Native respawn placement returned nonfinite visual/physical pose");
      if(k!=1&&(std::abs(visual-spawn[12+k])>.01||std::abs(physical-spawn[12+k])>.01))
        return pending("Native respawn visual/character-body horizontal positions do not yet agree at authored spawn");
      if(k==1&&(visual< -1.2f||visual> -.3f||std::abs(physical-visual)>3.f))
        return pending("Native respawn vertical pose outside authored floor band");}
    if(PPC_LOAD_U32(body+8)!=CurrentNativeCollisionWorld(base))return failed("Respawned native character belongs to another physics world");
    scope.Reset();em2::ResetPlayerMotionHistory(ctx,base);
    state.placed=true;TraceCharacterBody(base,face,"verified authored respawn placement");
    REXLOG_INFO("[em1] original Player placed through complete native respawn/physics path at authored courtyard spawn");
  }
  if(!state.camera_requested){
    scope.Reset();ctx.r3.u64=camera;ctx.r4.u64=face;sub_82A104F8(ctx,base);state.camera_requested=true;
  }
  if(!PPC_LOAD_U32(camera+564))return pending("Native MickeyTracker proxy handle not installed");
  scope.Reset();ctx.r3.u64=camera+564;sub_82A166D8(ctx,base);state.proxy=ctx.r3.u32;
  if(!Mapped(base,state.proxy,16))return pending("Native MickeyTracker proxy component not ready");
  scope.Reset();ctx.r3.u64=manager+20;sub_82A28250(ctx,base);state.input=ctx.r3.u32;
  if(!Mapped(base,state.input,180))return pending("Native player-input component not ready");
  if(!state.controls_requested){
    scope.Reset();ctx.r3.u64=manager;ctx.r4.u64=0;ctx.r5.u64=1;ctx.r6.u64=1;sub_82A27328(ctx,base);state.controls_requested=true;
  }
  state.motion=Component(base,face,0x833daf54,976);
  if(!state.motion)return pending("Native BasicMotion component not ready");
  const auto motion_input=PPC_LOAD_U32(state.motion+80);
  if(!Mapped(base,motion_input,180)||!PPC_LOAD_U32(motion_input+176)||!base[motion_input+145])return pending("Native BasicMotion input listeners/default policy not active");
  if(PPC_LOAD_U32(manager+32)!=0||!base[manager+37]||!PPC_LOAD_U32(camera+392)||PPC_LOAD_U32(camera+24)!=PPC_LOAD_U32(camera+392))
    return pending("Native human mode/default gameplay camera activation not complete");
  diagnostic="Original player, authored spawn, native camera proxy and input listeners registered; contact/standing acceptance still requires observation";
  REXLOG_INFO("[em1] native player bootstrap ready: entity={:08X} handle={:08X} camera={:08X} proxy={:08X} input={:08X} motion={:08X}; no synthesized gameplay input",face,state.handle,camera,state.proxy,state.input,state.motion);
  return NativePlayerBootstrapStatus::Ready;
}
bool TraceNativeCourtyardCollisionState(PPCContext& ctx,uint8_t* base,
    const NativePlayerBootstrap& player,const NativeCollisionAttachment& collision,std::string& error){
  Context scope(ctx);error.clear();
  if(!Mapped(base,player.entity,96)||!Mapped(base,player.motion,976)||!Mapped(base,collision.world,128)){
    error="Native collision snapshot identities unavailable";return false;
  }
  TraceCharacterBody(base,player.entity,"collision snapshot");
  const auto filter=PPC_LOAD_U32(collision.world+124);
  if(Mapped(base,filter,20)){
    const auto table=PPC_LOAD_U32(filter);
    REXLOG_INFO("[em1] collision world filter: world={:08X} filter={:08X} vtables/words0..16={:08X}/{:08X}/{:08X}/{:08X}/{:08X}",collision.world,filter,table,
      PPC_LOAD_U32(filter+4),PPC_LOAD_U32(filter+8),PPC_LOAD_U32(filter+12),PPC_LOAD_U32(filter+16));
    if(Mapped(base,table,32))REXLOG_INFO("[em1] collision filter primary methods0..28={:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}",
      PPC_LOAD_U32(table),PPC_LOAD_U32(table+4),PPC_LOAD_U32(table+8),PPC_LOAD_U32(table+12),PPC_LOAD_U32(table+16),PPC_LOAD_U32(table+20),PPC_LOAD_U32(table+24),PPC_LOAD_U32(table+28));
  }
  for(const auto& owner:collision.objects){
    if(!Mapped(base,owner.body,544)){error="Authored collision snapshot body unmapped";return false;}
    const auto shape=PPC_LOAD_U32(owner.body+16);
    REXLOG_INFO("[em1] authored body snapshot '{}' body={:08X} world={:08X} filter={:08X} userdata={:08X} quality={} response={} shape={:08X} shapeType={}",
      owner.name,owner.body,PPC_LOAD_U32(owner.body+8),PPC_LOAD_U32(owner.body+44),PPC_LOAD_U32(owner.body+12),unsigned(base[owner.body+42]),unsigned(base[owner.body+136]),
      shape,Mapped(base,shape,16)?PPC_LOAD_U32(shape+12):UINT32_MAX);
  }
  return true;
}
bool SampleNativeCourtyardLightPosition(PPCContext& ctx,uint8_t* base,
    uint32_t& entity,std::array<float,3>& position,uint32_t* script_identity) {
  if(script_identity)*script_identity=0;
  Context scope(ctx);
  if(ctx.r1.u32<32768 || !Mapped(base,ctx.r1.u32-32768,32768,true) ||
     !Mapped(base,0x833dab60,4))return false;
  // Bootstrap ownership ends after placement. Respawn may replace the manager
  // or actor, so resolve the current engine-owned player on every sample.
  const auto manager=PPC_LOAD_U32(0x833dab60);
  if(!Mapped(base,manager,40))return false;
  const auto current=Resolve(scope,base,manager);
  if(!Mapped(base,current,96) || !Component(base,current,0x833dbc3c,16))return false;
  auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->memory())return false;
  Scratch work(*kernel->memory());if(!work.address || !Mapped(base,work.address,512,true))return false;
  scope.Reset();ctx.r3.u64=current;ctx.r4.u64=work.address;__imp__sub_8275C160(ctx,base);
  std::array<float,3> sample{};
  for(unsigned i=0;i<3;++i){sample[i]=std::bit_cast<float>(PPC_LOAD_U32(work.address+i*4));if(!std::isfinite(sample[i]))return false;}
  entity=current;position=sample;
  // Retail GetPlayer (82737940 -> 82769FB8) returns this entity reference,
  // while 8276B878 resolves the same manager slot for the lighting sample.
  // Preserve that engine-owned reference rather than exporting its address.
  if(script_identity)*script_identity=PPC_LOAD_U32(manager+20);
  return true;
}
bool ObserveNativeCourtyardStanding(PPCContext& ctx,uint8_t* base,
    const NativePlayerBootstrap& player,uint32_t ticks,
    NativeStandingObservation& observation,std::string& diagnostic){
  Context scope(ctx);diagnostic.clear();
  auto invalid=[&](const std::string& reason){observation.complete=true;observation.standing=false;diagnostic=reason;return false;};
  if(observation.complete)return true;
  if(observation.last_tick!=UINT32_MAX){
    if(ticks<observation.last_tick)return invalid("Standing observer tick count moved backward");
    if(ticks==observation.last_tick)return true;
  }else if(ticks!=0)return invalid("Standing observer requires a baseline at tick0 before waiting120 native ticks");
  if(ticks>120)return invalid("Standing observer exceeded its bounded120-tick window");
  if(ctx.r1.u32<32768||!Mapped(base,ctx.r1.u32-32768,32768,true))return invalid("Standing observer lacks initialized native stack");
  if(!Mapped(base,0x833dab60,4)||PPC_LOAD_U32(0x833dab60)!=player.manager||!Mapped(base,player.manager,40)||
      PPC_LOAD_U32(player.manager+20)!=player.handle||!Mapped(base,player.entity,96)||!Mapped(base,player.motion,976))
    return invalid("Standing observer player identity retired or became unmapped");
  if(Resolve(scope,base,player.manager)!=player.entity||Component(base,player.entity,0x833daf54,976)!=player.motion||
      !Component(base,player.entity,0x833dbc3c,16))return invalid("Standing observer actor/component identity changed");
  const auto world=CurrentNativeCollisionWorld(base);
  if(!Mapped(base,world,496))return invalid("Standing observer native world unavailable");
  if(observation.world&&observation.world!=world)return invalid("Standing observer world changed");
  const auto time=std::bit_cast<float>(PPC_LOAD_U32(world+480));
  if(!std::isfinite(time))return invalid("Standing observer native step time is nonfinite");
  if(observation.samples&&time==observation.world_time)return true;
  auto* kernel=rex::kernel::kernel_state();if(!kernel||!kernel->memory())return invalid("Standing observer scratch allocator unavailable");
  Scratch work(*kernel->memory());if(!work.address||!Mapped(base,work.address,512,true))return invalid("Standing observer scratch allocation failed");
  std::memset(base+work.address,0,512);
  scope.Reset();ctx.r3.u64=player.entity;ctx.r4.u64=work.address;__imp__sub_8275C160(ctx,base);
  const auto character_body=CharacterBody(base,player.entity);
  if(!character_body||PPC_LOAD_U32(character_body+8)!=world)return invalid("Standing observer character rigid body is absent or in another world");
  NativeStandingSample sample;
  for(unsigned k=0;k<3;++k){sample.position[k]=std::bit_cast<float>(PPC_LOAD_U32(work.address+k*4));
    // BasicMotion+240 is desired velocity and may stayzero during free fall.
    // Native82EB8D50 returns actual rigid-body linear velocity atbody+1B0.
    sample.velocity[k]=std::bit_cast<float>(PPC_LOAD_U32(character_body+0x1b0+k*4));
    if(!std::isfinite(sample.position[k])||!std::isfinite(sample.velocity[k]))return invalid("Standing observer found nonfinite native position/velocity");}
  sample.ground_state=PPC_LOAD_U32(player.motion+384);
  if(!observation.samples){observation.world=world;observation.first_world_time=time;}
  if(time<observation.world_time&&observation.samples)return invalid("Standing observer native world time moved backward");
  observation.world_time=time;observation.last_tick=ticks;++observation.samples;
  observation.tail[observation.tail_next]=sample;observation.tail_next=(observation.tail_next+1)%observation.tail.size();
  observation.tail_count=std::min(uint32_t(observation.tail.size()),observation.tail_count+1);
  if(ticks<120)return true;
  observation.complete=true;
  float min_y=sample.position[1],max_y=min_y,max_vy=0;unsigned grounded=0;bool near_spawn=true;
  constexpr float floor_y=-.9846437326f;
  for(unsigned j=0;j<observation.tail_count;++j){const auto& s=observation.tail[j];
    min_y=std::min(min_y,s.position[1]);max_y=std::max(max_y,s.position[1]);max_vy=std::max(max_vy,std::abs(s.velocity[1]));
    grounded+=s.ground_state==0;
    near_spawn&=std::abs(s.position[0]-51.41127014f)<.5f&&std::abs(s.position[2]-5.21368694f)<.5f&&s.position[1]>floor_y-.1f&&s.position[1]<floor_y+.5f;
  }
  const float advanced=observation.world_time-observation.first_world_time;
  observation.standing=observation.tail_count==32&&advanced>=1.f&&grounded==32&&max_vy<.15f&&max_y-min_y<.03f&&near_spawn;
  diagnostic=std::string(observation.standing?"Post-tick standing observed":"Post-tick standing NOT established")+
    ": samples="+std::to_string(observation.samples)+" nativeSeconds="+std::to_string(advanced)+
    " groundedTail="+std::to_string(grounded)+"/"+std::to_string(observation.tail_count)+
    " yRange="+std::to_string(min_y)+".."+std::to_string(max_y)+" maxAbsVy="+std::to_string(max_vy)+
    " nearAuthoredSpawn="+std::to_string(near_spawn)+" lastGround="+std::to_string(sample.ground_state);
  REXLOG_INFO("[em1] {}",diagnostic);return true;
}
}
