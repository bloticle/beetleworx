//! Independent semantic check of new native mesh streams against original Wii geometry.
#[path="D:/Repos/bluethinner/src/wii_mesh.rs"] mod reference;
use epic_mickey_lib_rs::{packfile::Packfile,nif_file::*};
fn main()->Result<(),Box<dyn std::error::Error>> {
 let args:Vec<_>=std::env::args().collect();let pak=Packfile::from_binary(std::fs::read(&args[1])?);let dir=std::path::Path::new(&args[2]);
 let mut checked=0;let mut meshes=0;let mut vertices=0;let mut triangles=0;let mut secondary=0;
 for line in std::fs::read_to_string(dir.join("manifest.tsv"))?.lines(){
  let row:Vec<_>=line.split('\t').collect();
  let file=pak.files.iter().find(|f|f.path.replace('\\',"/").eq_ignore_ascii_case(row[0])).ok_or("Missing original")?;
  let old=NifFile::load_bytes(&file.data)?;let new=NifFile::load_bytes(&std::fs::read(dir.join(row[1]))?)?;
  let mut count=0;
  for (i,obj) in old.blocks.iter().enumerate(){
   let NiObject::NiMesh(mesh)=obj else{continue;};
   if !mesh.stream_refs.iter().flat_map(|s|&s.semantics).any(|s|s.name=="DISPLAYLIST"){continue;}
   let NiObject::NiMesh(converted)=&new.blocks[i] else{return Err("Converted mesh ID changed".into())};
   let a=reference::extract(&old,mesh)?;let b=reference::extract(&new,converted)?;
   if a.mesh.positions!=b.mesh.positions||a.mesh.normals!=b.mesh.normals||a.mesh.colors!=b.mesh.colors||a.mesh.uvs!=b.mesh.uvs||a.mesh.indices!=b.mesh.indices||a.extra_uvs!=b.extra_uvs {
    return Err(format!("Semantic mesh mismatch {}#{}: vertices {}/{} triangles {}/{} position={} normal={} color={} UV={} index={} secondary={}",file.path,i,a.mesh.vertex_count,b.mesh.vertex_count,a.mesh.indices.len()/3,b.mesh.indices.len()/3,a.mesh.positions==b.mesh.positions,a.mesh.normals==b.mesh.normals,a.mesh.colors==b.mesh.colors,a.mesh.uvs==b.mesh.uvs,a.mesh.indices==b.mesh.indices,a.extra_uvs==b.extra_uvs).into());
   }
   count+=1;meshes+=1;vertices+=a.mesh.vertex_count;triangles+=a.mesh.indices.len()/3;secondary+=usize::from(!a.extra_uvs.is_empty());
  }
  if count!=row[2].parse::<usize>()?{return Err("Mesh conversion count mismatch".into());}checked+=1;
 }
 println!("PASS independent converted-NIF semantic comparison files={} meshes={} vertices={} triangles={} secondary_UV_meshes={}",checked,meshes,vertices,triangles,secondary);Ok(())
}
