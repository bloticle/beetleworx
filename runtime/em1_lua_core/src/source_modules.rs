//! Source-owned cooked Lua modules. Openfile executes the original imported
//! chunk synchronously; it never reads the host filesystem or deduplicates an
//! explicit repeated Openfile request.
use mlua::{ChunkMode,Function,Lua,MultiValue,Result,Value};
use std::{collections::BTreeMap,sync::Arc};

fn error(message:&str)->mlua::Error{mlua::Error::RuntimeError(message.into())}
pub fn key(path:&str)->Result<String>{
    if path.is_empty() || path.len()>2048 || path.starts_with(['/', '\\']) || path.chars().any(|c|c<' ' || c=='\u{7f}' || c==':'){
        return Err(error("Invalid original Lua module path"));
    }
    let normalized=path.replace('\\',"/").to_ascii_lowercase();let mut parts=Vec::new();
    for part in normalized.split('/'){
        match part{
            ""=>return Err(error("Empty Lua module path component")),
            "."=>{},".."=>{if parts.pop().is_none(){return Err(error("Lua module path leaves its source root"));}},
            _=>parts.push(part),
        }
    }
    let path=parts.join("/");if !path.ends_with(".lua"){return Err(error("Original module must name a Lua resource"));}Ok(path)
}
pub fn install(vm:&Lua,programs:BTreeMap<String,Arc<[u8]>>)->Result<()> {
    let programs=Arc::new(programs);
    vm.globals().raw_set("Openfile",vm.create_function(move |vm,args:MultiValue|{
        if args.len()!=1{return Err(error("Openfile requires exactly one source module path"));}
        let Some(Value::String(path))=args.front() else{return Err(error("Openfile requires a source module path"));};
        let path=path.to_str()?;let name=key(&path)?;
        let bytes=programs.get(&name).ok_or_else(||error(&format!("Original Openfile module is not imported: {name}")))?;
        vm.load(bytes.as_ref()).set_mode(ChunkMode::Binary).set_name(&name).exec()
    })?)?;
    crate::callback_preflight::remember_service(vm,"Openfile",16)?;
    // Original retail REL26cd5c returns this literal. Strict.lua uses the
    // shipped build identity, not the host's Debug/Release build mode.
    vm.globals().raw_set("BuildConfig",vm.create_function(|_,args:MultiValue|{
        if !args.is_empty(){return Err(error("BuildConfig takes no arguments"));}Ok("FINAL")
    })?)?;
    // globalscripts.lua copies coroutine.yield to wait/Wait. Expose only the
    // validated wait wrapper, not create/resume paths that bypass task budgets.
    crate::scheduler::install_wait(vm)?;
    let coroutine=vm.create_table()?;coroutine.raw_set("yield",vm.globals().raw_get::<Function>("wait")?)?;
    vm.globals().raw_set("coroutine",coroutine)?;Ok(())
}

#[cfg(test)]
mod tests{
    use super::*;
    #[test]fn original_module_order_repeated_opens_and_root_restrictions(){
        let vm=crate::definition_vm().unwrap();
        let mut programs=BTreeMap::new();
        programs.insert("scripts/one.lua".into(),Arc::from(vm.load("trace=trace..'1'; Openfile('Scripts/Two.lua')").into_function().unwrap().dump(true)));
        programs.insert("scripts/two.lua".into(),Arc::from(vm.load("trace=trace..'2'; assert(BuildConfig()=='FINAL')").into_function().unwrap().dump(true)));
        install(&vm,programs).unwrap();
        vm.load("trace=''; Openfile('./SCRIPTS/one.lua'); Openfile('Scripts\\\\Two.lua'); Openfile('Scripts/Sub/../Two.lua')").exec().unwrap();
        assert_eq!(vm.globals().get::<String>("trace").unwrap(),"1222");
        for code in ["Openfile()","Openfile(1)","Openfile('scripts/two.lua',1)","Openfile('scripts/missing.lua')","Openfile('../scripts/one.lua')","Openfile('/scripts/one.lua')","Openfile('C:/scripts/one.lua')","Openfile('scripts//two.lua')"]{
            assert!(vm.load(code).exec().is_err(),"{code}");
        }
        for name in ["create","resume","wrap","running","status"]{assert!(matches!(vm.globals().get::<mlua::Table>("coroutine").unwrap().get::<Value>(name).unwrap(),Value::Nil));}
    }
}
