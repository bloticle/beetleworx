"""Read-only audit of native vertex fetch declarations and export fixups.
Original bytecode stays in ignored analysis files; this does not patch effects.
"""
import argparse, hashlib, json, runpy, struct
from pathlib import Path

USAGES = {0:"POSITION", 1:"BLENDWEIGHT", 2:"BLENDINDICES", 3:"NORMAL", 5:"TEXCOORD", 6:"TANGENT", 10:"COLOR"}

def inspect(path):
    data=path.read_bytes()
    extract=runpy.run_path(str(Path(__file__).with_name("inspect-scene-shaders.py")))["extract"]
    results=[]
    for program in extract(data,False):
        base=program["container_offset"]
        header=struct.unpack_from(">9I",data,base)
        at=base+header[6]
        native=struct.unpack_from(">8I",data,at)
        at+=32
        fixups=struct.unpack_from(">I",data,at)[0];at+=4
        constant_count,input_count=native[6:8]
        output_count=((native[2]>>20)&15)+1
        expected=at+4*(constant_count+input_count+output_count+fixups)
        if expected!=program["code_offset"]:raise ValueError("Unrecognized native declaration layout")
        constants=list(struct.unpack_from(f">{constant_count}I",data,at));at+=constant_count*4
        inputs=[];words=program["words"]
        for _ in range(input_count):
            raw=struct.unpack_from(">I",data,at)[0];at+=4
            instruction=raw&4095
            if instruction*3+2>=len(words) or (words[instruction*3]&31)!=0:raise ValueError("Declaration does not reference a vertex fetch")
            inputs.append({"usage":USAGES.get((raw>>12)&15,str((raw>>12)&15)),"index":(raw>>16)&15,"instruction":instruction,"raw":f"{raw:08X}"})
        outputs=list(struct.unpack_from(f">{output_count}I",data,at));at+=output_count*4
        components=sum(((record>>12)&15).bit_count() for record in outputs)
        signature=(components<<10)|(output_count<<5)|output_count
        if native[5]!=signature:raise ValueError("Interpolator component count disagrees with declarations")
        exports=[]
        for _ in range(fixups):
            raw=struct.unpack_from(">I",data,at)[0];at+=4
            instruction=raw&4095
            if instruction*3+2>=len(words) or not(words[instruction*3]&0x8000):raise ValueError("Fixup does not reference an export")
            exports.append({"instruction":instruction,"raw":f"{raw:08X}"})
        results.append({"container_offset":base,"native_offset":base+header[6],"code_offset":program["code_offset"],"code_bytes":program["code_bytes"],"native_header":[f"{v:08X}" for v in native],"constant_records":[f"{v:08X}" for v in constants],"inputs":inputs,"output_records":[f"{v:08X}" for v in outputs],"export_fixups":exports})
    return {"file":str(path),"sha256":hashlib.sha256(data).hexdigest(),"programs":results,"scope":"Pinned container layout and instruction-reference consistency. Not a GPU execution or live stream-binding test."}

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("effects",nargs="+",type=Path);parser.add_argument("--output",type=Path,required=True);args=parser.parse_args()
    result=[inspect(path) for path in args.effects]
    with args.output.open("x") as stream:json.dump(result,stream,indent=2);stream.write("\n")
    print(json.dumps([{ "file":r["file"],"inputs":p["inputs"]} for r in result for p in r["programs"]],indent=2))
