//! Automatic-only EM1 standard Lua 5.1 source recovery. No game-script tables.
//! Source is released only after compile-only executable-signature equality.
use std::{ffi::c_void, panic::{catch_unwind, AssertUnwindSafe}, ptr};
mod entities;
mod primary_player;
mod animation_events;
mod engine_bridge;
mod scheduler;
mod global_state;
mod global_bridge;
mod source_modules;
mod callback_preflight;
mod callback_arity;
mod callback_calls;
mod prefab_data;
mod source_vector;
mod source_map;
mod disc_images;

const MAX_CHUNK: usize = 1024 * 1024;
const MAX_SOURCE: usize = 4 * 1024 * 1024;
const MAX_PROTOS: usize = 4096;
const MAX_INSTRUCTIONS: usize = 131072;
const MAX_DEPTH: usize = 96;
const VM_MEMORY: usize = 64 * 1024 * 1024;
const INVALID:u32=1;
const DECOMPILE:u32=2;
const SYNTAX:u32=3;
const DIFFERENT:u32=4;
const BUDGET:u32=5;
const INTERNAL:u32=6;
const REENTRANT:u32=7;
const SCRIPT_FAILED:u32=8;
#[derive(Debug)]struct ScriptTaskFailures(String);
impl std::fmt::Display for ScriptTaskFailures {fn fmt(&self,f:&mut std::fmt::Formatter<'_>)->std::fmt::Result{f.write_str(&self.0)}}
impl std::error::Error for ScriptTaskFailures {}
thread_local!{static ACTIVE_WORLDS:std::cell::RefCell<std::collections::BTreeSet<usize>>=const{std::cell::RefCell::new(std::collections::BTreeSet::new())};}
// Keep ownership outside Em1LuaWorld: inspecting a busy flag inside it would
// already alias the mutable scheduler borrow held by an outer engine tick.
struct WorldAccess(usize);
impl WorldAccess{
    fn enter(world:*const Em1LuaWorld)->Option<Self>{
        let key=world as usize;
        ACTIVE_WORLDS.try_with(|active|active.try_borrow_mut().ok().and_then(|mut set|set.insert(key).then_some(Self(key)))).ok().flatten()
    }
}
impl Drop for WorldAccess{fn drop(&mut self){let _=ACTIVE_WORLDS.try_with(|active|{if let Ok(mut set)=active.try_borrow_mut(){set.remove(&self.0);}});}}
type Fault=(u32,String);
type Checked<T>=Result<T,Fault>;
fn demand(ok:bool,status:u32,reason:&str)->Checked<()> {
    if ok {Ok(())} else {Err((status,reason.into()))}
}

// Use the Windows platform SHA-256 provider, also used by the native project.
// There is no extra crypto executable/runtime or custom hash implementation.
#[link(name="bcrypt")]
unsafe extern "system" {
    fn BCryptOpenAlgorithmProvider(out:*mut *mut c_void,name:*const u16,provider:*const u16,flags:u32)->i32;
    fn BCryptHash(algorithm:*mut c_void,key:*const u8,key_size:u32,input:*const u8,input_size:u32,out:*mut u8,out_size:u32)->i32;
    fn BCryptCloseAlgorithmProvider(algorithm:*mut c_void,flags:u32)->i32;
}
fn sha256(bytes:&[u8])->Checked<[u8;32]> {
    demand(bytes.len()<=u32::MAX as usize,BUDGET,"SHA-256 input exceeds platform length")?;
    let mut handle=ptr::null_mut();let name=[83u16,72,65,50,53,54,0];
    // SAFETY: fixed valid provider name and caller-owned handle/output slots.
    let opened=unsafe{BCryptOpenAlgorithmProvider(&mut handle,name.as_ptr(),ptr::null(),0)};
    demand(opened>=0 && !handle.is_null(),INTERNAL,"Windows SHA-256 provider is unavailable")?;
    struct Algorithm(*mut c_void);
    impl Drop for Algorithm {fn drop(&mut self){unsafe{BCryptCloseAlgorithmProvider(self.0,0);}}}
    let algorithm=Algorithm(handle);let mut digest=[0;32];
    let status=unsafe{BCryptHash(algorithm.0,ptr::null(),0,bytes.as_ptr(),bytes.len() as u32,digest.as_mut_ptr(),32)};
    demand(status>=0,INTERNAL,"Windows SHA-256 calculation failed")?;Ok(digest)
}

// Canonical executable includes header/layout flags, instruction words,
// constants (exact f64 bits), nested prototypes/order and register metadata.
// Only source names, line metadata and debug arrays are omitted. No opcode,
// register, constant ordering, jump, upvalue flag or function normalization.
struct Reader<'a> {
    bytes:&'a [u8],at:usize,little:bool,size_t:usize,output_size_t:usize,
    canonical:Vec<u8>,protos:usize,instructions:usize,
}
impl<'a> Reader<'a> {
    fn take(&mut self,count:usize)->Checked<&'a [u8]> {
        let end=self.at.checked_add(count).ok_or((INVALID,"Lua offset overflow".into()))?;
        let part=self.bytes.get(self.at..end).ok_or((INVALID,"Truncated Lua chunk".into()))?;
        self.at=end;Ok(part)
    }
    fn integer(&mut self,size:usize)->Checked<u64> {
        let little=self.little;let bytes=self.take(size)?;let mut value=0;
        if little {for (i,b) in bytes.iter().enumerate(){value|=u64::from(*b)<<(8*i);}}
        else {for b in bytes{value=(value<<8)|u64::from(*b);}}
        Ok(value)
    }
    fn count(&mut self,limit:usize)->Checked<usize> {
        let n=self.integer(4)? as usize;
        demand(n<=limit && n<=self.bytes.len(),BUDGET,"Lua array exceeds bounded recovery limits")?;Ok(n)
    }
    fn word(&mut self)->Checked<u32> {Ok(self.integer(4)? as u32)}
    fn put(&mut self,value:u32){self.canonical.extend(value.to_le_bytes());}
    fn put_size(&mut self,value:u32){
        if self.output_size_t==8 {self.canonical.extend(u64::from(value).to_le_bytes());}
        else {self.put(value);}
    }
    fn string(&mut self,retain:bool,constant:bool)->Checked<()> {
        let size=self.integer(self.size_t)?;
        demand(size<=self.bytes.len() as u64,INVALID,"Invalid Lua string size")?;
        let text=self.take(size as usize)?;
        demand((!constant || !text.is_empty()) && (text.is_empty() || text.last()==Some(&0)),INVALID,"Invalid Lua string terminator")?;
        if retain {self.put_size(size as u32);self.canonical.extend(text);}
        Ok(())
    }
    fn prototype(&mut self,depth:usize)->Checked<()> {
        demand(depth<MAX_DEPTH,BUDGET,"Lua prototype nesting exceeds recovery limit")?;
        self.protos+=1;demand(self.protos<=MAX_PROTOS,BUDGET,"Too many Lua prototypes")?;
        self.string(false,false)?;self.word()?;self.word()?;
        self.put_size(0);self.put(0);self.put(0);
        let flags=self.take(4)?;
        demand(flags[0]<=60 && flags[1]<=flags[3] && flags[2]&!7==0 && (2..=250).contains(&flags[3]),INVALID,"Invalid Lua register/upvalue metadata")?;
        self.canonical.extend(flags);
        let instructions=self.count(MAX_INSTRUCTIONS)?;
        self.instructions+=instructions;
        demand(self.instructions<=MAX_INSTRUCTIONS,BUDGET,"Too many Lua instructions")?;
        self.put(instructions as u32);
        for _ in 0..instructions {let value=self.word()?;self.put(value);}
        let constants=self.count(MAX_INSTRUCTIONS)?;self.put(constants as u32);
        for _ in 0..constants {
            let tag=self.take(1)?[0];self.canonical.push(tag);
            match tag {
                0=>{},
                1=>{let b=self.take(1)?[0];demand(b<=1,INVALID,"Invalid Lua boolean")?;self.canonical.push(b);},
                3=>{let bits=self.integer(8)?;self.canonical.extend(bits.to_le_bytes());},
                4=>self.string(true,true)?,
                _=>return Err((INVALID,format!("Unsupported Lua constant tag {tag}"))),
            }
        }
        let children=self.count(MAX_PROTOS)?;self.put(children as u32);
        for _ in 0..children {self.prototype(depth+1)?;}
        let lines=self.count(MAX_INSTRUCTIONS)?;demand(lines==0 || lines==instructions,INVALID,"Invalid Lua line table")?;
        self.take(lines.checked_mul(4).ok_or((INVALID,"Lua line size overflow".into()))?)?;
        let locals=self.count(MAX_INSTRUCTIONS)?;
        for _ in 0..locals {
            self.string(false,false)?;let start=self.word()?;let end=self.word()?;
            demand(start<=end && end<=instructions as u32,INVALID,"Invalid Lua local lifetime")?;
        }
        let upvalues=self.count(60)?;
        for _ in 0..upvalues {self.string(false,false)?;}
        self.put(0);self.put(0);self.put(0);Ok(())
    }
}
fn executable(bytes:&[u8])->Checked<Vec<u8>> {
    demand(bytes.len()<=MAX_CHUNK,BUDGET,"Original Lua chunk exceeds input limit")?;
    executable_with_layout(bytes,4)
}
// Endian/size_t conversion only, retaining every instruction and exact number
// bit. This produces standard Lua 5.1 bytecode, never HavokScript bytecode.
fn executable_with_layout(bytes:&[u8],output_size_t:usize)->Checked<Vec<u8>> {
    demand([4,8].contains(&output_size_t),INVALID,"Unsupported Lua output size_t")?;
    demand((12..=MAX_CHUNK*2).contains(&bytes.len()),BUDGET,"Lua transport chunk must contain 12..2097152 bytes")?;
    demand(&bytes[..6]==b"\x1bLua\x51\0" && bytes[6]<=1 && bytes[7]==4 && [4,8].contains(&bytes[8]) && bytes[9]==4 && bytes[10]==8 && bytes[11]==0,
        INVALID,"Expected standard Lua 5.1 format0 with 32-bit instructions and f64 numbers; HavokScript bytecode is not accepted")?;
    let mut reader=Reader{bytes,at:12,little:bytes[6]!=0,size_t:bytes[8] as usize,output_size_t,
        canonical:b"\x1bLua\x51\0\x01\x04\x04\x04\x08\0".to_vec(),protos:0,instructions:0};
    reader.canonical[8]=output_size_t as u8;
    reader.prototype(0)?;demand(reader.at==bytes.len(),INVALID,"Unexpected trailing Lua chunk data")?;
    Ok(reader.canonical)
}
fn compile_only(source:&str)->Checked<Vec<u8>> {
    demand(source.len()<=MAX_SOURCE && !source.as_bytes().contains(&0),BUDGET,"Recovered source exceeds limit or contains NUL")?;
    let vm=mlua::Lua::new_with(mlua::StdLib::NONE,mlua::LuaOptions::default()).map_err(|e|(INTERNAL,e.to_string()))?;
    vm.set_memory_limit(VM_MEMORY).map_err(|e|(INTERNAL,e.to_string()))?;
    let function=vm.load(source).set_name("@EM1Automatic").into_function().map_err(|e|(SYNTAX,format!("Automatic source failed compile-only Lua 5.1 validation: {e}")))?;
    let bytes=function.dump(true);
    demand(bytes.len()<=MAX_CHUNK,BUDGET,"Recompiled Lua chunk exceeds limit")?;
    Ok(bytes)
}

pub struct Em1LuaRecovery {
    source:Vec<u8>,error:Vec<u8>,original:[u8;32],executable:[u8;32],status:u32,
}
impl Em1LuaRecovery {
    fn failure(status:u32,error:impl Into<String>)->Self {
        let mut text=error.into();if text.len()>4096 {let mut end=4096;while !text.is_char_boundary(end){end-=1;}text.truncate(end);}
        Self{source:vec![],error:text.into_bytes(),original:[0;32],executable:[0;32],status}
    }
}
fn recover(bytes:&[u8])->Checked<Em1LuaRecovery> {
    let normalized=executable(bytes)?;
    let original=sha256(bytes)?;let signature=sha256(&normalized)?;
    let options=unluac::decompile::DecompileOptions{dialect:unluac::decompile::DecompileDialect::Lua51,..Default::default()};
    let reconstructed=unluac::decompile::decompile(bytes,options).map_err(|e|(DECOMPILE,format!("Automatic Lua recovery failed: {e}")))?;
    let source=reconstructed.state.generated.ok_or((DECOMPILE,"Automatic Lua recovery produced no source".into()))?.source;
    let recompiled=compile_only(&source)?;
    let rebuilt=executable(&recompiled)?;
    // Compare canonical bytes themselves as well as publishing their hash.
    if normalized!=rebuilt {
        let offset=normalized.iter().zip(&rebuilt).position(|(a,b)|a!=b).unwrap_or(normalized.len().min(rebuilt.len()));
        return Err((DIFFERENT,format!("Automatic source compiled but changed executable layout at canonical byte {offset} ({} -> {} bytes); source refused",normalized.len(),rebuilt.len())));
    }
    Ok(Em1LuaRecovery{source:source.into_bytes(),error:vec![],original,executable:signature,status:0})
}
fn contain(body:impl FnOnce()->Checked<Em1LuaRecovery>)->Em1LuaRecovery {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(value))=>value,Ok(Err((status,error)))=>Em1LuaRecovery::failure(status,error),
        Err(_)=>Em1LuaRecovery::failure(INTERNAL,"Automatic Lua recovery panicked; no source was accepted"),
    }
}
#[repr(C)]
pub struct Em1LuaRecoveryView {
    source:*const u8,source_size:usize,error:*const u8,error_size:usize,
    original_sha256:[u8;32],executable_sha256:[u8;32],status:u32,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_recover(input:*const u8,length:usize)->*mut Em1LuaRecovery {
    let result=contain(||{
        demand(!input.is_null(),INVALID,"Original Lua input pointer is null")?;
        demand((12..=MAX_CHUNK).contains(&length),BUDGET,"Original Lua chunk exceeds recovery size limit")?;
        // SAFETY: caller supplies a readable span; sizes checked before creation.
        recover(unsafe{std::slice::from_raw_parts(input,length)})
    });Box::into_raw(Box::new(result))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_view(result:*const Em1LuaRecovery,output:*mut Em1LuaRecoveryView)->u32 {
    if result.is_null() || output.is_null(){return INVALID;}
    let result=unsafe{&*result};
    unsafe{ptr::write(output,Em1LuaRecoveryView{source:result.source.as_ptr(),source_size:result.source.len(),
        error:result.error.as_ptr(),error_size:result.error.len(),original_sha256:result.original,
        executable_sha256:result.executable,status:result.status});}result.status
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_release(result:*mut Em1LuaRecovery) {
    if !result.is_null(){let _=catch_unwind(AssertUnwindSafe(||{drop(unsafe{Box::from_raw(result)});}));}
}

pub struct Em1LuaChunk {
    bytes:Vec<u8>,error:Vec<u8>,original:[u8;32],executable:[u8;32],status:u32,
}
impl Em1LuaChunk {
    fn failure(status:u32,error:impl Into<String>)->Self {
        let failure=Em1LuaRecovery::failure(status,error);
        Self{bytes:vec![],error:failure.error,original:[0;32],executable:[0;32],status}
    }
}
fn prepare_chunk(original:&[u8])->Checked<Em1LuaChunk> {
    let signature=executable(original)?;
    let bytes=executable_with_layout(original,std::mem::size_of::<usize>())?;
    demand(bytes.len()<=MAX_CHUNK*2,BUDGET,"Native Lua chunk exceeds transport limit")?;
    let vm=mlua::Lua::new_with(mlua::StdLib::NONE,mlua::LuaOptions::default()).map_err(|e|(INTERNAL,e.to_string()))?;
    vm.set_memory_limit(VM_MEMORY).map_err(|e|(INTERNAL,e.to_string()))?;
    let loaded=vm.load(&bytes).set_mode(mlua::ChunkMode::Binary).into_function().map_err(|e|(INVALID,format!("Native Lua loader rejected original chunk: {e}")))?;
    demand(executable_with_layout(&loaded.dump(true),4)?==signature,DIFFERENT,"Native Lua load changed executable contents")?;
    Ok(Em1LuaChunk{bytes,error:vec![],original:sha256(original)?,executable:sha256(&signature)?,status:0})
}
#[repr(C)]
pub struct Em1LuaChunkView {
    bytecode:*const u8,bytecode_size:usize,error:*const u8,error_size:usize,
    original_sha256:[u8;32],executable_sha256:[u8;32],status:u32,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_prepare_chunk(input:*const u8,length:usize)->*mut Em1LuaChunk {
    let result=catch_unwind(AssertUnwindSafe(||{
        demand(!input.is_null(),INVALID,"Original Lua input pointer is null")?;
        demand((12..=MAX_CHUNK).contains(&length),BUDGET,"Original Lua chunk exceeds input limit")?;
        // Caller owns a readable input span for this call only.
        prepare_chunk(unsafe{std::slice::from_raw_parts(input,length)})
    }));
    Box::into_raw(Box::new(match result {
        Ok(Ok(value))=>value,Ok(Err((status,error)))=>Em1LuaChunk::failure(status,error),
        Err(_)=>Em1LuaChunk::failure(INTERNAL,"Native Lua chunk preparation panicked; no bytecode accepted"),
    }))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_chunk_view(result:*const Em1LuaChunk,output:*mut Em1LuaChunkView)->u32 {
    if result.is_null() || output.is_null(){return INVALID;}
    let result=unsafe{&*result};
    unsafe{ptr::write(output,Em1LuaChunkView{bytecode:result.bytes.as_ptr(),bytecode_size:result.bytes.len(),
        error:result.error.as_ptr(),error_size:result.error.len(),original_sha256:result.original,
        executable_sha256:result.executable,status:result.status});}result.status
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_chunk_release(result:*mut Em1LuaChunk) {
    if !result.is_null(){let _=catch_unwind(AssertUnwindSafe(||{drop(unsafe{Box::from_raw(result)});}));}
}

fn definition_vm()->mlua::Result<mlua::Lua> {
    let vm=mlua::Lua::new_with(mlua::StdLib::MATH|mlua::StdLib::STRING|mlua::StdLib::TABLE,mlua::LuaOptions::default())?;
    vm.set_memory_limit(VM_MEMORY)?;
    // No engine bridge, file loaders, dynamic compilation, coroutine escape,
    // error-catching loop or userdata finalizers in this initialization probe.
    let yield_function:mlua::Function=vm.globals().get::<mlua::Table>("coroutine")?.get("yield")?;
    vm.set_named_registry_value(scheduler::YIELD_REGISTRY,yield_function)?;
    for name in ["loadfile","dofile","load","loadstring","require","module","pcall","xpcall","coroutine","newproxy"] {
        vm.globals().raw_set(name,mlua::Value::Nil)?;
    }
    let ticks=std::cell::Cell::new(0u32);
    vm.set_hook(mlua::HookTriggers::new().every_nth_instruction(1000),move |_,_|{
        ticks.set(ticks.get()+1);
        if ticks.get()>=100 {Err(mlua::Error::RuntimeError("Original script initialization instruction budget exceeded".into()))}
        else {Ok(mlua::VmState::Continue)}
    })?;
    Ok(vm)
}

#[repr(C)]
pub struct Em1LuaWorldInput { bytecode:*const u8,bytecode_size:usize,executable_sha256:[u8;32] }
#[repr(C)]
pub struct Em1LuaWorldSource {chunk:Em1LuaWorldInput,path:*const u8,path_size:usize,startup:u32,reserved:u32}
pub struct Em1LuaWorld { _vm:Option<mlua::Lua>,error:Vec<u8>,status:u32,scripts:u32,functions:u32,globals:u32,player:u32,player_generation:u64,scheduler:scheduler::Scheduler,native_globals:Option<global_state::Database>,global_bridge:global_bridge::Bridge,global_schema:[u8;32],entities:entities::Registry,engine_bridge:engine_bridge::Bridge }
// Opaque native identity, never a guest pointer exposed as a Lua number.
// Future entity-consuming bindings must check both fields against live ownership.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
struct PlayerEntity { entity:u32,generation:u64 }
impl mlua::UserData for PlayerEntity {}
const PLAYER_REGISTRY:&str="em1.native.current_player";
#[repr(C)]
pub struct Em1LuaWorldView { status:u32,scripts:u32,functions:u32,globals:u32,error:*const u8,error_size:usize }
#[repr(C)]
pub struct Em1GlobalSummary {count:u32,id_limit:u32,nonzero:u32,flagged:u32}
#[repr(C)]
pub struct Em1GlobalDefinitionView {name:*const u8,name_size:usize,id:u32,initial:i32,flag:u32,native_id:u32}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_load_globals(world:*mut Em1LuaWorld,bytes:*const u8,size:usize)->u32 {
    unsafe{world_operation(world,|world|{
        if bytes.is_null() || !(8..=4*1024*1024).contains(&size){return Err(mlua::Error::RuntimeError("Invalid original global database span".into()));}
        let db=global_state::parse(std::slice::from_raw_parts(bytes,size)).map_err(mlua::Error::RuntimeError)?;
        let hash=sha256(std::slice::from_raw_parts(bytes,size)).map_err(|(_,e)|mlua::Error::RuntimeError(e))?;
        world.native_globals=Some(db);world.global_schema=hash;Ok(())
    })}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_global_schema(world:*const Em1LuaWorld,out:*mut u8)->u32 {
    if world.is_null() || out.is_null(){return INVALID;}
    let Some(_access)=WorldAccess::enter(world)else{return REENTRANT;};
    let world=unsafe{&*world};if world.native_globals.is_none(){return INVALID;}
    unsafe{ptr::copy_nonoverlapping(world.global_schema.as_ptr(),out,32);}0
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_global_summary(world:*const Em1LuaWorld,out:*mut Em1GlobalSummary)->u32 {
    if world.is_null() || out.is_null(){return INVALID;}
    let Some(_access)=WorldAccess::enter(world)else{return REENTRANT;};
    let Some(db)=(unsafe{&*world}).native_globals.as_ref() else{return INVALID;};
    unsafe{ptr::write(out,Em1GlobalSummary{count:db.entries.len() as u32,id_limit:db.id_limit as u32,nonzero:db.entries.iter().filter(|e|e.initial!=0).count() as u32,flagged:db.entries.iter().filter(|e|e.flag).count() as u32});}0
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_global_definition(world:*const Em1LuaWorld,index:usize,out:*mut Em1GlobalDefinitionView)->u32 {
    if world.is_null() || out.is_null(){return INVALID;}
    let Some(_access)=WorldAccess::enter(world)else{return REENTRANT;};
    let Some(entry)=(unsafe{&*world}).native_globals.as_ref().and_then(|db|db.entries.get(index)) else{return INVALID;};
    unsafe{ptr::write(out,Em1GlobalDefinitionView{name:entry.name.as_ptr(),name_size:entry.name.len(),id:entry.id as u32,initial:entry.initial as i32,flag:entry.flag as u32,native_id:entry.native_id as u32});}0
}
impl Em1LuaWorld {
    fn failure(status:u32,error:impl Into<String>)->Self {
        Self{_vm:None,error:Em1LuaRecovery::failure(status,error).error,status,scripts:0,functions:0,globals:0,player:0,player_generation:0,scheduler:scheduler::Scheduler::default(),native_globals:None,global_bridge:global_bridge::Bridge::default(),global_schema:[0;32],entities:entities::Registry::default(),engine_bridge:engine_bridge::Bridge::default()}
    }
}
unsafe fn initialize_world(inputs:*const Em1LuaWorldInput,count:usize)->Checked<Em1LuaWorld> {
    unsafe{initialize_world_sources(inputs,count,None)}
}
unsafe fn initialize_world_sources(inputs:*const Em1LuaWorldInput,count:usize,modules:Option<Vec<(String,bool)>>)->Checked<Em1LuaWorld> {
    demand(!inputs.is_null() && (1..=512).contains(&count),INVALID,"Expected 1..512 prepared scripts")?;
    let inputs=unsafe{std::slice::from_raw_parts(inputs,count)};
    let vm=definition_vm().map_err(|e|(INTERNAL,e.to_string()))?;
    let global_bridge=global_bridge::install(&vm).map_err(|e|(INTERNAL,e.to_string()))?;
    let entities=entities::Registry::default();
    let engine_bridge=engine_bridge::install(&vm,entities.clone()).map_err(|e|(INTERNAL,e.to_string()))?;
    let thread_dispatch=scheduler::install_fire_thread(&vm).map_err(|e|(INTERNAL,e.to_string()))?;
    let mut total=0usize;let mut programs=Vec::with_capacity(count);
    for input in inputs{
        demand(!input.bytecode.is_null() && (12..=MAX_CHUNK*2).contains(&input.bytecode_size),INVALID,"Invalid prepared script span")?;
        total+=input.bytecode_size;demand(total<=64*1024*1024,BUDGET,"Prepared scripts exceed world byte limit")?;
        let bytes=unsafe{std::slice::from_raw_parts(input.bytecode,input.bytecode_size)};
        demand(sha256(&executable_with_layout(bytes,4)?)?==input.executable_sha256,DIFFERENT,"Prepared script executable hash mismatch")?;
        programs.push(std::sync::Arc::<[u8]>::from(bytes));
    }
    if let Some(names)=&modules{
        let mut library=std::collections::BTreeMap::new();
        for ((name,_),bytes) in names.iter().zip(&programs){demand(library.insert(name.clone(),bytes.clone()).is_none(),INVALID,"Duplicate original Lua module path")?;}
        source_modules::install(&vm,library).map_err(|e|(INVALID,e.to_string()))?;
    }
    let baseline=vm.globals().pairs::<String,mlua::Value>().map(|p|p.map(|p|p.0)).collect::<mlua::Result<std::collections::BTreeSet<_>>>().map_err(|e|(INTERNAL,e.to_string()))?;
    for (i,bytes) in programs.iter().enumerate(){
        if modules.as_ref().is_some_and(|names|!names[i].1){continue;}
        let name=modules.as_ref().map_or_else(||format!("Script {i}"),|names|names[i].0.clone());
        vm.load(bytes.as_ref()).set_mode(mlua::ChunkMode::Binary).set_name(&name).exec().map_err(|e|(INVALID,format!("{name} initialization failed: {e}")))?;
    }
    let mut functions=0;let mut globals=0;
    for pair in vm.globals().pairs::<String,mlua::Value>() {
        let (name,value)=pair.map_err(|e|(INVALID,e.to_string()))?;if baseline.contains(&name){continue;}globals+=1;
        if matches!(value,mlua::Value::Function(_)){functions+=1;}
    }
    vm.set_named_registry_value(PLAYER_REGISTRY,mlua::Value::Nil).map_err(|e|(INTERNAL,e.to_string()))?;
    let get_player=vm.create_function(|lua,()|lua.named_registry_value::<mlua::Value>(PLAYER_REGISTRY)).map_err(|e|(INTERNAL,e.to_string()))?;
    vm.globals().raw_set("GetPlayer",get_player).map_err(|e|(INTERNAL,e.to_string()))?;
    callback_preflight::remember_single_result(&vm,"GetPlayer").map_err(|e|(INTERNAL,e.to_string()))?;
    if modules.is_none(){scheduler::install_wait(&vm).map_err(|e|(INTERNAL,e.to_string()))?;}
    Ok(Em1LuaWorld{_vm:Some(vm),error:vec![],status:0,scripts:count as u32,functions,globals,player:0,player_generation:0,scheduler:scheduler::Scheduler::with_dispatch(thread_dispatch),native_globals:None,global_bridge,global_schema:[0;32],entities,engine_bridge})
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_bind_entity(world:*mut Em1LuaWorld,id:u32)->u32 {
    unsafe{world_operation(world,|world|world.entities.bind(world._vm.as_ref().unwrap(),id))}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_unbind_entity(world:*mut Em1LuaWorld,id:u32)->u32 {
    unsafe{world_operation(world,|world|{let vm=world._vm.as_ref().unwrap();
        if let Ok(identity)=world.entities.snapshot(id){let generation=identity.borrow::<entities::Entity>()?.generation;prefab_data::forget_generation(vm,generation)?;}
        primary_player::revoke(vm,id)?;world.entities.unbind(id);Ok(())})}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_set_bound_player(world:*mut Em1LuaWorld,id:u32)->u32 {
    unsafe{world_operation(world,|world|{world.player=0;primary_player::set(world._vm.as_ref().unwrap(),&world.entities,id)})}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_set_player(world:*mut Em1LuaWorld,entity:u32)->u32 {
    if world.is_null(){return INVALID;}
    let Some(_access)=WorldAccess::enter(world)else{return REENTRANT;};
    match catch_unwind(AssertUnwindSafe(||->mlua::Result<()>{
        let world=unsafe{&mut *world};
        let vm=world._vm.as_ref().ok_or_else(||mlua::Error::RuntimeError("World is not initialized".into()))?;
        if world.player==entity && entity!=0{return Ok(());}
        // Invalidate first: an allocation failure must not preserve a stale actor.
        vm.set_named_registry_value(PLAYER_REGISTRY,mlua::Value::Nil)?;
        world.player=0;world.player_generation=world.player_generation.checked_add(1).ok_or_else(||mlua::Error::RuntimeError("Entity generation exhausted".into()))?;
        if entity!=0 {
            let handle=vm.create_userdata(PlayerEntity{entity,generation:world.player_generation})?;
            vm.set_named_registry_value(PLAYER_REGISTRY,handle)?;world.player=entity;
        }
        Ok(())
    })) {Ok(Ok(()))=>0,_=>INTERNAL}
}
unsafe fn world_operation(world:*mut Em1LuaWorld,action:impl FnOnce(&mut Em1LuaWorld)->mlua::Result<()>)->u32 {
    if world.is_null(){return INVALID;}
    let Some(_access)=WorldAccess::enter(world)else{return REENTRANT;};
    let world=unsafe{&mut *world};
    if world._vm.is_none(){return INVALID;}
    match catch_unwind(AssertUnwindSafe(||action(world))) {
        Ok(Ok(()))=>{world.error.clear();0},
        Ok(Err(e))=>{let status=match &e{mlua::Error::ExternalError(value) if value.is::<ScriptTaskFailures>()=>SCRIPT_FAILED,_=>INVALID};world.error=Em1LuaRecovery::failure(status,e.to_string()).error;status},
        Err(_)=>{world.scheduler.cancel();world.error=b"Callback scheduler panicked; pending callbacks cancelled".to_vec();INTERNAL},
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_start(world:*mut Em1LuaWorld,name:*const u8,length:usize)->u32 {
    unsafe{em1_lua_world_start_args(world,name,length,ptr::null(),0)}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_callback_exists(world:*mut Em1LuaWorld,name:*const u8,length:usize,exists:*mut u32)->u32 {
    unsafe{world_operation(world,|world|{
        if exists.is_null() || name.is_null() || length==0 || length>256{return Err(mlua::Error::RuntimeError("Invalid callback lookup span".into()));}
        let name=std::str::from_utf8(std::slice::from_raw_parts(name,length)).map_err(|_|mlua::Error::RuntimeError("Invalid callback lookup UTF-8".into()))?;
        if name.contains('\0'){return Err(mlua::Error::RuntimeError("Invalid callback lookup name".into()));}
        let value=world._vm.as_ref().unwrap().globals().raw_get::<mlua::Value>(name)?;
        *exists=u32::from(matches!(value,mlua::Value::Function(_)));Ok(())
    })}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_preflight_callback(world:*mut Em1LuaWorld,name:*const u8,length:usize,capabilities:u32)->u32 {
    unsafe{world_operation(world,|world|{
        if name.is_null() || length==0 || length>256{return Err(mlua::Error::RuntimeError("Invalid callback preflight name".into()));}
        let name=std::str::from_utf8(std::slice::from_raw_parts(name,length)).map_err(|_|mlua::Error::RuntimeError("Invalid callback preflight UTF-8".into()))?;
        if name.contains('\0'){return Err(mlua::Error::RuntimeError("Invalid callback preflight name".into()));}
        callback_preflight::check(world._vm.as_ref().unwrap(),name,capabilities)
    })}
}
#[repr(C)]
pub struct Em1LuaArgument {kind:u32,reserved:u32,number:f64,bytes:*const u8,size:usize}
unsafe fn callback_values(world:&Em1LuaWorld,args:*const Em1LuaArgument,count:usize)->mlua::Result<mlua::MultiValue> {
    unsafe {
    let bad=||mlua::Error::RuntimeError("Invalid EM1 callback argument".into());
        if count>16 || (count!=0 && args.is_null()){return Err(bad());}
        let args=if count==0{&[]}else{std::slice::from_raw_parts(args,count)};
        let vm=world._vm.as_ref().unwrap();let mut values=Vec::with_capacity(count);let mut total=0;
        for arg in args {
            if arg.reserved!=0{return Err(bad());}
            values.push(match arg.kind {
                0=>mlua::Value::Nil,
                1 if arg.number==0.0 || arg.number==1.0=>mlua::Value::Boolean(arg.number!=0.0),
                2 if arg.number.is_finite()=>mlua::Value::Number(arg.number),
                3=>{
                    if arg.size>65536 || (arg.size!=0 && arg.bytes.is_null()){return Err(bad());}
                    total+=arg.size;if total>262144{return Err(bad());}
                    let bytes=if arg.size==0{&[]}else{std::slice::from_raw_parts(arg.bytes,arg.size)};
                    mlua::Value::String(vm.create_string(bytes)?)
                },
                4=>{let value=vm.named_registry_value::<mlua::Value>(PLAYER_REGISTRY)?;if !matches!(value,mlua::Value::UserData(_)){return Err(bad());}value},
                5 if arg.number.is_finite() && arg.number>=1.0 && arg.number<=u32::MAX as f64 && arg.number.fract()==0.0=>
                    mlua::Value::UserData(world.entities.snapshot(arg.number as u32)?),
                6=>{
                    if arg.bytes.is_null() || arg.size==0 || arg.size>256{return Err(bad());}
                    let bytes=std::slice::from_raw_parts(arg.bytes,arg.size);
                    let name=std::str::from_utf8(bytes).map_err(|_|bad())?;
                    if name.contains('\0'){return Err(bad());}
                    mlua::Value::Function(vm.globals().raw_get::<mlua::Function>(name)?)
                },
                _=>return Err(bad()),
            });
        }
        Ok(mlua::MultiValue::from_vec(values))
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_start_args(world:*mut Em1LuaWorld,name:*const u8,length:usize,args:*const Em1LuaArgument,count:usize)->u32 {
    unsafe{em1_lua_world_start_args_after(world,name,length,args,count,0.0)}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_start_args_after(world:*mut Em1LuaWorld,name:*const u8,length:usize,args:*const Em1LuaArgument,count:usize,delay:f64)->u32 {
    unsafe{world_operation(world,|world|{
        if name.is_null() || length==0 || length>256{return Err(mlua::Error::RuntimeError("Invalid callback name span".into()));}
        let name=std::str::from_utf8(std::slice::from_raw_parts(name,length)).map_err(|_|mlua::Error::RuntimeError("Invalid callback name UTF-8".into()))?;
        if name.contains('\0'){return Err(mlua::Error::RuntimeError("Invalid callback name".into()));}
        let values=callback_values(world,args,count)?;
        let vm=world._vm.as_ref().unwrap();
        world.scheduler.start_args_after(vm,name,values,delay)
    })}
}
#[repr(C)]
pub struct Em1LuaEventInput {name:*const u8,name_size:usize,args:*const Em1LuaArgument,count:usize,delay:f64}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_start_batch(world:*mut Em1LuaWorld,events:*const Em1LuaEventInput,count:usize)->u32 {
    unsafe{world_operation(world,|world|{
        if count>256 || (count!=0 && events.is_null()){return Err(mlua::Error::RuntimeError("Invalid original event batch".into()));}
        let events=if count==0{&[]}else{std::slice::from_raw_parts(events,count)};
        let mut staged=world.scheduler.clone();let vm=world._vm.as_ref().unwrap();
        for event in events{
            if event.name.is_null() || event.name_size==0 || event.name_size>256{return Err(mlua::Error::RuntimeError("Invalid batch callback name".into()));}
            let name=std::str::from_utf8(std::slice::from_raw_parts(event.name,event.name_size)).map_err(|_|mlua::Error::RuntimeError("Invalid batch callback UTF-8".into()))?;
            if name.contains('\0'){return Err(mlua::Error::RuntimeError("Invalid batch callback name".into()));}
            let values=callback_values(world,event.args,event.count)?;
            staged.start_args_after(vm,name,values,event.delay)?;
        }
        world.scheduler=staged;Ok(())
    })}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_tick(world:*mut Em1LuaWorld,simulation_delta:f64)->u32 {
    unsafe{world_operation(world,|world|{
        let failures=world.scheduler.tick(simulation_delta)?;
        if failures.is_empty(){Ok(())}else{Err(mlua::Error::external(ScriptTaskFailures(failures.join("\n"))))}
    })}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_tick_native(world:*mut Em1LuaWorld,simulation_delta:f64,callbacks:*const global_bridge::Callbacks)->u32 {
    if callbacks.is_null(){return INVALID;}
    unsafe{world_operation(world,|world|{
        let _scope=world.global_bridge.enter(*callbacks)?;
        let failures=world.scheduler.tick(simulation_delta)?;
        if failures.is_empty(){Ok(())}else{Err(mlua::Error::external(ScriptTaskFailures(failures.join("\n"))))}
    })}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_tick_engine(world:*mut Em1LuaWorld,delta:f64,globals:*const global_bridge::Callbacks,engine:*const engine_bridge::Callbacks)->u32 {
    if globals.is_null() || engine.is_null(){return INVALID;}
    unsafe{world_operation(world,|world|{
        let _globals=world.global_bridge.enter(*globals)?;
        let _engine=world.engine_bridge.enter(*engine)?;
        let failures=world.scheduler.tick(delta)?;
        if failures.is_empty(){Ok(())}else{Err(mlua::Error::external(ScriptTaskFailures(failures.join("\n"))))}
    })}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_reset_prefab_data(world:*mut Em1LuaWorld)->u32 {
    unsafe{world_operation(world,|world|prefab_data::clear(world._vm.as_ref().unwrap()))}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_reset_map(world:*mut Em1LuaWorld)->u32 {
    unsafe{world_operation(world,|world|source_map::clear(world._vm.as_ref().unwrap()))}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_map_view(world:*const Em1LuaWorld,out:*mut source_map::View)->u32 {
    if world.is_null() || out.is_null(){return INVALID;}
    let Some(_access)=WorldAccess::enter(world)else{return REENTRANT;};
    let Some(vm)=(unsafe{&*world})._vm.as_ref()else{return INVALID;};
    match source_map::view(vm){Ok(view)=>{unsafe{ptr::write(out,view);}0},Err(_)=>INVALID}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_cancel(world:*mut Em1LuaWorld)->u32 {
    unsafe{world_operation(world,|world|{world.scheduler.cancel();Ok(())})}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_scheduler_stats(world:*const Em1LuaWorld,out:*mut scheduler::Stats)->u32 {
    if world.is_null() || out.is_null(){return INVALID;}
    let Some(_access)=WorldAccess::enter(world)else{return REENTRANT;};
    match catch_unwind(AssertUnwindSafe(||{
        let world=unsafe{&*world};if world._vm.is_none(){return INVALID;}
        unsafe{ptr::write(out,world.scheduler.stats())};0
    })){Ok(status)=>status,Err(_)=>INTERNAL}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_load_animation_events(world:*mut Em1LuaWorld,bytes:*const u8,size:usize)->u32 {
    if bytes.is_null() || size==0 || size>32*1024*1024{return INVALID;}
    unsafe{world_operation(world,|world|{animation_events::install(world._vm.as_ref().unwrap(),std::slice::from_raw_parts(bytes,size))?;Ok(())})}
}
#[repr(C)]pub struct Em1LuaAnimationVariableTarget{name:*const u8,length:usize,id:u32,reserved:u32}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_install_native_animation_variables(world:*mut Em1LuaWorld,targets:*const Em1LuaAnimationVariableTarget,count:usize)->u32{
    unsafe{world_operation(world,|world|{
        if targets.is_null() || !(1..=4096).contains(&count){return Err(mlua::Error::RuntimeError("Invalid checked native animation target span".into()));}
        let mut catalog=std::collections::BTreeMap::new();
        for target in std::slice::from_raw_parts(targets,count){
            if target.reserved!=0 || target.name.is_null() || !(1..=127).contains(&target.length){return Err(mlua::Error::RuntimeError("Invalid checked native animation target record".into()));}
            let name=std::str::from_utf8(std::slice::from_raw_parts(target.name,target.length)).map_err(mlua::Error::external)?.to_owned();
            if catalog.insert(name,target.id).is_some(){return Err(mlua::Error::RuntimeError("Duplicate checked native animation variable name".into()));}
        }
        animation_events::install_targets(world._vm.as_ref().unwrap(),catalog)
    })}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_create(inputs:*const Em1LuaWorldInput,count:usize)->*mut Em1LuaWorld {
    let result=catch_unwind(AssertUnwindSafe(||unsafe{initialize_world(inputs,count)}));
    Box::into_raw(Box::new(match result {
        Ok(Ok(world))=>world,Ok(Err((status,error)))=>Em1LuaWorld::failure(status,error),
        Err(_)=>Em1LuaWorld::failure(INTERNAL,"Script initialization panicked; staged world discarded"),
    }))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_create_sources(inputs:*const Em1LuaWorldSource,count:usize)->*mut Em1LuaWorld {
    let result=catch_unwind(AssertUnwindSafe(||->Checked<Em1LuaWorld>{
        demand(!inputs.is_null() && (1..=512).contains(&count),INVALID,"Expected 1..512 original script modules")?;
        let inputs=unsafe{std::slice::from_raw_parts(inputs,count)};let mut chunks=Vec::with_capacity(count);let mut names=Vec::with_capacity(count);
        for input in inputs{
            demand(input.reserved==0 && input.startup<=1 && !input.path.is_null() && (1..=2048).contains(&input.path_size),INVALID,"Invalid original script module metadata")?;
            let path=std::str::from_utf8(unsafe{std::slice::from_raw_parts(input.path,input.path_size)}).map_err(|_|(INVALID,"Invalid original script path UTF-8".into()))?;
            let path=source_modules::key(path).map_err(|e|(INVALID,e.to_string()))?;
            chunks.push(Em1LuaWorldInput{bytecode:input.chunk.bytecode,bytecode_size:input.chunk.bytecode_size,executable_sha256:input.chunk.executable_sha256});
            names.push((path,input.startup!=0));
        }
        demand(names.iter().any(|(_,startup)|*startup),INVALID,"Original script library has no startup module")?;
        unsafe{initialize_world_sources(chunks.as_ptr(),count,Some(names))}
    }));
    Box::into_raw(Box::new(match result{
        Ok(Ok(world))=>world,Ok(Err((status,error)))=>Em1LuaWorld::failure(status,error),
        Err(_)=>Em1LuaWorld::failure(INTERNAL,"Original module initialization panicked; staged world discarded"),
    }))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_view(world:*const Em1LuaWorld,output:*mut Em1LuaWorldView)->u32 {
    if world.is_null() || output.is_null(){return INVALID;}
    let Some(_access)=WorldAccess::enter(world)else{return REENTRANT;};
    let world=unsafe{&*world};
    unsafe{ptr::write(output,Em1LuaWorldView{status:world.status,scripts:world.scripts,functions:world.functions,globals:world.globals,error:world.error.as_ptr(),error_size:world.error.len()});}world.status
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_lua_world_release(world:*mut Em1LuaWorld){
    if !world.is_null(){let Some(_access)=WorldAccess::enter(world)else{return;};let _=catch_unwind(AssertUnwindSafe(||drop(unsafe{Box::from_raw(world)})));}
}

#[cfg(test)]
mod tests {
    #[test]fn checked_target_variable_records_are_copied_transactional_and_immutable(){unsafe{
        let Some(path)=std::env::var_os("EM1_ANIMATION_REL_FIXTURE")else{return;};
        let chunk=super::prepare_chunk(&super::compile_only("function event(v) AnimVarInt(v,VAR_In_Conversation,1) end").unwrap()).unwrap();
        let input=super::Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=super::em1_lua_world_create(&input,1);let rel=std::fs::read(path).unwrap();assert_eq!(super::em1_lua_world_load_animation_events(world,rel.as_ptr(),rel.len()),0);
        assert_ne!(super::em1_lua_world_preflight_callback(world,b"event".as_ptr(),5,512),0);
        let mut name=b"VAR_In_Conversation".to_vec();let mut record=super::Em1LuaAnimationVariableTarget{name:name.as_ptr(),length:name.len(),id:99,reserved:1};
        assert_ne!(super::em1_lua_world_install_native_animation_variables(world,&record,1),0);record.reserved=0;
        assert_eq!(super::em1_lua_world_install_native_animation_variables(world,&record,1),0);assert_eq!(super::em1_lua_world_preflight_callback(world,b"event".as_ptr(),5,512),0);
        assert_eq!(super::em1_lua_world_install_native_animation_variables(world,&record,1),0);record.id=72;
        assert_ne!(super::em1_lua_world_install_native_animation_variables(world,&record,1),0);record.id=99;
        assert_eq!(super::em1_lua_world_install_native_animation_variables(world,&record,1),0);
        name.fill(0);assert_eq!(super::em1_lua_world_preflight_callback(world,b"event".as_ptr(),5,512),0);
        let mut stats=super::scheduler::Stats::default();assert_eq!(super::em1_lua_world_scheduler_stats(world,&mut stats),0);assert_eq!(stats,super::scheduler::Stats::default());
        assert_eq!(std::mem::size_of::<super::Em1LuaAnimationVariableTarget>(),24);super::em1_lua_world_release(world);
    }}
    #[test]fn native_engine_same_world_reentry_rejects_before_borrows_or_output_writes(){unsafe{
        struct State{world:*mut super::Em1LuaWorld,statuses:Vec<u32>,untouched:bool}
        unsafe extern "C" fn get(_:usize,_:*const u8,_:usize,value:*mut i32)->u32{unsafe{*value=0};0}
        unsafe extern "C" fn set(_:usize,_:*const u8,_:usize,_:i32,_:u32)->u32{0}
        unsafe extern "C" fn callback(context:usize,_:u32,_:f32)->u32{unsafe{
            let state=&mut*(context as *mut State);let world=state.world;
            state.statuses.push(super::em1_lua_world_preflight_callback(world,b"event".as_ptr(),5,u32::MAX));
            let event=super::Em1LuaEventInput{name:b"event".as_ptr(),name_size:5,args:std::ptr::null(),count:0,delay:0.};
            state.statuses.push(super::em1_lua_world_start_batch(world,&event,1));
            let mut view=super::Em1LuaWorldView{status:91,scripts:92,functions:93,globals:94,error:std::ptr::null(),error_size:95};
            state.statuses.push(super::em1_lua_world_view(world,&mut view));
            let mut stats=super::scheduler::Stats{submitted:11,resumes:12,completed:13,failed:14,cancelled:15,pending:16};
            state.statuses.push(super::em1_lua_world_scheduler_stats(world,&mut stats));
            let mut exists=77;state.statuses.push(super::em1_lua_world_callback_exists(world,b"event".as_ptr(),5,&mut exists));
            let mut schema=[0xa5;32];state.statuses.push(super::em1_lua_world_global_schema(world,schema.as_mut_ptr()));
            state.statuses.push(super::em1_lua_world_set_player(world,99));
            super::em1_lua_world_release(world);
            state.untouched=view.status==91 && view.scripts==92 && view.functions==93 && view.globals==94 && view.error.is_null() && view.error_size==95 &&
                stats==super::scheduler::Stats{submitted:11,resumes:12,completed:13,failed:14,cancelled:15,pending:16} && exists==77 && schema==[0xa5;32];0
        }}
        let chunk=super::prepare_chunk(&super::compile_only("count=0; function event(e) count=count+1; SetRotatorMaxSpeed(e,1) end").unwrap()).unwrap();
        let input=super::Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=super::em1_lua_world_create(&input,1);assert_eq!(super::em1_lua_world_bind_entity(world,71),0);
        let arg=super::Em1LuaArgument{kind:5,reserved:0,number:71.,bytes:std::ptr::null(),size:0};
        assert_eq!(super::em1_lua_world_start_args(world,b"event".as_ptr(),5,&arg,1),0);
        let mut state=State{world,statuses:vec![],untouched:false};
        let globals=super::global_bridge::Callbacks{context:0,get:Some(get),set:Some(set)};
        let engine=super::engine_bridge::Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(callback),set_particle_emission:None,
            resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        assert_eq!(super::em1_lua_world_tick_engine(world,0.1,&globals,&engine),0);
        assert_eq!(state.statuses,vec![super::REENTRANT;7]);assert!(state.untouched);
        let mut stats=super::scheduler::Stats::default();assert_eq!(super::em1_lua_world_scheduler_stats(world,&mut stats),0);
        assert_eq!((stats.submitted,stats.completed,stats.pending,stats.failed),(1,1,0,0));
        assert_eq!((*world)._vm.as_ref().unwrap().globals().get::<u32>("count").unwrap(),1);
        assert!((*world).error.is_empty());super::em1_lua_world_release(world);
    }}
    use super::*;
    #[test]fn source_library_ffi_executes_only_authored_roots_and_copies_openfile_modules(){unsafe{
        assert_eq!(std::mem::size_of::<Em1LuaWorldSource>(),72);
        let sources=[
            ("scripts/unused.lua","error('unreferenced library module executed')",0),
            ("scripts/root.lua","wait=coroutine.yield; Wait=coroutine.yield; trace='r'; Openfile('./scripts/child.lua'); Openfile('Scripts/Child.lua'); function Later() Openfile('scripts/child.lua'); wait(0); done=true end",1),
            ("scripts/child.lua","trace=trace..'c'; assert(BuildConfig()=='FINAL')",0),
        ];
        let chunks=sources.iter().map(|(_,code,_)|prepare_chunk(&compile_only(code).unwrap()).unwrap()).collect::<Vec<_>>();
        let inputs=sources.iter().zip(&chunks).map(|((path,_,startup),c)|Em1LuaWorldSource{
            chunk:Em1LuaWorldInput{bytecode:c.bytes.as_ptr(),bytecode_size:c.bytes.len(),executable_sha256:c.executable},
            path:path.as_ptr(),path_size:path.len(),startup:*startup,reserved:0,
        }).collect::<Vec<_>>();
        let world=em1_lua_world_create_sources(inputs.as_ptr(),inputs.len());assert_eq!((*world).status,0,"{}",String::from_utf8_lossy(&(*world).error));
        drop(inputs);drop(chunks);let vm=(*world)._vm.as_ref().unwrap().clone();
        assert_eq!(vm.globals().get::<String>("trace").unwrap(),"rcc");
        assert_eq!(em1_lua_world_start(world,b"Later".as_ptr(),5),0);assert_eq!(em1_lua_world_tick(world,0.1),0);
        assert_eq!(vm.globals().get::<String>("trace").unwrap(),"rccc");
        assert_eq!(em1_lua_world_tick(world,0.0),0);assert!(matches!(vm.globals().get::<mlua::Value>("done").unwrap(),mlua::Value::Nil));
        assert_eq!(em1_lua_world_tick(world,0.1),0);assert!(vm.globals().get::<bool>("done").unwrap());
        em1_lua_world_release(world);
    }}
    #[test]fn callback_lookup_is_raw_and_never_dispatches_or_consumes_event_slots(){unsafe{
        let chunk=prepare_chunk(&compile_only("called=0; value=7; function event() called=called+1 end; setmetatable(_G,{__index=function() error('unexpected global lookup') end})").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let vm=(*world)._vm.as_ref().unwrap().clone();let mut exists=77;
        for (name,expected) in [(&b"event"[..],1),(&b"missing"[..],0),(&b"value"[..],0)]{
            assert_eq!(em1_lua_world_callback_exists(world,name.as_ptr(),name.len(),&mut exists),0);assert_eq!(exists,expected);
        }
        assert_eq!(vm.globals().get::<u32>("called").unwrap(),0);
        exists=77;assert_ne!(em1_lua_world_callback_exists(world,ptr::null(),3,&mut exists),0);assert_eq!(exists,77);
        assert_ne!(em1_lua_world_callback_exists(world,b"event\0".as_ptr(),6,&mut exists),0);assert_eq!(exists,77);
        assert_eq!(em1_lua_world_start(world,b"event".as_ptr(),5),0);assert_eq!(em1_lua_world_tick(world,0.1),0);
        assert_eq!(vm.globals().get::<u32>("called").unwrap(),1);em1_lua_world_release(world);
    }}
    #[test]fn event_batches_are_atomic_on_late_missing_function_bad_arguments_and_capacity(){unsafe{
        assert_eq!(std::mem::size_of::<Em1LuaEventInput>(),40);
        let chunk=prepare_chunk(&compile_only("count=0; function event() count=count+1 end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);let vm=(*world)._vm.as_ref().unwrap().clone();
        let event=||Em1LuaEventInput{name:b"event".as_ptr(),name_size:5,args:ptr::null(),count:0,delay:0.0};
        let invalid=Em1LuaArgument{kind:99,reserved:0,number:0.0,bytes:ptr::null(),size:0};
        let missing=Em1LuaEventInput{name:b"missing".as_ptr(),name_size:7,..event()};
        let bad=Em1LuaEventInput{args:&invalid,count:1,..event()};
        for last in [missing,bad]{let events=[event(),last];assert_ne!(em1_lua_world_start_batch(world,events.as_ptr(),2),0);assert_eq!(em1_lua_world_tick(world,0.1),0);assert_eq!(vm.globals().get::<u32>("count").unwrap(),0);}
        let mut stats=scheduler::Stats::default();assert_eq!(em1_lua_world_scheduler_stats(world,&mut stats),0);assert_eq!(stats,scheduler::Stats::default());
        for _ in 0..255{assert_eq!(em1_lua_world_start(world,b"event".as_ptr(),5),0);}
        let events=[event(),event()];assert_ne!(em1_lua_world_start_batch(world,events.as_ptr(),2),0);
        assert_eq!(em1_lua_world_scheduler_stats(world,&mut stats),0);assert_eq!(stats.submitted,255);assert_eq!(stats.pending,255);assert_eq!(stats.completed,0);
        for _ in 0..8{assert_eq!(em1_lua_world_tick(world,0.1),0);}assert_eq!(vm.globals().get::<u32>("count").unwrap(),255);
        assert_eq!(em1_lua_world_start_batch(world,events.as_ptr(),2),0);assert_eq!(em1_lua_world_tick(world,0.1),0);assert_eq!(vm.globals().get::<u32>("count").unwrap(),257);
        assert_eq!(em1_lua_world_scheduler_stats(world,&mut stats),0);assert_eq!(stats.submitted,257);assert_eq!(stats.completed,257);assert_eq!(stats.pending,0);
        em1_lua_world_release(world);
    }}
    #[test]fn original_global_ffi_ownership_and_failure(){unsafe{
        let Some(path)=std::env::var_os("EM1_GLOBAL_STATE_FIXTURE") else{return;};
        assert_eq!(std::mem::size_of::<Em1GlobalSummary>(),16);assert_eq!(std::mem::size_of::<Em1GlobalDefinitionView>(),32);
        let chunk=prepare_chunk(&compile_only("return").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let bytes=std::fs::read(path).unwrap();let expected_hash=sha256(&bytes).unwrap();assert_eq!(em1_lua_world_load_globals(world,bytes.as_ptr(),bytes.len()),0);drop(bytes);
        let mut schema=[0;32];assert_eq!(em1_lua_world_global_schema(world,schema.as_mut_ptr()),0);assert_eq!(schema,expected_hash);
        let mut summary=std::mem::MaybeUninit::<Em1GlobalSummary>::uninit();assert_eq!(em1_lua_world_global_summary(world,summary.as_mut_ptr()),0);assert_eq!(summary.assume_init().count,5731);
        let mut item=std::mem::MaybeUninit::<Em1GlobalDefinitionView>::uninit();assert_eq!(em1_lua_world_global_definition(world,0,item.as_mut_ptr()),0);let item=item.assume_init();
        assert_eq!(std::slice::from_raw_parts(item.name,item.name_size),b"2dDirection");assert_eq!(item.initial,1);
        assert_ne!(em1_lua_world_load_globals(world,b"bad".as_ptr(),3),0);
        let malformed=[0,0,0,30,0,0,0xcd,0xcd,0];
        assert_ne!(em1_lua_world_load_globals(world,malformed.as_ptr(),malformed.len()),0);
        assert_eq!(em1_lua_world_global_schema(world,schema.as_mut_ptr()),0);assert_eq!(schema,expected_hash);
        assert_eq!(std::slice::from_raw_parts(item.name,item.name_size),b"2dDirection");
        let mut invalid_view=std::mem::MaybeUninit::<Em1GlobalDefinitionView>::uninit();
        assert_ne!(em1_lua_world_global_definition(world,5731,invalid_view.as_mut_ptr()),0);
        // Definitions are metadata, not a second mutable progression store.
        assert!(matches!((*world)._vm.as_ref().unwrap().globals().get::<mlua::Value>("GetGlobal").unwrap(),mlua::Value::Function(_)));
        assert!((*world)._vm.as_ref().unwrap().load("GetGlobal('2dDirection')").exec().is_err());
        em1_lua_world_release(world);
    }}
    #[test]fn function_arguments_capture_validated_function_identity(){unsafe{
        let source="function cb() result=17 end; function dispatch(f) f() end";
        let chunk=prepare_chunk(&compile_only(source).unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let vm=(*world)._vm.as_ref().unwrap().clone();
        let arg=Em1LuaArgument{kind:6,reserved:0,number:0.0,bytes:b"cb".as_ptr(),size:2};
        assert_eq!(em1_lua_world_start_args(world,b"dispatch".as_ptr(),8,&arg,1),0);
        vm.load("cb=function() result=99 end").exec().unwrap();
        assert_eq!(em1_lua_world_tick(world,0.1),0);assert_eq!(vm.globals().get::<u32>("result").unwrap(),17);
        for name in [&b""[..],&b"unknown"[..],&b"result"[..],&b"cb\0"[..],&b"\xff"[..]]{
            let bad=Em1LuaArgument{bytes:name.as_ptr(),size:name.len(),..arg};
            assert_ne!(em1_lua_world_start_args(world,b"dispatch".as_ptr(),8,&bad,1),0);
        }
        em1_lua_world_release(world);
    }}
    #[test]fn registered_primary_uses_the_event_token_and_revokes_on_unbind(){unsafe{
        let chunk=prepare_chunk(&compile_only("function event(v) cached=v; matches=(v==GetPlayer()) end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let vm=(*world)._vm.as_ref().unwrap().clone();
        assert_ne!(em1_lua_world_set_bound_player(world,71),0);
        assert_eq!(em1_lua_world_bind_entity(world,71),0);assert_eq!(em1_lua_world_set_bound_player(world,71),0);
        let arg=Em1LuaArgument{kind:5,reserved:0,number:71.0,bytes:ptr::null(),size:0};
        assert_eq!(em1_lua_world_start_args(world,b"event".as_ptr(),5,&arg,1),0);
        assert_eq!(em1_lua_world_tick(world,0.1),0);assert!(vm.globals().get::<bool>("matches").unwrap());
        let old=vm.globals().get::<mlua::AnyUserData>("cached").unwrap();
        assert_eq!((*world).entities.resolve(&old).unwrap(),71);
        assert_eq!(em1_lua_world_unbind_entity(world,71),0);assert!((*world).entities.resolve(&old).is_err());
        assert!(vm.load("return GetPlayer()==nil").eval::<bool>().unwrap());
        assert_eq!(em1_lua_world_bind_entity(world,71),0);assert_eq!(em1_lua_world_set_bound_player(world,71),0);
        assert!(!vm.load("return cached==GetPlayer()").eval::<bool>().unwrap());
        assert_eq!(em1_lua_world_set_player(world,0),0);assert!(vm.load("return GetPlayer()==nil").eval::<bool>().unwrap());
        em1_lua_world_release(world);
    }}
    #[test]fn typed_callback_arguments_owned_and_resumed_once(){unsafe{
        assert_eq!(std::mem::size_of::<Em1LuaArgument>(),32);
        let source="function event(...) count=select('#',...); local a,b,c,d,e,f=...; assert(a==nil and b==true and f==nil); value=c; payload=d; target=e; resumed=wait(0); completed=true end";
        let chunk=prepare_chunk(&compile_only(source).unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        assert_eq!(em1_lua_world_set_player(world,62),0);
        let vm=(*world)._vm.as_ref().unwrap().clone();let mut bytes=vec![b'a',0,255];
        let arg=|kind,number|Em1LuaArgument{kind,reserved:0,number,bytes:ptr::null(),size:0};
        let mut args=vec![arg(0,0.0),arg(1,1.0),arg(2,9007199254740991.0),arg(3,0.0),arg(4,0.0),arg(0,0.0)];
        args[3].bytes=bytes.as_ptr();args[3].size=bytes.len();
        assert_eq!(em1_lua_world_start_args(world,b"event".as_ptr(),5,args.as_ptr(),args.len()),0);
        bytes.fill(0);drop(bytes);drop(args);
        assert_eq!(em1_lua_world_tick(world,0.1),0);
        assert_eq!(vm.globals().get::<i32>("count").unwrap(),6);
        assert_eq!(vm.globals().get::<f64>("value").unwrap(),9007199254740991.0);
        assert_eq!(&*vm.globals().get::<mlua::String>("payload").unwrap().as_bytes(),&[b'a',0,255]);
        assert!(vm.load("return target == GetPlayer()").eval::<bool>().unwrap());
        assert_eq!(em1_lua_world_tick(world,0.1),0);
        assert!(vm.globals().get::<bool>("completed").unwrap());assert!(matches!(vm.globals().get::<mlua::Value>("resumed").unwrap(),mlua::Value::Nil));
        for bad in [arg(99,0.0),arg(2,f64::NAN),arg(1,2.0),Em1LuaArgument{kind:3,reserved:0,number:0.0,bytes:ptr::null(),size:1}] {
            assert_ne!(em1_lua_world_start_args(world,b"event".as_ptr(),5,&bad,1),0);
        }
        assert_ne!(em1_lua_world_start_args(world,b"event".as_ptr(),5,ptr::null(),17),0);
        let oversized=vec![0u8;65537];
        let big=Em1LuaArgument{kind:3,reserved:0,number:0.0,bytes:oversized.as_ptr(),size:oversized.len()};
        assert_ne!(em1_lua_world_start_args(world,b"event".as_ptr(),5,&big,1),0);
        let total=(0..5).map(|_|Em1LuaArgument{kind:3,reserved:0,number:0.0,bytes:oversized.as_ptr(),size:65536}).collect::<Vec<_>>();
        assert_ne!(em1_lua_world_start_args(world,b"event".as_ptr(),5,total.as_ptr(),total.len()),0);
        let reserved=Em1LuaArgument{reserved:1,..arg(0,0.0)};
        assert_ne!(em1_lua_world_start_args(world,b"event".as_ptr(),5,&reserved,1),0);
        assert_eq!(em1_lua_world_set_player(world,0),0);assert_ne!(em1_lua_world_start_args(world,b"event".as_ptr(),5,&arg(4,0.0),1),0);
        em1_lua_world_release(world);
    }}
    #[test]fn delayed_callback_ffi_copies_payload_and_function_before_dispatch(){unsafe{
        let chunk=prepare_chunk(&compile_only("function event(v) received=v; wait(0); completed=true end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let vm=(*world)._vm.as_ref().unwrap().clone();let mut bytes=vec![b'a',0,255];
        let argument=Em1LuaArgument{kind:3,reserved:0,number:0.0,bytes:bytes.as_ptr(),size:bytes.len()};
        for delay in [-1.0,f64::NAN,f64::INFINITY]{assert_ne!(em1_lua_world_start_args_after(world,b"event".as_ptr(),5,&argument,1,delay),0);}
        assert_eq!(em1_lua_world_start_args_after(world,b"event".as_ptr(),5,&argument,1,0.5),0);
        bytes.fill(0);drop(bytes);vm.load("event=function() error('replaced callback') end").exec().unwrap();
        assert_eq!(em1_lua_world_tick(world,0.0),0);assert_eq!(em1_lua_world_tick(world,0.25),0);
        assert!(matches!(vm.globals().get::<mlua::Value>("received").unwrap(),mlua::Value::Nil));
        assert_eq!(em1_lua_world_tick(world,0.25),0);
        assert_eq!(vm.globals().get::<mlua::String>("received").unwrap().as_bytes().as_ref(),b"a\0\xff");
        assert!(matches!(vm.globals().get::<mlua::Value>("completed").unwrap(),mlua::Value::Nil));
        assert_eq!(em1_lua_world_tick(world,0.0),0);
        assert_eq!(em1_lua_world_tick(world,0.125),0);assert!(vm.globals().get::<bool>("completed").unwrap());
        em1_lua_world_release(world);
    }}
    #[test]fn original_callable_admission_runs_dynamic_lua_once_and_isolates_runtime_thread_failures(){unsafe{
        let chunk=prepare_chunk(&compile_only("count=0; key='run'; handlers={run=function()count=count+1 end}; function dynamic_event() handlers[key]() end; function broken() MissingActualApi() end; function good() good_count=(good_count or 0)+1; wait(0); good_count=good_count+1 end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        assert_eq!(em1_lua_world_preflight_callback(world,b"dynamic_event".as_ptr(),13,0),INVALID);
        assert_eq!(em1_lua_world_start(world,b"dynamic_event".as_ptr(),13),0);assert_eq!(em1_lua_world_tick(world,0.1),0);
        let vm=(*world)._vm.as_ref().unwrap().clone();assert_eq!(vm.globals().get::<u32>("count").unwrap(),1);
        assert_eq!(em1_lua_world_start(world,b"broken".as_ptr(),6),0);assert_eq!(em1_lua_world_start(world,b"good".as_ptr(),4),0);
        assert_eq!(em1_lua_world_tick(world,0.1),SCRIPT_FAILED);assert_eq!(vm.globals().get::<u32>("good_count").unwrap(),1);
        let mut stats=scheduler::Stats::default();assert_eq!(em1_lua_world_scheduler_stats(world,&mut stats),0);assert_eq!((stats.submitted,stats.completed,stats.failed,stats.cancelled,stats.pending),(3,1,1,0,1));
        assert_eq!(em1_lua_world_tick(world,0.1),0);assert_eq!(vm.globals().get::<u32>("good_count").unwrap(),2);assert_eq!(em1_lua_world_scheduler_stats(world,&mut stats),0);assert_eq!((stats.completed,stats.failed,stats.pending),(2,1,0));
        assert_eq!(em1_lua_world_tick(world,f64::NAN),INVALID);assert_eq!(em1_lua_world_reset_prefab_data(world),0);em1_lua_world_release(world);
    }}
    #[test]fn native_and_engine_ticks_report_individual_script_failure_without_cancelling_other_tasks(){unsafe{
        unsafe extern "C" fn get(_:usize,_:*const u8,_:usize,_:*mut i32)->u32{1}
        unsafe extern "C" fn set(_:usize,_:*const u8,_:usize,_:i32,_:u32)->u32{0}
        unsafe extern "C" fn rot(_:usize,_:u32,_:f32)->u32{0}
        let chunk=prepare_chunk(&compile_only("function broken() error('expected original Lua error') end; function good() count=(count or 0)+1 end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let globals=global_bridge::Callbacks{context:0,get:Some(get),set:Some(set)};
        let engine=engine_bridge::Callbacks{context:0,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        for native in [false,true]{let world=em1_lua_world_create(&input,1);assert_eq!(em1_lua_world_start(world,b"broken".as_ptr(),6),0);assert_eq!(em1_lua_world_start(world,b"good".as_ptr(),4),0);
            let status=if native{em1_lua_world_tick_native(world,0.1,&globals)}else{em1_lua_world_tick_engine(world,0.1,&globals,&engine)};assert_eq!(status,SCRIPT_FAILED);
            let mut stats=scheduler::Stats::default();assert_eq!(em1_lua_world_scheduler_stats(world,&mut stats),0);assert_eq!((stats.failed,stats.completed,stats.pending,stats.cancelled),(1,1,0,0));
            assert_eq!((*world)._vm.as_ref().unwrap().globals().get::<u32>("count").unwrap(),1);assert!(!(*world).error.is_empty());em1_lua_world_release(world);
        }
    }}
    #[test]fn native_owner_unbind_evicts_prefab_cache_without_invalidating_lua_table_references(){unsafe{
        let chunk=prepare_chunk(&compile_only("function event() end").unwrap()).unwrap();let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};let world=em1_lua_world_create(&input,1);
        assert_eq!(em1_lua_world_bind_entity(world,71),0);let vm=(*world)._vm.as_ref().unwrap().clone();let token=(*world).entities.snapshot(71).unwrap();let generation=token.borrow::<entities::Entity>().unwrap().generation;
        prefab_data::prepare(&vm,[77,0,0,0],generation,"source-owned",&[b"value=41".to_vec()],&[]).unwrap();let table=prefab_data::get(&vm,[77,0,0,0],generation).unwrap().unwrap();
        assert_eq!(em1_lua_world_unbind_entity(world,71),0);assert!(prefab_data::get(&vm,[77,0,0,0],generation).unwrap().is_none());assert_eq!(table.get::<u32>("value").unwrap(),41);
        assert_eq!(em1_lua_world_bind_entity(world,71),0);let next=(*world).entities.snapshot(71).unwrap().borrow::<entities::Entity>().unwrap().generation;assert!(next>generation);em1_lua_world_release(world);
    }}
    #[test]fn native_engine_dispatch_is_shared_by_immediate_children_and_revalidates_yielded_tokens(){unsafe{
        struct State{world:*mut Em1LuaWorld,calls:u32,busy:u32}
        unsafe extern "C" fn get(_:usize,_:*const u8,_:usize,_:*mut i32)->u32{1}
        unsafe extern "C" fn set(_:usize,_:*const u8,_:usize,_:i32,_:u32)->u32{0}
        unsafe extern "C" fn rot(context:usize,id:u32,_:f32)->u32{let state=unsafe{&mut*(context as *mut State)};assert_eq!(id,71);state.calls+=1;
            let mut view=Em1LuaWorldView{status:91,scripts:92,functions:93,globals:94,error:std::ptr::null(),error_size:95};state.busy=unsafe{em1_lua_world_view(state.world,&mut view)};assert_eq!(view.status,91);0}
        let chunk=prepare_chunk(&compile_only("function event(e) FireThread(function(value) SetRotatorMaxSpeed(value,1); wait(0); SetRotatorMaxSpeed(value,2) end,e); parent_done=true end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};let world=em1_lua_world_create(&input,1);assert_eq!(em1_lua_world_bind_entity(world,71),0);
        let arg=Em1LuaArgument{kind:5,reserved:0,number:71.,bytes:std::ptr::null(),size:0};assert_eq!(em1_lua_world_start_args(world,b"event".as_ptr(),5,&arg,1),0);
        let mut state=State{world,calls:0,busy:0};let globals=global_bridge::Callbacks{context:0,get:Some(get),set:Some(set)};
        let engine=engine_bridge::Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        assert_eq!(em1_lua_world_tick_engine(world,0.1,&globals,&engine),0);assert_eq!(state.calls,1);assert_eq!(state.busy,REENTRANT);assert!((*world)._vm.as_ref().unwrap().globals().get::<bool>("parent_done").unwrap());
        assert_eq!(em1_lua_world_unbind_entity(world,71),0);assert_eq!(em1_lua_world_bind_entity(world,71),0);assert_eq!(em1_lua_world_tick_engine(world,0.1,&globals,&engine),SCRIPT_FAILED);assert_eq!(state.calls,1);
        let mut stats=scheduler::Stats::default();assert_eq!(em1_lua_world_scheduler_stats(world,&mut stats),0);assert_eq!((stats.submitted,stats.completed,stats.failed,stats.pending),(2,1,1,0));em1_lua_world_release(world);
    }}
    #[test]fn scheduler_ffi_lifecycle(){unsafe{
        let chunk=prepare_chunk(&compile_only("phase=0; function event() phase=1; wait(0.5); phase=2 end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let vm=(*world)._vm.as_ref().unwrap().clone();
        assert_eq!(em1_lua_world_start(world,b"event".as_ptr(),5),0);
        assert_eq!(em1_lua_world_tick(world,0.1),0);assert_eq!(vm.globals().get::<i32>("phase").unwrap(),1);
        assert_eq!(em1_lua_world_cancel(world),0);assert_eq!(em1_lua_world_tick(world,1.0),0);assert_eq!(vm.globals().get::<i32>("phase").unwrap(),1);
        assert_eq!(em1_lua_world_start(world,b"event".as_ptr(),5),0);assert_eq!(em1_lua_world_tick(world,0.1),0);
        assert_eq!(em1_lua_world_tick(world,0.5),0);assert_eq!(vm.globals().get::<i32>("phase").unwrap(),2);
        assert_ne!(em1_lua_world_start(world,ptr::null(),5),0);assert!(!(*world).error.is_empty());
        assert_ne!(em1_lua_world_tick(world,f64::NAN),0);assert_eq!(em1_lua_world_tick(world,0.0),0);
        em1_lua_world_release(world);
    }}
    #[test]fn registered_entity_arguments_survive_wait_without_resurrecting_identity(){unsafe{
        let chunk=prepare_chunk(&compile_only("function receive(e) received=e; wait(0); resumed=e end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let argument=|number|Em1LuaArgument{kind:5,reserved:0,number,bytes:ptr::null(),size:0};
        let start=|number|em1_lua_world_start_args(world,b"receive".as_ptr(),7,&argument(number),1);
        assert_ne!(start(118.0),0);assert_eq!(em1_lua_world_bind_entity(world,118),0);
        for invalid in [0.0,-1.0,118.5,f64::NAN,f64::INFINITY,4294967296.0]{assert_ne!(start(invalid),0);}
        assert_eq!(start(118.0),0);assert_eq!(em1_lua_world_tick(world,0.1),0);
        let vm=(*world)._vm.as_ref().unwrap().clone();let first=vm.globals().get::<mlua::AnyUserData>("received").unwrap();
        assert_eq!((*world).entities.resolve(&first).unwrap(),118);
        assert_eq!(em1_lua_world_unbind_entity(world,118),0);assert_eq!(em1_lua_world_bind_entity(world,118),0);
        assert_eq!(em1_lua_world_tick(world,0.1),0);
        let resumed=vm.globals().get::<mlua::AnyUserData>("resumed").unwrap();assert!((*world).entities.resolve(&resumed).is_err());
        assert_eq!(start(118.0),0);assert_eq!(em1_lua_world_tick(world,0.1),0);
        let fresh=vm.globals().get::<mlua::AnyUserData>("received").unwrap();assert_eq!((*world).entities.resolve(&fresh).unwrap(),118);
        assert!((*world).entities.resolve(&first).is_err());em1_lua_world_release(world);
    }}
    #[test]fn live_player_binding_identity_and_invalidation(){unsafe{
        let chunk=prepare_chunk(&compile_only("function OriginalPlayerLookup() return GetPlayer() end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let vm=(*world)._vm.as_ref().unwrap().clone();
        let lookup=vm.globals().get::<mlua::Function>("OriginalPlayerLookup").unwrap();
        assert!(matches!(lookup.call::<mlua::Value>(()).unwrap(),mlua::Value::Nil));
        assert_eq!(em1_lua_world_set_player(world,0x81234560),0);
        let first=lookup.call::<mlua::AnyUserData>(()).unwrap();let token=*first.borrow::<PlayerEntity>().unwrap();
        assert_eq!(token.entity,0x81234560);
        assert_eq!(em1_lua_world_set_player(world,token.entity),0);
        vm.globals().set("cached_first",first.clone()).unwrap();assert!(vm.load("return cached_first == GetPlayer()").eval::<bool>().unwrap());
        assert_eq!(em1_lua_world_set_player(world,0),0);
        assert!(matches!(lookup.call::<mlua::Value>(()).unwrap(),mlua::Value::Nil));
        // Reused native address after destruction must produce a new identity.
        assert_eq!(em1_lua_world_set_player(world,token.entity),0);
        let reused=lookup.call::<mlua::AnyUserData>(()).unwrap();assert!(!vm.load("return cached_first == GetPlayer()").eval::<bool>().unwrap());
        assert!(reused.borrow::<PlayerEntity>().unwrap().generation>token.generation);
        assert_eq!(em1_lua_world_set_player(world,0x81234600),0);
        assert_eq!(lookup.call::<mlua::AnyUserData>(()).unwrap().borrow::<PlayerEntity>().unwrap().entity,0x81234600);
        assert!(vm.load("return GetPlayer().entity").eval::<mlua::Value>().is_err());
        assert!(vm.load("return SetHealth(GetPlayer(), 100)").exec().is_err());
        em1_lua_world_release(world);assert_eq!(em1_lua_world_set_player(ptr::null_mut(),1),INVALID);
    }}
    #[test]fn persistent_world_transaction_and_ownership(){unsafe{
        fn sendable<T:Send>(){}sendable::<Em1LuaWorld>();
        assert_eq!(std::mem::size_of::<Em1LuaWorldInput>(),48);assert_eq!(std::mem::size_of::<Em1LuaWorldView>(),32);
        let create=|sources:&[&str]|{
            let chunks=sources.iter().map(|s|prepare_chunk(&compile_only(s).unwrap()).unwrap()).collect::<Vec<_>>();
            let inputs=chunks.iter().map(|c|Em1LuaWorldInput{bytecode:c.bytes.as_ptr(),bytecode_size:c.bytes.len(),executable_sha256:c.executable}).collect::<Vec<_>>();
            em1_lua_world_create(inputs.as_ptr(),inputs.len())
        };
        let world=create(&["world_value=5; function WorldRead() return world_value end","world_value=world_value+7"]);
        let mut view=std::mem::MaybeUninit::<Em1LuaWorldView>::uninit();
        assert_eq!(em1_lua_world_view(world,view.as_mut_ptr()),0);let view=view.assume_init();
        assert_eq!((view.scripts,view.functions,view.globals),(2,1,2));
        assert_eq!((*world)._vm.as_ref().unwrap().globals().get::<mlua::Function>("WorldRead").unwrap().call::<i64>(()).unwrap(),12);
        for source in ["world_value=99; error('stage failed')","while true do end","GetPlayer()"] {
            let failed=create(&["partial_state=true",source]);assert_ne!((*failed).status,0);assert!((*failed)._vm.is_none());assert_eq!((*failed).globals,0);em1_lua_world_release(failed);
        }
        assert_eq!((*world)._vm.as_ref().unwrap().globals().get::<i64>("world_value").unwrap(),12);
        // The caller's input buffers have already gone away. Move/drop the
        // successful VM on another thread, as the C++ scene owner does.
        let owned=Box::from_raw(world);
        std::thread::spawn(move ||assert_eq!(owned._vm.as_ref().unwrap().globals().get::<i64>("world_value").unwrap(),12)).join().unwrap();
        let chunk=prepare_chunk(&compile_only("return").unwrap()).unwrap();
        let bad=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:[0;32]};
        let failed=em1_lua_world_create(&bad,1);assert_eq!((*failed).status,DIFFERENT);assert!((*failed)._vm.is_none());em1_lua_world_release(failed);
        let failed=em1_lua_world_create(ptr::null(),0);assert_ne!((*failed).status,0);em1_lua_world_release(failed);
        em1_lua_world_release(ptr::null_mut());assert_eq!(em1_lua_world_view(ptr::null(),ptr::null_mut()),INVALID);
    }}
    #[test]fn isolated_definition_limits(){
        let vm=definition_vm().unwrap();
        for name in ["io","os","package","debug","loadfile","dofile","pcall","xpcall","newproxy","GetPlayer"] {
            assert!(matches!(vm.globals().raw_get::<mlua::Value>(name).unwrap(),mlua::Value::Nil));
        }
        let original=compile_only("fixture_state=7; function FixtureLater() error('deferred function must not run') end").unwrap();
        vm.load(&prepare_chunk(&original).unwrap().bytes).set_mode(mlua::ChunkMode::Binary).exec().unwrap();
        assert_eq!(vm.globals().get::<i64>("fixture_state").unwrap(),7);
        assert!(matches!(vm.globals().get::<mlua::Value>("FixtureLater").unwrap(),mlua::Value::Function(_)));
        let loop_chunk=prepare_chunk(&compile_only("while true do end").unwrap()).unwrap();
        assert!(vm.load(&loop_chunk.bytes).set_mode(mlua::ChunkMode::Binary).exec().unwrap_err().to_string().contains("instruction budget"));
    }
    #[test]fn known_hash(){assert_eq!(sha256(b"abc").unwrap(),[0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad]);}
    #[test]fn normalized_signature_is_exact(){
        let a=compile_only("function F(x) return x+1 end").unwrap();
        let b=compile_only("-- other source\n function F(y) return y+1 end").unwrap();
        let c=compile_only("function F(x) return x+2 end").unwrap();
        assert_eq!(executable(&a).unwrap(),executable(&b).unwrap());assert_ne!(executable(&a).unwrap(),executable(&c).unwrap());
        assert!(executable(&a[..a.len()-1]).is_err());let mut bad=a.clone();bad.push(0);assert!(executable(&bad).is_err());
        bad=a;bad[5]=12;assert!(executable(&bad).is_err());
    }
    #[test]fn source_is_not_executed(){assert!(compile_only("error('must not run'); _G.must_not_be_set=true").is_ok());assert!(compile_only("local broken =").is_err());}
    #[test]fn portable_original_chunk(){
        let original=compile_only("local n=7; local function f(x) return x*n+0.125 end; return f(3), 'a\\000b'").unwrap();
        let canonical=executable(&original).unwrap();
        let native=executable_with_layout(&canonical,std::mem::size_of::<usize>()).unwrap();
        assert_eq!(executable(&native).unwrap(),canonical);
        let vm=mlua::Lua::new_with(mlua::StdLib::NONE,mlua::LuaOptions::default()).unwrap();
        vm.set_memory_limit(VM_MEMORY).unwrap();
        let function=vm.load(&native).set_mode(mlua::ChunkMode::Binary).into_function().unwrap();
        assert_eq!(executable(&function.dump(true)).unwrap(),canonical);
        // Execute only this generated arithmetic/string fixture, never game code.
        let (number,text):(f64,mlua::String)=function.call(()).unwrap();
        assert_eq!(number,21.125);assert_eq!(text.as_bytes().as_ref(),b"a\0b");
        assert!(executable_with_layout(&original,16).is_err());
        assert!(executable_with_layout(&original[..original.len()-1],8).is_err());
    }
    #[test]fn native_chunk_ffi_ownership(){unsafe{
        assert_eq!(std::mem::size_of::<Em1LuaChunkView>(),104);
        assert_eq!(std::mem::offset_of!(Em1LuaChunkView,status),96);
        let original=compile_only("error('must not execute'); _G.side_effect=true").unwrap();
        let result=em1_lua_prepare_chunk(original.as_ptr(),original.len());assert!(!result.is_null());
        let mut view=std::mem::MaybeUninit::<Em1LuaChunkView>::uninit();
        assert_eq!(em1_lua_chunk_view(result,view.as_mut_ptr()),0);let view=view.assume_init();
        assert_eq!(view.original_sha256,sha256(&original).unwrap());
        assert_eq!(executable(std::slice::from_raw_parts(view.bytecode,view.bytecode_size)).unwrap(),executable(&original).unwrap());
        assert_eq!(em1_lua_chunk_view(result,ptr::null_mut()),INVALID);
        em1_lua_chunk_release(result);
        for (input,length) in [(ptr::null(),12),(original.as_ptr(),MAX_CHUNK+1),(original.as_ptr(),original.len()-1)] {
            let failed=em1_lua_prepare_chunk(input,length);let mut out=std::mem::MaybeUninit::<Em1LuaChunkView>::uninit();
            assert_ne!(em1_lua_chunk_view(failed,out.as_mut_ptr()),0);let out=out.assume_init();
            assert_eq!(out.bytecode_size,0);assert!(out.error_size>0);em1_lua_chunk_release(failed);
        }
        assert_eq!(em1_lua_chunk_view(ptr::null(),ptr::null_mut()),INVALID);em1_lua_chunk_release(ptr::null_mut());
    }}
    #[test]fn native_chunk_string_width_growth(){
        let vm=mlua::Lua::new_with(mlua::StdLib::NONE,mlua::LuaOptions::default()).unwrap();
        vm.set_memory_limit(VM_MEMORY).unwrap();
        let mut source=String::from("return {");
        for i in 0..60000 {source+=&format!("'K{i:05}',");}source.push('}');
        let native=vm.load(&source).into_function().unwrap().dump(true);
        assert!(native.len()>MAX_CHUNK);
        let original=executable_with_layout(&native,4).unwrap();assert!(original.len()<MAX_CHUNK);
        let prepared=prepare_chunk(&original).unwrap();assert!(prepared.bytes.len()>MAX_CHUNK);
        assert!(prepared.bytes.len()<=2*MAX_CHUNK);
        assert_eq!(executable_with_layout(&prepared.bytes,4).unwrap(),original);
        assert!(prepare_chunk(&native).is_err());
    }
    #[test]fn ffi_ownership_and_failure(){unsafe{
        assert_eq!(em1_lua_view(ptr::null(),ptr::null_mut()),INVALID);
        let result=em1_lua_recover(ptr::null(),12);assert!(!result.is_null());assert_eq!((*result).status,INVALID);assert!((*result).source.is_empty());em1_lua_release(result);em1_lua_release(ptr::null_mut());
        let original=compile_only("return 17").unwrap();
        for _ in 0..4 {
            let result=em1_lua_recover(original.as_ptr(),original.len());let mut view=std::mem::MaybeUninit::<Em1LuaRecoveryView>::uninit();
            assert_eq!(em1_lua_view(result,view.as_mut_ptr()),0,"{}",String::from_utf8_lossy(&(*result).error));
            let view=view.assume_init();assert!(view.source_size>0);assert_eq!(view.error_size,0);
            assert_eq!(std::slice::from_raw_parts(view.source,view.source_size),&(*result).source);
            assert_eq!(view.original_sha256,sha256(&original).unwrap());em1_lua_release(result);
        }
        // Rejected before constructing/dereferencing the deliberately tiny span.
        let result=em1_lua_recover(original.as_ptr(),MAX_CHUNK+1);assert_eq!((*result).status,BUDGET);em1_lua_release(result);
    }}
    #[test]fn panics_do_not_escape(){let result=contain(||panic!("fixture panic"));assert_eq!(result.status,INTERNAL);assert!(result.source.is_empty());}
    #[test]fn original_fixture_inventory(){
        let Some(root)=std::env::var_os("EM1_LUA_ORIGINAL_FIXTURES") else{return;};
        let output=std::env::var_os("EM1_LUA_RECOVERED_OUTPUT").map(std::path::PathBuf::from);
        if let Some(out)=&output {
            let repository=std::env::current_dir().unwrap().parent().unwrap().parent().unwrap().to_path_buf();
            assert!(out.is_absolute() && out.starts_with(repository.join("analysis")) && out.is_dir());
        }
        let mut report=String::from("file\tstatus\toriginal_bytes\tverified_source_bytes\toriginal_sha256\terror\n");
        let mut native_report=String::from("file\toriginal_bytes\tnative_bytes\texecutable_exact_after_native_load\n");
        let mut initialization_report=String::from("file\tinitialized\tnew_functions\tnew_globals\terror\n");
        let mut count=0;for entry in std::fs::read_dir(root).unwrap(){let path=entry.unwrap().path();if path.extension().is_some_and(|x|x=="lua"){
            let bytes=std::fs::read(&path).unwrap();let result=contain(||recover(&bytes));
            let prepared=prepare_chunk(&bytes).unwrap();let native=prepared.bytes;
            let vm=mlua::Lua::new_with(mlua::StdLib::NONE,mlua::LuaOptions::default()).unwrap();
            vm.set_memory_limit(VM_MEMORY).unwrap();
            let loaded=vm.load(&native).set_mode(mlua::ChunkMode::Binary).into_function().unwrap();
            assert_eq!(executable(&bytes).unwrap(),executable(&loaded.dump(true)).unwrap());
            let initial=definition_vm().unwrap();
            let before=initial.globals().pairs::<String,mlua::Value>().map(|p|p.unwrap().0).collect::<std::collections::BTreeSet<_>>();
            let initialization=initial.load(&native).set_mode(mlua::ChunkMode::Binary).exec();
            let mut functions=std::collections::BTreeSet::new();let mut globals=0;
            for pair in initial.globals().pairs::<String,mlua::Value>() {
                let (name,value)=pair.unwrap();if before.contains(&name){continue;}globals+=1;
                if matches!(value,mlua::Value::Function(_)){functions.insert(name);}
            }
            initialization_report+=&format!("{}\t{}\t{}\t{}\t{}\n",path.file_name().unwrap().to_string_lossy(),initialization.is_ok(),functions.into_iter().collect::<Vec<_>>().join(","),globals,initialization.err().map(|e|e.to_string().replace(['\t','\r','\n']," ")).unwrap_or_default());
            native_report+=&format!("{}\t{}\t{}\ttrue\n",path.file_name().unwrap().to_string_lossy(),bytes.len(),native.len());
            println!("AUTO_ORIGINAL {} status={} bytes={} source={} error={}",path.file_name().unwrap().to_string_lossy(),result.status,bytes.len(),result.source.len(),String::from_utf8_lossy(&result.error));
            let hash=sha256(&bytes).unwrap().iter().map(|v|format!("{v:02x}")).collect::<String>();
            report+=&format!("{}\t{}\t{}\t{}\t{}\t{}\n",path.file_name().unwrap().to_string_lossy(),result.status,bytes.len(),result.source.len(),hash,String::from_utf8_lossy(&result.error).replace(['\t','\r','\n']," "));
            if let Some(out)=&output {if result.status==0{std::fs::write(out.join(path.file_name().unwrap()),&result.source).unwrap();}}
            assert_eq!(&bytes[..6],b"\x1bLua\x51\0");assert!(result.status==0 || result.source.is_empty());count+=1;
        }}assert!(count>0);if let Some(out)=output{std::fs::write(out.join("automatic-inventory.tsv"),report).unwrap();std::fs::write(out.join("native-chunk-inventory.tsv"),native_report).unwrap();std::fs::write(out.join("definition-initialization.tsv"),initialization_report).unwrap();}
    }
    #[test]fn original_shared_bootstrap_uses_original_openfile_order(){unsafe{
        let Some(root)=std::env::var_os("EM1_LUA_ORIGINAL_FIXTURES") else{return;};
        let mut paths=std::fs::read_dir(root).unwrap().map(|e|e.unwrap().path()).filter(|p|p.extension().is_some_and(|e|e=="lua")).collect::<Vec<_>>();
        paths.sort();let mut modules=Vec::new();let mut chunks=Vec::new();
        for path in paths{
            modules.push(format!("environments/_shared/scripts/{}",path.file_name().unwrap().to_str().unwrap()));
            chunks.push(prepare_chunk(&std::fs::read(path).unwrap()).unwrap());
        }
        let inputs=modules.iter().zip(&chunks).map(|(name,c)|Em1LuaWorldSource{
            chunk:Em1LuaWorldInput{bytecode:c.bytes.as_ptr(),bytecode_size:c.bytes.len(),executable_sha256:c.executable},
            path:name.as_ptr(),path_size:name.len(),startup:u32::from(name.ends_with("globalscripts.lua")),reserved:0,
        }).collect::<Vec<_>>();
        let world=em1_lua_world_create_sources(inputs.as_ptr(),inputs.len());
        assert_eq!((*world).status,0,"Original global script bootstrap failed: {}",String::from_utf8_lossy(&(*world).error));
        let vm=(*world)._vm.as_ref().unwrap();
        for name in ["LoadLevel","SaveCheckpoint","RestoreCheckpoint","InitWand","PrintTable"]{
            println!("ORIGINAL_BOOTSTRAP {name} {}",vm.globals().get::<mlua::Value>(name).unwrap().type_name());
        }
        assert!(matches!(vm.globals().get::<mlua::Value>("LoadLevel").unwrap(),mlua::Value::Function(_)));
        println!("ORIGINAL_BOOTSTRAP imported={} functions={} globals={}; original top-level Openfile and FINAL build identity; gameplay engine APIs not invoked",modules.len(),(*world).functions,(*world).globals);
        em1_lua_world_release(world);
    }}
    #[test]fn original_shared_initialization(){
        let Some(root)=std::env::var_os("EM1_LUA_ORIGINAL_FIXTURES") else{return;};
        let root=std::path::PathBuf::from(root);let order=root.join("initialization-order.txt");
        if !order.exists(){return;}
        let vm=definition_vm().unwrap();
        let baseline=vm.globals().pairs::<String,mlua::Value>().map(|p|p.unwrap().0).collect::<std::collections::BTreeSet<_>>();
        let mut count=0;let mut seen=std::collections::BTreeSet::new();let mut chunks=Vec::new();
        for name in std::fs::read_to_string(order).unwrap().lines(){
            assert_eq!(std::path::Path::new(name).file_name().unwrap(),name);assert!(seen.insert(name.to_owned()));
            let bytes=std::fs::read(root.join(name)).unwrap();let prepared=prepare_chunk(&bytes).unwrap();
            vm.load(&prepared.bytes).set_mode(mlua::ChunkMode::Binary).exec().unwrap_or_else(|error|panic!("Shared initialization failed at {name}: {error}"));count+=1;chunks.push(prepared);
        }
        assert!(count>0);let mut functions=0;let mut globals=0;
        for pair in vm.globals().pairs::<String,mlua::Value>() {
            let (name,value)=pair.unwrap();if baseline.contains(&name){continue;}globals+=1;
            if matches!(value,mlua::Value::Function(_)){functions+=1;}
        }
        println!("SHARED_ORIGINAL initialized={count} functions={functions} globals={globals}; import order, no engine bridge or gameplay callback dispatch");
        let inputs=chunks.iter().map(|c|Em1LuaWorldInput{bytecode:c.bytes.as_ptr(),bytecode_size:c.bytes.len(),executable_sha256:c.executable}).collect::<Vec<_>>();
        unsafe {
            let world=em1_lua_world_create(inputs.as_ptr(),inputs.len());let mut view=std::mem::MaybeUninit::<Em1LuaWorldView>::uninit();
            assert_eq!(em1_lua_world_view(world,view.as_mut_ptr()),0,"{}",String::from_utf8_lossy(&(*world).error));let view=view.assume_init();
            assert_eq!((view.scripts,view.functions,view.globals),(count,functions,globals));
            assert!((*world)._vm.is_some());
            // Exercise an actual original authored callback, not a rewritten
            // source substitute: this function converts its second argument.
            let name=b"DBC_1st_SetSide";let value=b"2";
            let args=[Em1LuaArgument{kind:0,reserved:0,number:0.0,bytes:ptr::null(),size:0},Em1LuaArgument{kind:3,reserved:0,number:0.0,bytes:value.as_ptr(),size:value.len()}];
            assert_eq!(em1_lua_world_start_args(world,name.as_ptr(),name.len(),args.as_ptr(),args.len()),0);
            assert_eq!(em1_lua_world_tick(world,1.0/30.0),0);
            assert_eq!((*world)._vm.as_ref().unwrap().globals().get::<i32>("DBC_1st_side").unwrap(),2);
            println!("ORIGINAL_CALLBACK DBC_1st_SetSide(nil, '2') -> DBC_1st_side=2");
            em1_lua_world_release(world);
        }
    }
}
