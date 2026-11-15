#pragma once
#include <array>
#include <string_view>

namespace em2 {
struct NativeScriptPersistenceProbe {std::string_view name,source;};
// Runs only in an explicitly opted-in private game's original VM. Most phases
// use allocated roots. The final phase captures/decodes the live script graph;
// a separate private opt-in immediately restores its actual tasks/VM roots at
// the same boundary. The physics phase has its own immediate-restore opt-in.
// No phase rewinds or replays a complete simulation frame.
// The native serializer uses a permanent-object table first, then a root Lua
// closure. Resolving the closure's existing environment avoids serializing all
// C functions in the global environment as raw process addresses.
inline constexpr std::array<NativeScriptPersistenceProbe,10> kNativeScriptPersistenceProbes{{
    {"native library",R"lua(
assert(type(serialize)=='table' and type(serialize.persist)=='function' and type(serialize.unpersist)=='function')
assert(type(coroutine)=='table' and type(coroutine.create)=='function')
return true,0
)lua"},
    {"cyclic tables and shared identity",R"lua(
local env=getfenv(0)
local shared={answer=42}
local root={left=shared,right=shared,flag=true,bytes='a\000b'};root.self=root
local bytes=serialize.persist({[env]='BW_ENV'},function() return root end)
assert(type(bytes)=='string' and #bytes>0)
root.left.answer=99
local restored=serialize.unpersist({BW_ENV=env},bytes)()
assert(restored~=root and restored.self==restored and restored.left==restored.right)
assert(restored.left.answer==42 and restored.flag and restored.bytes=='a\000b' and root.left.answer==99)
return true,#bytes
)lua"},
    {"mutable closure upvalues",R"lua(
local env=getfenv(0)
local function create()
 local value=7
 return {add=function(n)value=value+n;return value end,read=function()return value end}
end
local root=create()
local bytes=serialize.persist({[env]='BW_ENV'},function()return root end)
assert(root.add(10)==17)
local restored=serialize.unpersist({BW_ENV=env},bytes)()
assert(restored.read()==7 and restored.add(2)==9 and restored.read()==9 and root.read()==17)
return true,#bytes
)lua"},
    {"suspended coroutine stack",R"lua(
local env=getfenv(0)
local thread=coroutine.create(function(value)
 local saved={value=value}
 local extra=coroutine.yield(saved)
 return saved.value+extra
end)
local ok,value=coroutine.resume(thread,12);assert(ok and value.value==12)
local bytes=serialize.persist({[env]='BW_ENV',[coroutine.yield]='BW_YIELD'},function()return thread end)
ok,value=coroutine.resume(thread,5);assert(ok and value==17)
local restored=serialize.unpersist({BW_ENV=env,BW_YIELD=coroutine.yield},bytes)()
assert(restored~=thread and coroutine.status(restored)=='suspended')
ok,value=coroutine.resume(restored,30);assert(ok and value==42 and coroutine.status(restored)=='dead')
return true,#bytes
)lua"},
    {"coroutine open-upvalue aliasing",R"lua(
local env=getfenv(0)
local read
local thread=coroutine.create(function()
 local value=12
 read=function()return value end
 coroutine.yield()
 value=value+30
 return value
end)
assert(coroutine.resume(thread));assert(read()==12)
local root={thread=thread,read=read}
local bytes=serialize.persist({[env]='BW_ENV',[coroutine.yield]='BW_YIELD'},function()return root end)
local ok,value=coroutine.resume(thread);assert(ok and value==42 and read()==42)
local restored=serialize.unpersist({BW_ENV=env,BW_YIELD=coroutine.yield},bytes)()
assert(restored.read()==12)
ok,value=coroutine.resume(restored.thread)
assert(ok and value==42 and restored.read()==42 and read()==42)
return true,#bytes
)lua"},
    {"running coroutine rejection",R"lua(
local env=getfenv(0)
local thread=coroutine.create(function()
 local running=coroutine.running();assert(running~=nil,'probe coroutine missing')
 local ok,error_text=pcall(serialize.persist,{[env]='BW_ENV'},function()return running end)
 assert(not ok,'serializer accepted its current coroutine')
 assert(type(error_text)=='string','serializer returned a non-string error')
 assert(string.find(string.lower(error_text),'state',1,true),error_text)
 return true
end)
local ok,accepted=coroutine.resume(thread)
if not ok then error(tostring(accepted)) end
assert(accepted==true,'running-state rejection probe failed')
return true,0
)lua"},
    {"native C++ checkpoint bridge",R"lua(
local env=getfenv(0)
local value=7
local thread=coroutine.create(function()
 local n=7;coroutine.yield(n);n=n+10;coroutine.yield(n);return n+20
end)
local ok,n=coroutine.resume(thread);assert(ok and n==7)
local bytes=serialize.persist({[env]='BW_ENV',[coroutine.yield]='BW_YIELD'},function()return thread end)
ok,n=coroutine.resume(thread);assert(ok and n==17)
local restored=serialize.unpersist({BW_ENV=env,BW_YIELD=coroutine.yield},bytes)()
return {[env]='BW_ENV'},{BW_ENV=env},function(n)value=value+(n or 0);return value end,function()collectgarbage('collect')end,thread,restored
)lua"},
    {"original physics contact replay",R"lua(
return true,0
)lua"},
    {"large local serializer roundtrip",R"lua(
local env=getfenv(0)
local payload=string.rep('Q',262144)
return {[env]='BW_ENV'},{BW_ENV=env},function()return payload end
)lua"},
    {"scheduler shared graph and live capture",R"lua(
local env=getfenv(0)
local perm={[env]='BW_ENV',[coroutine.yield]='BW_YIELD'}
local inverse={BW_ENV=env,BW_YIELD=coroutine.yield}
local registry=debug.getregistry();assert(type(registry)=='table')
perm[registry]='BW_REGISTRY';inverse.BW_REGISTRY=registry
for k,v in next,registry do
 if type(v)=='thread' then
  local ok,status=pcall(coroutine.status,v)
  if ok and status=='running' then perm[v]='BW_MAIN';inverse.BW_MAIN=v end
 end
end
local seen={};local scanned=0;local mapped=0
local function exports(t,path,depth)
 if seen[t] or depth>4 then return end;seen[t]=true
 for k,v in next,t do
  scanned=scanned+1;if scanned>16384 then error('Native API permanence scan exceeded bound')end
  local keytype=type(k)
  if keytype=='string' or keytype=='number' then
   local key=keytype=='string' and ('s'..#k..':'..k)or('n:'..tostring(k))
   local token=path..'/'..key
   if type(v)=='function' then
    local ok,info=pcall(debug.getinfo,v,'S')
    local valid,name=pcall(debug.getupvalue,v,1)
    if ok and info.what=='C' and valid and name==nil then
     perm[v]=token;inverse[token]=v;mapped=mapped+1
    end
   elseif type(v)=='table' then exports(v,token,depth+1)end
  end
 end
end
exports(env,'BW_API',0)
exports(registry,'BW_REGISTRY_API',0)
-- Standard process streams are infrastructure, not rewindable gameplay data.
-- Opened files and arbitrary native userdata remain unsupported unless encoded.
if type(io)=='table' then
 for _,name in ipairs({'stdin','stdout','stderr'})do
  local stream=rawget(io,name)
  if type(stream)=='userdata' then local token='BW_STDIO:'..name;perm[stream]=token;inverse[token]=stream end
 end
end
print('Native scheduler API permanents',mapped)
local shared={value=7}
local fixture_env={counter=shared,removed=42};local fixture_registry={alias=shared,removed=5}
perm[fixture_env]='BW_FIX_ENV';inverse.BW_FIX_ENV=fixture_env
perm[fixture_registry]='BW_FIX_REG';inverse.BW_FIX_REG=fixture_registry
local a=coroutine.create(function()coroutine.yield(shared.value);shared.value=shared.value+1;fixture_env.added=true;fixture_env.removed=nil;fixture_registry.removed=nil;coroutine.yield(shared.value)end)
local b=coroutine.create(function()coroutine.yield(shared.value);shared.value=shared.value+10;fixture_registry.added=true;coroutine.yield(shared.value)end)
local ok,v=coroutine.resume(a);assert(ok and v==7)
ok,v=coroutine.resume(b);assert(ok and v==7)
local rawset_=rawset;local next_=next;local type_=type;local error_=error
local candidates={};local labels={}
for k,v in next_,env do
 local kind=type_(v)
 if type_(k)=='string' and perm[v]==nil and kind~='number' and kind~='boolean' and kind~='string' then candidates[#candidates+1]=k end
end
table.sort(candidates)
for i,k in ipairs(candidates)do labels[i]=k..':'..type_(rawget(env,k))end
local function contents(t)
 local copy={};local count=0;for k,v in next_,t do count=count+1;if count>65536 then error_('VM root table exceeds capture bound')end;copy[k]=v end;return copy
end
local applied_context
local function apply(root)
 local c=root.__bw_vm
 if type_(c)~='table' or type_(c.env)~='table' or type_(c.registry)~='table' or type_(c.env_values)~='table' or type_(c.registry_values)~='table' then error_('Invalid VM root context')end
 if c.mode~=0 then error_('Diagnostic partial VM roots cannot be restored')end
 local function replace(dst,src)
  local keys={};for k in next_,dst do keys[#keys+1]=k end
  for i=1,#keys do rawset_(dst,keys[i],nil)end
  for k,v in next_,src do rawset_(dst,k,v)end
 end
 replace(c.env,c.env_values);replace(c.registry,c.registry_values)
 if c.env==env then applied_context=c end
 if c.env==fixture_env and (c.env.counter~=c.registry.alias or c.env.counter.value~=7 or c.env.removed~=42 or c.registry.removed~=5 or c.env.added~=nil or c.registry.added~=nil)then error_('VM root aliases/state/keys were not restored')end
end
local context_mode=0;local selected_root=''
local observed_env,observed_registry
local function factory(...)
 local root={...};local e,r=env,registry
 local fixture=#root==2 and root[1]==a and root[2]==b
 if fixture then e,r=fixture_env,fixture_registry end
 local mode=fixture and 0 or context_mode
 local globals=mode~=2 and contents(e)or{}
 if not fixture and selected_root~='' then
  if string.sub(selected_root,1,1)=='@' then
   local lo,hi=1,#candidates
   for bit in string.gmatch(string.sub(selected_root,2),'.')do
    local middle=math.floor((lo+hi)/2)
    if bit=='L' then hi=middle elseif bit=='R' then lo=middle+1 else error_('Global partition accepts only L/R')end
   end
   if lo>hi then error_('Global partition is empty')end
   globals={};for i=lo,hi do local k=candidates[i];globals[k]=rawget(env,k)end
  else
   local value=env
   for part in string.gmatch(selected_root,'[^.]+')do
    if type_(value)~='table' then error_('Selected global path crosses a non-table')end
    value=rawget(value,tonumber(part)or part)
   end
   if value==nil then error_('Selected global path is absent')end
   globals={[selected_root]=value}
  end
 end
 root.__bw_vm={env=e,registry=r,env_values=globals,registry_values=(mode~=1 and contents(r)or{}),mode=mode}
 if not fixture then observed_env=contents(env);observed_registry=contents(registry)end
 return function()return root end
end
local function verify_detached(root,...)
 local c=root.__bw_vm
 assert(c.mode==0 and c.env==env and c.registry==registry and type(c.env_values)=='table' and type(c.registry_values)=='table','Decoded VM context identity mismatch')
 local refs={...};assert(#root==#refs,'Decoded task count mismatch')
 for i,ref in ipairs(refs)do assert(type(root[i])=='thread' and root[i]==c.registry_values[ref],'Decoded registry/thread alias mismatch')end
 local function unchanged(current,before)
  for k,v in next_,before do assert(rawget(current,k)==v,'Detached decode changed an original VM binding')end
  for k,v in next_,current do assert(rawget(before,k)==v,'Detached decode added an original VM binding')end
 end
 unchanged(env,observed_env);unchanged(registry,observed_registry)
end
local function verify_applied()
 local c=applied_context;assert(c and c.env==env and c.registry==registry,'No actual context applied')
 local function same(a,b)
  for k,v in next_,a do assert(rawget(b,k)==v,'Applied VM binding differs from snapshot')end
  for k,v in next_,b do assert(rawget(a,k)==v,'Applied VM binding missing from snapshot')end
 end
 same(env,c.env_values);same(registry,c.registry_values)
end
return perm,inverse,
 factory,a,b,apply,function()assert(fixture_env.counter.value==18 and fixture_registry.alias==fixture_env.counter and fixture_env.added and fixture_registry.added and fixture_env.removed==nil and fixture_registry.removed==nil,'VM roots do not share restored coroutine state')end,function(mode,path)context_mode=mode;selected_root=path or'' end,table.concat(labels,';'),verify_detached,verify_applied
)lua"}
}};
}
