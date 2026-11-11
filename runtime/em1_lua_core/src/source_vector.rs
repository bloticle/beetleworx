//! Audited original vector4 subset: four-lane subtraction and XZ length2.
//! Source method table REL4BD7F0: __sub279994 and length227A000.
//! Other original vector4 indexing/operators/methods remain unimplemented.
use mlua::{AnyUserData,MetaMethod,UserData,UserDataMethods};
#[derive(Clone,Copy,Debug)]
pub struct Vector4(pub [f32;4]);
const ESTIMATE:[(i64,i64);32]=[
 (0x1a7e800,-0x568),(0x17cb800,-0x4f3),(0x1552800,-0x48d),(0x130c000,-0x435),
 (0x10f2000,-0x3e7),(0x0eff000,-0x3a2),(0x0d2e000,-0x365),(0x0b7c000,-0x32e),
 (0x09e5000,-0x2fc),(0x0867000,-0x2d0),(0x06ff000,-0x2a8),(0x05ab800,-0x283),
 (0x046a000,-0x261),(0x0339800,-0x243),(0x0218800,-0x226),(0x0105800,-0x20b),
 (0x3ffa000,-0x7a4),(0x3c29000,-0x700),(0x38aa000,-0x670),(0x3572000,-0x5f2),
 (0x3279000,-0x584),(0x2fb7000,-0x524),(0x2d26000,-0x4cc),(0x2ac0000,-0x47e),
 (0x2881000,-0x43a),(0x2665000,-0x3fa),(0x2468000,-0x3c2),(0x2287000,-0x38e),
 (0x20c1000,-0x35e),(0x1f12000,-0x332),(0x1d79000,-0x30a),(0x1bf4000,-0x2e6),
];
fn estimate(value:f32)->f64{
 let bits=f64::from(value).to_bits();let exponent=((bits>>52)&2047)as i32-1023;
 let fraction=(bits&((1u64<<52)-1))>>37;let bank=if exponent%2==0{16}else{0};
 let (base,delta)=ESTIMATE[bank+(fraction>>11)as usize];
 let mantissa=(base+delta*(fraction&2047)as i64)as u64;
 f64::from_bits((((1022-exponent.div_euclid(2))as u64)<<52)|(mantissa<<26))
}
fn fast_sqrt(value:f32)->f32{
 // DOL800071F0 frsqrte/refinement/fsel: zero, negative and nonfinite
 // inputs select the original input after an unordered refinement. No
 // exact special NaN payload parity is claimed by this bounded adapter.
 if !(value>0.) || !value.is_finite(){return value*value;}
 let inverse=estimate(value);let bits=inverse.to_bits();
 // Gekko fmuls rounds its C operand to25 significant bits first. The
 // estimate's remaining precision must not be narrowed to f32 beforehand.
 let operand=f64::from_bits(((bits>>28)+((bits>>27)&1))<<28);
 let squared=(inverse*operand)as f32;let half=(inverse*0.5)as f32;
 let correction=-squared.mul_add(value,-3.);let refined=correction*half;
 value*if refined>=0.{refined}else{value}
}
impl Vector4 {
 fn subtract(self,other:Self)->Self{Self(std::array::from_fn(|i|self.0[i]-other.0[i]))}
 fn length2(self)->f32{
  // REL27A04C loads Z and X only: fmuls(Z,Z), fmadds(X,X,Z2).
  // length2 is a horizontal magnitude, not a squared/vector4 magnitude.
  fast_sqrt(self.0[0].mul_add(self.0[0],self.0[2]*self.0[2]))
 }
}
impl UserData for Vector4 {
 fn add_methods<M:UserDataMethods<Self>>(methods:&mut M){
  methods.add_meta_function(MetaMethod::Sub,|_,(a,b):(AnyUserData,AnyUserData)|{
   let a=*a.borrow::<Vector4>()?;let b=*b.borrow::<Vector4>()?;Ok(a.subtract(b))
  });
  // Original method consumes receiver1, ignores additional arguments and
  // pushes the resulting single-precision value as a Lua numeric value.
  methods.add_method("length2",|_,value,_:mlua::MultiValue|Ok(value.length2()));
 }
}
#[cfg(test)]mod tests{
 use super::*;
 #[test]fn original_scalar_instruction_fixtures_match_float_bits(){
  // Independently executed original REL27A04C..64 and DOL800071F0 words.
  // analysis/em1-vector-length2-original-fixture.json records binary hashes.
  let cases=[
   ([1077936128,1148829696,1082130432,1123418112],1084227584),
   ([0,3259498496,3231711232,3248488448],1084227584),
   ([1065353216,0,1065353216,0],1068827891),
   ([786163455,0,786163455,0],790331026),
   ([1594541859,1120272384,1594541859,1084227584],1598309108),
   ([0,1120272384,2147483648,0],0),([1,0,0,0],0),
   ([2139095039,0,2139095039,0],2139095040),
   ([3228404694,1082700732,1105023509,1120403456],1105153467),
   ([1065353217,0,1065353215,0],1068827892),
  ];
  for(lanes,result)in cases{assert_eq!(Vector4(lanes.map(f32::from_bits)).length2().to_bits(),result,"{lanes:?}");}
 }
 #[test]fn lua_userdata_subtracts_all_lanes_without_mutating_inputs_and_ignores_yw_for_length(){
  let vm=crate::definition_vm().unwrap();vm.globals().set("a",Vector4([3.,100.,4.,7.])).unwrap();vm.globals().set("b",Vector4([0.,-200.,0.,-5.])).unwrap();
  let value=vm.load("return a-b").eval::<AnyUserData>().unwrap();assert_eq!(value.borrow::<Vector4>().unwrap().0,[3.,300.,4.,12.]);
  assert_eq!(vm.load("return (a-b):length2('ignored',{},nil)").eval::<f64>().unwrap(),5.);
  assert_eq!(vm.globals().get::<AnyUserData>("a").unwrap().borrow::<Vector4>().unwrap().0,[3.,100.,4.,7.]);
  assert!(vm.load("return a-1").eval::<mlua::Value>().is_err());
  let registry=crate::entities::Registry::default();registry.bind(&vm,17).unwrap();vm.globals().set("actor",registry.snapshot(17).unwrap()).unwrap();assert!(vm.load("return a-actor").eval::<mlua::Value>().is_err());
 }
 #[test]fn original_projector_distance_helper_executes_real_source_bytecode(){
  let Some(root)=std::env::var_os("EM1_LUA_ORIGINAL_FIXTURES")else{return;};let path=std::path::Path::new(&root).join("Prefab_Projector.lua");if !path.exists(){return;}
  let vm=crate::definition_vm().unwrap();let original=std::fs::read(path).unwrap();let chunk=crate::prepare_chunk(&original).unwrap();vm.load(&chunk.bytes).set_mode(mlua::ChunkMode::Binary).exec().unwrap();
  vm.globals().set("a",Vector4([3.,900.,4.,80.])).unwrap();vm.globals().set("b",Vector4([0.,-900.,0.,-80.])).unwrap();
  assert_eq!(vm.load("return Prefab_ProjectorMickeyPosition(a,b)").eval::<f64>().unwrap(),5.);
 }
}

