//! Bounded read-only CHD bridge, linked into the existing game static library.
use std::{ffi::c_void,fs::{File,OpenOptions},io::{BufReader,Read,Seek,SeekFrom},panic::{catch_unwind,AssertUnwindSafe},ptr,slice};
use chd::{Chd,header::Header,map::{MapEntry,CompressionTypeLegacy as L,CompressionTypeV5 as V}};
struct Mount {image:Chd<BufReader<File>>,logical:u64,hunk:usize,resolved:Vec<u32>,cache:Vec<u8>,encoded:Vec<u8>,cached:Option<u32>}
fn require(ok:bool,text:&str)->Result<(),String>{if ok{Ok(())}else{Err(text.into())}}
fn extent(offset:u64,size:u64,physical:u64)->Result<(),String>{require(offset<=physical && size<=physical-offset,"CHD block extends outside its container")}
fn open(path:&str)->Result<Mount,String>{
 let mut options=OpenOptions::new();options.read(true);
 #[cfg(windows)]{use std::os::windows::fs::OpenOptionsExt;options.share_mode(1);}
 let mut file=BufReader::with_capacity(65536,options.open(path).map_err(|e|e.to_string())?);
 let physical=file.get_ref().metadata().map_err(|e|e.to_string())?.len();
 require(physical>=76 && physical<=16*1024*1024*1024,"Invalid CHD container size")?;
 // Legacy header parsing scans metadata to infer sector units. Bound that list
 // before calling the decoder so a cyclic list cannot hang or grow indefinitely.
 let mut prefix=[0u8;16];file.read_exact(&mut prefix).map_err(|e|e.to_string())?;
 let version=u32::from_be_bytes(prefix[12..16].try_into().unwrap());let header_size=u32::from_be_bytes(prefix[8..12].try_into().unwrap());
 require(&prefix[..8]==b"MComprHD" && matches!((version,header_size),(1,76)|(2,80)|(3,120)|(4,108)|(5,124)),"Invalid CHD version or header length")?;
 if version>=3{
  file.seek(SeekFrom::Start(if version==5{48}else{36})).map_err(|e|e.to_string())?;let mut value=[0;8];file.read_exact(&mut value).map_err(|e|e.to_string())?;let mut at=u64::from_be_bytes(value);
  let mut seen=std::collections::HashSet::new();let mut total=0u64;
  while at!=0{
   require(seen.len()<4096 && seen.insert(at),"CHD metadata list is cyclic or too long")?;extent(at,16,physical)?;file.seek(SeekFrom::Start(at)).map_err(|e|e.to_string())?;
   let mut entry=[0;16];file.read_exact(&mut entry).map_err(|e|e.to_string())?;let length=(u32::from_be_bytes(entry[4..8].try_into().unwrap())&0x00ffffff) as u64;total+=length;
   require(length<=1024*1024 && total<=16*1024*1024,"CHD metadata exceeds its budget")?;extent(at,16+length,physical)?;at=u64::from_be_bytes(entry[8..16].try_into().unwrap());
  }
 }
 file.seek(SeekFrom::Start(0)).map_err(|e|e.to_string())?;
 let header=Header::try_read_header(&mut file).map_err(|e|e.to_string())?;
 let logical=header.logical_bytes();let hunk=header.hunk_size() as usize;let count=header.hunk_count() as usize;
 require(logical>=0x10800 && logical<=16*1024*1024*1024 && logical%2048==0 && hunk>=2048 && hunk<=1024*1024 && hunk%2048==0 && count>0 && count<=8*1024*1024 && count as u64==(logical+hunk as u64-1)/hunk as u64,"Invalid CHD logical size, hunk count or hunk size")?;
 require(matches!(header.unit_bytes(),1|512|2048) || header.unit_bytes()==hunk as u32,"Select a flat Xbox disc CHD, not a CD-sector/audio CHD")?;
 require(!header.has_parent(),"This CHD needs a parent image; use a standalone CHD containing the complete disc")?;
 match &header{
  Header::V5Header(h)=>{
   if header.is_compressed(){
    extent(h.map_offset,16,physical)?;file.seek(SeekFrom::Start(h.map_offset)).map_err(|e|e.to_string())?;let mut length=[0;4];file.read_exact(&mut length).map_err(|e|e.to_string())?;
    let bytes=u32::from_be_bytes(length) as u64;require(bytes<=64*1024*1024,"CHD compressed map exceeds its budget")?;extent(h.map_offset,16+bytes,physical)?;
   }else{extent(h.map_offset,count as u64*4,physical)?;}
  },
  _=>{require(count<=4*1024*1024,"Legacy CHD map exceeds its budget")?;}
 }
 file.seek(SeekFrom::Start(0)).map_err(|e|e.to_string())?;
 let image=Chd::open(file,None).map_err(|e|e.to_string())?;
 require(image.map().len()==count,"CHD map count does not match its header")?;
 let mut resolved=vec![u32::MAX;count];let mut links=vec![0u32;count];
 for i in 0..count{
  let entry=image.map().get_entry(i).ok_or("Missing CHD map entry")?;
  let (reference,data)=match entry{
   MapEntry::V5Compressed(e)=>match e.hunk_type().map_err(|e|e.to_string())?{
    V::CompressionSelf=>(Some(e.block_offset()),None),V::CompressionParent=>return Err("CHD map unexpectedly references a parent".into()),
    V::CompressionType0|V::CompressionType1|V::CompressionType2|V::CompressionType3|V::CompressionNone=>(None,Some((e.block_offset(),e.block_size()))),
    _=>return Err("Invalid decoded CHD map type".into())},
   MapEntry::V5Uncompressed(e)=>(None,if e.block_offset()==0{None}else{Some((e.block_offset(),e.block_size()))}),
   MapEntry::LegacyEntry(e)=>match e.hunk_type().map_err(|e|e.to_string())?{
    L::SelfHunk=>(Some(e.block_offset()),None),L::Mini=>(None,None),L::Compressed|L::Uncompressed=>(None,Some((e.block_offset(),e.block_size()))),
    _=>return Err("Unsupported legacy CHD map type".into())},
  };
  if let Some(target)=reference{require(target<count as u64,"CHD self-reference is outside the map")?;links[i]=target as u32;}
  else{resolved[i]=i as u32;}
  if let Some((offset,bytes))=data{require(bytes>0 && bytes as usize<=hunk*2+65536,"CHD encoded hunk exceeds its budget")?;extent(offset,bytes as u64,physical)?;}
 }
 // Resolve self references once, without recursion, and reject cycles before decoding.
 let mut active=vec![false;count];let mut path=Vec::new();
 for i in 0..count{let mut at=i;path.clear();while resolved[at]==u32::MAX{
   require(!active[at],"CHD hunk map contains a self-reference cycle")?;active[at]=true;path.push(at);at=links[at] as usize;
  }
  let target=resolved[at];for &node in &path{resolved[node]=target;active[node]=false;}
 }
 Ok(Mount{image,logical,hunk,resolved,cache:vec![0;hunk],encoded:Vec::new(),cached:None})
}
unsafe fn message(out:*mut u8,capacity:usize,text:&str){if !out.is_null() && capacity>0{let n=text.len().min(capacity-1);unsafe{ptr::copy_nonoverlapping(text.as_ptr(),out,n);*out.add(n)=0;}}}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bw_chd_open(path:*const u8,length:usize,logical:*mut u64,error:*mut u8,capacity:usize)->*mut c_void{
 let result=catch_unwind(AssertUnwindSafe(||{
  require(!path.is_null() && !logical.is_null() && length>0 && length<=32768,"Invalid CHD open arguments")?;
  let path=std::str::from_utf8(unsafe{slice::from_raw_parts(path,length)}).map_err(|e|e.to_string())?;open(path)
 }));
 match result{Ok(Ok(m))=>{unsafe{*logical=m.logical};Box::into_raw(Box::new(m)).cast()},Ok(Err(e))=>{unsafe{message(error,capacity,&e)};ptr::null_mut()},Err(_)=>{unsafe{message(error,capacity,"Malformed CHD rejected by decoder")};ptr::null_mut()}}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bw_chd_close(handle:*mut c_void){if !handle.is_null(){drop(unsafe{Box::from_raw(handle.cast::<Mount>())});}}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bw_chd_read(handle:*mut c_void,offset:u64,out:*mut u8,length:usize,error:*mut u8,capacity:usize)->i32{
 let result=catch_unwind(AssertUnwindSafe(||->Result<(),String>{
  require(!handle.is_null() && (!out.is_null() || length==0) && length<=256*1024*1024,"Invalid CHD read arguments")?;
  let m=unsafe{&mut *handle.cast::<Mount>()};require(offset<=m.logical && length as u64<=m.logical-offset,"Read outside CHD logical disc")?;if length==0{return Ok(());}
  let output=unsafe{slice::from_raw_parts_mut(out,length)};let mut copied=0;
  while copied<length{let at=offset+copied as u64;let index=m.resolved[(at/m.hunk as u64) as usize];let within=(at%m.hunk as u64) as usize;
   if m.cached!=Some(index){m.cached=None;let written=m.image.hunk(index).map_err(|e|e.to_string())?.read_hunk_in(&mut m.encoded,&mut m.cache).map_err(|e|e.to_string())?;require(written==m.hunk,"Short CHD hunk decode")?;m.cached=Some(index);}
   let count=(length-copied).min(m.hunk-within);output[copied..copied+count].copy_from_slice(&m.cache[within..within+count]);copied+=count;
  }Ok(())
 }));
 match result{Ok(Ok(()))=>0,Ok(Err(e))=>{unsafe{message(error,capacity,&e)};-1},Err(_)=>{unsafe{message(error,capacity,"Malformed CHD rejected while reading")};-1}}
}
