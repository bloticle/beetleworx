//! Original JPSPrefabComponent data is Lua, not strings or JSON.
//! REL34DE70..34E8FC applies defaults then overrides, retains first key order,
//! evaluates `return <expression>`, and keeps only its first returned value.
use mlua::{Lua,Result,Table,Value};
use std::collections::BTreeMap;
const TABLES:&str="em1.source.prefab.tables";
fn error(s:impl Into<String>)->mlua::Error{mlua::Error::RuntimeError(s.into())}
pub type Guid=[u32;4];
fn key(owner:Guid,identity:u64)->[u8;24]{let mut key=[0;24];for(i,value)in owner.iter().enumerate(){key[i*4..i*4+4].copy_from_slice(&value.to_be_bytes());}key[16..].copy_from_slice(&identity.to_be_bytes());key}
fn assignment(row:&[u8])->Option<(Vec<u8>,Vec<u8>)>{
 // Original fields are C strings. Ignore bytes after the first NUL; do not
 // trim expression trailing bytes or reinterpret quoted strings/booleans.
 let row=row.split(|&v|v==0).next().unwrap_or_default();
 let mut at=0;while row.get(at).is_some_and(|v|matches!(v,b' '|b'\t')){at+=1;}
 let start=at;while row.get(at).is_some_and(|v|v.is_ascii_alphanumeric()||*v==b'_'){at+=1;}
 if at==start{return None;}let name=row[start..at].to_vec();
 while row.get(at).is_some_and(|v|matches!(v,b' '|b'\t')){at+=1;}
 if row.get(at)!=Some(&b'='){return None;}at+=1;
 while row.get(at).is_some_and(|v|matches!(v,b' '|b'\t')){at+=1;}
 if at==row.len() || row[at]==b'=' || name.len()>62 || row.len()-at>248{return None;}
 Some((name,row[at..].to_vec()))
}
fn tables(vm:&Lua)->Result<Table>{match vm.named_registry_value::<Value>(TABLES)?{
 Value::Table(value)=>Ok(value),_=>{let value=vm.create_table()?;vm.set_named_registry_value(TABLES,value.clone())?;Ok(value)},
}}
pub fn clear(vm:&Lua)->Result<()>{vm.set_named_registry_value(TABLES,vm.create_table()?) }
// The host supplies an exact retained owner GUID and original property arrays.
// It must validate native provenance before exposing the resulting table to a
// live entity. No name-prefix/member heuristic authorizes a table lookup.
pub fn prepare(vm:&Lua,owner:Guid,identity:u64,origin:&str,defaults:&[Vec<u8>],overrides:&[Vec<u8>])->Result<()> {
 if owner==[0;4] || identity==0 || origin.is_empty() || origin.len()>4096 || defaults.len()+overrides.len()>16384{return Err(error("Invalid original prefab data owner/origin/budget"));}
 let registry=tables(vm)?;let encoded=vm.create_string(key(owner,identity))?;
 if !registry.raw_get::<Value>(encoded.clone())?.is_nil(){return Err(error("Original prefab data owner was already initialized"));}
 let mut order=Vec::new();let mut expressions=BTreeMap::new();let mut bytes=0usize;
 for row in defaults.iter().chain(overrides){bytes=bytes.checked_add(row.len()).ok_or_else(||error("Original prefab data byte overflow"))?;
  if bytes>4*1024*1024{return Err(error("Original prefab data byte budget"));}
  let Some((name,value))=assignment(row)else{continue;};if !expressions.contains_key(&name){order.push(name.clone());}expressions.insert(name,value);
 }
 registry.raw_set(encoded.clone(),false)?; // original initialized flag remains false during expression evaluation.
 let evaluated=(||->Result<Table>{let table=vm.create_table()?;
 for name in order{let mut chunk=b"return ".to_vec();chunk.extend(&expressions[&name]);
  let values=vm.load(&chunk).set_name(format!("@original-prefab {origin}.{}",String::from_utf8_lossy(&name))).eval::<mlua::MultiValue>()?;
  if let Some(value)=values.front(){table.raw_set(vm.create_string(&name)?,value.clone())?;}
 }
 // Publish only the complete table. Evaluation is ordinary original Lua and
 // can have authored effects; no rollback of global Lua effects is claimed.
 Ok(table)})();
 match evaluated{Ok(table)=>registry.raw_set(encoded,table),Err(error)=>{registry.raw_set(encoded,Value::Nil)?;Err(error)}}
}
pub fn get(vm:&Lua,owner:Guid,identity:u64)->Result<Option<Table>>{Ok(match tables(vm)?.raw_get::<Value>(vm.create_string(key(owner,identity))?)?{Value::Table(table)=>Some(table),Value::Nil|Value::Boolean(false)=>None,_=>return Err(error("Invalid original prefab cache entry"))})}
pub fn initializing(vm:&Lua,owner:Guid,identity:u64)->Result<bool>{Ok(matches!(tables(vm)?.raw_get::<Value>(vm.create_string(key(owner,identity))?)?,Value::Boolean(false)))}
pub fn forget_generation(vm:&Lua,generation:u64)->Result<()> {
 let table=tables(vm)?;let mut keys=Vec::new();for pair in table.clone().pairs::<mlua::String,Value>(){let (key,_)=pair?;let bytes=key.as_bytes();if bytes.len()==24 && bytes[16..]==generation.to_be_bytes(){keys.push(key);}}
 for key in keys{table.raw_set(key,Value::Nil)?;}Ok(())
}
#[cfg(test)]mod tests{
 use super::*;
 fn rows(values:&[&str])->Vec<Vec<u8>>{values.iter().map(|v|v.as_bytes().to_vec()).collect()}
 #[test]fn original_defaults_and_overrides_keep_types_order_first_return_and_persistent_writes(){
  let vm=Lua::new();vm.load("order=''; function record(k,v) order=order..k; return v end; function none() end; function SourceCallable() return 17 end").exec().unwrap();
  let owner=[11,22,2,0];prepare(&vm,owner,1,"source:scene/root",&rows(&["a=record('a',1)","b=record('b',true)","nested={enabled=true}","discard=4","multi=3,7","missing=none()","call=SourceCallable"]),&rows(&["a=record('A',2)","discard=nil"])).unwrap();
  let table=get(&vm,owner,1).unwrap().unwrap();assert_eq!(table.get::<u32>("a").unwrap(),2);assert!(table.get::<bool>("b").unwrap());assert!(table.get::<Table>("nested").unwrap().get::<bool>("enabled").unwrap());assert!(table.get::<Value>("discard").unwrap().is_nil());assert!(table.get::<Value>("missing").unwrap().is_nil());assert_eq!(table.get::<u32>("multi").unwrap(),3);assert_eq!(table.get::<mlua::Function>("call").unwrap().call::<u32>(()).unwrap(),17);assert_eq!(vm.globals().get::<String>("order").unwrap(),"Ab");
  table.set("a",42).unwrap();assert_eq!(get(&vm,owner,1).unwrap().unwrap().to_pointer(),table.to_pointer());assert_eq!(get(&vm,owner,1).unwrap().unwrap().get::<u32>("a").unwrap(),42);
  assert!(prepare(&vm,owner,1,"source:scene/root",&[],&[]).is_err());clear(&vm).unwrap();assert!(get(&vm,owner,1).unwrap().is_none());
 }
 #[test]fn scanner_preserves_original_cstring_whitespace_and_skips_native_malformed_fields(){
  assert_eq!(assignment(b" \t 123_key \t= \t 'ok'  \0ignored").unwrap(),(b"123_key".to_vec(),b"'ok'  ".to_vec()));
  for bad in [b"".as_slice(),b" = 1",b"bad-key=1",b"key",b"key=",b"key==2",b"key \n=1"]{assert!(assignment(bad).is_none(),"{bad:?}");}
 }
 #[test]fn source_scanner_buffer_limits_and_transport_bounds_match_original(){
  assert!(assignment(format!("{}=1","a".repeat(62)).as_bytes()).is_some());assert!(assignment(format!("{}=1","a".repeat(63)).as_bytes()).is_none());
  assert!(assignment(format!("key={}","1".repeat(248)).as_bytes()).is_some());assert!(assignment(format!("key={}","1".repeat(249)).as_bytes()).is_none());
  assert_eq!(decode(&[0;8]).unwrap(),(vec![],vec![]));for bad in [&[][..],&[0;7],&[0;9],&[0,0,0,1,0,0,0,4,1,2][..]]{assert!(decode(bad).is_err());}
 }
 #[test]fn malformed_rows_follow_original_skip_policy_and_failed_expressions_do_not_publish_partial_tables(){
  let vm=Lua::new();let owner=[1,2,3,0];prepare(&vm,owner,1,"origin",&rows(&["first=1","malformed","bad==2"]),&[]).unwrap();assert_eq!(get(&vm,owner,1).unwrap().unwrap().get::<u32>("first").unwrap(),1);clear(&vm).unwrap();
  assert!(prepare(&vm,owner,1,"origin",&rows(&["first=1","bad=MissingActualEngineFunction()"]),&[]).is_err());assert!(get(&vm,owner,1).unwrap().is_none());
  prepare(&vm,owner,1,"origin",&rows(&["first=2"]),&[]).unwrap();assert_eq!(get(&vm,owner,1).unwrap().unwrap().get::<u32>("first").unwrap(),2);
 }
}

// Owned transport from the scoped native property reader. Copy before evaluating
// Lua: an authored expression may itself perform another source property query.
pub fn decode(packet:&[u8])->Result<(Vec<Vec<u8>>,Vec<Vec<u8>>)> {
 if packet.len()>4*1024*1024+16384*8+8{return Err(error("Original prefab transport byte budget"));}
 let mut at=0usize;
 let mut word=||->Result<usize>{let bytes=packet.get(at..at+4).ok_or_else(||error("Truncated original prefab row transport"))?;at+=4;Ok(u32::from_be_bytes(bytes.try_into().unwrap())as usize)};
 // Separate local reader to avoid retaining a mutable cursor borrow over rows.
 let first=word()?;drop(word);let mut arrays=Vec::new();let mut count=first;
 for index in 0..2{if count>16384{return Err(error("Original prefab row count budget"));}let mut rows=Vec::with_capacity(count);
  for _ in 0..count{let bytes=packet.get(at..at+4).ok_or_else(||error("Truncated original prefab row size"))?;at+=4;let n=u32::from_be_bytes(bytes.try_into().unwrap())as usize;
   let end=at.checked_add(n).ok_or_else(||error("Original prefab row size overflow"))?;rows.push(packet.get(at..end).ok_or_else(||error("Truncated original prefab row bytes"))?.to_vec());at=end;
  }arrays.push(rows);if index==0{let bytes=packet.get(at..at+4).ok_or_else(||error("Truncated original prefab override count"))?;at+=4;count=u32::from_be_bytes(bytes.try_into().unwrap())as usize;}
 }
 if at!=packet.len(){return Err(error("Trailing original prefab transport bytes"));}Ok((arrays.remove(0),arrays.remove(0)))
}
