//! Original ApprenticeGlobalState.bsq v30 definitions. No invented defaults.
use std::collections::BTreeSet;
#[derive(Debug)]
pub struct Definition {pub name:String,pub id:u16,pub initial:i16,pub flag:bool,pub native_id:u16}
#[derive(Debug)]
pub struct Database {pub id_limit:u16,pub entries:Vec<Definition>}
pub fn parse(bytes:&[u8])->Result<Database,String> {
    if bytes.len()<8 || bytes.len()>4*1024*1024{return Err("Invalid global database size".into());}
    if bytes[..4]!=30u32.to_be_bytes(){return Err("Unsupported global database version".into());}
    fn short(bytes:&[u8],at:usize)->Result<u16,String>{
        let v=bytes.get(at..at+4).ok_or("Truncated global short")?;
        if v[2..]!=[0xcd,0xcd] && v[2..]!=[0xff,0xff]{return Err("Invalid global short padding".into());}
        Ok(u16::from_be_bytes([v[0],v[1]]))
    }
    let id_limit=short(bytes,4)?;let mut at=8;let mut entries=vec![];let mut names=BTreeSet::new();
    while at<bytes.len(){
        if entries.len()>=65536{return Err("Global database entry limit exceeded".into());}
        // Two original string metadata bytes precede the terminated text. Like
        // the original-format reader, use the terminator and absolute alignment.
        let begin=at.checked_add(2).ok_or("Global string offset overflow")?;
        let remaining=bytes.get(begin..).ok_or("Truncated global name")?;
        let length=remaining.iter().take(65534).position(|v|*v==0).ok_or("Unterminated or oversized global name")?;
        let name=std::str::from_utf8(&remaining[..length]).map_err(|_|"Invalid global name UTF-8")?.to_owned();
        if name.is_empty() || !names.insert(name.clone()){return Err("Empty or duplicate global name".into());}
        at=(begin+length+4)&!3;
        let id=short(bytes,at)?;let initial=short(bytes,at+4)? as i16;
        let flag=*bytes.get(at+8).ok_or("Truncated global flag")?!=0;
        bytes.get(at+8..at+12).ok_or("Truncated global flag padding")?;at+=12;
        // Zero IDs repeat in the original database; do not reject or merge them.
        entries.push(Definition{name,id,initial,flag,native_id:id});
    }
    assign_native_ids(&mut entries,id_limit)?;
    Ok(Database{id_limit,entries})
}
fn assign_native_ids(entries:&mut [Definition],id_limit:u16)->Result<(),String>{
    let mut used=BTreeSet::new();
    for entry in entries.iter().filter(|e|e.id!=0){if !used.insert(entry.id){return Err("Duplicate nonzero global ID".into());}}
    // Preserve authored IDs and allocate only above their declared/observed
    // range. Name ordering makes this independent of database record ordering.
    let mut next=u32::from(id_limit).max(u32::from(used.last().copied().unwrap_or(0)))+1;
    let mut zero=entries.iter().enumerate().filter(|(_,e)|e.id==0).map(|(i,_)|i).collect::<Vec<_>>();
    zero.sort_by(|&a,&b|entries[a].name.cmp(&entries[b].name));
    for index in zero {
        if next>u16::MAX as u32{return Err("Native global save ID range exhausted".into());}
        entries[index].native_id=next as u16;next+=1;
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    fn record(name:&str,id:u16,value:i16,flag:u8,out:&mut Vec<u8>){
        out.extend([0,0]);out.extend(name.bytes());out.push(0);while out.len()%4!=0{out.push(0);}
        out.extend(id.to_be_bytes());out.extend([0xcd,0xcd]);out.extend(value.to_be_bytes());out.extend([0xff,0xff]);out.extend([flag,0,0,0]);
    }
    #[test]fn signed_values_zero_ids_and_bounds(){
        let mut b=vec![0,0,0,30,0,2,0xcd,0xcd];record("a",0,-32768,255,&mut b);record("b",0,32767,0,&mut b);
        let db=parse(&b).unwrap();assert_eq!(db.entries.len(),2);assert_eq!(db.entries[0].initial,-32768);assert!(db.entries[0].flag);assert_eq!(db.entries[1].initial,32767);
        for n in 0..b.len(){if n!=8 && n!=24 {assert!(parse(&b[..n]).is_err(),"truncation {n}");}}
        let mut duplicate=b.clone();record("a",1,0,0,&mut duplicate);assert!(parse(&duplicate).is_err());
        b[7]=0;assert!(parse(&b).is_err());
    }
    #[test]fn original_database(){
        let Some(path)=std::env::var_os("EM1_GLOBAL_STATE_FIXTURE") else{return;};
        let db=parse(&std::fs::read(path).unwrap()).unwrap();
        assert_eq!(db.entries.len(),5731);assert_eq!(db.id_limit,3950);
        assert_eq!(db.entries.iter().filter(|v|v.initial!=0).count(),183);
        assert_eq!(db.entries.iter().filter(|v|v.flag).count(),125);
        assert_eq!(db.entries.iter().map(|v|v.native_id).collect::<BTreeSet<_>>().len(),5731);
        assert!(db.entries.iter().all(|v|v.native_id!=0 && (v.id==0 || v.id==v.native_id)));
        assert_eq!(db.entries.iter().filter(|v|v.id==0).map(|v|v.native_id).min(),Some(3951));
        assert_eq!(db.entries.iter().map(|v|v.native_id).max(),Some(5733));
        let portal=db.entries.iter().find(|v|v.name=="DBC_1st_ZoneC_PortalOpen").unwrap();assert_eq!((portal.id,portal.initial),(497,0));
        println!("ORIGINAL_GLOBALS entries=5731 id_limit=3950 nonzero=183 flagged=125 portal=0");
    }
    #[test]fn native_ids_stable_and_bounded(){
        let mut a=vec![0,0,0,30,0,10,0xcd,0xcd];
        record("z",0,1,0,&mut a);record("fixed",7,2,0,&mut a);record("a",0,3,0,&mut a);
        let mut b=vec![0,0,0,30,0,10,0xcd,0xcd];
        record("a",0,3,0,&mut b);record("z",0,1,0,&mut b);record("fixed",7,2,0,&mut b);
        let pairs=|db:Database|db.entries.into_iter().map(|e|(e.name,e.native_id)).collect::<std::collections::BTreeMap<_,_>>();
        assert_eq!(pairs(parse(&a).unwrap()),pairs(parse(&b).unwrap()));
        let ids=pairs(parse(&a).unwrap());assert_eq!(ids["a"],11);assert_eq!(ids["z"],12);assert_eq!(ids["fixed"],7);
        record("duplicate",7,0,0,&mut a);assert!(parse(&a).is_err());
        b[4]=255;b[5]=255;assert!(parse(&b).is_err());
    }
}
