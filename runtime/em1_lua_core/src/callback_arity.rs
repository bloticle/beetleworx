//! Conservative control-flow analysis of statically named GB calls, before
//! admission can consume an authored one-shot. MOVE aliases and joined paths
//! retain possible callee identities; dynamic argument counts are rejected.
use mlua::Result;
use std::collections::VecDeque;
fn error(s:&str)->mlua::Error{mlua::Error::RuntimeError(s.into())}
fn symbol(value:Option<&Vec<u8>>)->u8{match value.map(Vec::as_slice){Some(b"AnimGBReset")=>1,Some(b"AnimGBSequence")=>2,Some(b"AnimVarInt")=>4,_=>0}}
pub fn check(code:&[u32],constants:&[Option<Vec<u8>>],children:&[u8])->Result<()>{
    check_impl(code,constants,children,None,&std::collections::BTreeSet::new(),&std::collections::BTreeSet::new())
}
pub fn check_reachable(code:&[u32],constants:&[Option<Vec<u8>>],children:&[u8],reachable:&std::collections::BTreeSet<usize>,single:&std::collections::BTreeSet<Vec<u8>>,trusted:&std::collections::BTreeSet<Vec<u8>>)->Result<()>{
    check_impl(code,constants,children,Some(reachable),single,trusted)
}
fn check_impl(code:&[u32],constants:&[Option<Vec<u8>>],children:&[u8],reachable:Option<&std::collections::BTreeSet<usize>>,single:&std::collections::BTreeSet<Vec<u8>>,trusted:&std::collections::BTreeSet<Vec<u8>>)->Result<()>{
    if code.is_empty() || !constants.iter().any(|v|symbol(v.as_ref())!=0){return Ok(());}
    if code.len()>16384{return Err(error("Gamebryo callback arity analysis exceeds instruction budget"));}
    let mut states:Vec<Option<Box<[u8;256]>>>=vec![None;code.len()];states[0]=Some(Box::new([0;256]));
    let mut tops:Vec<Option<usize>>=vec![None;code.len()];let mut globals_valid=vec![true;code.len()];
    let mut queue=VecDeque::from([0usize]);let mut steps=0;
    while let Some(pc)=queue.pop_front(){
        steps+=1;if steps>code.len()*128{return Err(error("Gamebryo callback arity control-flow budget"));}
        let mut state=states[pc].as_ref().unwrap().clone();let mut top=tops[pc];let mut globals_live=globals_valid[pc];let w=code[pc];let op=w&63;let a=((w>>6)&255)as usize;
        let b=(w>>23)as usize;let c=((w>>14)&511)as usize;let bx=(w>>14)as usize;
        let mut successors=vec![(pc+1)as i64];
        match op{
            0=>state[a]=state.get(b).copied().unwrap_or(0),
            5=>{let key=constants.get(bx).and_then(Option::as_ref);state[a]=symbol(key)|if globals_live && key.is_some_and(|key|single.contains(key)){64}else{0}|if globals_live && key.is_some_and(|key|single.contains(key)||trusted.contains(key)){128}else{0};},
            3=>{for v in state.iter_mut().take(b.min(255)+1).skip(a){*v=0;}},
            28|29=>{
                // Lua CALL B=0 consumes the producer's actual open results.
                // Audited getters return exactly one result even for missing
                // entities (nil/zero userdata), so nested getter calls are
                // statically countable. Unknown/multiple returns stay unknown.
                let b=if b==0{top.filter(|&top|top>a).map_or(0,|top|top-a)}else{b};
                let one_result=state[a]&64!=0;
                if state[a]&128==0{globals_live=false;for value in state.iter_mut(){*value&=63;}}
                if state[a]&1!=0 && b!=2{return Err(error("Original AnimGBReset requires one argument before native admission"));}
                if state[a]&2!=0 && b!=3{return Err(error("Original AnimGBSequence optional/dynamic argument form is not implemented; rejected before effects"));}
                if state[a]&4!=0 && b!=4{return Err(error("Original AnimVarInt requires three arguments before native admission"));}
                let end=if c==0{256}else{(a+c-1).min(256)};for v in state.iter_mut().take(end).skip(a){*v=0;}
                top=if c==0 && one_result{Some(a+1)}else{None};
                if op==29{successors.clear();}
            },
            22|31|32=>{let target=pc as i64+1+bx as i64-131071;successors=if op==31{vec![(pc+1)as i64,target]}else{vec![target]};},
            23..=26=>successors.push((pc+2)as i64),
            27=>{state[a]=0;successors.push((pc+2)as i64);},
            30=>successors.clear(),
            33=>{for v in state.iter_mut().take((a+3+c).min(256)).skip((a+3).min(256)){*v=0;}successors.push((pc+2)as i64);},
            36=>{state[a]=0;let upvalues=*children.get(bx).ok_or_else(||error("Invalid callback child prototype"))?;successors=vec![(pc+1+upvalues as usize)as i64];},
            34=>{if c==0{successors=vec![(pc+2)as i64];}},
            7=>{if constants.get(bx).and_then(Option::as_ref).is_some_and(|key|single.contains(key)||trusted.contains(key)){globals_live=false;}},
            8..=9|35=>{},
            _=>{state[a]=0;if op==11 && a<255{state[a+1]=0;}if op==2 && c!=0{successors=vec![(pc+2)as i64];}},
        }
        for next in successors{
            if next<0 || next>code.len()as i64{return Err(error("Invalid callback control-flow target"));}
            if next==code.len()as i64{continue;}let next=next as usize;
            if reachable.is_some_and(|allowed|!allowed.contains(&next)){continue;}
            if let Some(previous)=states[next].as_mut(){
                let mut changed=false;for i in 0..256{let union=((previous[i]|state[i])&63)|((previous[i]&state[i])&192);changed|=union!=previous[i];previous[i]=union;}
                let joined=if tops[next]==top{top}else{None};changed|=joined!=tops[next];tops[next]=joined;
                let live=globals_valid[next]&&globals_live;changed|=live!=globals_valid[next];globals_valid[next]=live;
                if changed{queue.push_back(next);}
            }else{states[next]=Some(state.clone());tops[next]=top;globals_valid[next]=globals_live;queue.push_back(next);}
        }
    }
    Ok(())
}

#[cfg(test)]mod tests{
    use super::*;
    fn globals(names:&[&[u8]])->Vec<Option<Vec<u8>>>{names.iter().map(|n|Some(n.to_vec())).collect()}
    fn get(a:u32,k:u32)->u32{5|a<<6|k<<14}
    fn call(a:u32,args:u32)->u32{28|a<<6|(args+1)<<23|1<<14}
    #[test]fn aliases_and_bad_static_or_dynamic_calls_reject(){
        let k=globals(&[b"AnimGBReset",b"AnimGBSequence"]);
        assert!(check(&[get(0,0),call(0,1),get(0,1),call(0,2),30],&k,&[]).is_ok());
        assert!(check(&[get(1,1),0|2<<6|1<<23,call(2,3),30],&k,&[]).is_err());
        assert!(check(&[get(0,1),28|1<<14,30],&k,&[]).is_err());
    }
    #[test]fn open_argument_counts_follow_only_verified_single_result_getters(){
        let k=globals(&[b"AnimGBReset",b"GetRelativePrefabEntity",b"UnknownResults"]);
        let open=|a:u32,args:u32|28|a<<6|(args+1)<<23; // C=0 keeps actual top.
        let outer=28|2<<14; // B=0,C=2.
        // GetPosition(GetRelativePrefabEntity(a,b)): inner A1 returns one.
        let singles=std::collections::BTreeSet::from([b"GetRelativePrefabEntity".to_vec()]);
        assert!(check_impl(&[get(0,0),get(1,1),open(1,2),outer,30],&k,&[],None,&singles,&singles).is_ok());
        assert!(check(&[get(0,0),get(1,1),open(1,2),outer,30],&k,&[]).is_err());
        // Extra fixed argument plus the same getter still means two args.
        assert!(check(&[get(0,0),get(2,1),open(2,2),outer,30],&k,&[]).is_err());
        assert!(check(&[get(0,0),get(1,2),open(1,2),outer,30],&k,&[]).is_err());
    }
    #[test]fn single_return_is_a_must_proof_across_unknown_branch_joins(){
        let k=globals(&[b"AnimGBReset",b"GetRelativePrefabEntity",b"UnknownResults"]);
        let singles=std::collections::BTreeSet::from([b"GetRelativePrefabEntity".to_vec()]);
        let jump=|offset:i32|22|((offset+131071)as u32)<<14;
        let code=[get(0,0),26,jump(2),get(1,1),jump(1),get(1,2),28|1<<6|1<<23,28|2<<14,30];
        assert!(check_impl(&code,&k,&[],None,&singles,&singles).is_err());
    }
    #[test]fn branch_join_does_not_forget_an_unsupported_possible_callee(){
        let k=globals(&[b"AnimGBReset",b"AnimGBSequence"]);
        let jump=|offset:i32|22|((offset+131071)as u32)<<14;
        // Both branches reach the final call; one needs1arg and one2args.
        let code=[26,jump(2),get(0,0),jump(1),get(0,1),call(0,2),30];
        assert!(check(&code,&k,&[]).is_err());
        assert!(check(&[get(0,0),call(0,1),get(0,1),call(0,2),30],&k,&[]).is_ok());
    }
}
