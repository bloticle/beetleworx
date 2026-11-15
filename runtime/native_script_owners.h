#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
struct PPCContext;
namespace em2 {
struct NativeScriptOwner {
    uint32_t address=0,state=0,value=0,type=0,next=0,previous=0;
    bool operator==(const NativeScriptOwner&)const=default;
};
struct NativeScriptOwners {
    uint32_t global=0,head=0,thread=0;
    std::vector<NativeScriptOwner> owners;
    bool operator==(const NativeScriptOwners&)const=default;
};
// The native global state's +712 intrusive list is an actual GC root set.
// Observe only at an exclusively owned completed VM boundary. It includes
// temporary C++ holders, not just persistent gameplay script owners.
bool ReadNativeScriptOwners(uint8_t*,uint32_t state,NativeScriptOwners&,std::string&);
struct NativeScriptOwnerReplacement {uint32_t owner=0,rooted_slot=0;};
// Trusted LOCAL roots only. Preflights the complete expected holder list and
// every replacement before calling original native LuaObject assignment.
// Preserves each holder's owning lua_State and native GC list membership.
// Does not restore scheduler state, stack holders that expired, or world state.
bool RebindNativeScriptOwners(PPCContext&,uint8_t*,uint32_t state,const NativeScriptOwners& expected,
    std::span<const NativeScriptOwnerReplacement>,std::string&);
}
