//! Independent development-only validation against epic-mickey-lib-rs.
use epic_mickey_lib_rs::{file_manipulator::EndianType,scene_file::SceneFile};
fn main() {
    let args:Vec<_>=std::env::args().collect();
    let source=std::fs::read(&args[1]).unwrap();
    let converted=std::fs::read(&args[2]).unwrap();
    let old=SceneFile::from_binary(&source,EndianType::BIG);
    let new=SceneFile::from_binary(&converted,EndianType::BIG);
    let mut a=old.to_dict();let mut b=new.to_dict();
    for value in [&mut a,&mut b] {let map=value.as_object_mut().unwrap();map.remove("version");map.remove("unique_id");}
    assert_eq!(a,b,"Independent scene semantic comparison failed");
    println!("{} entities validated independently",new.objects.len());
}
