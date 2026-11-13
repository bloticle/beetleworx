//! Read-only multi-format Wii disc bridge. Codecs are linked into the game.
use std::{ffi::c_void,io::{Read,Seek,SeekFrom},panic::{catch_unwind,AssertUnwindSafe},ptr,slice};
struct File {path:String,node:nod::Node}
struct Mount {_disc:nod::Disc,part:Box<dyn nod::PartitionBase>,files:Vec<File>,id:[u8;6]}
unsafe fn message(out:*mut u8,capacity:usize,text:&str){if !out.is_null() && capacity>0{let n=text.len().min(capacity-1);unsafe{ptr::copy_nonoverlapping(text.as_ptr(),out,n);*out.add(n)=0;}}}
fn open(path:&str)->Result<Mount,String>{
 let disc=nod::Disc::new(path).map_err(|e|e.to_string())?;
 let id=disc.header().game_id;
 if !disc.header().is_wii() || &id[..3]!=b"SEM" {return Err("Select an Epic Mickey Wii disc image.".into());}
 let mut part=disc.open_partition_kind(nod::PartitionKind::Data).map_err(|e|e.to_string())?;
 let meta=part.meta().map_err(|e|e.to_string())?;let fst=meta.fst().map_err(str::to_owned)?;
 if fst.nodes.len()>100000{return Err("Wii disc has too many filesystem entries".into());}
 let mut stack=vec![(fst.nodes.len(),String::new())];let mut files=Vec::new();let mut names=std::collections::HashSet::new();
 for (i,node,name) in fst.iter(){
  while stack.last().is_some_and(|s|i>=s.0){stack.pop();}
  let parent=stack.last().ok_or("Invalid Wii FST directory range")?;
  let name=name.map_err(|e|e.to_string())?;
  if name.is_empty() || name=="." || name==".." || name.contains(['/', '\\', ':','\0']) {return Err("Invalid Wii disc filename".into());}
  let path=format!("{}{}",parent.1,name).to_lowercase();
  if path.len()>2048 || !names.insert(path.clone()){return Err("Ambiguous or oversized Wii disc filename".into());}
  if node.is_dir(){let end=node.length() as usize;if end<=i || end>parent.0 || stack.len()>=256{return Err("Invalid Wii FST directory extent".into());}stack.push((end,path+"/"));}
  else if node.is_file(){files.push(File{path,node});}
  else{return Err("Invalid Wii FST node type".into());}
 }
 if !files.iter().any(|f|f.path=="packfiles/levels.pak"){return Err("The Wii image does not contain Epic Mickey's game files".into());}
 Ok(Mount{_disc:disc,part,files,id})
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_disc_open(path:*const u8,length:usize,error:*mut u8,capacity:usize)->*mut c_void{
 let result=catch_unwind(AssertUnwindSafe(||{if path.is_null() || length==0 || length>32768{return Err("Invalid disc path".to_string());}let text=std::str::from_utf8(unsafe{slice::from_raw_parts(path,length)}).map_err(|e|e.to_string())?;open(text)}));
 match result{Ok(Ok(m))=>Box::into_raw(Box::new(m)).cast(),Ok(Err(e))=>{unsafe{message(error,capacity,&e)};ptr::null_mut()},Err(_)=>{unsafe{message(error,capacity,"Disc decoder rejected malformed image")};ptr::null_mut()}}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_disc_close(handle:*mut c_void){if !handle.is_null(){drop(unsafe{Box::from_raw(handle.cast::<Mount>())});}}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_disc_count(handle:*mut c_void,id:*mut u8)->usize{if handle.is_null() || id.is_null(){return 0;}let m=unsafe{&*handle.cast::<Mount>()};unsafe{ptr::copy_nonoverlapping(m.id.as_ptr(),id,6)};m.files.len()}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_disc_info(handle:*mut c_void,index:usize,path:*mut u8,capacity:usize,size:*mut u64)->i32{
 if handle.is_null() || path.is_null() || size.is_null(){return -1;}let m=unsafe{&*handle.cast::<Mount>()};let Some(f)=m.files.get(index)else{return -1;};if f.path.len()+1>capacity{return -1;}unsafe{message(path,capacity,&f.path);*size=f.node.length();}0
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn em1_disc_read(handle:*mut c_void,index:usize,offset:u64,out:*mut u8,length:usize,error:*mut u8,capacity:usize)->i32{
 let result=catch_unwind(AssertUnwindSafe(||->Result<(),String>{
  if handle.is_null() || (out.is_null() && length!=0) || length>256*1024*1024{return Err("Invalid Wii disc read request".into());}
  let m=unsafe{&mut *handle.cast::<Mount>()};let f=m.files.get(index).ok_or("Unknown Wii disc file")?;
  if offset>f.node.length() || length as u64>f.node.length()-offset{return Err("Read beyond Wii disc file".into());}
  let mut stream=m.part.open_file(f.node).map_err(|e|e.to_string())?;stream.seek(SeekFrom::Start(offset)).map_err(|e|e.to_string())?;
  if length>0{stream.read_exact(unsafe{slice::from_raw_parts_mut(out,length)}).map_err(|e|e.to_string())?;}Ok(())
 }));
 match result{Ok(Ok(()))=>0,Ok(Err(e))=>{unsafe{message(error,capacity,&e)};-1},Err(_)=>{unsafe{message(error,capacity,"Malformed Wii disc block")};-1}}
}
