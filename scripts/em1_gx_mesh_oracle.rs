//! Development-only oracle. Includes the authorized reference read-only.
#[path = "D:/Repos/bluethinner/src/wii_mesh.rs"] mod reference;
use epic_mickey_lib_rs::{packfile::Packfile, nif_file::*};
use std::{fs,io::{self,Write}};
fn word(out:&mut Vec<u8>, x:u32){out.extend_from_slice(&x.to_le_bytes());}
fn floats(out:&mut Vec<u8>, x:[f32;4]){for v in x {word(out,v.to_bits());}}
fn blob(out:&mut Vec<u8>, x:&[u8]){word(out,x.len() as u32);out.extend_from_slice(x);}
fn main()->io::Result<()> {
 let args:Vec<_>=std::env::args().collect();
 let pak=Packfile::from_binary(fs::read(&args[1])?);
 let mut records=Vec::new();let mut accepted=0u32;let mut rejected=0u32;
 for file in pak.files {
  if !file.data.starts_with(b"Gamebryo File Format"){continue;}
  let nif=NifFile::load_bytes(&file.data).map_err(io::Error::other)?;
  for (block,obj) in nif.blocks.iter().enumerate(){
   let NiObject::NiMesh(mesh)=obj else{continue;};
   if !mesh.stream_refs.iter().flat_map(|s|&s.semantics).any(|s|s.name=="DISPLAYLIST"){continue;}
   let decoded=match reference::extract(&nif,mesh){Ok(v)=>v,Err(e)=>{eprintln!("REJECT {}#{}: {}",file.path,block,e);rejected+=1;continue;}};
   let mut attributes=Vec::new();let mut commands=Vec::new();
   for r in &mesh.stream_refs {
    let NiObject::NiDataStream(stream)=&nif.blocks[r.stream_link as usize] else{panic!("validated stream")};
    let region=&stream.regions[r.submesh_region_indices.first().copied().unwrap_or(0) as usize];
    let start=region.start_index as usize;let count=region.count as usize;
    for (i,s) in r.semantics.iter().enumerate(){
     let kind=match s.name.as_str(){"POSITION"=>0,"NORMAL"=>1,"COLOR"=>2,"TEXCOORD"=>3,"DISPLAYLIST"=>{commands=stream.buffer[start..start+count].to_vec();continue;},_=>continue};
     let values:Vec<[f32;4]>=match kind {
      0|1=>extract_f32x3_stream(nif.source_is_little_endian,stream,i,start,count).into_iter().map(|v|[v[0],v[1],v[2],1.]).collect(),
      2=>extract_f32x4_stream(nif.source_is_little_endian,stream,i,start,count),
      _=>extract_f32x2_stream(nif.source_is_little_endian,stream,i,start,count).into_iter().map(|v|[v[0],v[1],0.,1.]).collect()};
     attributes.push((kind,s.index,values));
    }
   }
   attributes.sort_by_key(|a|(a.0,a.1));
   if attributes.iter().any(|a|a.0<3&&a.1!=0){eprintln!("REJECT secondary non-UV oracle attribute {}#{}",file.path,block);rejected+=1;continue;}
   blob(&mut records,format!("{}#{}",file.path,block).as_bytes());blob(&mut records,&commands);
   word(&mut records,attributes.len() as u32);
   for (kind,index,values) in &attributes {
    word(&mut records,*kind);word(&mut records,*index);word(&mut records,values.len() as u32);
    for v in values{floats(&mut records,*v);}
   }
   word(&mut records,decoded.mesh.vertex_count as u32);
   word(&mut records,decoded.mesh.indices.len() as u32);
   for v in &decoded.mesh.indices{word(&mut records,*v);}
   for vertex in 0..decoded.mesh.vertex_count {
    word(&mut records,decoded.source_indices[vertex]);
    word(&mut records,decoded.colour_indices.as_ref().map(|v|v[vertex]).unwrap_or(u32::MAX));
    for (kind,index,_) in &attributes {
     let value=match kind {
      0=>{let v=decoded.mesh.positions[vertex];[v[0],v[1],v[2],1.]},
      1=>{let v=decoded.mesh.normals.as_ref().unwrap()[vertex];[v[0],v[1],v[2],1.]},
      2=>decoded.mesh.colors.as_ref().unwrap()[vertex],
      _=>{let v=if *index==0{decoded.mesh.uvs.as_ref().unwrap()[vertex]}else{decoded.extra_uvs[index][vertex]};[v[0],v[1],0.,1.]}};
     floats(&mut records,value);
    }
   }
   accepted+=1;
  }
 }
 let mut out=fs::File::create(&args[2])?;out.write_all(b"E1GX")?;out.write_all(&1u32.to_le_bytes())?;out.write_all(&accepted.to_le_bytes())?;out.write_all(&records)?;
 println!("Native GX oracle cases={} reference-rejected={} bytes={}",accepted,rejected,records.len());Ok(())
}
