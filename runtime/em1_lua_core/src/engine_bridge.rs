//! Synchronous engine access is granted only while the host advances this VM.
use crate::entities::Registry;
use crate::source_vector::Vector4 as NativeVector4;
use std::sync::{Arc,Mutex};
#[repr(C)]
#[derive(Clone,Copy)]
pub struct Callbacks {
    pub context:usize,
    pub set_cutscene_input:Option<unsafe extern "C" fn(usize,u32)->u32>,
    pub set_rotator_speed:Option<unsafe extern "C" fn(usize,u32,f32)->u32>,
    pub set_particle_emission:Option<unsafe extern "C" fn(usize,u32,u32)->u32>,
    pub resolve_group:Option<unsafe extern "C" fn(usize,*const u8,u32,*mut u32,u32,*mut u32)->u32>,
    pub resolve_entity_name:Option<unsafe extern "C" fn(usize,*const u8,u32,*mut u32)->u32>,
    pub destroy_entity:Option<unsafe extern "C" fn(usize,u32)->u32>,
    pub animate_entity:Option<unsafe extern "C" fn(usize,u32,*const u8,u32)->u32>,
    pub gamebryo_animation:Option<unsafe extern "C" fn(usize,u32,u32,*const u8,u32)->u32>,
    pub control_entity:Option<unsafe extern "C" fn(usize,u32,u32)->u32>,
    pub set_animation_variable:Option<unsafe extern "C" fn(usize,u32,*const u8,u32,i32)->u32>,
    pub control_component:Option<unsafe extern "C" fn(usize,u32,*const u8,u32,u32)->u32>,
    pub get_position:Option<unsafe extern "C" fn(usize,u32,*mut f32)->u32>,
    pub teleport_entity:Option<unsafe extern "C" fn(usize,u32,u32,u32,u32)->u32>,
    pub set_parent:Option<unsafe extern "C" fn(usize,u32,u32)->u32>,
    pub resolve_relative_entity:Option<unsafe extern "C" fn(usize,u32,*const u8,u32,*mut u32)->u32>,
    pub get_prefab_data:Option<unsafe extern "C" fn(usize,u32,*mut u32,*mut *const u8,*mut usize)->u32>,
    pub fade_screen:Option<unsafe extern "C" fn(usize,u32,f32)->u32>,
    pub camera_control:Option<unsafe extern "C" fn(usize,u32,f32,f32,f32)->u32>,
}
#[derive(Clone,Default)]
pub struct Bridge(Arc<Mutex<Option<Callbacks>>>);
pub struct Scope(Bridge);
impl Drop for Scope {fn drop(&mut self){*self.0.0.lock().unwrap_or_else(|e|e.into_inner())=None;}}
fn error(message:&str)->mlua::Error{mlua::Error::RuntimeError(message.into())}
fn named_entity(callbacks:Callbacks,name:&mlua::String)->mlua::Result<Option<u32>>{
    let bytes=name.as_bytes();if bytes.is_empty() || bytes.len()>256 || bytes.contains(&0){return Err(error("Invalid native entity name"));}
    let call=callbacks.resolve_entity_name.ok_or_else(||error("Native entity name binding unavailable"))?;
    let mut id=0;
    match unsafe{call(callbacks.context,bytes.as_ptr(),bytes.len() as u32,&mut id)}{
        0 if id!=0=>Ok(Some(id)),1=>Ok(None),_=>Err(error("Native entity is foreign, stale or unavailable")),
    }
}
fn owned_reference(callbacks:Callbacks,entities:&Registry,value:mlua::Value)->mlua::Result<u32>{
    match value{
        mlua::Value::Nil=>Ok(0),mlua::Value::UserData(value)=>entities.resolve(&value),
        mlua::Value::String(name)=>match named_entity(callbacks,&name)?{None=>Ok(0),Some(id)=>entities.resolve(&entities.snapshot(id)?)},
        _=>Err(error("Native spatial operation requires an owned entity or native entity name")),
    }
}
fn native_boolean(vm:&mlua::Lua,value:mlua::Value)->mlua::Result<u32>{
    if let Some(number)=vm.coerce_number(value.clone())?{return Ok(u32::from(number!=0.));}
    Ok(match value{mlua::Value::Nil=>0,mlua::Value::Boolean(v)=>u32::from(v),
        mlua::Value::String(v) if v.as_bytes().eq_ignore_ascii_case(b"false")=>0,_=>1})
}
// REL2786B0: original parameter reader accepts numeric strings, booleans,
// named numeric Lua globals and nil. fctiwz truncates, with signed saturation.
fn original_integer(vm:&mlua::Lua,value:mlua::Value)->mlua::Result<i32>{
    fn converted(number:f64)->i32{if number.is_nan(){i32::MIN}else if number>=i32::MAX as f64{i32::MAX}else if number<=i32::MIN as f64{i32::MIN}else{number.trunc()as i32}}
    if let Some(number)=vm.coerce_number(value.clone())?{return Ok(converted(number));}
    match value{
        mlua::Value::Boolean(value)=>Ok(i32::from(value)),
        mlua::Value::String(name)=>{let bytes=name.as_bytes();let end=bytes.iter().position(|&c|c==0).unwrap_or(bytes.len());
            let value=vm.globals().get::<mlua::Value>(vm.create_string(&bytes[..end])?)?;
            Ok(vm.coerce_number(value)?.map(converted).unwrap_or(0))},
        _=>Ok(0),
    }
}
impl Bridge {
    pub fn enter(&self,callbacks:Callbacks)->mlua::Result<Scope>{
        if callbacks.set_rotator_speed.is_none(){return Err(error("Incomplete native engine callbacks"));}
        let mut active=self.0.lock().unwrap_or_else(|e|e.into_inner());
        if active.is_some(){return Err(error("Nested native engine dispatch"));}
        *active=Some(callbacks);Ok(Scope(self.clone()))
    }
}
pub fn install(vm:&mlua::Lua,entities:Registry)->mlua::Result<Bridge>{
    // Original retail REL277D8C performs every actual global tostring call,
    // including booleans. It suppresses output entirely; no Xbox output flag
    // is removed, and source metamethod/conversion side effects still run.
    vm.globals().raw_set("Print",vm.load(r#"local kind,fail,select=type,error,select
        return function(...)
            local stringify=tostring
            for i=1,select('#',...) do
                local item=select(i,...); local value=stringify(item)
                if kind(value)~='string' and kind(value)~='number' then
                    fail("'tostring' must return a string to 'Print'")
                end
            end
        end"#).eval::<mlua::Function>()?)?;
    let bridge=Bridge::default();
    crate::source_map::install(vm)?;
    {
        let access=bridge.clone();
        vm.globals().raw_set("SetMap",vm.create_function(move |vm,args:mlua::MultiValue|{
            let _callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            crate::source_map::set(vm,args)
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("GetPosition",vm.create_function(move |_,mut args:mlua::MultiValue|{
            if args.len()!=1{return Err(error("Original GetPosition requires one entity argument"));}
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let id=owned_reference(callbacks,&entities,args.pop_back().unwrap())?;
            let call=callbacks.get_position.ok_or_else(||error("Native position binding unavailable"))?;
            let mut value=[f32::NAN;4];
            match unsafe{call(callbacks.context,id,value.as_mut_ptr())}{0|1 if value.iter().all(|v|v.is_finite())=>Ok(NativeVector4(value)),_=>Err(error("Native position rejected its target or returned invalid coordinates"))}
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("TeleportToEntity",vm.create_function(move |vm,mut args:mlua::MultiValue|{
            if args.len()<2{return Err(error("Original TeleportToEntity requires at least entity and destination"));}
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let count=args.len();
            let reset=if count>3{native_boolean(vm,args.pop_back().unwrap())?}else{0};
            let orientation=if count>2{native_boolean(vm,args.pop_back().unwrap())?}else{1};
            let destination=owned_reference(callbacks,&entities,args.pop_back().unwrap())?;
            let entity=owned_reference(callbacks,&entities,args.pop_back().unwrap())?;
            let call=callbacks.teleport_entity.ok_or_else(||error("Native teleport binding unavailable"))?;
            match unsafe{call(callbacks.context,entity,destination,reset,orientation)}{0|1=>Ok(()),_=>Err(error("Original native teleport rejected its owned references"))}
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("SetParentEntity",vm.create_function(move |_,mut args:mlua::MultiValue|{
            if args.len()!=2{return Err(error("Original SetParentEntity requires child and parent arguments"));}
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let parent=owned_reference(callbacks,&entities,args.pop_back().unwrap())?;
            let child=owned_reference(callbacks,&entities,args.pop_back().unwrap())?;
            let call=callbacks.set_parent.ok_or_else(||error("Native parent binding unavailable"))?;
            match unsafe{call(callbacks.context,child,parent)}{0|1=>Ok(()),_=>Err(error("Original parent change rejected its owned references"))}
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("GetRelativePrefabEntity",vm.create_function(move |_,mut args:mlua::MultiValue|{
            if args.len()!=2{return Err(error("Original GetRelativePrefabEntity requires entity and relative name"));}
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let Some(mlua::Value::String(name))=args.pop_back()else{return Err(error("Relative prefab entity requires a native name string"));};
            let bytes=name.as_bytes();if bytes.len()>256 || bytes.contains(&0){return Err(error("Invalid relative prefab entity name"));}
            let owner=owned_reference(callbacks,&entities,args.pop_back().unwrap())?;
            let call=callbacks.resolve_relative_entity.ok_or_else(||error("Native relative prefab lookup unavailable"))?;
            let mut id=0;
            match unsafe{call(callbacks.context,owner,bytes.as_ptr(),bytes.len()as u32,&mut id)}{
                0 if id!=0=>Ok(Some(entities.snapshot(id)?)),1=>Ok(None),_=>Err(error("Native relative prefab entity is foreign, stale or unavailable")),
            }
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("GetPrefabData",vm.create_function(move |vm,mut args:mlua::MultiValue|{
            if args.len()!=1{return Err(error("Original GetPrefabData requires one entity"));}
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let entity=owned_reference(callbacks,&entities,args.pop_back().unwrap())?;
            let call=callbacks.get_prefab_data.ok_or_else(||error("Native source prefab data unavailable"))?;
            let mut owner=[0u32;5];let mut bytes=std::ptr::null();let mut length=0usize;
            let status=unsafe{call(callbacks.context,entity,owner.as_mut_ptr(),&mut bytes,&mut length)};
            if status==1{return Ok(None);}if status!=0 || owner[..4]==[0;4] || owner[4]==0 || bytes.is_null() || length>4*1024*1024+16384*8+8{return Err(error("Original prefab data owner/transport was rejected"));}
            let identity=entities.snapshot(owner[4])?;entities.resolve(&identity)?;
            let generation=identity.borrow::<crate::entities::Entity>()?.generation;let guid=owner[..4].try_into().unwrap();
            if let Some(table)=crate::prefab_data::get(vm,guid,generation)?{return Ok(Some(table));}
            if crate::prefab_data::initializing(vm,guid,generation)?{return Ok(None);}
            let packet=unsafe{std::slice::from_raw_parts(bytes,length)}.to_vec();
            let (defaults,overrides)=crate::prefab_data::decode(&packet)?;
            crate::prefab_data::prepare(vm,guid,generation,&format!("source-owner:{:08x}",guid[0]),&defaults,&overrides)?;
            crate::prefab_data::get(vm,guid,generation)
        })?)?;
    }
    for (name,operation) in [("CameraReset",0u32),("SetDefaultCameraValues",1u32)]{
        let access=bridge.clone();
        vm.globals().raw_set(name,vm.create_function(move |vm,args:mlua::MultiValue|{
            let expected=if operation==0{0}else{3};
            if args.len()!=expected{return Err(error("Original camera function received an incorrect argument count"));}
            let mut values=[0f32;3];
            // REL27883C is Lua_tonumber followed by frsp; no boolean/global coercion.
            for (index,value) in args.into_iter().enumerate(){
                let number=vm.coerce_number(value)?.unwrap_or(0.);
                if !number.is_finite() || !(number as f32).is_finite(){return Err(error("Original camera value must fit a finite native float"));}
                values[index]=number as f32;
            }
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let call=callbacks.camera_control.ok_or_else(||error("Native primary camera binding unavailable"))?;
            match unsafe{call(callbacks.context,operation,values[0],values[1],values[2])}{0|1=>Ok(()),_=>Err(error("Original native primary camera rejected its context"))}
        })?)?;
    }
    for (name,operation) in [("StartFadeOut",0u32),("StartFadeIn",1u32),("StartFadeInNoHUD",2u32)]{
        let access=bridge.clone();
        vm.globals().raw_set(name,vm.create_function(move |vm,mut args:mlua::MultiValue|{
            if operation==0 && args.len()!=1{return Err(error("Original StartFadeOut requires exactly one duration argument"));}
            // Original Lua_tonumber returns zero for nonnumeric values; both
            // optional fade-ins retain the native -1 configured-time sentinel.
            let seconds=match args.pop_back(){None=>-1.,Some(value)=>vm.coerce_number(value)?.unwrap_or(0.)};
            if !seconds.is_finite() || !(seconds as f32).is_finite(){return Err(error("Original fade duration must fit a finite native float"));}
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let call=callbacks.fade_screen.ok_or_else(||error("Native screen fade binding unavailable"))?;
            match unsafe{call(callbacks.context,operation,seconds as f32)}{0=>Ok(()),_=>Err(error("Original native screen fade rejected its context"))}
        })?)?;
    }
    for (name,operation) in [("Enable",0u32),("Disable",1u32),("Hide",2u32),("Show",3u32),("SetAppCulled",4u32),("ClearAppCulled",5u32)]{
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set(name,vm.create_function(move |_,mut args:mlua::MultiValue|{
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            // Original stack readers consume the last Lua argument; absent/
            // nil and missing names resolve to null and native helpers no-op.
            let id=match args.pop_back().unwrap_or(mlua::Value::Nil){
                mlua::Value::Nil=>return Ok(()),
                mlua::Value::UserData(value)=>entities.resolve(&value)?,
                mlua::Value::String(value)=>match named_entity(callbacks,&value)?{
                    None=>return Ok(()),Some(id)=>entities.resolve(&entities.snapshot(id)?)?,
                },_=>return Err(error("Entity control requires an owned entity or native entity name")),
            };
            let call=callbacks.control_entity.ok_or_else(||error("Native entity control binding unavailable"))?;
            match unsafe{call(callbacks.context,id,operation)}{0|1=>Ok(()),_=>Err(error("Original native entity control rejected its target"))}
        })?)?;
    }
    vm.globals().raw_set("Unhide",vm.globals().raw_get::<mlua::Function>("Show")?)?;
    for (name,disabled) in [("EnableComponent",0u32),("DisableComponent",1u32)]{
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set(name,vm.create_function(move |_,mut args:mlua::MultiValue|{
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let Some(mlua::Value::String(component))=args.pop_back()else{return Err(error("Component control requires a native component name as its last argument"));};
            let bytes=component.as_bytes();if bytes.is_empty() || bytes.len()>256 || bytes.contains(&0){return Err(error("Invalid native component name"));}
            let id=match args.pop_back().unwrap_or(mlua::Value::Nil){
                mlua::Value::Nil=>return Ok(()),mlua::Value::UserData(value)=>entities.resolve(&value)?,
                mlua::Value::String(value)=>match named_entity(callbacks,&value)?{None=>return Ok(()),Some(id)=>entities.resolve(&entities.snapshot(id)?)?},
                _=>return Err(error("Component control requires an owned entity or native entity name")),
            };
            let call=callbacks.control_component.ok_or_else(||error("Native component control binding unavailable"))?;
            match unsafe{call(callbacks.context,id,bytes.as_ptr(),bytes.len()as u32,disabled)}{0|1=>Ok(()),_=>Err(error("Original native component control rejected its target"))}
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("AnimVarInt",vm.create_function(move |vm,mut args:mlua::MultiValue|{
            if args.len()!=3{return Err(error("AnimVarInt requires the verified entity, original variable ID, integer value form"));}
            let value=original_integer(vm,args.pop_back().unwrap())?;
            let variable=f64::from(original_integer(vm,args.pop_back().unwrap())?);
            let name=crate::animation_events::variable_name(vm,variable)?;
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let id=match args.pop_back().unwrap(){
                mlua::Value::Nil=>return Ok(()),mlua::Value::UserData(value)=>entities.resolve(&value)?,
                mlua::Value::String(value)=>match named_entity(callbacks,&value)?{None=>return Ok(()),Some(id)=>entities.resolve(&entities.snapshot(id)?)?},
                _=>return Err(error("AnimVarInt requires an owned entity or native entity name")),
            };
            let call=callbacks.set_animation_variable.ok_or_else(||error("Native animation variable binding unavailable"))?;
            match unsafe{call(callbacks.context,id,name.as_ptr(),name.len()as u32,value)}{0|1=>Ok(()),_=>Err(error("Original animation variable has no admitted native target/mapping"))}
        })?)?;
    }
    for (name,operation,arity) in [("AnimGBReset",0u32,1usize),("AnimGBSequence",1u32,2usize)]{
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set(name,vm.create_function(move |_,mut args:mlua::MultiValue|{
            // These are the verified forms used by the original opening script.
            // Optional sequence timing/callback forms require separate ABI work.
            if args.len()!=arity{return Err(error("Gamebryo binding requires verified entity/reset or entity/sequence form"));}
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let id=match args.pop_front().unwrap(){
                mlua::Value::UserData(value)=>entities.resolve(&value)?,
                mlua::Value::String(value)=>match named_entity(callbacks,&value)?{
                    None if operation==0=>return Ok(()),None=>return Err(error("AnimGBSequence target is absent")),
                    Some(id)=>entities.resolve(&entities.snapshot(id)?)?,
                },_=>return Err(error("Gamebryo animation requires an owned entity or native entity name")),
            };
            let sequence=if operation==1{
                let Some(mlua::Value::String(name))=args.pop_front()else{return Err(error("Gamebryo sequence name must be a string"));};
                let bytes=name.as_bytes();if bytes.is_empty() || bytes.len()>256 || bytes.contains(&0){return Err(error("Invalid Gamebryo sequence name"));}
                bytes.to_vec()
            }else{vec![]};
            let call=callbacks.gamebryo_animation.ok_or_else(||error("Native Gamebryo animation binding unavailable"))?;
            match unsafe{call(callbacks.context,id,operation,sequence.as_ptr(),sequence.len() as u32)}{
                0=>Ok(()),1 if operation==0=>Ok(()),_=>Err(error("Gamebryo target/sequence is unavailable; original clip conversion or retargeting is required")),
            }
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("GetEntity",vm.create_function(move |_,name:mlua::String|{
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            named_entity(callbacks,&name)?.map(|id|entities.snapshot(id)).transpose()
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("AnimEvent",vm.create_function(move |vm,(value,event):(mlua::Value,f64)|{
            let name=crate::animation_events::name(vm,event)?;
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let id=match value{
                mlua::Value::UserData(value)=>entities.resolve(&value)?,
                mlua::Value::String(value)=>match named_entity(callbacks,&value)?{
                    None=>return Ok(()),Some(id)=>entities.resolve(&entities.snapshot(id)?)?,
                },_=>return Err(error("AnimEvent requires an owned entity or native entity name")),
            };
            let call=callbacks.animate_entity.ok_or_else(||error("Native animation binding unavailable"))?;
            match unsafe{call(callbacks.context,id,name.as_ptr(),name.len() as u32)}{
                0|1=>Ok(()),_=>Err(error("Original animation event has no admitted native target/mapping")),
            }
        })?)?;
    }
    {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set("DestroyEntity",vm.create_function(move |vm,value:mlua::Value|{
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let id=match value{
                mlua::Value::UserData(value)=>entities.resolve(&value)?,
                mlua::Value::String(name)=>match named_entity(callbacks,&name)?{
                    None=>return Ok(()),Some(id)=>{let value=entities.snapshot(id)?;entities.resolve(&value)?},
                },
                _=>return Err(error("DestroyEntity requires an owned entity or native entity name")),
            };
            let call=callbacks.destroy_entity.ok_or_else(||error("Native destruction binding unavailable"))?;
            match unsafe{call(callbacks.context,id)}{
                0=>{},1=>return Ok(()),_=>return Err(error("Original native DestroyEntity rejected the entity")),
            }
            let generation=entities.snapshot(id)?.borrow::<crate::entities::Entity>()?.generation;
            crate::prefab_data::forget_generation(vm,generation)?;crate::primary_player::revoke(vm,id)?;entities.unbind(id);Ok(())
        })?)?;
    }
    let access=bridge.clone();let rotator_entities=entities.clone();
    vm.globals().raw_set("SetRotatorMaxSpeed",vm.create_function(move |_,(entity,speed):(mlua::AnyUserData,f64)|{
        if !speed.is_finite() || !(speed as f32).is_finite(){return Err(error("Rotator speed must fit a finite native float"));}
        let id=rotator_entities.resolve(&entity)?;
        let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
        match unsafe{callbacks.set_rotator_speed.unwrap()(callbacks.context,id,speed as f32)}{
            0=>Ok(()),_=>Err(error("Native SetRotatorMaxSpeed failed")),
        }
    })?)?;
    for (name,restricted) in [("RestrictCutSceneInput",1u32),("UnrestrictCutSceneInput",0u32)] {
        let access=bridge.clone();
        vm.globals().raw_set(name,vm.create_function(move |_,args:mlua::MultiValue|{
            if !args.is_empty(){return Err(error("Only the no-argument cutscene input binding is available"));}
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let call=callbacks.set_cutscene_input.ok_or_else(||error("Native cutscene input binding unavailable"))?;
            match unsafe{call(callbacks.context,restricted)}{0=>Ok(()),_=>Err(error("Native cutscene input operation failed"))}
        })?)?;
    }
    for (name,enabled) in [("StartEmitters",1u32),("StopEmitters",0u32)] {
        let access=bridge.clone();let entities=entities.clone();
        vm.globals().raw_set(name,vm.create_function(move |_,mut args:mlua::MultiValue|{
            let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
            let id=match args.pop_back().unwrap_or(mlua::Value::Nil){
                mlua::Value::Nil=>return Ok(()),mlua::Value::UserData(entity)=>entities.resolve(&entity)?,
                mlua::Value::String(name)=>match named_entity(callbacks,&name)?{None=>return Ok(()),Some(id)=>entities.resolve(&entities.snapshot(id)?)?},
                _=>return Err(error("Emitter commands require an owned entity or native entity name")),
            };
            let call=callbacks.set_particle_emission.ok_or_else(||error("Native particle binding unavailable"))?;
            match unsafe{call(callbacks.context,id,enabled)}{0|1=>Ok(()),_=>Err(error("Native emitter operation failed"))}
        })?)?;
    }
    let access=bridge.clone();let group_entities=entities.clone();
    let resolve=vm.create_function(move |vm,value:mlua::Value|{
        let mlua::Value::String(name)=value else{return Err(error("Group name must be a string"));};
        let bytes=name.as_bytes();
        if bytes.is_empty() || bytes.len()>256 || bytes.contains(&0){return Err(error("Invalid group name"));}
        let callbacks=(*access.0.lock().unwrap_or_else(|e|e.into_inner())).ok_or_else(||error("Native engine unavailable outside simulation dispatch"))?;
        let call=callbacks.resolve_group.ok_or_else(||error("Native group binding unavailable"))?;
        let mut handles=vec![0u32;4096];let mut count=0u32;
        if unsafe{call(callbacks.context,bytes.as_ptr(),bytes.len() as u32,handles.as_mut_ptr(),handles.len() as u32,&mut count)}!=0 || count as usize>handles.len(){return Err(error("Native group resolution failed"));}
        let members=vm.create_table()?;
        for (index,id) in handles[..count as usize].iter().enumerate(){
            // Native82731310 pushes nil for a zero reference. Preserve those
            // positions and repeated members exactly as the native iterator.
            if *id!=0{members.raw_set(index+1,group_entities.snapshot(*id)?)?;}
        }
        Ok((members,count))
    })?;
    let access=bridge.clone();
    let validate=vm.create_function(move |_,entity:mlua::AnyUserData|{
        if access.0.lock().unwrap_or_else(|e|e.into_inner()).is_none(){return Err(error("Native engine unavailable outside simulation dispatch"));}
        entities.resolve(&entity).map(|_|())
    })?;
    // Keep the iteration in Lua: callbacks may yield without crossing a Rust
    // callback frame. Revalidate the snapshot before each callback after resume.
    let factory=vm.load(r#"return function(resolve, validate)
        local kind, fail = type, error
        return function(callback, group, ...)
            if kind(callback) ~= 'function' then fail('ForEachEntityInGroup requires a function and group name') end
            local members, count = resolve(group)
            for i=1,count do
                local member = members[i]
                if member ~= nil then validate(member) end
                callback(member, ...)
            end
        end
    end"#).eval::<mlua::Function>()?;
    vm.globals().raw_set("ForEachEntityInGroup",factory.call::<mlua::Function>((resolve,validate))?)?;
    for (name,capability) in [("SetRotatorMaxSpeed",1),("RestrictCutSceneInput",2),("UnrestrictCutSceneInput",2),
        ("StartEmitters",4),("StopEmitters",4),("ForEachEntityInGroup",8),("GetEntity",32),("DestroyEntity",32),
        ("AnimEvent",64),("AnimGBReset",128),("AnimGBSequence",128),("Enable",256),("Disable",256),("Hide",256),("Show",256),("Unhide",256),("SetAppCulled",256),("ClearAppCulled",256),
        ("AnimVarInt",512),("EnableComponent",1024),("DisableComponent",1024),("GetPosition",2048),("TeleportToEntity",2048),("SetParentEntity",4096),("GetRelativePrefabEntity",8192),("GetPrefabData",16384),("StartFadeOut",32768),("StartFadeIn",32768),("StartFadeInNoHUD",32768),("SetMap",65536),("CameraReset",131072),("SetDefaultCameraValues",131072)]{
        crate::callback_preflight::remember_service(vm,name,capability)?;
    }
    for name in ["GetPosition","GetEntity","GetRelativePrefabEntity","GetPrefabData"]{crate::callback_preflight::remember_single_result(vm,name)?;}
    Ok(bridge)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]fn spatial_snapshots_and_teleports_preserve_flags_and_validate_both_owners(){
        #[derive(Default)]struct State{calls:Vec<(u32,u32,u32,u32)>,status:u32}
        unsafe extern "C" fn rot(_:usize,_:u32,_:f32)->u32{0}
        unsafe extern "C" fn position(_:usize,id:u32,out:*mut f32)->u32{
            for (i,v) in (if id==0{[0.;4]}else{[-3.5,4.25,27.,0.]}).iter().enumerate(){unsafe{*out.add(i)=*v;}}0
        }
        unsafe extern "C" fn teleport(c:usize,a:u32,b:u32,r:u32,o:u32)->u32{let s=unsafe{&mut*(c as *mut State)};s.calls.push((a,b,r,o));s.status}
        let vm=crate::definition_vm().unwrap();let registry=Registry::default();let bridge=install(&vm,registry.clone()).unwrap();
        registry.bind(&vm,118).unwrap();registry.bind(&vm,119).unwrap();
        vm.globals().set("a",registry.snapshot(118).unwrap()).unwrap();vm.globals().set("b",registry.snapshot(119).unwrap()).unwrap();
        let mut state=State::default();let callbacks=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:Some(position),teleport_entity:Some(teleport),set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        assert!(vm.load("GetPosition(a)").exec().is_err());
        let scope=bridge.enter(callbacks).unwrap();
        let v=vm.load("return GetPosition(a)").eval::<mlua::AnyUserData>().unwrap();assert_eq!(v.borrow::<NativeVector4>().unwrap().0,[-3.5,4.25,27.,0.]);
        let v=vm.load("return GetPosition(nil)").eval::<mlua::AnyUserData>().unwrap();assert_eq!(v.borrow::<NativeVector4>().unwrap().0,[0.;4]);
        vm.load("TeleportToEntity(a,b); TeleportToEntity(a,b,false); TeleportToEntity('ignored',a,b,'0','true'); TeleportToEntity(nil,b)").exec().unwrap();
        assert_eq!(state.calls,[(118,119,0,1),(118,119,0,0),(118,119,1,0),(0,119,0,1)]);
        registry.unbind(119);let before=state.calls.len();assert!(vm.load("TeleportToEntity(nil,b)").exec().is_err());assert_eq!(state.calls.len(),before);
        assert!(vm.load("GetPosition(a,a)").exec().is_err());assert!(vm.load("TeleportToEntity(a)").exec().is_err());
        drop(scope);assert!(vm.load("TeleportToEntity(a,nil)").exec().is_err());
    }
    #[test]fn original_parent_abi_preserves_detach_and_refuses_stale_parent_before_effects(){
        #[derive(Default)]struct State{calls:Vec<(u32,u32)>,status:u32}
        unsafe extern "C" fn rot(_:usize,_:u32,_:f32)->u32{0}
        unsafe extern "C" fn parent(c:usize,a:u32,b:u32)->u32{let s=unsafe{&mut*(c as *mut State)};s.calls.push((a,b));s.status}
        let vm=crate::definition_vm().unwrap();let registry=Registry::default();let bridge=install(&vm,registry.clone()).unwrap();
        registry.bind(&vm,118).unwrap();registry.bind(&vm,119).unwrap();vm.globals().set("a",registry.snapshot(118).unwrap()).unwrap();vm.globals().set("b",registry.snapshot(119).unwrap()).unwrap();
        let mut state=State::default();let callbacks=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:Some(parent),resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        let _scope=bridge.enter(callbacks).unwrap();vm.load("SetParentEntity(a,b); SetParentEntity(a,nil); SetParentEntity(nil,b)").exec().unwrap();assert_eq!(state.calls,[(118,119),(118,0),(0,119)]);
        registry.unbind(119);assert!(vm.load("SetParentEntity(nil,b)").exec().is_err());assert_eq!(state.calls.len(),3);
        assert!(vm.load("SetParentEntity(a)").exec().is_err());assert!(vm.load("SetParentEntity(a,nil,false)").exec().is_err());
        state.status=2;assert!(vm.load("SetParentEntity(a,nil)").exec().is_err());
    }
    #[test]fn relative_prefab_query_is_native_scoped_and_never_admits_foreign_returned_handles(){
        struct State{status:u32,result:u32,calls:Vec<(u32,String)>}
        unsafe extern "C" fn rot(_:usize,_:u32,_:f32)->u32{0}
        unsafe extern "C" fn relative(c:usize,owner:u32,name:*const u8,n:u32,out:*mut u32)->u32{
            let s=unsafe{&mut*(c as *mut State)};s.calls.push((owner,String::from_utf8_lossy(unsafe{std::slice::from_raw_parts(name,n as usize)}).into()));unsafe{*out=s.result;}s.status
        }
        let vm=crate::definition_vm().unwrap();let registry=Registry::default();let bridge=install(&vm,registry.clone()).unwrap();
        registry.bind(&vm,118).unwrap();registry.bind(&vm,119).unwrap();vm.globals().set("a",registry.snapshot(118).unwrap()).unwrap();
        let mut state=State{status:0,result:119,calls:vec![]};let callbacks=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:Some(relative),get_prefab_data:None,fade_screen:None,camera_control:None};
        let _scope=bridge.enter(callbacks).unwrap();let result=vm.load("return GetRelativePrefabEntity(a,'marker')").eval::<mlua::AnyUserData>().unwrap();assert_eq!(registry.resolve(&result).unwrap(),119);assert_eq!(state.calls,[(118,"marker".into())]);
        state.status=1;assert!(vm.load("return GetRelativePrefabEntity(nil,'')").eval::<mlua::Value>().unwrap().is_nil());
        state.status=0;state.result=999;assert!(vm.load("GetRelativePrefabEntity(a,'marker')").exec().is_err());
        registry.unbind(118);let before=state.calls.len();assert!(vm.load("GetRelativePrefabEntity(a,'marker')").exec().is_err());assert_eq!(state.calls.len(),before);
    }
    #[test]fn original_print_calls_actual_tostring_once_per_argument_including_boolean_and_emits_no_result(){
        let vm=crate::definition_vm().unwrap();install(&vm,Registry::default()).unwrap();
        vm.load("seen={}; tostring=function(value,...) if select('#',...)~=0 then error('extra conversion args') end; seen[#seen+1]=type(value); return 'converted' end; assert(select('#',Print('a',2,false))==0); assert(table.concat(seen,',')=='string,number,boolean'); tostring=function() return {} end").exec().unwrap();
        assert!(vm.load("Print(1)").exec().is_err());vm.load("tostring=nil; Print()").exec().unwrap();assert!(vm.load("Print(1)").exec().is_err());
    }
    #[test]fn original_prefab_tables_use_verified_owner_generation_and_preserve_real_lua_values(){
        struct State{packet:Vec<u8>,owner:u32,status:u32}
        unsafe extern "C" fn rot(_:usize,_:u32,_:f32)->u32{0}
        unsafe extern "C" fn data(c:usize,_:u32,owner:*mut u32,out:*mut *const u8,size:*mut usize)->u32{let s=unsafe{&mut*(c as *mut State)};for (i,v)in [77,0,0,0,s.owner].iter().enumerate(){unsafe{*owner.add(i)=*v;}}unsafe{*out=s.packet.as_ptr();*size=s.packet.len();}s.status}
        let mut packet=Vec::new();packet.extend(3u32.to_be_bytes());for row in [b"value=1".as_slice(),b"flag=false",b"Handler=SourceCallable"]{packet.extend((row.len()as u32).to_be_bytes());packet.extend(row);}packet.extend(0u32.to_be_bytes());
        let vm=crate::definition_vm().unwrap();let registry=Registry::default();let bridge=install(&vm,registry.clone()).unwrap();for id in [118,119,120]{registry.bind(&vm,id).unwrap();}vm.globals().set("member",registry.snapshot(118).unwrap()).unwrap();vm.load("function SourceCallable() return 41 end").exec().unwrap();
        let mut state=State{packet,owner:119,status:0};let callbacks=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:Some(data),fade_screen:None,camera_control:None};
        let _scope=bridge.enter(callbacks).unwrap();vm.load("saved=GetPrefabData(member); assert(saved.value==1 and saved.flag==false and saved.Handler()==41); saved.value=99; assert(GetPrefabData(member)==saved)").exec().unwrap();
        let old=registry.snapshot(119).unwrap();let generation=old.borrow::<crate::entities::Entity>().unwrap().generation;registry.unbind(119);drop(old);vm.gc_collect().unwrap();registry.bind(&vm,119).unwrap();assert!(registry.snapshot(119).unwrap().borrow::<crate::entities::Entity>().unwrap().generation>generation);
        vm.load("assert(GetPrefabData(member).value==1 and saved.value==99)").exec().unwrap();
        state.owner=120;vm.load("assert(GetPrefabData(member).value==1); GetPrefabData(member).value=52").exec().unwrap();state.owner=119;vm.load("assert(GetPrefabData(member).value==1)").exec().unwrap();
        state.owner=999;assert!(vm.load("GetPrefabData(member)").exec().is_err());state.status=1;assert!(vm.load("return GetPrefabData(nil)").eval::<mlua::Value>().unwrap().is_nil());
    }
    #[test]fn original_camera_calls_have_source_arity_coercion_scope_and_host_errors(){
        #[derive(Default)]struct State{calls:Vec<(u32,[f32;3])>,status:u32}
        unsafe extern "C" fn rot(_:usize,_:u32,_:f32)->u32{0}
        unsafe extern "C" fn camera(c:usize,op:u32,a:f32,b:f32,d:f32)->u32{let s=unsafe{&mut*(c as *mut State)};s.calls.push((op,[a,b,d]));s.status}
        let vm=crate::definition_vm().unwrap();let bridge=install(&vm,Registry::default()).unwrap();let mut state=State::default();
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:Some(camera)};
        assert!(vm.load("CameraReset()").exec().is_err());let scope=bridge.enter(cb).unwrap();
        vm.load("CameraReset(); SetDefaultCameraValues('12.5', false, nil)").exec().unwrap();
        assert_eq!(state.calls,vec![(0,[0.;3]),(1,[12.5,0.,0.])]);
        for code in ["CameraReset(nil)","SetDefaultCameraValues(1,2)","SetDefaultCameraValues(1,2,3,4)","SetDefaultCameraValues(1,0/0,3)"]{assert!(vm.load(code).exec().is_err());}
        assert_eq!(state.calls.len(),2);
        if let Some(root)=std::env::var_os("EM1_LUA_ORIGINAL_FIXTURES"){
            let bytes=std::fs::read(std::path::Path::new(&root).join("HUB_MeanStreet_Shared.lua")).unwrap();let chunk=crate::prepare_chunk(&bytes).unwrap();
            vm.load(chunk.bytes.as_slice()).set_mode(mlua::ChunkMode::Binary).exec().unwrap();
            vm.globals().get::<mlua::Function>("MS_SetDefaultCameraValues").unwrap().call::<()>(()).unwrap();
            assert_eq!(state.calls.len(),3);assert_eq!(state.calls[2].0,1);
        }
        state.status=1;vm.load("CameraReset()").exec().unwrap();state.status=2;assert!(vm.load("CameraReset()").exec().is_err());
        vm.load("saved=SetDefaultCameraValues; SetDefaultCameraValues=nil; function CameraAlias() saved(1,2,3) end").exec().unwrap();
        assert!(crate::callback_preflight::check(&vm,"CameraAlias",65536).is_err());
        crate::callback_preflight::check(&vm,"CameraAlias",131072).unwrap();drop(scope);assert!(vm.load("saved(1,2,3)").exec().is_err());
    }
    #[test]fn original_screen_fades_preserve_optional_default_last_numeric_conversion_and_native_failures(){
        #[derive(Default)]struct State{calls:Vec<(u32,f32)>,status:u32}
        unsafe extern "C" fn rot(_:usize,_:u32,_:f32)->u32{0}
        unsafe extern "C" fn fade(c:usize,op:u32,time:f32)->u32{let s=unsafe{&mut*(c as *mut State)};s.calls.push((op,time));s.status}
        let vm=crate::definition_vm().unwrap();let bridge=install(&vm,Registry::default()).unwrap();let mut state=State::default();
        let callbacks=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(rot),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:Some(fade),camera_control:None};
        assert!(vm.load("StartFadeOut(0)").exec().is_err());let scope=bridge.enter(callbacks).unwrap();
        vm.load("StartFadeOut(0); StartFadeIn(); StartFadeInNoHUD(); StartFadeIn('ignored','1.25'); StartFadeOut(nil); StartFadeIn(false); StartFadeOut(-.5)").exec().unwrap();
        assert_eq!(state.calls,[(0,0.),(1,-1.),(2,-1.),(1,1.25),(0,0.),(1,0.),(0,-0.5)]);
        let count=state.calls.len();for source in ["StartFadeOut()","StartFadeOut(0,1)","StartFadeOut(0/0)","StartFadeIn(1/0)","StartFadeOut(1e300)"]{assert!(vm.load(source).exec().is_err(),"{source}");}assert_eq!(state.calls.len(),count);
        state.status=2;assert!(vm.load("StartFadeIn()").exec().is_err());drop(scope);assert!(vm.load("StartFadeInNoHUD()").exec().is_err());
    }
    #[test]fn component_control_marshals_last_two_arguments_and_revalidates_native_owners(){
        #[derive(Default)]struct State{calls:Vec<(u32,String,u32)>,status:u32}
        unsafe extern "C" fn find(_:usize,name:*const u8,len:u32,out:*mut u32)->u32{
            match unsafe{std::slice::from_raw_parts(name,len as usize)}{b"owned"=>{unsafe{*out=71};0},b"foreign"=>{unsafe{*out=99};0},_=>1}
        }
        unsafe extern "C" fn control(context:usize,id:u32,name:*const u8,len:u32,disabled:u32)->u32{
            let state=unsafe{&mut*(context as *mut State)};
            state.calls.push((id,String::from_utf8(unsafe{std::slice::from_raw_parts(name,len as usize)}.to_vec()).unwrap(),disabled));state.status
        }
        let vm=mlua::Lua::new();let entities=Registry::default();entities.bind(&vm,71).unwrap();let actor=entities.snapshot(71).unwrap();vm.globals().set("actor",actor.clone()).unwrap();
        let bridge=install(&vm,entities.clone()).unwrap();let mut state=State::default();
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:None,resolve_group:None,
            resolve_entity_name:Some(find),destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:Some(control),get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        {
            let _scope=bridge.enter(cb).unwrap();vm.load("EnableComponent(actor,'PhysicsRigidBodyCharacterComponent'); DisableComponent('owned','InputOutputComponent'); EnableComponent('ignored',actor,'ExactCase'); EnableComponent(nil,'missing'); DisableComponent('missing','missing')").exec().unwrap();
            assert_eq!(state.calls,vec![(71,"PhysicsRigidBodyCharacterComponent".into(),0),(71,"InputOutputComponent".into(),1),(71,"ExactCase".into(),0)]);
            let count=state.calls.len();
            for code in ["EnableComponent(71,'bad')","EnableComponent(actor,'bad\\0name')","DisableComponent('foreign','missing')","EnableComponent(actor)"]{assert!(vm.load(code).exec().is_err());}
            assert_eq!(state.calls.len(),count);state.status=1;vm.load("EnableComponent(actor,'missing')").exec().unwrap();
            state.status=2;assert!(vm.load("EnableComponent(actor,'rejected')").exec().is_err());
            entities.unbind(71);entities.bind(&vm,71).unwrap();let count=state.calls.len();assert!(vm.load("EnableComponent(actor,'stale')").exec().is_err());assert_eq!(state.calls.len(),count);
        }
        assert!(vm.load("DisableComponent('owned','InputOutputComponent')").exec().is_err());
    }
    #[test]fn original_variable_ordinals_translate_to_names_before_native_integer_dispatch(){
        let Some(path)=std::env::var_os("EM1_ANIMATION_REL_FIXTURE")else{return;};
        #[derive(Default)]struct State{calls:Vec<(u32,String,i32)>,status:u32}
        unsafe extern "C" fn variable(context:usize,id:u32,name:*const u8,len:u32,value:i32)->u32{
            let state=unsafe{&mut*(context as *mut State)};
            state.calls.push((id,String::from_utf8(unsafe{std::slice::from_raw_parts(name,len as usize)}.to_vec()).unwrap(),value));state.status
        }
        let vm=mlua::Lua::new();crate::animation_events::install(&vm,&std::fs::read(path).unwrap()).unwrap();
        crate::animation_events::install_targets(&vm,[("VAR_In_Conversation".into(),99)].into()).unwrap();let entities=Registry::default();entities.bind(&vm,71).unwrap();
        let actor=entities.snapshot(71).unwrap();vm.globals().set("actor",actor).unwrap();let bridge=install(&vm,entities.clone()).unwrap();let mut state=State::default();
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,
            destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:Some(variable),control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        {
            let _scope=bridge.enter(cb).unwrap();vm.load("assert(VAR_In_Conversation==72); AnimVarInt(actor,VAR_In_Conversation,-2147483648)").exec().unwrap();
            assert_eq!(state.calls,vec![(71,"VAR_In_Conversation".into(),i32::MIN)]);
            vm.load("NamedInteger='7.9'; AnimVarInt(actor,72,true); AnimVarInt(actor,72,false); AnimVarInt(actor,72,nil); AnimVarInt(actor,72,'3.75'); AnimVarInt(actor,72,-3.75); AnimVarInt(actor,'VAR_In_Conversation','NamedInteger'); AnimVarInt(actor,72,{}); AnimVarInt(actor,72,2147483648)").exec().unwrap();
            assert_eq!(state.calls.iter().map(|c|c.2).collect::<Vec<_>>(),[i32::MIN,1,0,0,3,-3,7,0,i32::MAX]);
            let count=state.calls.len();for code in ["AnimVarInt(actor,73,1)","AnimVarInt(actor,72,1,true)","AnimVarInt(71,72,1)"]{assert!(vm.load(code).exec().is_err());}
            assert_eq!(state.calls.len(),count);state.status=1;vm.load("AnimVarInt(actor,72,1)").exec().unwrap();state.status=2;assert!(vm.load("AnimVarInt(actor,72,1)").exec().is_err());
            entities.unbind(71);entities.bind(&vm,71).unwrap();let count=state.calls.len();assert!(vm.load("AnimVarInt(actor,72,1)").exec().is_err());assert_eq!(state.calls.len(),count);
        }
        assert!(vm.load("AnimVarInt(actor,72,1)").exec().is_err());
    }
    #[test]fn entity_controls_preserve_scope_last_argument_names_and_stale_generations(){
        #[derive(Default)]struct State{calls:Vec<(u32,u32)>,reject:bool}
        unsafe extern "C" fn find(_:usize,name:*const u8,len:u32,out:*mut u32)->u32{
            match unsafe{std::slice::from_raw_parts(name,len as usize)}{b"owned"=>{unsafe{*out=71};0},b"foreign"=>{unsafe{*out=99};0},_=>1}
        }
        unsafe extern "C" fn control(context:usize,id:u32,operation:u32)->u32{
            let state=unsafe{&mut*(context as *mut State)};if state.reject{return 2;}state.calls.push((id,operation));0
        }
        let vm=mlua::Lua::new();let entities=Registry::default();entities.bind(&vm,71).unwrap();
        let actor=entities.snapshot(71).unwrap();vm.globals().set("actor",actor.clone()).unwrap();
        let bridge=install(&vm,entities.clone()).unwrap();let mut state=State::default();
        assert_eq!(vm.globals().get::<mlua::Function>("Unhide").unwrap().to_pointer(),vm.globals().get::<mlua::Function>("Show").unwrap().to_pointer());
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:None,
            resolve_group:None,resolve_entity_name:Some(find),destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:Some(control),set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        {
            let _scope=bridge.enter(cb).unwrap();
            vm.load("Enable(actor); Disable('owned'); Hide(actor); Show('owned'); Enable('ignored',actor); Hide(nil); Show(); Disable('missing'); SetAppCulled(actor); ClearAppCulled('owned'); SetAppCulled(nil); ClearAppCulled('missing')").exec().unwrap();
            assert_eq!(state.calls,vec![(71,0),(71,1),(71,2),(71,3),(71,0),(71,4),(71,5)]);
            let count=state.calls.len();
            for code in ["Enable(71)","Hide('foreign')","Show('bad\\0name')"]{assert!(vm.load(code).exec().is_err());}
            assert_eq!(state.calls.len(),count);state.reject=true;assert!(vm.load("Enable(actor)").exec().is_err());state.reject=false;
            assert_eq!(entities.resolve(&actor).unwrap(),71);
            entities.unbind(71);entities.bind(&vm,71).unwrap();
            for name in ["Enable","Disable","Hide","Show","SetAppCulled","ClearAppCulled"]{assert!(vm.load(format!("{name}(actor)")).exec().is_err());}
            assert_eq!(state.calls.len(),count);
        }
        assert!(vm.load("Enable('owned')").exec().is_err());
    }
    #[test]fn gamebryo_bindings_keep_names_exact_and_reject_missing_clips_or_stale_owners(){
        #[derive(Default)]struct State{calls:Vec<(u32,u32,String)>,status:u32}
        unsafe extern "C" fn find(_:usize,name:*const u8,len:u32,out:*mut u32)->u32{
            if unsafe{std::slice::from_raw_parts(name,len as usize)}!=b"Camera"{return 1;}
            unsafe{*out=71};0
        }
        unsafe extern "C" fn gamebryo(context:usize,id:u32,operation:u32,name:*const u8,len:u32)->u32{
            let state=unsafe{&mut*(context as *mut State)};
            let name=String::from_utf8(unsafe{std::slice::from_raw_parts(name,len as usize)}.to_vec()).unwrap();
            let missing=name=="Missing";state.calls.push((id,operation,name));if missing{2}else{state.status}
        }
        let vm=mlua::Lua::new();let entities=Registry::default();entities.bind(&vm,71).unwrap();
        let actor=entities.snapshot(71).unwrap();vm.globals().set("actor",actor.clone()).unwrap();
        let bridge=install(&vm,entities.clone()).unwrap();let mut state=State::default();
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:None,
            resolve_group:None,resolve_entity_name:Some(find),destroy_entity:None,animate_entity:None,gamebryo_animation:Some(gamebryo),control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        {
            let _scope=bridge.enter(cb).unwrap();vm.load("AnimGBReset(actor); AnimGBSequence('Camera','Intro')").exec().unwrap();
            assert_eq!(state.calls,vec![(71,0,String::new()),(71,1,"Intro".into())]);
            assert!(vm.load("AnimGBSequence(actor,'Missing')").exec().is_err());assert_eq!(state.calls.last().unwrap().2,"Missing");
            let count=state.calls.len();vm.load("AnimGBReset('absent')").exec().unwrap();
            for source in ["AnimGBSequence('absent','Intro')","AnimGBSequence(actor,'')","AnimGBSequence(actor,'bad\\0clip')","AnimGBSequence(actor,'Intro',true)","AnimGBReset(actor,true)","AnimGBReset(71)"]{
                assert!(vm.load(source).exec().is_err());
            }
            assert_eq!(state.calls.len(),count);
            state.status=1;vm.load("AnimGBReset(actor)").exec().unwrap();assert!(vm.load("AnimGBSequence(actor,'Intro')").exec().is_err());
            let count=state.calls.len();entities.unbind(71);entities.bind(&vm,71).unwrap();
            assert!(vm.load("AnimGBReset(actor)").exec().is_err());assert_eq!(state.calls.len(),count);
        }
        assert!(vm.load("AnimGBSequence('Camera','Intro')").exec().is_err());
    }
    #[test]fn original_animation_ordinals_dispatch_names_and_reject_stale_or_unknown_ids(){
        let Some(path)=std::env::var_os("EM1_ANIMATION_REL_FIXTURE")else{return;};
        #[derive(Default)]struct State{names:Vec<String>,handles:Vec<u32>,status:u32}
        unsafe extern "C" fn animate(context:usize,id:u32,name:*const u8,length:u32)->u32{
            let state=unsafe{&mut*(context as *mut State)};
            state.names.push(String::from_utf8(unsafe{std::slice::from_raw_parts(name,length as usize)}.to_vec()).unwrap());
            state.handles.push(id);state.status
        }
        let vm=mlua::Lua::new();crate::animation_events::install(&vm,&std::fs::read(path).unwrap()).unwrap();
        let entities=Registry::default();entities.bind(&vm,71).unwrap();let actor=entities.snapshot(71).unwrap();
        vm.globals().set("actor",actor.clone()).unwrap();let bridge=install(&vm,entities.clone()).unwrap();let mut state=State::default();
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:None,
            resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:Some(animate),gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        {
            let _scope=bridge.enter(cb).unwrap();
            vm.load("assert(EVENT_Force_Idle==194); AnimEvent(actor,EVENT_Force_Idle)").exec().unwrap();
            assert_eq!(state.names,vec!["EVENT_Force_Idle"]);assert_eq!(state.handles,vec![71]);
            for source in ["AnimEvent(actor,-1)","AnimEvent(actor,0.5)","AnimEvent(actor,543)","AnimEvent(71,194)"]{
                assert!(vm.load(source).exec().is_err());
            }
            assert_eq!(state.names.len(),1);
            state.status=1;vm.load("AnimEvent(actor,194)").exec().unwrap();assert_eq!(entities.resolve(&actor).unwrap(),71);
            state.status=2;assert!(vm.load("AnimEvent(actor,194)").exec().is_err());assert_eq!(entities.resolve(&actor).unwrap(),71);
            entities.unbind(71);entities.bind(&vm,71).unwrap();assert!(vm.load("AnimEvent(actor,194)").exec().is_err());assert_eq!(state.names.len(),3);
        }
        assert!(vm.load("AnimEvent(actor,194)").exec().is_err());
    }
    #[test]fn named_entities_and_native_destruction_preserve_ownership_and_scope(){
        #[derive(Default)]struct State{lookups:u32,destroyed:Vec<u32>}
        unsafe extern "C" fn find(context:usize,name:*const u8,length:u32,out:*mut u32)->u32{
            let state=unsafe{&mut*(context as *mut State)};state.lookups+=1;
            let name=unsafe{std::slice::from_raw_parts(name,length as usize)};
            let id=match name{b"local"=>71,b"player"=>72,b"foreign"=>99,_=>return 1};
            unsafe{*out=id};0
        }
        unsafe extern "C" fn destroy(context:usize,id:u32)->u32{
            if id==72{return 1;}if id==73{return 2;}
            unsafe{&mut*(context as *mut State)}.destroyed.push(id);0
        }
        assert_eq!(std::mem::size_of::<Callbacks>(),152);
        let vm=mlua::Lua::new();let entities=Registry::default();
        entities.bind(&vm,71).unwrap();entities.bind(&vm,72).unwrap();entities.bind(&vm,73).unwrap();
        let old=entities.snapshot(71).unwrap();vm.globals().set("actor",old.clone()).unwrap();
        let rejected=entities.snapshot(73).unwrap();vm.globals().set("rejected",rejected.clone()).unwrap();
        let bridge=install(&vm,entities.clone()).unwrap();let mut state=State::default();
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:None,
            resolve_group:None,resolve_entity_name:Some(find),destroy_entity:Some(destroy),animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        {
            let _scope=bridge.enter(cb).unwrap();
            assert!(vm.load("return GetEntity('local') == actor").eval::<bool>().unwrap());
            assert!(vm.load("return GetEntity('missing') == nil").eval::<bool>().unwrap());
            assert!(vm.load("GetEntity('foreign')").exec().is_err());
            let count=state.lookups;assert!(vm.load("GetEntity('bad\\0name')").exec().is_err());assert_eq!(state.lookups,count);
            vm.load("DestroyEntity(GetEntity('player'))").exec().unwrap();assert_eq!(entities.resolve(&entities.snapshot(72).unwrap()).unwrap(),72);
            assert!(vm.load("DestroyEntity(rejected)").exec().is_err());assert_eq!(entities.resolve(&rejected).unwrap(),73);
            vm.load("DestroyEntity('missing')").exec().unwrap();assert!(state.destroyed.is_empty());
            vm.load("DestroyEntity(actor)").exec().unwrap();assert_eq!(state.destroyed,vec![71]);assert!(entities.resolve(&old).is_err());
            assert!(vm.load("DestroyEntity(actor)").exec().is_err());assert_eq!(state.destroyed,vec![71]);
            assert!(vm.load("GetEntity('local')").exec().is_err());
            entities.bind(&vm,71).unwrap();assert!(!vm.load("return actor == GetEntity('local')").eval::<bool>().unwrap());
            vm.load("DestroyEntity('local')").exec().unwrap();assert_eq!(state.destroyed,vec![71,71]);
            for code in ["DestroyEntity(71)","DestroyEntity(nil)"]{assert!(vm.load(code).exec().is_err());}
        }
        assert!(vm.load("GetEntity('local')").exec().is_err());assert!(vm.load("DestroyEntity('missing')").exec().is_err());
    }
    #[derive(Default)]struct Calls{count:u32,id:u32,speed:f32}
    unsafe extern "C" fn set(context:usize,id:u32,speed:f32)->u32{
        let calls=unsafe{&mut *(context as *mut Calls)};
        calls.count+=1;calls.id=id;calls.speed=speed;if speed<0.0{2}else{0}
    }
    #[test]fn scoped_native_calls_reject_stale_entities_and_bad_numbers(){
        let vm=mlua::Lua::new();let entities=Registry::default();entities.bind(&vm,118).unwrap();
        let bridge=install(&vm,entities.clone()).unwrap();let function=vm.globals().get::<mlua::Function>("SetRotatorMaxSpeed").unwrap();
        let first=entities.snapshot(118).unwrap();let mut calls=Calls::default();
        let callbacks=Callbacks{context:(&mut calls as *mut Calls) as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        assert!(function.call::<()>((first.clone(),360.0)).is_err());
        {
            let _scope=bridge.enter(callbacks).unwrap();assert!(bridge.enter(callbacks).is_err());
            for bad in [f64::NAN,f64::INFINITY,f64::MAX]{assert!(function.call::<()>((first.clone(),bad)).is_err());}
            function.call::<()>((first.clone(),360.0)).unwrap();assert_eq!((calls.count,calls.id,calls.speed),(1,118,360.0));
            entities.unbind(118);entities.bind(&vm,118).unwrap();
            assert!(function.call::<()>((first.clone(),360.0)).is_err());assert_eq!(calls.count,1);
            let fresh=entities.snapshot(118).unwrap();assert!(function.call::<()>((fresh.clone(),-1.0)).is_err());
            function.call::<()>((fresh,0.0)).unwrap();assert_eq!(calls.count,3);
        }
        assert!(function.call::<()>((entities.snapshot(118).unwrap(),360.0)).is_err());assert_eq!(calls.count,3);
    }
    #[test]fn cutscene_calls_require_scopes_validate_arguments_and_survive_yields(){
        #[derive(Default)]struct State{calls:Vec<u32>,fail:bool}
        unsafe extern "C" fn input(context:usize,restricted:u32)->u32{
            let state=unsafe{&mut *(context as *mut State)};
            if state.fail{return 2;}state.calls.push(restricted);0
        }
        let vm=mlua::Lua::new();let bridge=install(&vm,Registry::default()).unwrap();
        let mut state=State::default();let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:Some(input),set_rotator_speed:Some(set),set_particle_emission:None,resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        assert!(vm.load("RestrictCutSceneInput()").exec().is_err());
        let thread=vm.create_thread(vm.load("return function() RestrictCutSceneInput(); coroutine.yield(); UnrestrictCutSceneInput() end").eval::<mlua::Function>().unwrap()).unwrap();
        {
            let _scope=bridge.enter(cb).unwrap();
            assert!(vm.load("RestrictCutSceneInput(123)").exec().is_err());
            thread.resume::<()>(()).unwrap();assert_eq!(state.calls,vec![1]);
        }
        assert!(vm.load("UnrestrictCutSceneInput()").exec().is_err());
        {
            let _scope=bridge.enter(cb).unwrap();thread.resume::<()>(()).unwrap();assert_eq!(state.calls,vec![1,0]);
            state.fail=true;assert!(vm.load("RestrictCutSceneInput()").exec().is_err());assert_eq!(state.calls.len(),2);
        }
        let _scope=bridge.enter(Callbacks{set_cutscene_input:None,..cb}).unwrap();
        assert!(vm.load("RestrictCutSceneInput()").exec().is_err());
    }

    #[test]fn emitter_calls_validate_identity_scope_failure_and_yields(){
        #[derive(Default)]struct State{calls:Vec<(u32,u32)>,fail:bool}
        unsafe extern "C" fn emit(context:usize,id:u32,enabled:u32)->u32{
            let state=unsafe{&mut *(context as *mut State)};
            if state.fail{return 2;}state.calls.push((id,enabled));0
        }
        let vm=mlua::Lua::new();let entities=Registry::default();entities.bind(&vm,118).unwrap();
        let bridge=install(&vm,entities.clone()).unwrap();let mut state=State::default();
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:Some(emit),resolve_group:None,resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        vm.globals().raw_set("actor",entities.snapshot(118).unwrap()).unwrap();
        assert!(vm.load("StartEmitters(actor)").exec().is_err());
        let thread=vm.create_thread(vm.load("return function() StartEmitters(actor); coroutine.yield(); StopEmitters(actor) end").eval::<mlua::Function>().unwrap()).unwrap();
        {
            let _scope=bridge.enter(cb).unwrap();
            for code in ["StartEmitters(118)","StartEmitters(actor, 1)"]{assert!(vm.load(code).exec().is_err());}
            vm.load("StartEmitters(); StopEmitters(nil)").exec().unwrap();assert!(state.calls.is_empty());
            let foreign=Registry::default();foreign.bind(&vm,118).unwrap();vm.globals().raw_set("foreign",foreign.snapshot(118).unwrap()).unwrap();
            assert!(vm.load("StartEmitters(foreign)").exec().is_err());
            thread.resume::<()>(()).unwrap();assert_eq!(state.calls,vec![(118,1)]);
        }
        assert!(vm.load("StopEmitters(actor)").exec().is_err());
        {
            let _scope=bridge.enter(cb).unwrap();thread.resume::<()>(()).unwrap();assert_eq!(state.calls,vec![(118,1),(118,0)]);
            state.fail=true;assert!(vm.load("StartEmitters(actor)").exec().is_err());state.fail=false;
            entities.unbind(118);entities.bind(&vm,118).unwrap();assert!(vm.load("StartEmitters(actor)").exec().is_err());
            vm.globals().raw_set("actor",entities.snapshot(118).unwrap()).unwrap();vm.load("StartEmitters(actor)").exec().unwrap();
        }
        let _scope=bridge.enter(Callbacks{set_particle_emission:None,..cb}).unwrap();
        assert!(vm.load("StopEmitters(actor)").exec().is_err());assert_eq!(state.calls.len(),3);
    }
    #[test]fn emitter_names_last_argument_and_original_noop_are_supported_without_foreign_access(){
        unsafe extern "C" fn find(_:usize,name:*const u8,len:u32,out:*mut u32)->u32{
            match unsafe{std::slice::from_raw_parts(name,len as usize)}{b"owned"=>{unsafe{*out=118};0},b"foreign"=>{unsafe{*out=119};0},_=>1}
        }
        unsafe extern "C" fn noop(context:usize,id:u32,enabled:u32)->u32{unsafe{&mut*(context as *mut Vec<(u32,u32)>)}.push((id,enabled));1}
        let vm=mlua::Lua::new();let entities=Registry::default();entities.bind(&vm,118).unwrap();vm.globals().set("actor",entities.snapshot(118).unwrap()).unwrap();
        let bridge=install(&vm,entities).unwrap();let mut calls:Vec<(u32,u32)>=Vec::new();
        let callbacks=Callbacks{context:&mut calls as *mut Vec<(u32,u32)> as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:Some(noop),
            resolve_group:None,resolve_entity_name:Some(find),destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        let _scope=bridge.enter(callbacks).unwrap();
        vm.load("StartEmitters('owned'); StopEmitters('ignored',actor); StartEmitters('missing'); StopEmitters(nil)").exec().unwrap();
        assert_eq!(calls,vec![(118,1),(118,0)]);assert!(vm.load("StartEmitters('foreign')").exec().is_err());assert_eq!(calls.len(),2);
    }

    #[test]fn group_iteration_snapshots_and_revalidates_across_yields(){
        #[derive(Default)]struct State{ids:Vec<u32>,fail:bool,overflow:bool}
        unsafe extern "C" fn resolve(ctx:usize,_name:*const u8,_len:u32,out:*mut u32,capacity:u32,count:*mut u32)->u32{
            let state=unsafe{&*(ctx as *const State)};
            if state.fail{return 2;}
            unsafe{*count=if state.overflow{capacity+1}else{state.ids.len() as u32};}
            if !state.overflow{for (i,id) in state.ids.iter().enumerate(){unsafe{*out.add(i)=*id;}}}0
        }
        let vm=mlua::Lua::new();let entities=Registry::default();
        for id in [118,119]{entities.bind(&vm,id).unwrap();}
        let bridge=install(&vm,entities.clone()).unwrap();let mut state=State{ids:vec![118,119],..Default::default()};
        let cb=Callbacks{context:&mut state as *mut State as usize,set_cutscene_input:None,set_rotator_speed:Some(set),set_particle_emission:None,resolve_group:Some(resolve),resolve_entity_name:None,destroy_entity:None,animate_entity:None,gamebryo_animation:None,control_entity:None,set_animation_variable:None,control_component:None,get_position:None,teleport_entity:None,set_parent:None,resolve_relative_entity:None,get_prefab_data:None,fade_screen:None,camera_control:None};
        assert!(vm.load("ForEachEntityInGroup(function() end, 'steam')").exec().is_err());
        let make=||vm.create_thread(vm.load("return function() seen=0; ForEachEntityInGroup(function(e,n,h,last) assert(n==17 and h==nil and last=='tail'); seen=seen+1; coroutine.yield() end, 'steam',17,nil,'tail') end").eval::<mlua::Function>().unwrap()).unwrap();
        let thread=make();
        {
            let _scope=bridge.enter(cb).unwrap();
            for code in ["ForEachEntityInGroup(1, 'steam')","ForEachEntityInGroup(function() end, 1)","ForEachEntityInGroup(function() end, '')"]{assert!(vm.load(code).exec().is_err());}
            thread.resume::<()>(()).unwrap();assert_eq!(vm.globals().get::<u32>("seen").unwrap(),1);
        }
        {
            let _scope=bridge.enter(cb).unwrap();thread.resume::<()>(()).unwrap();thread.resume::<()>(()).unwrap();assert_eq!(vm.globals().get::<u32>("seen").unwrap(),2);
            let stale=make();stale.resume::<()>(()).unwrap();entities.unbind(119);entities.bind(&vm,119).unwrap();
            assert!(stale.resume::<()>(()).is_err());assert_eq!(vm.globals().get::<u32>("seen").unwrap(),1);
            vm.globals().raw_set("attempts",0).unwrap();
            state.ids=vec![118,999];assert!(vm.load("ForEachEntityInGroup(function() attempts=attempts+1 end, 'steam')").exec().is_err());
            assert_eq!(vm.globals().get::<u32>("attempts").unwrap(),0);
            state.ids=vec![118,0,118,119];
            vm.globals().raw_set("first",entities.snapshot(118).unwrap()).unwrap();
            vm.globals().raw_set("last",entities.snapshot(119).unwrap()).unwrap();
            vm.load(r#"local visits={}; ForEachEntityInGroup(function(e,...)
                assert(select('#',...)==4); local n,h,flag,label=...
                assert(n==17 and h==nil and flag==false and label=='extra')
                visits[#visits+1]=e==first and 118 or e==last and 119 or 0
            end,'steam',17,nil,false,'extra'); assert(table.concat(visits,',')=='118,0,118,119')"#).exec().unwrap();
            state.ids.clear();vm.load("ForEachEntityInGroup(function() error('empty group') end, 'empty')").exec().unwrap();
            state.fail=true;assert!(vm.load("ForEachEntityInGroup(function() end, 'steam')").exec().is_err());state.fail=false;
            state.overflow=true;assert!(vm.load("ForEachEntityInGroup(function() end, 'steam')").exec().is_err());
        }
        let _scope=bridge.enter(Callbacks{resolve_group:None,..cb}).unwrap();assert!(vm.load("ForEachEntityInGroup(function() end, 'steam')").exec().is_err());
    }

}

