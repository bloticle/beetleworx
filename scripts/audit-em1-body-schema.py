"""Recover Havok class layouts from original Wii initializer constants.

Read-only evidence tool. Requires an extracted original main.dol and capstone;
does not execute game code or infer layouts from the destination engine.
"""
import argparse
import json
import re
import struct
from pathlib import Path

import capstone


def extract(path, xbox=False):
    blob = path.read_bytes()
    if xbox:
        sections = [(0, 0x82000000, len(blob))]
        code = [(0x12d0000, 0x832d0000, 0x80000)]
        constructor, slots = 0x8238e4e0, (0x54, 0x5c, 0x7c)
    else:
        offsets = struct.unpack_from('>18I', blob, 0)
        addresses = struct.unpack_from('>18I', blob, 0x48)
        sizes = struct.unpack_from('>18I', blob, 0x90)
        sections = list(zip(offsets, addresses, sizes))
        code = sections[:7]
        constructor, slots = 0x8015b1a4, (8, 0xc, 0x1c)

    def read(address, size):
        for offset, start, length in sections:
            if start <= address and address + size <= start + length:
                at = offset + address - start
                return blob[at:at + size]
        raise ValueError('Unmapped DOL address')

    def string(address):
        result = bytearray()
        for i in range(256):
            value = read(address + i, 1)[0]
            if not value:
                return result.decode('ascii')
            result.append(value)
        raise ValueError('Unterminated class/member name')

    decoder = capstone.Cs(capstone.CS_ARCH_PPC,
                          capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    decoder.skipdata = True
    classes = {}
    for offset, address, size in code:
        registers, stack = {}, {}
        start = address
        for instruction in decoder.disasm(blob[offset:offset + size], address):
            op = instruction.mnemonic
            args = instruction.op_str.split(', ')
            if op == 'mflr':
                registers, stack, start = {}, {}, instruction.address
            if op in ('lis', 'li'):
                registers[args[0]] = (int(args[1], 0) << (16 if op == 'lis' else 0)) & 0xffffffff
            elif op in ('addi', 'ori', 'mr'):
                value = registers.get(args[1])
                if value is None:
                    registers.pop(args[0], None)
                else:
                    if op == 'addi':
                        value += int(args[2], 0)
                    elif op == 'ori':
                        value |= int(args[2], 0)
                    registers[args[0]] = value & 0xffffffff
            elif op == 'stw':
                match = re.fullmatch(r'(-?0x[0-9a-f]+|-?\d+)\(r1\)', args[1])
                if match:
                    slot = int(match[1], 0)
                    stack.pop(slot, None)
                    if args[0] in registers:
                        stack[slot] = registers[args[0]]
            elif op == 'bl':
                if (int(args[0], 0) == constructor and
                        all('r' + str(n) in registers for n in (3, 4, 5, 6)) and
                        all(n in stack for n in slots)):
                    # Audited hkClass ctor: caller stack8=members,c=count,
                    # 10=defaults,14=attributes,18=flags,1c=version.
                    name = string(registers['r4'])
                    fields = []
                    for index in range(stack[slots[1]]):
                        raw = read(stack[slots[0]] + index * 24, 24)
                        member, klass, enum = struct.unpack_from('>III', raw)
                        count, flags, position = struct.unpack_from('>HHH', raw, 14)
                        fields.append(dict(name=string(member), klass=f'{klass:08X}',
                                           type=raw[12], subtype=raw[13], count=count,
                                           flags=flags, offset=position))
                    classes[name] = dict(address=f'{registers["r3"]:08X}',
                                         parent=f'{registers["r5"]:08X}', size=registers['r6'],
                                         version=stack[slots[2]], ctor=f'{start:08X}', fields=fields)
                for index in range(3, 13):
                    registers.pop('r' + str(index), None)
            elif args[0].startswith('r') and op.startswith(('lwz', 'lbz', 'lhz', 'lmw')):
                registers.pop(args[0], None)
    return classes


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--dol', type=Path)
    source.add_argument('--xbox-image', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    result = extract(options.xbox_image or options.dol, xbox=options.xbox_image is not None)
    options.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'Recovered {len(result)} original Havok classes')
