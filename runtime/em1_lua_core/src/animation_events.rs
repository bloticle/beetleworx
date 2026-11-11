//! Original animation IDs are recovered from the mounted, verified EM1 REL.
//! Dispatch sends their semantic name to the actual Xbox registry, never the Wii ordinal.
//! Audited USA REL SHA-256 ac25d520f329a3d0507d656f0ab1cf1c34025bb36ff866187ad082ae3e0a4ebf;
//! registrations2ec098..2ee278 branch to2eb91c, string base4c6bf4.
use mlua::{Lua,Result,UserData,Value};
const REGISTRY:&str="em1.native.original_animation_events";
const VARIABLES:&str="em1.native.original_animation_variables";
const TARGETS:&str="em1.native.target_animation_variables";
const EXPECTED:[u8;32]=[0xac,0x25,0xd5,0x20,0xf3,0x29,0xa3,0xd0,0x50,0x7d,0x65,0x6f,0x0a,0xb1,0xcf,0x1c,0x34,0x02,0x5b,0xb3,0x6f,0xf8,0x66,0x18,0x7a,0xd0,0x82,0xae,0x3e,0x0a,0x4e,0xbf];
struct Catalog(Vec<String>);
impl UserData for Catalog {}
struct Targets(std::collections::BTreeMap<String,u32>);
impl UserData for Targets {}
fn error(s:impl Into<String>)->mlua::Error{mlua::Error::RuntimeError(s.into())}
fn decode_range(bytes:&[u8],first:usize,last:usize,prefix:&str)->Result<Vec<String>>{
    let word=|at:usize|->Result<u32>{Ok(u32::from_be_bytes(bytes.get(at..at+4).ok_or_else(||error("Truncated original event catalog"))?.try_into().unwrap()))};
    let mut names=Vec::new();let mut unique=std::collections::BTreeSet::new();
    for call in (first..=last).step_by(16){
        let (a,b)=(word(call-8)?,word(call-4)?);
        let (symbol,ordinal)=if a&0xffff0000==0x389f0000{(a,b)}else{(b,a)};
        if symbol&0xffff0000!=0x389f0000 || ordinal&0xffff0000!=0x38a00000 || (ordinal&65535)as usize!=names.len(){return Err(error("Original animation registration shape/ordinal differs"));}
        let branch=word(call)?;let displacement=((branch&0x03fffffc)as i32)<<6>>6;
        if branch&0xfc000003!=0x48000001 || call as i32+displacement!=0x2eb91c{return Err(error("Original animation registration target differs"));}
        let at=(0x4c6bf4i64+i64::from(symbol as i16)) as usize;
        let tail=bytes.get(at..).ok_or_else(||error("Original animation symbol pointer exceeds module"))?;
        let end=tail.iter().take(128).position(|b|*b==0).ok_or_else(||error("Invalid original animation symbol length"))?;
        let name=std::str::from_utf8(&tail[..end]).map_err(|_|error("Invalid original animation symbol encoding"))?;
        if !name.starts_with(prefix) || !name.bytes().all(|b|b.is_ascii_alphanumeric() || b==b'_') || !unique.insert(name.to_owned()) {return Err(error("Invalid/duplicate original animation symbol"));}
        names.push(name.to_owned());
    }
    Ok(names)
}
pub fn install(vm:&Lua,bytes:&[u8])->Result<usize>{
    if bytes.len()>32*1024*1024 || crate::sha256(bytes).map_err(|e|error(e.1))?!=EXPECTED{return Err(error("Unsupported original animation executable identity"));}
    let names=decode_range(bytes,0x2ec098,0x2ee278,"EVENT_")?;
    let variables=decode_range(bytes,0x2ebba8,0x2ec028,"VAR_")?;
    if !matches!(vm.named_registry_value::<Value>(REGISTRY)?,Value::Nil) || !matches!(vm.named_registry_value::<Value>(VARIABLES)?,Value::Nil){return Err(error("Original animation catalog already installed"));}
    for (id,name) in names.iter().chain(variables.iter()).enumerate(){
        if !matches!(vm.globals().raw_get::<Value>(name.as_str())?,Value::Nil){return Err(error("Original animation constant conflicts with authored globals"));}
        // Validate conversions before publishing any values.
        let _=u32::try_from(id).map_err(|_|error("Original animation ordinal overflow"))?;
    }
    let count=names.len();
    for (id,name) in names.iter().enumerate(){vm.globals().raw_set(name.as_str(),id as u32)?;}
    for (id,name) in variables.iter().enumerate(){vm.globals().raw_set(name.as_str(),id as u32)?;}
    vm.set_named_registry_value(REGISTRY,vm.create_userdata(Catalog(names))?)?;
    vm.set_named_registry_value(VARIABLES,vm.create_userdata(Catalog(variables))?)?;
    Ok(count)
}
pub fn name(vm:&Lua,id:f64)->Result<String>{
    catalog_name(vm,id,REGISTRY)
}
pub fn variable_name(vm:&Lua,id:f64)->Result<String>{
    let name=catalog_name(vm,id,VARIABLES)?;
    let targets=vm.named_registry_value::<mlua::AnyUserData>(TARGETS).map_err(|_|error("Checked native animation variable catalog unavailable"))?;
    if !targets.borrow::<Targets>()?.0.contains_key(&name){return Err(error(format!("Original animation variable has no native semantic equivalent: {name}")));}
    Ok(name)
}
pub fn install_targets(vm:&Lua,targets:std::collections::BTreeMap<String,u32>)->Result<()>{
    if !matches!(vm.named_registry_value::<Value>(VARIABLES)?,Value::UserData(_)){return Err(error("Original variable catalog must be installed first"));}
    if targets.is_empty() || targets.len()>4096{return Err(error("Invalid native animation variable catalog count"));}
    let mut ids=std::collections::BTreeSet::new();
    for (name,&id) in &targets{
        if !name.starts_with("VAR_") || name.len()>127 || !name.bytes().all(|c|c.is_ascii_alphanumeric() || c==b'_') || id>i32::MAX as u32 || !ids.insert(id){return Err(error("Invalid/duplicate native animation variable name or ID"));}
    }
    match vm.named_registry_value::<Value>(TARGETS)?{
        Value::Nil=>vm.set_named_registry_value(TARGETS,vm.create_userdata(Targets(targets))?),
        Value::UserData(existing) if existing.borrow::<Targets>()?.0==targets=>Ok(()),
        _=>Err(error("Cannot replace a retained native animation variable catalog")),
    }
}
fn catalog_name(vm:&Lua,id:f64,registry:&str)->Result<String>{
    if !id.is_finite() || id<0.0 || id.fract()!=0.0 || id>u32::MAX as f64{return Err(error("AnimEvent requires an original integer event ID"));}
    let catalog=vm.named_registry_value::<mlua::AnyUserData>(registry).map_err(|_|error("Original animation catalog unavailable"))?;
    let catalog=catalog.borrow::<Catalog>()?;
    catalog.0.get(id as usize).cloned().ok_or_else(||error("Unknown original animation event ID"))
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn rejects_unverified_module_without_publishing_constants(){
        let vm=Lua::new();assert!(install(&vm,b"bad executable").is_err());assert!(vm.globals().get::<Value>("EVENT_Force_Idle").unwrap().is_nil());
        assert!(name(&vm,194.0).is_err());
    }
    #[test]fn original_verified_rel_recovers_ordinals_and_rejects_tampering(){
        let Some(path)=std::env::var_os("EM1_ANIMATION_REL_FIXTURE")else{return;};
        let bytes=std::fs::read(path).unwrap();let vm=Lua::new();assert_eq!(install(&vm,&bytes).unwrap(),543);
        assert_eq!(vm.globals().get::<u32>("EVENT_Force_Idle").unwrap(),194);assert_eq!(name(&vm,194.0).unwrap(),"EVENT_Force_Idle");
        assert_eq!(vm.globals().get::<u32>("VAR_In_Conversation").unwrap(),72);
        assert!(variable_name(&vm,72.).is_err());install_targets(&vm,[("VAR_In_Conversation".into(),99)].into()).unwrap();assert_eq!(variable_name(&vm,72.0).unwrap(),"VAR_In_Conversation");
        assert!(variable_name(&vm,13.).unwrap_err().to_string().contains("VAR_SuperSaturated_Blend"));
        assert!(variable_name(&vm,73.).is_err());
        for id in [-1.0,0.5,f64::NAN,543.0]{assert!(name(&vm,id).is_err());}
        let mut altered=bytes.clone();altered[0x2ec098]^=1;let fresh=Lua::new();assert!(install(&fresh,&altered).is_err());
        assert!(fresh.globals().get::<Value>("EVENT_Force_Idle").unwrap().is_nil());assert!(install(&vm,&bytes).is_err());
    }
}
