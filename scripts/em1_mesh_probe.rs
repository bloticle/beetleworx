//! Development-only audit of real source mesh decoding and opaque preservation.
use epic_mickey_lib_rs::{packfile::Packfile,nif_file::{NifFile,NiObject,extract_mesh_with_source_indices}};
fn main(){
 let args:Vec<_>=std::env::args().collect();let out=std::path::Path::new(&args[2]);std::fs::create_dir_all(out).unwrap();
 let pak=Packfile::from_binary(std::fs::read(&args[1]).unwrap());
 let(mut meshes,mut decoded,mut failures)=(0,0,0);
 for file in pak.files {
  if !file.data.starts_with(b"Gamebryo File Format"){continue;}
  let nif=match NifFile::load_bytes(&file.data){Ok(n)=>n,Err(_)=>{failures+=1;continue}};
  let mut has_mesh=false;
  for (index,obj) in nif.blocks.iter().enumerate(){if let NiObject::NiMesh(mesh)=obj{
   meshes+=1;has_mesh=true;
   let semantics:Vec<_>=mesh.stream_refs.iter().flat_map(|r|r.semantics.iter().map(|s|format!("{}:{}",s.name,s.index))).collect();
   println!("META {:?} {} submeshes={} skin={} modifiers={} semantics={:?}",file.path,index,mesh.submesh_count,mesh.hw_skin_flag,mesh.modifiers.len(),semantics);
   if let Some((geometry,sources))=extract_mesh_with_source_indices(&nif,mesh){
    let valid=geometry.positions.iter().flatten().all(|x|x.is_finite())&&geometry.indices.iter().all(|i|(*i as usize)<geometry.positions.len())&&geometry.indices.len()%3==0;
    if valid {decoded+=1;}else{failures+=1;}
    println!("MESH {:?} {} vertices={} triangles={} source_indices={} normals={} uvs={} colors={} valid={}",file.path,index,geometry.positions.len(),geometry.indices.len()/3,sources.len(),geometry.normals.is_some(),geometry.uvs.is_some(),geometry.colors.is_some(),valid);
   }else{println!("UNDECODED {:?} {}",file.path,index);failures+=1;}
  }}
  if has_mesh {
   let dest=out.join(file.path.replace("\\","/"));std::fs::create_dir_all(dest.parent().unwrap()).unwrap();
   match nif.to_bytes(){Ok(bytes)=>std::fs::write(dest,bytes).unwrap(),Err(e)=>println!("SERIALIZE_ERROR {:?}: {}",file.path,e)}
  }
 }
 println!("TOTAL meshes={} decoded={} failures={}",meshes,decoded,failures);
}
