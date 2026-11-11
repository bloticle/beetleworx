//! Conservative admission of original callbacks before a native one-shot is
//! consumed. Missing globals/host services reject before any Lua is executed.
use mlua::{Function,Lua,Result,Value};
use std::collections::BTreeSet;
fn error(s:impl Into<String>)->mlua::Error{mlua::Error::RuntimeError(s.into())}
fn label(key:&[u8])->String{key.split(|&b|b==0).map(String::from_utf8_lossy).collect::<Vec<_>>().join(".")}
struct Reader<'a>{vm:&'a Lua,bytes:&'a[u8],at:usize,little:bool,size_t:usize,protos:usize,words:usize}
impl Reader<'_>{
 fn take(&mut self,n:usize)->Result<&[u8]>{let end=self.at.checked_add(n).ok_or_else(||error("Callback bytecode offset overflow"))?;let b=self.bytes.get(self.at..end).ok_or_else(||error("Truncated callback bytecode"))?;self.at=end;Ok(b)}
 fn number(&mut self,n:usize)->Result<usize>{let little=self.little;let b=self.take(n)?;let mut v=0usize;for(i,x)in b.iter().enumerate(){v|=usize::from(*x)<<(if little{i*8}else{(n-1-i)*8});}Ok(v)}
 fn count(&mut self)->Result<usize>{let n=self.number(4)?;if n>131072{return Err(error("Callback bytecode array budget"));}Ok(n)}
 fn text(&mut self)->Result<Vec<u8>>{let n=self.number(self.size_t)?;if n>2*1024*1024{return Err(error("Callback string budget"));}let b=self.take(n)?;if n==0{return Ok(vec![]);}if b[n-1]!=0{return Err(error("Callback string terminator"));}Ok(b[..n-1].to_vec())}
 fn proto(&mut self,depth:usize,names:&mut BTreeSet<Vec<u8>>,invokes:&mut BTreeSet<Vec<u8>>,arity_errors:&mut Vec<String>,opaque:&mut bool)->Result<(u8,u64)>{
  self.protos+=1;if depth>96 || self.protos>4096{return Err(error("Callback prototype budget"));}
  self.text()?;let upvalues=self.take(12)?[8];let n=self.count()?;self.words+=n;if self.words>131072{return Err(error("Callback instruction budget"));}
  let mut code=Vec::with_capacity(n);let mut globals=Vec::new();for _ in 0..n{let w=self.number(4)? as u32;code.push(w);if w&63==5{globals.push((w>>14)as usize);}}
  let n=self.count()?;let mut constants=Vec::with_capacity(n);let mut truth=Vec::with_capacity(n);let mut numbers=Vec::with_capacity(n);
  for _ in 0..n{let tag=self.number(1)?;let mut number=None;let (text,value)=match tag{0=>(None,3),1=>{let value=self.number(1)?;(None,if value==0{1}else{2})},3=>{
   let little=self.little;let b=self.take(8)?;number=Some(if little{u64::from_le_bytes(b.try_into().unwrap())}else{u64::from_be_bytes(b.try_into().unwrap())});(None,2)},4=>(Some(self.text()?),2),_=>return Err(error("Unsupported callback constant"))};constants.push(text);truth.push(value);numbers.push(number);}
  for index in globals{let _=constants.get(index).and_then(|v|v.as_ref()).ok_or_else(||error("Invalid callback global constant"))?;}
  let mut children=Vec::new();let mut nested=Vec::new();let mut child_calls=Vec::new();
  for _ in 0..self.count()?{let mut reads=BTreeSet::new();let mut calls=BTreeSet::new();let mut errors=Vec::new();let mut dynamic=false;
   let (count,upvalue_calls)=self.proto(depth+1,&mut reads,&mut calls,&mut errors,&mut dynamic)?;children.push(count);child_calls.push(upvalue_calls);nested.push((reads,calls,errors,dynamic));}
  let usage=crate::callback_calls::analyze(&code,&constants,&truth,&numbers,&children,&child_calls,&|key|{
   let value=resolve_path(self.vm,key)?;
   Ok(crate::callback_calls::Reference{truth:match value{Value::Nil=>3,Value::Boolean(_)=>0,_=>2},service:match &value{Value::Function(f)=>service_identity(self.vm,f)?,_=>0},
    number:match value{Value::Integer(n)=>Some((n as f64).to_bits()),Value::Number(n)=>Some(n.to_bits()),_=>None}})
  })?;
  let mut singles=BTreeSet::new();let mut trusted=BTreeSet::new();
  for key in constants.iter().flatten(){if let Value::Function(f)=resolve_path(self.vm,key)?{
   if single_result_identity(self.vm,&f)?{singles.insert(key.clone());}
   if service_identity(self.vm,&f)?!=0{trusted.insert(key.clone());}
  }}
  if let Err(e)=crate::callback_arity::check_reachable(&code,&constants,&children,&usage.reachable,&singles,&trusted){arity_errors.push(e.to_string());}
  if usage.dynamic_variable{arity_errors.push("Original AnimVarInt requires three arguments and a statically proven source variable ID before effects".into());}
  for &id in &usage.variables{if let Err(e)=crate::animation_events::variable_name(self.vm,f64::from_bits(id)){arity_errors.push(e.to_string());}}
  names.extend(usage.reads);invokes.extend(usage.invokes);*opaque|=usage.opaque_call;
  for child in usage.children{let (reads,calls,errors,dynamic)=&nested[child];names.extend(reads.iter().cloned());invokes.extend(calls.iter().cloned());arity_errors.extend(errors.iter().cloned());*opaque|=*dynamic;}
  let n=self.count()?;self.take(n*4)?;for _ in 0..self.count()?{self.text()?;self.take(8)?;}for _ in 0..self.count()?{self.text()?;}Ok((upvalues,usage.upvalue_calls))
 }
}
fn dependencies(vm:&Lua,function:&Function)->Result<(BTreeSet<Vec<u8>>,BTreeSet<Vec<u8>>,bool,u64)>{
 let b=function.dump(true);if b.len()<12 || &b[..6]!=b"\x1bLua\x51\0" || b[7]!=4 || ![4,8].contains(&b[8]) || b[9]!=4 || b[10]!=8 || b[11]!=0{return Err(error("Unsupported callback bytecode ABI"));}
 let mut reader=Reader{vm,bytes:&b,at:12,little:b[6]==1,size_t:b[8]as usize,protos:0,words:0};let mut names=BTreeSet::new();let mut invokes=BTreeSet::new();let mut arity_errors=Vec::new();let mut opaque=false;
 let (_,upvalue_calls)=reader.proto(0,&mut names,&mut invokes,&mut arity_errors,&mut opaque)?;
 if !arity_errors.is_empty(){return Err(error(arity_errors.join("; ")));}
 if reader.at!=b.len(){return Err(error("Trailing callback bytecode"));}Ok((names,invokes,opaque,upvalue_calls))
}
#[cfg(test)]fn globals(vm:&Lua,function:&Function)->Result<BTreeSet<Vec<u8>>>{Ok(dependencies(vm,function)?.0)}
fn required(name:&[u8])->u32{match name{
 b"SetRotatorMaxSpeed"=>1,b"RestrictCutSceneInput"|b"UnrestrictCutSceneInput"=>2,
 b"StartEmitters"|b"StopEmitters"=>4,b"ForEachEntityInGroup"=>8,b"Openfile"=>16,
 b"DestroyEntity"|b"GetEntity"=>32,b"AnimEvent"=>64,b"AnimGBReset"|b"AnimGBSequence"=>128,
 b"Enable"|b"Disable"|b"Hide"|b"Show"|b"Unhide"|b"SetAppCulled"|b"ClearAppCulled"=>256,b"AnimVarInt"=>512,b"EnableComponent"|b"DisableComponent"=>1024,b"GetPosition"|b"TeleportToEntity"=>2048,b"SetParentEntity"=>4096,b"GetRelativePrefabEntity"=>8192,b"GetPrefabData"=>16384,b"StartFadeOut"|b"StartFadeIn"|b"StartFadeInNoHUD"=>32768,b"SetMap"=>65536,b"CameraReset"|b"SetDefaultCameraValues"=>131072,_=>0,
}}
pub fn remember_service(vm:&Lua,name:&str,capability:u32)->Result<()>{
 let registry=match vm.named_registry_value::<Value>("em1.preflight.host_services")?{
  Value::Table(table)=>table,_=>{let table=vm.create_table()?;vm.set_named_registry_value("em1.preflight.host_services",table.clone())?;table},
 };
 registry.raw_set(vm.globals().raw_get::<Function>(name)?,capability)
}
pub fn remember_single_result(vm:&Lua,name:&str)->Result<()>{
 let registry=match vm.named_registry_value::<Value>("em1.preflight.single_returns")?{
  Value::Table(table)=>table,_=>{let table=vm.create_table()?;vm.set_named_registry_value("em1.preflight.single_returns",table.clone())?;table},
 };
 registry.raw_set(vm.globals().raw_get::<Function>(name)?,true)
}
fn single_result_identity(vm:&Lua,function:&Function)->Result<bool>{
 Ok(match vm.named_registry_value::<Value>("em1.preflight.single_returns")?{Value::Table(table)=>table.raw_get::<Option<bool>>(function.clone())?.unwrap_or(false),_=>false})
}
fn service_identity(vm:&Lua,function:&Function)->Result<u32>{
 Ok(match vm.named_registry_value::<Value>("em1.preflight.host_services")?{Value::Table(table)=>table.raw_get::<Option<u32>>(function.clone())?.unwrap_or(0),_=>0})
}
fn inspect(vm:&Lua,name:&[u8],function:Function,capabilities:u32,seen:&mut BTreeSet<usize>,depth:usize,missing:&mut BTreeSet<String>)->Result<()>{
 let identity_capability=service_identity(vm,&function)?;
 let needed=required(name)|identity_capability;if needed!=0 && capabilities&needed!=needed{missing.insert(format!("unavailable native service: {}",String::from_utf8_lossy(name)));return Ok(());}
 if name==b"<captured function>" && identity_capability&512!=0{missing.insert("captured AnimVarInt requires a statically proven source variable call".into());return Ok(());}
 if depth>64 || seen.len()>=512{return Err(error("Original callback dependency budget"));}
 if !seen.insert(function.to_pointer()as usize) || function.info().what=="C" || needed!=0{return Ok(());}
 let (reads,invokes,opaque,upvalue_calls)=dependencies(vm,&function)
  .map_err(|e|error(format!("Original callback '{}': {e}",label(name))))?;
 if opaque{missing.insert(format!("unresolved dynamic callee in {}",String::from_utf8_lossy(name)));}
 for key in reads{
  let value=resolve_path(vm,&key)?;
  match value{
   Value::Nil if invokes.contains(&key)=>{missing.insert(format!("unresolved global: {}",label(&key)));},
   Value::Function(f) if invokes.contains(&key)=>inspect(vm,&key,f,capabilities,seen,depth+1,missing)?,_=>{},
  }
 }
 if upvalue_calls!=0{
  // Read closure cells without invoking Lua, metamethods or a debug library.
  // Values remain owned mlua references; no raw pointer escapes this call.
  let values:mlua::MultiValue=unsafe{vm.exec_raw(function,|state|{
   if mlua::ffi::lua_checkstack(state,64)==0{mlua::ffi::lua_settop(state,0);return;}
   for i in 1..=60{if mlua::ffi::lua_getupvalue(state,1,i).is_null(){break;}}
   mlua::ffi::lua_remove(state,1);
  })}?;
  for i in 0..60{if upvalue_calls&(1u64<<i)!=0{match values.get(i){
   Some(Value::Function(f))=>inspect(vm,b"<captured function>",f.clone(),capabilities,seen,depth+1,missing)?,
   Some(Value::Nil)|None=>{missing.insert(format!("unresolved captured callee/value in {}",String::from_utf8_lossy(name)));},_=>{},
  }}}
 }
 Ok(())
}
fn resolve_path(vm:&Lua,key:&[u8])->Result<Value>{
 let mut parts=key.split(|&b|b==0);let mut value=vm.globals().raw_get::<Value>(vm.create_string(parts.next().unwrap_or_default())?)?;
 for part in parts{value=match value{Value::Table(table)=>table.raw_get::<Value>(vm.create_string(part)?)?,_=>Value::Nil};}
 Ok(value)
}
pub fn check(vm:&Lua,name:&str,capabilities:u32)->Result<()>{
 let callback=vm.globals().raw_get::<Function>(name)?;let mut missing=BTreeSet::new();
 inspect(vm,name.as_bytes(),callback,capabilities,&mut BTreeSet::new(),0,&mut missing)?;
 if missing.is_empty(){Ok(())}else{Err(error(format!("Original callback dependencies: {}",missing.into_iter().collect::<Vec<_>>().join("; "))))}
}
#[cfg(test)]mod tests{
 use super::*;
 #[test]fn spatial_native_services_require_owned_capabilities_before_effects(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("function position(v) return GetPosition(v) end; function teleport(a,b) TeleportToEntity(a,b) end; function parent(a,b) SetParentEntity(a,b) end; function bad_position(v) Marker=1; GetPosition(v,v) end; function bad_parent(v) Marker=1; SetParentEntity(v) end").exec().unwrap();
  for name in ["position","teleport"]{assert!(check(&vm,name,0).unwrap_err().to_string().contains("unavailable native service"));check(&vm,name,2048).unwrap();}
  assert!(check(&vm,"parent",2048).is_err());check(&vm,"parent",4096).unwrap();
  for name in ["bad_position","bad_parent"]{check(&vm,name,2048|4096).unwrap();}assert!(vm.globals().get::<Value>("Marker").unwrap().is_nil());
 }
 #[test]fn nested_owned_getters_have_proven_open_result_counts_but_replaced_globals_do_not(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("function nested(v) return GetPosition(GetRelativePrefabEntity(v,'marker')) end").exec().unwrap();
  check(&vm,"nested",2048|8192).unwrap();
  vm.load("function multi(v) return v,v end; function branch(v,flag) local f;if flag then f=GetRelativePrefabEntity else f=multi end; return GetPosition(f(v,'marker')) end; function mutate() GetRelativePrefabEntity=multi end; function after_helper(v) mutate(); return GetPosition(GetRelativePrefabEntity(v,'marker')) end; function direct_mutation(v) GetRelativePrefabEntity=multi; return GetPosition(GetRelativePrefabEntity(v,'marker')) end; function alias(v) local getter=GetRelativePrefabEntity; return GetPosition(getter(v,'marker')) end").exec().unwrap();
  check(&vm,"alias",2048|8192).unwrap();
  for name in ["branch","after_helper","direct_mutation"]{let result=check(&vm,name,2048|8192);assert!(!result.err().is_some_and(|e|e.to_string().contains("GetPosition requires")),"{name}");}
  vm.load("GetRelativePrefabEntity=function(v) return v,v end").exec().unwrap();check(&vm,"nested",2048|8192).unwrap();
 }
 #[test]fn dormant_reads_optional_nil_guards_and_unused_closures_do_not_execute_helpers(){
  let vm=crate::definition_vm().unwrap();
  vm.load("function bad() MissingActualApi() end; function read_only() local ref=bad; local optional=NotInstalled end; function unused_closure() local function dormant() MissingDormantApi() end end; function guarded() if NotInstalled then NotInstalled() end end; function guarded_alias() local ref=NotInstalled; if ref then ref() end end; function guarded_nil() if NotInstalled==nil then return end; NotInstalled() end; function future_visit() if V6_MS_StreamNPC then FireThread(V6_MS_StreamNPC) end end").exec().unwrap();
  for name in ["read_only","unused_closure","guarded","guarded_alias","guarded_nil","future_visit"]{check(&vm,name,0).unwrap_or_else(|e|panic!("{name}: {e}"));}
  vm.load("function V6_MS_StreamNPC() MissingVisitApi() end").exec().unwrap();
  let error=check(&vm,"future_visit",0).unwrap_err().to_string();assert!(error.contains("FireThread") && error.contains("MissingVisitApi"));
 }
 #[test]fn actual_calls_escaped_callbacks_and_closure_captures_keep_all_dependencies(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("function bad() MissingActualApi() end; function direct() local f=bad; f() end; function passed() ForEachEntityInGroup('group',bad) end; function stored() FutureHandler=bad end; function returned() return bad end; function captured() local f=bad; local function child() f() end; child() end; function nested() local function child() MissingNestedApi() end; child() end; function missing_data() Enable(MissingEntityConstant) end; function missing_string_dispatch() FireThread('future') end").exec().unwrap();
  for name in ["direct","passed","stored","returned","captured"]{assert!(check(&vm,name,8|256).unwrap_err().to_string().contains("MissingActualApi"),"{name}");}
  assert!(check(&vm,"nested",0).unwrap_err().to_string().contains("MissingNestedApi"));
  assert!(check(&vm,"missing_data",256).unwrap_err().to_string().contains("MissingEntityConstant"));
  assert!(check(&vm,"missing_string_dispatch",0).unwrap_err().to_string().contains("FireThread"));
 }
 #[test]fn captured_cells_include_later_writes_sibling_mutation_and_real_external_upvalues(){
  let vm=crate::definition_vm().unwrap();
  vm.load("function bad() MissingActualApi() end; function late() local f; local function inner() f() end; f=bad; inner() end; function sibling() local f; local function inner() f() end; local function mutate() f=bad end; mutate(); inner() end; local external=bad; function external_capture() external() end; function changed_global() Optional=bad; if Optional then Optional() end end").exec().unwrap();
  for name in ["late","sibling","external_capture","changed_global"]{assert!(check(&vm,name,0).unwrap_err().to_string().contains("MissingActualApi"),"{name}");}
 }
 #[test]fn static_field_aliases_resolve_actual_functions_and_dynamic_callees_reject(){
  let vm=crate::definition_vm().unwrap();
  vm.load("function bad() MissingActualApi() end; Namespace={bad=bad}; function field_alias() local f=Namespace.bad; f() end; function method() Namespace:bad() end; function safe_field() local f=math.abs; return f(-1) end; function unknown_field(key) Namespace[key]() end").exec().unwrap();
  for name in ["field_alias","method"]{assert!(check(&vm,name,0).unwrap_err().to_string().contains("MissingActualApi"));}
  check(&vm,"safe_field",0).unwrap();assert!(check(&vm,"unknown_field",0).unwrap_err().to_string().contains("dynamic callee"));
 }
 #[test]fn captured_host_functions_keep_capability_identity_after_global_replacement(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("local captured=Enable; AliasEnable=Enable; Enable=nil; function event(v) captured(v) end; function alias(v) AliasEnable(v) end").exec().unwrap();
  for name in ["event","alias"]{assert!(check(&vm,name,0).unwrap_err().to_string().contains("unavailable native service"));check(&vm,name,256).unwrap();}
 }
 #[test]fn missing_nested_engine_calls_reject_before_any_partial_execution(){
  let vm=crate::definition_vm().unwrap();vm.load("effect=0; function deeper() MissingNativeApi() end; function event() effect=effect+1; deeper() end; function safe() effect=effect+1 end").exec().unwrap();
  assert!(check(&vm,"event",0).unwrap_err().to_string().contains("MissingNativeApi"));assert_eq!(vm.globals().get::<u32>("effect").unwrap(),0);check(&vm,"safe",0).unwrap();
 }
 #[test]fn scoped_services_require_explicit_host_capabilities(){
  let vm=crate::definition_vm().unwrap();vm.globals().set("StartEmitters",vm.create_function(|_,()|Ok(())).unwrap()).unwrap();
  vm.load("function event() StartEmitters() end").exec().unwrap();assert!(check(&vm,"event",0).is_err());check(&vm,"event",4).unwrap();
  vm.load("function a() b() end; function b() a() end").exec().unwrap();check(&vm,"a",0).unwrap();
 }
 #[test]fn destruction_requires_wired_native_capability_before_any_effects(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("effects=0; function event(v) effects=effects+1; DestroyEntity(v) end").exec().unwrap();
  assert!(check(&vm,"event",0).unwrap_err().to_string().contains("DestroyEntity"));
  assert_eq!(vm.globals().get::<u32>("effects").unwrap(),0);check(&vm,"event",32).unwrap();
 }
 #[test]fn gamebryo_requires_wired_service_before_any_partial_cutscene_effect(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("effects=0; function event(v) effects=effects+1; AnimGBReset(v); AnimGBSequence(v,'Intro') end").exec().unwrap();
  assert!(check(&vm,"event",0).is_err());assert_eq!(vm.globals().get::<u32>("effects").unwrap(),0);check(&vm,"event",128).unwrap();
 }
 #[test]fn entity_controls_require_host_capability_before_one_shot_or_effects(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("effects=0; function event(v) effects=effects+1; Enable(v); Disable(v); Hide(v); Show(v); SetAppCulled(v); ClearAppCulled(v) end").exec().unwrap();
  assert!(check(&vm,"event",0).unwrap_err().to_string().contains("unavailable native service"));assert_eq!(vm.globals().get::<u32>("effects").unwrap(),0);
  check(&vm,"event",256).unwrap();
 }
 #[test]fn application_culling_requires_capability_through_aliases_and_captures(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("effects=0; CullAlias=SetAppCulled; local clear=ClearAppCulled; function direct(v) effects=effects+1; SetAppCulled(v) end; function alias(v) effects=effects+1; CullAlias(v) end; function captured(v) effects=effects+1; clear(v) end; SetAppCulled=nil; ClearAppCulled=nil").exec().unwrap();
  for name in ["alias","captured"]{
   assert!(check(&vm,name,0).unwrap_err().to_string().contains("unavailable native service"));
   assert_eq!(vm.globals().get::<u32>("effects").unwrap(),0);check(&vm,name,256).unwrap();
  }
  assert!(check(&vm,"direct",256).unwrap_err().to_string().contains("SetAppCulled"));
 }
 #[test]fn variable_and_component_services_require_capabilities_and_variable_arity_proof(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("effects=0; function variable(v) effects=effects+1; AnimVarInt(v,0,1) end; function wrong(v) effects=effects+1; AnimVarInt(v,0,1,true) end; function component(v) effects=effects+1; DisableComponent(v,'InputOutputComponent'); Unhide(v) end").exec().unwrap();
  assert!(check(&vm,"variable",0).is_err());
  if let Some(path)=std::env::var_os("EM1_ANIMATION_REL_FIXTURE"){
   crate::animation_events::install(&vm,&std::fs::read(path).unwrap()).unwrap();
   crate::animation_events::install_targets(&vm,[("VAR_Normalized_Forward_Speed".into(),99)].into()).unwrap();check(&vm,"variable",512).unwrap();
  }else{assert!(check(&vm,"variable",512).is_err());}
  assert!(check(&vm,"wrong",512).unwrap_err().to_string().contains("three arguments"));
  assert!(check(&vm,"component",256).is_err());check(&vm,"component",256|1024).unwrap();
  assert_eq!(vm.globals().get::<u32>("effects").unwrap(),0);
 }
 #[test]fn unsupported_and_dynamic_variable_ids_reject_before_effects_including_aliases(){
  let Some(path)=std::env::var_os("EM1_ANIMATION_REL_FIXTURE")else{return;};
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  crate::animation_events::install(&vm,&std::fs::read(path).unwrap()).unwrap();
  crate::animation_events::install_targets(&vm,[("VAR_In_Conversation".into(),99)].into()).unwrap();
  vm.load("effects=0; AliasVar=AnimVarInt; Namespace={set=AnimVarInt}; function good(v) effects=effects+1; AnimVarInt(v,VAR_In_Conversation,1) end; function absent(v) effects=effects+1; AnimVarInt(v,VAR_SuperSaturated_Blend,1) end; function absent_number(v) effects=effects+1; AliasVar(v,13,1) end; function field_good(v) Namespace.set(v,VAR_In_Conversation,1) end; function field_bad(v) Namespace.set(v,13,1) end; function dynamic(v,id) AnimVarInt(v,id,1) end").exec().unwrap();
  for name in ["good","field_good"]{check(&vm,name,512).unwrap();}
  for name in ["absent","absent_number","field_bad"]{assert!(check(&vm,name,512).unwrap_err().to_string().contains("VAR_SuperSaturated_Blend"),"{name}");}
  assert!(check(&vm,"dynamic",512).unwrap_err().to_string().contains("statically proven"));assert_eq!(vm.globals().get::<u32>("effects").unwrap(),0);
 }
 #[test]fn original_meanstreet_setup_references_real_entity_lifecycle_functions(){
  let Some(root)=std::env::var_os("EM1_LUA_ORIGINAL_FIXTURES")else{return;};
  let bytes=std::fs::read(std::path::Path::new(&root).join("HUB_MeanStreet_V1.lua")).unwrap();
  let chunk=crate::prepare_chunk(&bytes).unwrap();let vm=crate::definition_vm().unwrap();
  vm.load(chunk.bytes.as_slice()).set_mode(mlua::ChunkMode::Binary).exec().unwrap();
  let launcher=globals(&vm,&vm.globals().get::<Function>("MS_V1_AlwaysSetup_Launcher").unwrap()).unwrap();
  let pete=globals(&vm,&vm.globals().get::<Function>("MS_HidePeteV1").unwrap()).unwrap();
  assert!(launcher.contains(b"MSV1_AlwaysSetup".as_slice()));assert!(pete.contains(b"Hide".as_slice()));
  crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  crate::global_bridge::install(&vm).unwrap();check(&vm,"MS_HidePeteV1",256).unwrap();
  if let Some(path)=std::env::var_os("EM1_ANIMATION_REL_FIXTURE"){
   crate::animation_events::install(&vm,&std::fs::read(path).unwrap()).unwrap();
   crate::animation_events::install_targets(&vm,[("VAR_In_Conversation".into(),72)].into()).unwrap();
  }
  let dependencies=check(&vm,"MS_V1_AlwaysSetup_Launcher",32|64|128|256).unwrap_err().to_string();
  println!("MEANSTREET_ORIGINAL_DEPENDENCIES {dependencies}");
  assert!(!dependencies.contains("GetPosition requires") && !dependencies.contains("SetParentEntity requires") && !dependencies.contains("TeleportToEntity requires"));
  assert!(!dependencies.split("; ").any(|s|s.ends_with("unresolved global: Enable")));
  if std::env::var_os("EM1_ANIMATION_REL_FIXTURE").is_some(){assert!(dependencies.contains("StartFadeOut"));}
 }
 #[test]fn unsupported_gamebryo_arity_rejects_before_effects_even_with_service_enabled(){
  let vm=crate::definition_vm().unwrap();crate::engine_bridge::install(&vm,crate::entities::Registry::default()).unwrap();
  vm.load("effects=0; function event(v) effects=effects+1; local f=AnimGBSequence; f(v,'Break',true) end").exec().unwrap();
  assert!(check(&vm,"event",128).unwrap_err().to_string().contains("rejected before effects"));assert_eq!(vm.globals().get::<u32>("effects").unwrap(),0);
 }
 #[test]fn original_zone_a_opening_arity_admitted_but_real_optional_form_rejected(){
  let Some(root)=std::env::var_os("EM1_LUA_ORIGINAL_FIXTURES")else{return;};
  let path=std::path::Path::new(&root).join("DBC_1st_ZoneA.lua");if !path.exists(){return;}
  let bytes=std::fs::read(path).unwrap();let chunk=crate::prepare_chunk(&bytes).unwrap();let vm=crate::definition_vm().unwrap();
  vm.load(chunk.bytes.as_slice()).set_mode(mlua::ChunkMode::Binary).exec().unwrap();
  globals(&vm,&vm.globals().get::<Function>("DBC_1st_ZoneA_cutscene").unwrap()).unwrap();
  assert!(globals(&vm,&vm.globals().get::<Function>("DBC_1st_ZoneA_cutscene_leftSideSmash").unwrap()).unwrap_err().to_string().contains("rejected before effects"));
 }
}
