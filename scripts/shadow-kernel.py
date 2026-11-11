"""Reschedule the pinned native receiver with independent PCF comparisons.

Resource/technique containers and native constant bindings are not rewritten.
The runtime substitutes only the verified pixel microcode before construction.
"""
import struct


def broadcast(lane):
    return sum(((lane-i)&3) << (2*i) for i in range(4))


def alu(dest, mask, op, a, b, a_swizzle=0, b_swizzle=0, b_constant=False, a_negate=False, b_negate=False):
    # Scalar RETAIN_PREV, no scalar writes/exports/clamps/addressing/predicate.
    return (0xC8000000 | (mask << 16) | dest,
            (a_swizzle << 16) | (b_swizzle << 8) | (int(a_negate)<<26) | (int(b_negate)<<25),
            0xA0000000 | (0 if b_constant else 0x40000000) |
            (op << 24) | (a << 16) | (b << 8))


def copy(dest, source):
    return alu(dest,15,2,source,source)


def scalar(dest,mask,op,source,lane):
    return ((op<<26)|(mask<<20)|(dest<<8),broadcast(lane),0xE2000000|source)


def generate(words, passes, adaptive=False, radius_probe=False):
    if passes not in (1,2,4):
        raise ValueError('Expected one, two or four comparison batches')
    if len(words)!=504:
        raise ValueError('Native receiver size mismatch')
    if radius_probe and (not adaptive or tuple(words[498:501])!=(0xC80F8000,0,0xE2020200)):
        raise ValueError('Radius probe requires the pinned native color export')
    tokens=[]
    # This pinned program has no jumps, loops or subroutine targets. Preserve
    # predicate/allocate/end records and each original instruction sequence bit.
    for pair in range(15):
        a,b,c=words[pair*3:pair*3+3]
        for side,cf in enumerate((a | ((b&65535)<<32), (b>>16) | (c<<16))):
            op=cf>>44
            if op==0:
                continue
            if op==12:
                tokens.append((cf,None,None,None,pair*2+side));continue
            if op not in (1,2,5):
                raise ValueError('Unexpected native control flow')
            address,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
            template=cf & ~((1<<28)-1)
            for i in range(count):
                index=address+i
                tokens.append((template,(sequence>>(i*2))&3,tuple(words[index*3:index*3+3]),index,pair*2+side))
    regions={69:(81,3,0,3),110:(122,1,2,4),142:(156,0,0,5)}
    result=[];position=0;serial=100
    while position<len(tokens):
        token=tokens[position];index=token[3]
        if index not in regions:
            if radius_probe and index==166:
                instruction=alu(0,15,2,40,40)
                instruction=(instruction[0]|0x8000|(8<<20),instruction[1],instruction[2])
                token=(token[0],token[1],instruction,index,token[4])
            result.append(token);position+=1;continue
        end,dest,lane,texture=regions[index]
        stop=position
        while tokens[stop][3]!=end:
            stop+=1
        block=tokens[position:stop+1];template=token[0]
        def emit(instructions):
            nonlocal serial
            serial+=1
            result.extend((template,0,instruction,None,serial) for instruction in instructions)
        # Only the original four comparisons determine the native scratch state
        # that later color/bounds logic sees. Extra batches restore all inputs.
        emit([copy(10+i,i) for i in range(10)])
        if adaptive:
            # Probe native projected depth before any compare overwrites it.
            # r37.y receiver, r38 four reverse-depth samples; none are exports.
            emit([alu(37,2,2,0,0,broadcast(1 if texture==5 else 0),broadcast(1 if texture==5 else 0))])
            seen=0
            for entry in block:
                if entry[1] is not None and entry[1]&1:
                    first,second,third=entry[2]
                    entry=(entry[0],entry[1],(first,second&~0xF000,third),entry[3],entry[4])
                result.append(entry)
                if entry[1] is not None and entry[1]&1:
                    seen+=1
                    if seen==4:emit([copy(38,{3:3,4:5,5:4}[texture])])
        else:
            result.extend(block)
        emit([copy(20+i,i) for i in range(10)])
        if adaptive:
            # Save scalar previous result, then compute sum/count of positive
            # biased blocker depths. Radius uses projected-depth separation.
            emit([scalar(34,8,50,0,0),
                  alu(36,15,0,38,251,0,broadcast(3),True,True),
                  alu(36,15,0,36,18+texture-3,0,broadcast(0),True),
                  alu(35,15,5,37,36,broadcast(1)),
                  alu(39,15,5,36,250,0,broadcast(0),True),
                  alu(35,15,1,35,39),alu(36,15,1,36,35),
                  alu(37,1,15,36,250,0,broadcast(1),True),
                  alu(37,4,15,35,250,0,broadcast(1),True),
                  alu(39,1,2,37,252,broadcast(0),broadcast(0),True),
                  scalar(39,1,19,39,0),
                  alu(33,1,1,37,37,broadcast(1),broadcast(2)),
                  alu(33,1,0,33,37,broadcast(0),broadcast(0),False,False,True),
                  alu(33,1,2,33,250,broadcast(0),broadcast(0),True),
                  alu(33,1,1,33,39,broadcast(0),broadcast(0)),
                  alu(33,1,1,33,251,broadcast(0),broadcast(0),True),
                  alu(33,1,0,33,249,broadcast(0),broadcast(2),True),
                  alu(33,1,3,33,251,broadcast(0),broadcast(0),True)])
            if radius_probe:emit([alu(40,1<<(texture-3),2,33,33,broadcast(0),broadcast(0))])
            first,second=(16,17) if texture==4 else (15,16)
            emit([alu(31,3,0,first,first,0,0xAA),alu(31,3,0,31,second),
                  alu(31,3,0,31,second,0,0xAA),alu(31,3,1,31,249,0,broadcast(2),True)])
        else:
            emit([alu(30,1,2,dest,dest,broadcast(lane),broadcast(lane))])
        for batch in range(0 if adaptive else 1,passes):
            emit([copy(i,10+i) for i in range(10)])
            fetch_index=0
            for base,sequence,instruction,original,group in block:
                if instruction is None:
                    continue  # allocate is emitted exactly once above
                if sequence&1:
                    first,second,third=instruction
                    if ((first>>20)&31)!=texture or first&31!=1:
                        raise ValueError('Unexpected fetch in comparison region')
                    if third & (0x7FFF<<16):
                        raise ValueError('Native comparison tap already has an offset')
                    # Half-texel integer units. 8-tap uses a balanced corner
                    # pattern. 16-tap adds a zero-centroid triangle about each
                    # native tap; rotate it per tap for aggregate symmetry.
                    if adaptive:
                        second &= ~0xF000  # Point depths, independent of sampler-state filtering.
                        source=(first>>5)&63;sx=(first>>26)&3;sy=(first>>28)&3
                        swizzle=sx|(((sy-1)&3)<<2)
                        emit([alu(32,3,0,source,31,swizzle,0,False,False,True),
                              alu(32,3,1,32,33,0,broadcast(0)),alu(32,3,0,32,31)])
                        first=(first&~((63<<5)|(63<<26)))|(32<<5)|(4<<26)
                    if batch==0:
                        x,y=0,0
                    elif passes==2:
                        x,y=((-1,-1),(1,-1),(1,1),(-1,1))[fetch_index]
                    else:
                        x,y=((-1,-1),(1,-1),(0,2))[batch-1]
                        for _ in range(fetch_index):
                            x,y=-y,x
                    instruction=(first,second,third | ((x&31)<<16) | ((y&31)<<21))
                    fetch_index+=1
                result.append((base,sequence,instruction,original,(group,batch)))
            if fetch_index!=4:
                raise ValueError('Expected four depth reads per native batch')
            emit([alu(30,1,2 if adaptive and batch==0 else 0,
                      dest if adaptive and batch==0 else 30,dest,
                      broadcast(lane if adaptive and batch==0 else 0),broadcast(lane))])
        if adaptive:emit([scalar(0,0,5,34,3)])
        emit([copy(i,20+i) for i in range(10)])
        if passes==1:
            emit([alu(dest,1<<lane,2,30,30,broadcast(0),broadcast(0))])
        else:
            constant,component=(251,1) if passes==2 else (249,2)
            emit([alu(dest,1<<lane,1,30,constant,broadcast(0),broadcast(component),True)])
        position=stop+1
    groups=[]
    for template,sequence,instruction,_,group in result:
        if instruction is None:
            groups.append((template,[] ,None));continue
        if not groups or groups[-1][0]!=template or groups[-1][2]!=group or len(groups[-1][1])==6:
            groups.append((template,[],group))
        groups[-1][1].append((sequence,instruction))
    if len(groups)&1:
        groups.append((0,[],None))
    cf_slots=len(groups)//2;body=[];control=[]
    for template,instructions,_ in groups:
        if not instructions:
            control.append(template);continue
        address=cf_slots+len(body)//3
        if address>=4096:
            raise ValueError('Receiver exceeds native CF address range')
        sequence=sum(s<<(i*2) for i,(s,_) in enumerate(instructions))
        control.append(template | address | (len(instructions)<<12) | (sequence<<16))
        for _,instruction in instructions:
            body.extend(instruction)
    output=[]
    for a,b in zip(control[::2],control[1::2]):
        output.extend((a&0xFFFFFFFF,(a>>32)|((b&65535)<<16),b>>16))
    return output+body
