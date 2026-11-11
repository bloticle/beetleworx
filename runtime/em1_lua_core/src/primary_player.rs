//! The registered world's player is an already admitted entity, never a second
//! address-only userdata. Native role provenance is checked by the caller.
use crate::{entities::{Entity,Registry},PLAYER_REGISTRY};
use mlua::{Lua,Result,Value};

pub fn set(vm:&Lua,entities:&Registry,id:u32)->Result<()> {
    // Fail closed if a caller supplies an unbound/revoked role.
    vm.set_named_registry_value(PLAYER_REGISTRY,Value::Nil)?;
    if id!=0 {vm.set_named_registry_value(PLAYER_REGISTRY,entities.snapshot(id)?)?;}
    Ok(())
}
pub fn revoke(vm:&Lua,id:u32)->Result<()> {
    if let Value::UserData(value)=vm.named_registry_value::<Value>(PLAYER_REGISTRY)? {
        if value.is::<Entity>() && value.borrow::<Entity>()?.id==id {
            vm.set_named_registry_value(PLAYER_REGISTRY,Value::Nil)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]fn player_and_event_arguments_share_identity_and_consumers(){
        let vm=Lua::new();let entities=Registry::default();entities.bind(&vm,71).unwrap();
        set(&vm,&entities,71).unwrap();
        let player=vm.named_registry_value::<mlua::AnyUserData>(PLAYER_REGISTRY).unwrap();
        let event=entities.snapshot(71).unwrap();
        assert_eq!(player.to_pointer(),event.to_pointer());assert_eq!(entities.resolve(&player).unwrap(),71);
        let foreign=Registry::default();foreign.bind(&vm,71).unwrap();
        assert!(entities.resolve(&foreign.snapshot(71).unwrap()).is_err());
    }
    #[test]fn role_reuse_revokes_old_player_and_clears_current_lookup(){
        let vm=Lua::new();let entities=Registry::default();entities.bind(&vm,71).unwrap();set(&vm,&entities,71).unwrap();
        let old=entities.snapshot(71).unwrap();revoke(&vm,71).unwrap();entities.unbind(71);
        assert!(matches!(vm.named_registry_value::<Value>(PLAYER_REGISTRY).unwrap(),Value::Nil));
        entities.bind(&vm,71).unwrap();set(&vm,&entities,71).unwrap();
        assert!(entities.resolve(&old).is_err());
        assert_ne!(old.to_pointer(),vm.named_registry_value::<mlua::AnyUserData>(PLAYER_REGISTRY).unwrap().to_pointer());
        assert!(set(&vm,&entities,72).is_err());
        assert!(matches!(vm.named_registry_value::<Value>(PLAYER_REGISTRY).unwrap(),Value::Nil));
    }
}
