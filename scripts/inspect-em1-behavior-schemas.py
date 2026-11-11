"""Compare original Havok behavior reflection with target game reflection.

Read-only development audit. This does not convert a behavior graph or imply
that a same-name field is semantically compatible across Havok versions.
"""
import argparse
import json
from pathlib import Path
import struct


def inspect(path):
    data = Path(path).read_bytes()
    def word(at):
        return struct.unpack_from('>I', data, at)[0]
    if data[:8] != bytes.fromhex('57e0e05710c0c010') or data[16:18] != b'\x04\0' or word(20) != 3:
        raise ValueError('Expected big-endian 32-bit three-section Havok packfile')
    sections = [struct.unpack_from('>7I', data, 84 + i * 48) for i in range(3)]
    pointers, classes = {}, {}
    def text(location):
        section, offset = location
        start, size = sections[section][:2]
        if not 0 <= offset < size:
            raise ValueError('Reflection string out of bounds')
        at = start + offset
        return data[at:data.index(0, at, start + size)].decode('ascii')
    for i, (start, local, glob, virtual, exports, imports, end) in enumerate(sections):
        if not 0 <= local <= glob <= virtual <= exports <= imports <= end or start + end > len(data):
            raise ValueError('Invalid Havok section')
        for begin, stop, stride, kind in [(local, glob, 8, 'local'), (glob, virtual, 12, 'global'), (virtual, exports, 12, 'class')]:
            for offset in range(begin, stop - 3, stride):
                at = start + offset
                field = word(at)
                if field == 0xffffffff:
                    break
                if offset + stride > stop:
                    raise ValueError('Truncated reflection fixup')
                if kind == 'local':
                    pointers[i, field] = (i, word(at + 4))
                elif kind == 'global':
                    pointers[i, field] = (word(at + 4), word(at + 8))
                else:
                    classes[i, field] = text((word(at + 4), word(at + 8)))
    def field_word(location, offset=0):
        section, start = location
        if not 0 <= start + offset <= sections[section][1] - 4:
            raise ValueError('Reflection field out of bounds')
        return word(sections[section][0] + start + offset)
    def string_field(location, offset=0):
        pointer = pointers.get((location[0], location[1] + offset))
        return text(pointer) if pointer is not None else None
    def class_field(location, offset):
        pointer = pointers.get((location[0], location[1] + offset))
        return string_field(pointer) if pointer is not None else None
    schemas = {}
    for location, kind in classes.items():
        if kind != 'hkClass':
            continue
        name, count = string_field(location), field_word(location, 28)
        if count > 1000:
            raise ValueError('Reflection member budget')
        member_pointer = pointers.get((location[0], location[1] + 24))
        members = []
        for i in range(count):
            member = (member_pointer[0], member_pointer[1] + i * 24)
            field_word(member, 20)  # Validate the complete member extent.
            at = sections[member[0]][0] + member[1]
            typ, subtype, array, flags, offset = struct.unpack_from('>BBHHH', data, at + 12)
            members.append(dict(name=string_field(member), type=typ, subtype=subtype,
                                array=array, flags=flags, offset=offset, klass=class_field(member, 4)))
        schemas[name] = dict(size=field_word(location, 8), version=field_word(location, 44),
                             parent=class_field(location, 4), members=members)
    return schemas


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', nargs='+', type=Path, required=True)
    parser.add_argument('--target', nargs='+', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source, target = {}, {}
    for paths, destination in [(args.source, source), (args.target, target)]:
        for path in paths:
            for name, schema in inspect(path).items():
                if name in destination and destination[name] != schema:
                    raise ValueError('Conflicting reflection: ' + name)
                destination[name] = schema
    missing = sorted(set(source) - set(target))
    differences = {}
    for name in source.keys() & target.keys():
        old = {x['name']: x for x in source[name]['members']}
        new = {x['name']: x for x in target[name]['members']}
        differences[name] = dict(source_size=source[name]['size'], target_size=target[name]['size'],
            changed=sorted(n for n in old.keys() & new.keys() if old[n] != new[n]),
            added=sorted(new.keys() - old.keys()), removed=sorted(old.keys() - new.keys()))
    args.output.write_text(json.dumps(dict(source=source, target=target, missing_target=missing,
                                           differences=differences), indent=2))
    print(json.dumps(dict(source_classes=len(source), target_classes=len(target), missing_target=missing)))


if __name__ == '__main__':
    main()
