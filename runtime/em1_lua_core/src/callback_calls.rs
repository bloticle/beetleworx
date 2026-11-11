//! Distinguish reading a function from invoking/escaping it. All potential
//! callees and function-valued arguments/returns/stores remain dependencies.
use std::collections::{BTreeSet,BTreeMap,VecDeque};
use mlua::Result;
fn error(s:&str)->mlua::Error{mlua::Error::RuntimeError(s.into())}
pub struct Usage{pub reads:BTreeSet<Vec<u8>>,pub invokes:BTreeSet<Vec<u8>>,pub children:BTreeSet<usize>,pub reachable:BTreeSet<usize>,pub opaque_call:bool,pub upvalue_calls:u64,pub variables:BTreeSet<u64>,pub dynamic_variable:bool}
pub struct Reference{pub truth:u8,pub service:u32,pub number:Option<u64>}
#[derive(Clone)]struct State{registers:[u64;256],truth:[u8;256],cells:[u64;256],numbers:[Option<u64>;256]}
pub fn analyze(code:&[u32],constants:&[Option<Vec<u8>>],constant_truth:&[u8],constant_numbers:&[Option<u64>],upvalues:&[u8],child_upvalue_calls:&[u64],resolve:&dyn Fn(&[u8])->Result<Reference>)->Result<Usage>{
 let mut names=Vec::new();let mut symbols=BTreeMap::new();
 for &w in code{if w&63==5{let k=(w>>14)as usize;
  let name=constants.get(k).and_then(Option::as_ref).ok_or_else(||error("Invalid callback global name"))?;
  if name.contains(&0){return Err(error("Unsupported binary callback global name"));}
  if !symbols.contains_key(name){symbols.insert(name.clone(),names.len());names.push(name.clone());}
 }}
 // Never silently lose provenance when a function exceeds the compact domain.
 if names.len()+upvalues.len()>64 || code.len()>16384{return Err(error("Callback call-provenance domain requires a larger audited analysis"));}
 let child_base=64-upvalues.len();
 let mut usage=Usage{reads:BTreeSet::new(),invokes:BTreeSet::new(),children:BTreeSet::new(),reachable:BTreeSet::new(),opaque_call:false,upvalue_calls:0,variables:BTreeSet::new(),dynamic_variable:false};
 if code.is_empty(){return Ok(usage);}
 let mut states:Vec<Option<Box<State>>>=vec![None;code.len()];states[0]=Some(Box::new(State{registers:[0;256],truth:[0;256],cells:[0;256],numbers:[None;256]}));
 let mut queue=VecDeque::from([0usize]);let mut escaped=0u64;let mut captures=vec![0u64;upvalues.len()];let mut steps=0;
 let mut captured_registers=vec![[false;256];upvalues.len()];let mut ever=[0u64;256];
 let mut capture_map=vec![Vec::new();upvalues.len()];let mut ever_cells=[0u64;256];
 while let Some(pc)=queue.pop_front(){
  usage.reachable.insert(pc);
  steps+=1;if steps>code.len()*128{return Err(error("Callback call-provenance control-flow budget"));}
  let mut state=states[pc].as_ref().unwrap().clone();let w=code[pc];let op=w&63;
  let a=((w>>6)&255)as usize;let b=(w>>23)as usize;let c=((w>>14)&511)as usize;let bx=(w>>14)as usize;
  let mut next=vec![(pc+1)as i64];
  let truth_before=state.truth;
  let cells_before=state.cells;
  let mut next_truth=0;
  let mut next_cell=0;
  let mut next_number=None;
  match op{
   0=>{state.registers[a]=state.registers.get(b).copied().unwrap_or(0);next_truth=*truth_before.get(b).unwrap_or(&0);next_cell=*cells_before.get(b).unwrap_or(&0);next_number=state.numbers.get(b).copied().flatten();},
   1=>{state.registers[a]=0;next_truth=*constant_truth.get(bx).unwrap_or(&0);next_number=constant_numbers.get(bx).copied().flatten();},
   2=>{state.registers[a]=0;next_truth=if b==0{1}else{2};if c!=0{next=vec![(pc+2)as i64];}},
   4=>{if b>=60{return Err(error("Invalid callback upvalue register"));}state.registers[a]=0;next_cell=1u64<<b;},
   5=>{let name=constants.get(bx).and_then(Option::as_ref).ok_or_else(||error("Invalid callback global name"))?;
    usage.reads.insert(name.clone());state.registers[a]=1u64<<symbols[name];let value=resolve(name)?;next_truth=value.truth;next_number=value.number;},
   6|11=>{
    let source=b;let mut refs=0;
    if c>=256{if let Some(Some(key))=constants.get(c-256){
     if key.contains(&0){return Err(error("Unsupported binary callback field key"));}
     for i in 0..names.len(){if state.registers.get(source).copied().unwrap_or(0)&(1u64<<i)!=0{
      let mut path=names[i].clone();path.push(0);path.extend(key);
      let index=if let Some(&index)=symbols.get(&path){index}else{
       if names.len()>=child_base{return Err(error("Callback static-field provenance domain exceeded"));}
       let index=names.len();names.push(path.clone());symbols.insert(path.clone(),index);index
      };
      let value=resolve(&path)?;
      if refs==0{next_truth=value.truth;next_number=value.number;}else{if next_truth!=value.truth{next_truth=0;}if next_number!=value.number{next_number=None;}}
      usage.reads.insert(path);refs|=1u64<<index;
     }}
    }}
    if op==11 && a<255{state.registers[a+1]=state.registers.get(b).copied().unwrap_or(0);state.truth[a+1]=truth_before.get(b).copied().unwrap_or(0);}
    state.registers[a]=refs;
   },
   3=>{for i in a..=b.min(255){state.registers[i]=0;state.truth[i]=3;state.cells[i]=0;state.numbers[i]=None;}next_truth=3;},
   // Assignments may publish the function for a later callback. Table keys
   // are also escapes: Lua callers can subsequently recover them with pairs.
   7|8=>{escaped|=state.registers[a];usage.upvalue_calls|=state.cells[a];},
   9=>{if b<256{escaped|=state.registers[b];usage.upvalue_calls|=state.cells[b];}if c<256{escaped|=state.registers[c];usage.upvalue_calls|=state.cells[c];}},
   28|29=>{
    if state.registers[a]==0 && state.cells[a]==0{usage.opaque_call=true;}
    let mut variable=false;for i in 0..names.len(){if state.registers[a]&(1u64<<i)!=0 && resolve(&names[i])?.service&512!=0{variable=true;}}
    if variable{
     if b!=4{usage.dynamic_variable=true;}
     let register=a+2;let mut proved=false;
     if let Some(number)=state.numbers.get(register).copied().flatten(){usage.variables.insert(number);proved=true;}
     else{for i in 0..names.len(){if state.registers.get(register).copied().unwrap_or(0)&(1u64<<i)!=0{if let Some(number)=resolve(&names[i])?.number{usage.variables.insert(number);proved=true;}else{usage.dynamic_variable=true;}}}}
     if !proved{usage.dynamic_variable=true;}
    }
    let end=if b==0{256}else{(a+b).min(256)};
    for i in a..end{escaped|=state.registers[i];usage.upvalue_calls|=state.cells[i];}
    let end=if c==0{256}else{(a+c-1).min(256)};
    for i in a..end{state.registers[i]=0;state.truth[i]=0;state.cells[i]=0;state.numbers[i]=None;}
    if op==29{next.clear();}
   },
   30=>{let end=if b==0{256}else{(a+b-1).min(256)};for i in a..end{escaped|=state.registers[i];usage.upvalue_calls|=state.cells[i];}next.clear();},
   22|31|32=>{let target=pc as i64+1+bx as i64-131071;next=if op==31{vec![(pc+1)as i64,target]}else{vec![target]};},
   23=>{let known=|rk:usize|if rk<256{truth_before[rk]}else{*constant_truth.get(rk-256).unwrap_or(&0)};
    let x=known(b);let y=known(c);
    if x!=0 && y!=0 && (x==3 || y==3){next=vec![(pc+if (x==3 && y==3)!=(a!=0){2}else{1})as i64];}
    else{next.push((pc+2)as i64);}},
   24|25=>next.push((pc+2)as i64),
   26=>{let t=truth_before[a];if t==0{next.push((pc+2)as i64);}else{next=vec![(pc+if (t==2)!=(c!=0){2}else{1})as i64];}},
   27=>{let t=*truth_before.get(b).unwrap_or(&0);
    if t!=0{if (t==2)==(c!=0){state.registers[a]=state.registers.get(b).copied().unwrap_or(0);next_cell=*cells_before.get(b).unwrap_or(&0);next_truth=t;}else{next=vec![(pc+2)as i64];next_truth=truth_before[a];next_cell=cells_before[a];}}
    else{state.registers[a]|=state.registers.get(b).copied().unwrap_or(0);next_cell=cells_before[a]|cells_before.get(b).copied().unwrap_or(0);next.push((pc+2)as i64);}},
   33=>{for &v in state.registers.iter().take((a+3).min(256)).skip(a){escaped|=v;}
    for i in (a+3).min(256)..(a+3+c).min(256){state.registers[i]=0;state.truth[i]=0;}next.push((pc+2)as i64);},
   34=>{let end=if b==0{256}else{(a+b+1).min(256)};for &v in state.registers.iter().take(end).skip(a+1){escaped|=v;}if c==0{next=vec![(pc+2)as i64];}},
   35=>{},
   36=>{let count=*upvalues.get(bx).ok_or_else(||error("Invalid callback child prototype"))? as usize;
    let metadata=code.get(pc+1..pc+1+count).ok_or_else(||error("Truncated callback closure metadata"))?;
    capture_map[bx]=metadata.iter().map(|&word|(word&63,(word>>23)as usize)).collect();
    for &capture in metadata{if capture&63==0{let register=(capture>>23)as usize;if register<256{captured_registers[bx][register]=true;captures[bx]|=state.registers[register];}}}
    state.registers[a]=1u64<<(child_base+bx);next_truth=2;next=vec![(pc+1+count)as i64];},
   _=>{state.registers[a]=0;if op==11 && a<255{state.registers[a+1]=0;state.truth[a+1]=truth_before.get(b).copied().unwrap_or(0);}},
  }
  if matches!(op,0..=6|10..=21|27..=29|33|36|37){state.truth[a]=next_truth;state.cells[a]=next_cell;state.numbers[a]=next_number;}
  for i in 0..256{ever[i]|=state.registers[i];ever_cells[i]|=state.cells[i];}
  for successor in next{
   if successor<0 || successor>code.len()as i64{return Err(error("Invalid callback call-provenance control-flow target"));}
   if successor==code.len()as i64{continue;}let successor=successor as usize;
   if let Some(previous)=states[successor].as_mut(){let mut changed=false;
    for i in 0..256{let union=previous.registers[i]|state.registers[i];changed|=union!=previous.registers[i];previous.registers[i]=union;
     let truth=if previous.truth[i]==state.truth[i]{previous.truth[i]}else{0};changed|=truth!=previous.truth[i];previous.truth[i]=truth;}
    for i in 0..256{let cells=previous.cells[i]|state.cells[i];changed|=cells!=previous.cells[i];previous.cells[i]=cells;}
    for i in 0..256{let number=if previous.numbers[i]==state.numbers[i]{previous.numbers[i]}else{None};changed|=number!=previous.numbers[i];previous.numbers[i]=number;}
    if changed{queue.push_back(successor);}
   }else{states[successor]=Some(state.clone());queue.push_back(successor);}
  }
 }
 for(i,registers)in captured_registers.iter().enumerate(){for j in 0..256{if registers[j]{captures[i]|=ever[j];}}}
 loop{let before=escaped;for(i,&capture)in captures.iter().enumerate(){if escaped&(1u64<<(child_base+i))!=0{
   escaped|=capture;
   let calls=child_upvalue_calls.get(i).copied().unwrap_or(0);
   for(cell,&(kind,index))in capture_map[i].iter().enumerate(){if calls&(1u64<<cell)!=0{
    match kind{0 if index<256=>{escaped|=ever[index];usage.upvalue_calls|=ever_cells[index];if ever[index]==0 && ever_cells[index]==0{usage.opaque_call=true;}},
     4 if index<60=>usage.upvalue_calls|=1u64<<index,_=>return Err(error("Invalid callback capture metadata")),}
   }}
  }}
  if before==escaped{break;}}
 for(i,name)in names.into_iter().enumerate(){if escaped&(1u64<<i)!=0{usage.invokes.insert(name);}}
 for i in 0..upvalues.len(){if escaped&(1u64<<(child_base+i))!=0{usage.children.insert(i);}}
 Ok(usage)
}
