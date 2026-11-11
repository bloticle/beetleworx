#include "em1_native_item_manager.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
PPC_EXTERN_FUNC(__imp__sub_82774C88);
PPC_EXTERN_FUNC(__imp__sub_82778DC0);
PPC_EXTERN_FUNC(__imp__sub_82778EC0);
PPC_EXTERN_FUNC(__imp__sub_82774DD0);
PPC_EXTERN_FUNC(__imp__sub_82778C20);
namespace em2::em1 {
namespace {
constexpr uint32_t singleton=0x833DADA0,prefix_size=0x10C;
struct LastError {DWORD value=GetLastError();~LastError(){SetLastError(value);}};
struct Restore {
  PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();
  explicit Restore(PPCContext& c):ctx(c),saved(c){ctx.r1.u64-=2048;}
  ~Restore(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}
};
bool Copy(uint8_t* base,uint32_t at,void* out,size_t bytes){SIZE_T read=0;
  return base && at>=4096 && uint64_t(at)+bytes<=0x100000000ull && ReadProcessMemory(GetCurrentProcess(),base+at,out,bytes,&read) && read==bytes;
}
bool Raw(uint8_t* base,uint32_t& address,std::array<uint8_t,prefix_size>& bytes){
  uint32_t raw=0;if(!Copy(base,singleton,&raw,4))return false;address=_byteswap_ulong(raw);
  if(!Copy(base,address,bytes.data(),bytes.size()))return false;
  uint32_t check=0;return Copy(base,singleton,&check,4) && check==raw;
}
}
bool ReadNativeItemManager(uint8_t* base,NativeItemManagerSnapshot& output,std::string& error){
  const auto saved_error=GetLastError();struct Last{DWORD value;~Last(){SetLastError(value);}} restore{saved_error};
  NativeItemManagerSnapshot result;std::array<uint8_t,prefix_size> bytes;
  if(!Raw(base,result.address,bytes)){error="Native EM2 ItemManager is unavailable or changing";return false;}
  auto word=[&](size_t at){return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];};
  auto number=[&](size_t at){return std::bit_cast<float>(word(at));};
  for(unsigned c=0;c<3;++c){const size_t at=0x3C+c*40;auto& entry=result.ammo[c];
    entry.amount=number(at);entry.effective_max=number(at+4);entry.configured_max=number(at+8);entry.segment_amount=number(at+12);
    entry.max_segments=std::bit_cast<int32_t>(word(at+16));entry.penalty_amount=number(at+20);entry.penalty_wait=number(at+24);
    entry.recharge_max=number(at+28);entry.recharge_rate=number(at+32);entry.recharge_wait=number(at+36);
    for(float value:{entry.amount,entry.effective_max,entry.configured_max,entry.segment_amount,entry.penalty_amount,entry.penalty_wait,entry.recharge_max,entry.recharge_rate,entry.recharge_wait})
      if(!std::isfinite(value)){error="Native ItemManager ammo record is not initialized";return false;}
  }
  result.etickets=std::bit_cast<int32_t>(word(0xB4));result.eticket_limit=std::bit_cast<int32_t>(word(0xB8));
  result.amount_writes_blocked=bytes[0x108]!=0;output=result;error.clear();return true;
}
namespace {
bool Set(PPCContext& ctx,uint8_t* base,NativeAmmoKind kind,float value,bool maximum,std::string& error){
  const auto channel=uint32_t(kind);if(channel>=3 || !std::isfinite(value) || ctx.r1.u32<32768){error="Invalid native ItemManager write";return false;}
  NativeItemManagerSnapshot before;if(!ReadNativeItemManager(base,before,error))return false;
  Restore restore(ctx);ctx.r3.u64=before.address;ctx.r4.u64=channel;ctx.f1.f64=value;
  if(maximum)__imp__sub_82778EC0(ctx,base);else __imp__sub_82778DC0(ctx,base);
  NativeItemManagerSnapshot after;if(!ReadNativeItemManager(base,after,error))return false;
  if(after.address!=before.address){error="Native ItemManager changed during write";return false;}
  const float actual=maximum?after.ammo[channel].configured_max:after.ammo[channel].amount;
  if(actual!=value){error="Native ItemManager rejected the requested value";return false;}
  error.clear();return true;
}
}
bool SetNativeItemAmmo(PPCContext& ctx,uint8_t* base,NativeAmmoKind kind,float value,std::string& error){return Set(ctx,base,kind,value,false,error);}
bool SetNativeItemAmmoMax(PPCContext& ctx,uint8_t* base,NativeAmmoKind kind,float value,std::string& error){return Set(ctx,base,kind,value,true,error);}
bool IsNativeInventoryProperty(std::string_view name){
  return name=="Paint" || name=="PaintMax" || name=="Thinner" || name=="ThinnerMax" || name=="ETicket" || name=="ETicketMax";
}
bool ReadNativeInventoryProperty(uint8_t* base,std::string_view name,float& value,std::string& error){
  LastError restore;
  if(!IsNativeInventoryProperty(name)){error="Legacy inventory field has no native player-manager binding";return false;}
  NativeItemManagerSnapshot manager;if(!ReadNativeItemManager(base,manager,error))return false;
  float result=0;
  if(name=="Paint")result=manager.ammo[0].amount;
  else if(name=="PaintMax")result=manager.ammo[0].effective_max;
  else if(name=="Thinner")result=manager.ammo[1].amount;
  else if(name=="ThinnerMax")result=manager.ammo[1].effective_max;
  else {
    const auto integer=name=="ETicket"?manager.etickets:manager.eticket_limit;
    if(integer<0 || double(float(integer))!=double(integer)){error="Native currency cannot be represented exactly by the legacy Float field";return false;}
    result=float(integer);
  }
  value=result;error.clear();return true;
}
bool WriteNativeInventoryProperty(PPCContext& ctx,uint8_t* base,std::string_view name,float value,std::string& error){
  if(!IsNativeInventoryProperty(name) || !std::isfinite(value) || value<0 || ctx.r1.u32<32768){error="Invalid native player inventory property write";return false;}
  if(name=="Paint" || name=="Thinner")return SetNativeItemAmmo(ctx,base,name=="Paint"?NativeAmmoKind::Paint:NativeAmmoKind::Thinner,value,error);
  if(name=="PaintMax" || name=="ThinnerMax")return SetNativeItemAmmoMax(ctx,base,name=="PaintMax"?NativeAmmoKind::Paint:NativeAmmoKind::Thinner,value,error);
  // EM1 exposes currency as Float, EM2 stores signed whole units. Reject
  // lossy conversion instead of silently truncating gameplay currency.
  if(double(value)>double(std::numeric_limits<int32_t>::max()) || std::trunc(value)!=value){error="Native E-tickets require exactly representable nonnegative whole units";return false;}
  NativeItemManagerSnapshot before;if(!ReadNativeItemManager(base,before,error))return false;
  if(before.eticket_limit<0){error="Native E-ticket limit is not initialized";return false;}
  Restore restore(ctx);const auto requested=int32_t(value);
  if(name=="ETicketMax"){
    // Retail 82776868..82776874 writes this native per-currency limit
    // directly. Like that path, changing the limit does not spend currency.
    PPC_STORE_U32(before.address+0xB8,uint32_t(requested));
  }else{
    ctx.r3.u64=before.address;ctx.r4.u64=0;ctx.r5.s64=requested;__imp__sub_82778C20(ctx,base);
  }
  NativeItemManagerSnapshot after;if(!ReadNativeItemManager(base,after,error))return false;
  const auto expected=name=="ETicketMax"?requested:(requested<before.eticket_limit?requested:before.eticket_limit);
  if(after.address!=before.address || (name=="ETicketMax"?after.eticket_limit:after.etickets)!=expected){error="Native ItemManager rejected the currency update";return false;}
  error.clear();return true;
}
namespace {
bool RuleOffset(std::string_view property,uint32_t& offset){
  uint32_t channel=0;
  if(property.starts_with("Paint"))property.remove_prefix(5);
  else if(property.starts_with("Thinner")){property.remove_prefix(7);channel=1;}
  else return false;
  uint32_t field=0;
  if(property=="PenaltyAmount")field=0x50;
  else if(property=="PenaltyWait")field=0x54;
  else if(property=="RechargeMax")field=0x58;
  else if(property=="RechargeRate")field=0x5C;
  else if(property=="RechargeWait")field=0x60;
  else return false;
  offset=field+channel*40;return true;
}
}
bool ReadLegacyAmmoRule(uint8_t* base,std::string_view property,float& output,std::string& error){
  LastError restore;
  uint32_t offset=0;if(!RuleOffset(property,offset)){error="Legacy property is not an ItemManager recharge rule";return false;}
  NativeItemManagerSnapshot manager;if(!ReadNativeItemManager(base,manager,error))return false;
  uint32_t raw=0;if(!Copy(base,manager.address+offset,&raw,4)){error="Native ItemManager rule unavailable";return false;}
  const auto value=std::bit_cast<float>(_byteswap_ulong(raw));
  if(!std::isfinite(value)){error="Native ItemManager rule is not initialized";return false;}
  output=value;error.clear();return true;
}
bool WriteLegacyAmmoRule(uint8_t* base,std::string_view property,float value,std::string& error){
  LastError restore;
  uint32_t offset=0;if(!RuleOffset(property,offset) || !std::isfinite(value)){error="Invalid native ItemManager recharge rule";return false;}
  NativeItemManagerSnapshot manager;if(!ReadNativeItemManager(base,manager,error))return false;
  // Retail SetAmmoRechargeMax writes this cache directly; initialization sets
  // the other four fields in the same cache. Existing native update consumes
  // them and owns timers, modifiers and refill. No extra tick is installed.
  PPC_STORE_U32(manager.address+offset,std::bit_cast<uint32_t>(value));
  float actual=0;if(!ReadLegacyAmmoRule(base,property,actual,error))return false;
  if(actual!=value){error="Native ItemManager rule write did not persist";return false;}
  error.clear();return true;
}
bool ProbeNativeItemManager(PPCContext& ctx,uint8_t* base,uint32_t scratch,NativeItemManagerSnapshot& output,std::string& error){
  NativeItemManagerSnapshot before;if(!ReadNativeItemManager(base,before,error))return false;
  uint32_t address=0;std::array<uint8_t,prefix_size> previous,current;
  if(!Raw(base,address,previous) || address!=before.address || scratch<4096 || ctx.r1.u32<32768){error="Native ItemManager probe requires private scratch and engine stack";return false;}
  Restore restore(ctx);
  constexpr uint32_t names[]={0x8207B804,0x8207B80C,0x8207B814};
  for(unsigned c=0;c<3;++c){PPC_STORE_U32(scratch,names[c]);ctx.r3.u64=address;ctx.r4.u64=scratch;__imp__sub_82774C88(ctx,base);
    if(ctx.r3.u32!=c){error="Retail ItemManager channel lookup disagrees with adapter";return false;}
    const auto kind=NativeAmmoKind(c);
    if(!SetNativeItemAmmo(ctx,base,kind,before.ammo[c].amount,error) || !SetNativeItemAmmoMax(ctx,base,kind,before.ammo[c].configured_max,error))return false;
  }
  PPC_STORE_U32(scratch,0x8207B844);ctx.r3.u64=address;ctx.r4.u64=scratch;__imp__sub_82774DD0(ctx,base);
  if(ctx.r3.u32!=0){error="Retail E-ticket channel lookup disagrees with inventory binding";return false;}
  for(const auto name:{"Paint","PaintMax","Thinner","ThinnerMax","ETicket","ETicketMax"}){float value=0;
    if(!ReadNativeInventoryProperty(base,name,value,error))return false;
    // Public Max reads effective capacity; its setter configures the requested
    // maximum, which may exceed the segment cap. Preserve that distinction in
    // this same-value diagnostic rather than changing a saved configuration.
    const auto write_value=std::string_view(name)=="PaintMax"?before.ammo[0].configured_max:
      std::string_view(name)=="ThinnerMax"?before.ammo[1].configured_max:value;
    if(std::string_view(name)=="ETicket" && before.etickets>before.eticket_limit)continue;
    if(!WriteNativeInventoryProperty(ctx,base,name,write_value,error))return false;}
  for(const auto name:{"PaintAmmoSegmentAmount","SketchCount","Invisible_Ammo","Unknown"}){float value=123;
    if(IsNativeInventoryProperty(name) || ReadNativeInventoryProperty(base,name,value,error) || value!=123){error="Native player inventory binding accepted an unsupported field";return false;}}
  if(WriteNativeInventoryProperty(ctx,base,"ETicket",0.5f,error) || WriteNativeInventoryProperty(ctx,base,"ETicketMax",-1,error)){
    error="Native player inventory binding accepted lossy currency";return false;}
  constexpr std::string_view rule_names[]={"PaintPenaltyAmount","PaintPenaltyWait","PaintRechargeMax","PaintRechargeRate","PaintRechargeWait",
    "ThinnerPenaltyAmount","ThinnerPenaltyWait","ThinnerRechargeMax","ThinnerRechargeRate","ThinnerRechargeWait"};
  for(const auto name:rule_names){float value=0;if(!ReadLegacyAmmoRule(base,name,value,error) || !WriteLegacyAmmoRule(base,name,value,error))return false;}
  for(const auto name:{"PaintCost","PaintJumpCost","ElectricRechargeRate","Unknown"}){float value=123;
    if(ReadLegacyAmmoRule(base,name,value,error) || value!=123){error="Native ItemManager rule facade accepted an unsupported property";return false;}}
  error.clear();
  if(!Raw(base,address,current) || address!=before.address || current!=previous){error="Same-value ItemManager probe modified native state";return false;}
  output=before;error.clear();return true;
}
bool ProbeNativeInventoryWrites(PPCContext& ctx,uint8_t* base,std::string& error){
  Restore restore(ctx);NativeItemManagerSnapshot before;
  uint32_t address=0;std::array<uint8_t,prefix_size> previous,current;
  float original_amount=0,original_limit=0;
  if(!ReadNativeItemManager(base,before,error) ||
     !ReadNativeInventoryProperty(base,"ETicket",original_amount,error) ||
     !ReadNativeInventoryProperty(base,"ETicketMax",original_limit,error))return false;
  if(before.etickets>before.eticket_limit || !Raw(base,address,previous) || address!=before.address){error="Private inventory mutation probe requires a valid restorable currency profile";return false;}
  bool valid=true;std::string failure;
  auto write=[&](std::string_view name,float value){std::string reason;
    const bool ok=WriteNativeInventoryProperty(ctx,base,name,value,reason);
    if(!ok && failure.empty())failure=std::string(name)+": "+reason;valid=ok && valid;};
  auto expect=[&](std::string_view name,float expected){float value=0;std::string reason;
    const bool ok=ReadNativeInventoryProperty(base,name,value,reason) && value==expected;
    if(!ok && failure.empty())failure=std::string(name)+" native readback mismatch: "+reason;valid=ok && valid;};
  write("ETicketMax",2);expect("ETicketMax",2);expect("ETicket",original_amount);
  write("ETicket",1);expect("ETicket",1);
  write("ETicket",3);expect("ETicket",2); // The retail setter clamps to cap.
  // Restore on every ordinary failure, using real native amount notifications.
  write("ETicketMax",original_limit);write("ETicket",original_amount);
  expect("ETicketMax",original_limit);expect("ETicket",original_amount);
  if(!Raw(base,address,current) || address!=before.address || current!=previous){valid=false;if(failure.empty())failure="Native manager prefix did not restore exactly";}
  if(!valid){error="Private native inventory mutation validation failed: "+failure;return false;}
  error.clear();return true;
}
}
