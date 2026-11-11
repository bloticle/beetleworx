//! Wii SetMap is a source map-manager state operation, not EM2's entity-map
//! selector. REL1F9618 pops two strings and an ignored integer, then1F7C64/B8
//! retain names at manager58/5C. There is no asset lookup or load-success test.
use mlua::{Lua,Result,UserData,Value};
use std::sync::{Arc,Mutex};
const KEY:&str="em1.source.map_manager";
#[derive(Default)]struct Data{full:Vec<u8>,sub:Vec<u8>,revision:u64}
#[derive(Clone,Default)]struct State(Arc<Mutex<Data>>);
impl UserData for State{}
fn error(s:&str)->mlua::Error{mlua::Error::RuntimeError(s.into())}
pub fn install(vm:&Lua)->Result<()>{vm.set_named_registry_value(KEY,vm.create_userdata(State::default())?)}
fn state(vm:&Lua)->Result<State>{Ok(vm.named_registry_value::<mlua::AnyUserData>(KEY)?.borrow::<State>()?.clone())}
fn string(vm:&Lua,value:Value)->Result<Vec<u8>>{
    //2788C4 uses lua_isstring/lua_tolstring (strings/numbers), C-string
    // interning; nil/other types produce the empty fixed string.
    let Some(value)=vm.coerce_string(value)?else{return Ok(Vec::new());};
    let bytes=value.as_bytes();let end=bytes.iter().position(|&b|b==0).unwrap_or(bytes.len());
    if end>4096{return Err(error("Original map name exceeds owned string budget"));}
    Ok(bytes[..end].to_vec())
}
pub fn set(vm:&Lua,mut args:mlua::MultiValue)->Result<()>{
    if args.len()!=3{return Err(error("Original SetMap requires exactly three arguments"));}
    let sub=string(vm,args.pop_back().unwrap())?;
    let full=string(vm,args.pop_back().unwrap())?;
    // First value is consumed by the source integer reader but never used.
    // No metamethod is invoked by that reader, so no host conversion needed.
    let _ignored=args.pop_back();let state=state(vm)?;let mut data=state.0.lock().unwrap_or_else(|e|e.into_inner());
    if data.full!=full || data.sub!=sub{data.full=full;data.sub=sub;data.revision=data.revision.wrapping_add(1);}
    Ok(())
}
pub fn clear(vm:&Lua)->Result<()>{let state=state(vm)?;*state.0.lock().unwrap_or_else(|e|e.into_inner())=Data::default();Ok(())}
#[repr(C)]pub struct View{pub full:*const u8,pub full_size:usize,pub sub:*const u8,pub sub_size:usize,pub revision:u64}
pub fn view(vm:&Lua)->Result<View>{let state=state(vm)?;let data=state.0.lock().unwrap_or_else(|e|e.into_inner());
    Ok(View{full:if data.full.is_empty(){std::ptr::null()}else{data.full.as_ptr()},full_size:data.full.len(),
        sub:if data.sub.is_empty(){std::ptr::null()}else{data.sub.as_ptr()},sub_size:data.sub.len(),revision:data.revision})}
#[cfg(test)]mod tests{
 use super::*;
 fn names(vm:&Lua)->(Vec<u8>,Vec<u8>,u64){let s=state(vm).unwrap();let d=s.0.lock().unwrap();(d.full.clone(),d.sub.clone(),d.revision)}
 #[test]fn original_map_manager_keeps_names_without_claiming_asset_load(){
  let vm=crate::definition_vm().unwrap();install(&vm).unwrap();
  vm.globals().set("set_map_test",vm.create_function(|vm,args:mlua::MultiValue|set(vm,args)).unwrap()).unwrap();
  vm.load("set_map_test(nil,'MAP_MSUSA_MINIHUB','HUB')").exec().unwrap();
  assert_eq!(names(&vm),(b"MAP_MSUSA_MINIHUB".to_vec(),b"HUB".to_vec(),1));
  vm.load("set_map_test({},'MAP_MSUSA_MINIHUB','HUB')").exec().unwrap();assert_eq!(names(&vm).2,1);
  for code in ["set_map_test()","set_map_test(nil,'x')","set_map_test(nil,'x','y',0)","set_map_test(nil,string.rep('x',4097),'y')"]{
   assert!(vm.load(code).exec().is_err());assert_eq!(names(&vm).2,1);
  }
  vm.load("set_map_test(false,23,'A\\0B')").exec().unwrap();assert_eq!(names(&vm),(b"23".to_vec(),b"A".to_vec(),2));
  vm.load("set_map_test(nil,false,{})").exec().unwrap();assert_eq!(names(&vm),(vec![],vec![],3));
  clear(&vm).unwrap();let v=view(&vm).unwrap();assert!(v.full.is_null()&&v.sub.is_null());assert_eq!(v.revision,0);
 }
 #[test]fn actual_meanstreet_map_helper_uses_scoped_original_manager(){
  unsafe extern "C" fn rot(_:usize,_:u32,_:f32)->u32{0}
  let vm=crate::definition_vm().unwrap();let bridge=crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  assert!(vm.load("SetMap(nil,'x','y')").exec().is_err());assert_eq!(names(&vm).2,0);
  let callbacks=crate::engine_bridge::Callbacks{context:0,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,
   resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,
   set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,
   get_prefab_data:None,fade_screen:None,camera_control:None};
  let scope=bridge.enter(callbacks).unwrap();
  if let Some(root)=std::env::var_os("EM1_LUA_ORIGINAL_FIXTURES"){
   let bytes=std::fs::read(std::path::Path::new(&root).join("HUB_MeanStreet_Shared.lua")).unwrap();let chunk=crate::prepare_chunk(&bytes).unwrap();
   vm.load(chunk.bytes.as_slice()).set_mode(mlua::ChunkMode::Binary).exec().unwrap();
   vm.globals().get::<mlua::Function>("MS_SetupMap").unwrap().call::<()>(()).unwrap();
  }else{vm.load("SetMap(nil,'MAP_MSUSA_MINIHUB','HUB')").exec().unwrap();}
  assert_eq!(names(&vm),(b"MAP_MSUSA_MINIHUB".to_vec(),b"HUB".to_vec(),1));
  vm.load("MapAlias=SetMap; function event() MapAlias(nil,'x','y') end").exec().unwrap();
  assert!(crate::callback_preflight::check(&vm,"event",0).is_err());crate::callback_preflight::check(&vm,"event",65536).unwrap();
  drop(scope);assert!(vm.load("SetMap(nil,'outside','')").exec().is_err());assert_eq!(names(&vm).2,1);
 }
}
