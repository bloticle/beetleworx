//! Stable Lua functions with engine access limited to one synchronous tick.
use std::sync::{Arc,Mutex};
#[repr(C)]
#[derive(Clone,Copy)]
pub struct Callbacks {
    pub context:usize,
    pub get:Option<unsafe extern "C" fn(usize,*const u8,usize,*mut i32)->u32>,
    pub set:Option<unsafe extern "C" fn(usize,*const u8,usize,i32,u32)->u32>,
}
#[derive(Clone,Default)]
pub struct Bridge(Arc<Mutex<Option<Callbacks>>>);
pub struct Scope(Bridge);
impl Drop for Scope {fn drop(&mut self){*self.0.0.lock().unwrap_or_else(|e|e.into_inner())=None;}}
fn error(message:&str)->mlua::Error {mlua::Error::RuntimeError(message.into())}
impl Bridge {
    pub fn enter(&self,callbacks:Callbacks)->mlua::Result<Scope>{
        if callbacks.get.is_none() || callbacks.set.is_none(){return Err(error("Incomplete native global callbacks"));}
        let mut active=self.0.lock().unwrap_or_else(|e|e.into_inner());
        if active.is_some(){return Err(error("Nested native global dispatch"));}
        *active=Some(callbacks);Ok(Scope(self.clone()))
    }
    fn active(&self)->mlua::Result<Callbacks>{self.0.lock().unwrap_or_else(|e|e.into_inner()).as_ref().copied().ok_or_else(||error("Native global state unavailable outside engine dispatch"))}
}
fn name(value:&mlua::String)->mlua::Result<mlua::BorrowedBytes<'_>>{
    let bytes=value.as_bytes();
    if bytes.is_empty() || bytes.len()>=1000 || bytes.contains(&0){return Err(error("Invalid native global name"));}Ok(bytes)
}
pub fn install(vm:&mlua::Lua)->mlua::Result<Bridge>{
    let bridge=Bridge::default();let read=bridge.clone();let write=bridge.clone();
    vm.globals().raw_set("GetGlobal",vm.create_function(move |_,key:mlua::String|{
        let key=name(&key)?;let callbacks=read.active()?;let mut value=0;
        match unsafe{callbacks.get.unwrap()(callbacks.context,key.as_ptr(),key.len(),&mut value)} {
            0=>Ok(Some(value)),1=>Ok(None),_=>Err(error("Native GetGlobal failed")),
        }
    })?)?;
    vm.globals().raw_set("SetGlobal",vm.create_function(move |_,(key,value,no_counter):(mlua::String,mlua::Integer,Option<bool>)|{
        let key=name(&key)?;let value=i32::try_from(value).map_err(|_|error("Global value exceeds native integer range"))?;let callbacks=write.active()?;
        match unsafe{callbacks.set.unwrap()(callbacks.context,key.as_ptr(),key.len(),value,no_counter.unwrap_or(false) as u32)} {
            0=>Ok(Some(value)),1=>Ok(None),_=>Err(error("Native SetGlobal failed")),
        }
    })?)?;Ok(bridge)
}
#[cfg(test)]mod tests {
    use super::*;
    struct State{value:i32,calls:u32,no_counter:u32,fail:std::cell::Cell<bool>}
    unsafe extern "C" fn get(c:usize,n:*const u8,l:usize,out:*mut i32)->u32{let s=unsafe{&mut *(c as *mut State)};s.calls+=1;if s.fail.get(){return 2;}if unsafe{std::slice::from_raw_parts(n,l)}!=b"known"{return 1;}unsafe{*out=s.value;}0}
    unsafe extern "C" fn set(c:usize,n:*const u8,l:usize,v:i32,f:u32)->u32{let s=unsafe{&mut *(c as *mut State)};s.calls+=1;if s.fail.get(){return 2;}if unsafe{std::slice::from_raw_parts(n,l)}!=b"known"{return 1;}s.value=v as i16 as i32;s.no_counter=f;0}
    #[test]fn scoped_globals_and_cached_functions(){
        let vm=mlua::Lua::new();let bridge=install(&vm).unwrap();vm.load("cached=GetGlobal").exec().unwrap();
        assert!(vm.load("return cached('known')").eval::<i32>().is_err());
        let mut state=State{value:-32768,calls:0,no_counter:0,fail:false.into()};let cb=Callbacks{context:&mut state as *mut _ as usize,get:Some(get),set:Some(set)};
        {
            let _scope=bridge.enter(cb).unwrap();assert!(bridge.enter(cb).is_err());
            vm.load("assert(cached('known') == -32768); assert(GetGlobal('missing') == nil); assert(SetGlobal('missing',4) == nil); assert(SetGlobal('known',70000,true) == 70000); assert(cached('known') == 4464)").exec().unwrap();
            assert_eq!(state.no_counter,1);let calls=state.calls;
            assert!(vm.load("GetGlobal('bad\\0name')").exec().is_err());assert!(vm.load("SetGlobal('known',2147483648)").exec().is_err());assert_eq!(state.calls,calls);
            state.fail.set(true);assert!(vm.load("GetGlobal('known')").exec().is_err());assert!(vm.load("SetGlobal('known',2)").exec().is_err());state.fail.set(false);
        }
        assert!(vm.load("GetGlobal('known')").exec().is_err());
        {let _scope=bridge.enter(cb).unwrap();assert_eq!(vm.load("return cached('known')").eval::<i32>().unwrap(),4464);}
        let _=std::panic::catch_unwind(std::panic::AssertUnwindSafe(||{let _scope=bridge.enter(cb).unwrap();panic!("scope test");}));
        assert!(vm.load("cached('known')").exec().is_err());
    }
    #[test]fn ffi_scope_across_wait_pause_and_script_error(){unsafe{
        use crate::*;
        let chunk=prepare_chunk(&compile_only("local get=GetGlobal; function event() SetGlobal('known',-2); wait(0); SetGlobal('known',get('known')+5) end; function bad() SetGlobal('known',9); error('failure') end").unwrap()).unwrap();
        let input=Em1LuaWorldInput{bytecode:chunk.bytes.as_ptr(),bytecode_size:chunk.bytes.len(),executable_sha256:chunk.executable};
        let world=em1_lua_world_create(&input,1);assert_eq!((*world).status,0);
        let mut state=State{value:0,calls:0,no_counter:0,fail:false.into()};let cb=Callbacks{context:&mut state as *mut _ as usize,get:Some(get),set:Some(set)};
        assert_eq!(em1_lua_world_start(world,b"event".as_ptr(),5),0);
        assert_eq!(em1_lua_world_tick_native(world,0.0,&cb),0);assert_eq!(state.calls,0);
        assert_eq!(em1_lua_world_tick_native(world,0.1,&cb),0);assert_eq!(state.value,-2);
        assert!((*world)._vm.as_ref().unwrap().load("GetGlobal('known')").exec().is_err());
        assert_eq!(em1_lua_world_tick_native(world,0.1,&cb),0);assert_eq!(state.value,3);
        assert_eq!(em1_lua_world_start(world,b"bad".as_ptr(),3),0);
        assert_ne!(em1_lua_world_tick_native(world,0.1,&cb),0);assert_eq!(state.value,9);
        assert!((*world)._vm.as_ref().unwrap().load("GetGlobal('known')").exec().is_err());
        assert_ne!(em1_lua_world_tick_native(world,0.1,std::ptr::null()),0);
        em1_lua_world_release(world);
    }}
}
