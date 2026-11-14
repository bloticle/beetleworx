//! Streaming 7z decode bridge. No output names are passed to the filesystem.
use std::{io::BufReader,panic::{catch_unwind,AssertUnwindSafe},ffi::c_void};
use sevenz_rust2::{ArchiveReader,Password,Error};
use super::archive_stream::*;
fn decode_error(s:impl Into<std::borrow::Cow<'static,str>>)->Error{Error::Other(s.into())}
fn require(ok:bool,text:&str)->Result<(),String>{if ok{Ok(())}else{Err(text.into())}}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bw_seven_unpack(physical:u64,context:*mut c_void,read:ReadAt,begin:Begin,entry:Entry,data:Data,error:*mut u8,capacity:usize)->i32{
 let result=catch_unwind(AssertUnwindSafe(||->Result<(),String>{
  require(!context.is_null() && (32..=16*1024*1024*1024).contains(&physical),"Invalid 7z source/size")?;
  let file=Source{context,read,length:physical,position:0};
  // Upstream bounds both encoded/decoded header buffers and each decoder's
  // dictionary/model. Output and entry/path budgets are checked separately.
  let mut reader=ArchiveReader::new_with_memory_limit(BufReader::with_capacity(65536,file),Password::empty(),128*1024).map_err(|e|e.to_string())?;
  reader.set_thread_count(1);
  let archive=reader.archive();require(!archive.files.is_empty() && archive.files.len()<=100000 && archive.blocks.len()<=100000,"7z entry/block count exceeds the setup budget")?;
  let mut total=0u64;let mut names=std::collections::HashSet::new();let mut path_bytes=0usize;
  for (i,file) in archive.files.iter().enumerate(){
   let name=normalized(&file.name,file.is_directory)?;path_bytes+=name.len();require(path_bytes<=64*1024*1024,"7z path metadata exceeds the setup budget")?;
   require(names.insert(name.to_ascii_lowercase()),"Duplicate 7z entry path")?;
   require(!file.is_anti_item && !archive.is_file_encrypted(i),"Encrypted or update/deletion 7z entries are unsupported")?;
   let mode=(file.windows_attributes>>16)&0xf000;
   require(!file.has_windows_attributes || ((file.windows_attributes&0x400)==0 && (mode==0 || mode==0x8000 && !file.is_directory || mode==0x4000 && file.is_directory)),"7z links and special file types are unsupported")?;
   require(!file.is_directory || file.size==0,"7z directory contains file data")?;
   total=total.checked_add(file.size).ok_or("7z output size overflow")?;require(total<=16*1024*1024*1024,"7z unpacked data exceeds the 16 GiB setup budget")?;
  }
  for block in &archive.blocks{require(block.coders.len()<=8 && block.get_unpack_size()<=16*1024*1024*1024,"7z compression block exceeds the setup budget")?;}
  require(unsafe{begin(context,total)}!=0,"7z output preparation canceled")?;
  let mut buffer=vec![0;65536];let mut decoded=0u64;
  reader.for_each_entries(|file,input|{
   let name=normalized(&file.name,file.is_directory).map_err(decode_error)?;
   if unsafe{entry(context,name.as_ptr(),name.len(),file.size,i32::from(file.is_directory))}==0{return Err(decode_error("7z entry canceled"));}
   let mut remaining=file.size;
   while remaining>0{
    let count=input.read(&mut buffer[..remaining.min(65536) as usize])?;
    if count==0{return Err(decode_error("Truncated 7z entry data"));}
    if unsafe{data(context,buffer.as_ptr(),count)}==0{return Err(decode_error("7z decoding canceled"));}
    remaining-=count as u64;decoded+=count as u64;
   }
   // Drain/probe so the decoder performs entry and solid-block CRC checks.
   let mut extra=[0];if input.read(&mut extra)?!=0{return Err(decode_error("7z entry exceeds its declared size"));}
   Ok(true)
  }).map_err(|e|e.to_string())?;
  require(decoded==total,"7z decoded size differs from its file table")?;Ok(())
 }));
 match result{Ok(Ok(()))=>0,Ok(Err(e))=>{unsafe{message(error,capacity,&e)};-1},Err(_)=>{unsafe{message(error,capacity,"Malformed 7z rejected by decoder")};-1}}
}
