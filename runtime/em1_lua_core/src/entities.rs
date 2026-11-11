//! Engine-owned identities for non-player event arguments. Cached references
//! survive Lua waits but are invalid after native ownership is revoked.
use mlua::{AnyUserData,Lua,Result,UserData};
use std::{collections::BTreeMap,sync::{Arc,Mutex}};

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct Entity {pub id:u32,pub generation:u64}
impl UserData for Entity {}
#[derive(Default)]
struct State {live:BTreeMap<u32,AnyUserData>,generation:u64}
#[derive(Clone,Default)]
pub struct Registry(Arc<Mutex<State>>);
fn error(message:&str)->mlua::Error {mlua::Error::RuntimeError(message.into())}
impl Registry {
    pub fn bind(&self,vm:&Lua,id:u32)->Result<()> {
        if id==0{return Err(error("Zero native entity identity"));}
        let mut state=self.0.lock().unwrap_or_else(|e|e.into_inner());
        if state.live.contains_key(&id){return Ok(());}
        if state.live.len()>=4096{return Err(error("Native entity binding limit"));}
        let generation=state.generation.checked_add(1).ok_or_else(||error("Entity generation exhausted"))?;
        let value=vm.create_userdata(Entity{id,generation})?;
        state.live.insert(id,value);state.generation=generation;Ok(())
    }
    pub fn unbind(&self,id:u32){self.0.lock().unwrap_or_else(|e|e.into_inner()).live.remove(&id);}
    pub fn snapshot(&self,id:u32)->Result<AnyUserData> {
        self.0.lock().unwrap_or_else(|e|e.into_inner()).live.get(&id).cloned().ok_or_else(||error("Unbound native entity identity"))
    }
    // Every future entity-consuming engine binding must call this at dispatch,
    // including after a coroutine resumes; checking only on enqueue is unsafe.
    pub fn resolve(&self,value:&AnyUserData)->Result<u32> {
        let token=*value.borrow::<Entity>()?;
        let current=self.snapshot(token.id)?;
        if current.to_pointer()!=value.to_pointer() || *current.borrow::<Entity>()?!=token{return Err(error("Stale or foreign native entity identity"));}
        Ok(token.id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]fn revoke_and_reuse_do_not_resurrect_cached_reference(){
        let vm=Lua::new();let registry=Registry::default();
        assert!(registry.bind(&vm,0).is_err());assert!(registry.snapshot(17).is_err());
        registry.bind(&vm,17).unwrap();let first=registry.snapshot(17).unwrap();
        let foreign=Registry::default();foreign.bind(&vm,17).unwrap();
        assert!(registry.resolve(&foreign.snapshot(17).unwrap()).is_err());
        registry.bind(&vm,17).unwrap();assert_eq!(registry.resolve(&first).unwrap(),17);
        registry.unbind(17);assert!(registry.resolve(&first).is_err());
        registry.bind(&vm,17).unwrap();assert!(registry.resolve(&first).is_err());
        assert_eq!(registry.resolve(&registry.snapshot(17).unwrap()).unwrap(),17);
        vm.globals().set("entity",first).unwrap();
        assert!(vm.load("return entity.id").eval::<u32>().is_err());
    }
}
