"""Inspect original EM1 REL relocation references without executing or patching it.

File offsets are reference locations, not loaded guest addresses. This developer
tool does not link original Wii components into EM2 or claim ABI compatibility.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def inspect(data, names, include_sites=False):
    def word(at):
        if at < 0 or at + 4 > len(data):
            raise ValueError('Truncated REL word')
        return struct.unpack_from('>I', data, at)[0]

    if len(data) < 0x4C or word(0x1C) != 3:
        raise ValueError('Expected original REL v3')
    module, count, table = word(0), word(0xC), word(0x10)
    if not 0 < count <= 256 or table + count * 8 > len(data):
        raise ValueError('Invalid REL section table')
    sections = []
    for i in range(count):
        encoded, size = word(table + i * 8), word(table + i * 8 + 4)
        offset = encoded & ~3
        if offset and offset + size > len(data):
            raise ValueError('REL section exceeds original file')
        sections.append(dict(index=i, offset=offset, size=size, executable=bool(encoded & 1)))
    strings = {}
    for name in names:
        pattern = name.encode('ascii') + b'\0'
        begin = 0
        while (at := data.find(pattern, begin)) >= 0:
            begin = at + 1
            section = next((s for s in sections if s['offset'] and s['offset'] <= at < s['offset'] + s['size']), None)
            if section:
                strings[at] = dict(name=name, section=section['index'], section_offset=at-section['offset'], file_offset=at, references=[])
    start, size = word(0x28), word(0x2C)
    if size % 8 or start + size > len(data) or size > 4096:
        raise ValueError('Invalid REL import table')
    imports = [(word(at), word(at+4)) for at in range(start, start+size, 8)]
    relocation_count = 0
    sites = {}
    for imported, at in imports:
        section, cursor = None, 0
        for _ in range(2_000_000):
            if at + 8 > len(data):
                raise ValueError('Unterminated REL relocation stream')
            delta, kind, target_section, addend = struct.unpack_from('>HBBI', data, at)
            at += 8
            if kind == 203:  # R_DOLPHIN_END
                break
            if kind == 202:  # R_DOLPHIN_SECTION
                if target_section >= len(sections):
                    raise ValueError('Invalid REL source section')
                section, cursor = sections[target_section], 0
                continue
            if section is None:
                raise ValueError('REL relocation before source section')
            cursor += delta
            if cursor >= section['size']:
                raise ValueError('REL relocation exceeds source section')
            if kind in (0, 201):  # NONE / R_DOLPHIN_NOP
                continue
            relocation_count += 1
            if section['offset']:
                sites[section['offset']+cursor] = (imported, target_section, addend, kind)
            if imported == module:
                if target_section >= len(sections):
                    raise ValueError('Invalid REL target section')
                target = sections[target_section]
                # Inventory only in-section string objects. Other relocation
                # expressions are retained in the count, not treated as strings.
                if addend >= target['size']:
                    continue
                if target['offset'] and (found := strings.get(target['offset']+addend)) is not None:
                    found['references'].append(dict(source_section=section['index'], source_section_offset=cursor,
                        source_file_offset=section['offset']+cursor if section['offset'] else None, relocation_type=kind))
        else:
            raise ValueError('REL relocation budget exceeded')
    for row in strings.values():
        row['creation_callback_candidates'] = []
        for ref in row['references']:
            site = ref['source_file_offset']
            if site is None or ref['relocation_type'] != 6:
                continue
            # Observed retail registration pattern: class name into r4,
            # followed by a matching HA/LO executable callback pair into r5.
            for at in range((site & ~3)+4, (site & ~3)+0x30, 4):
                pair = sites.get(at+2)
                if not pair or pair[0] != module or pair[3] != 6 or word(at) >> 16 != 0x3CA0:
                    continue
                target = sections[pair[1]]
                if not target['executable'] or not target['offset'] or pair[2]+48 > target['size']:
                    continue
                low = next((later for later in range(at+4, at+20, 4)
                    if sites.get(later+2) == (pair[0], pair[1], pair[2], 4) and word(later) >> 16 == 0x38A5), None)
                if low is None:
                    continue
                callback = target['offset'] + pair[2]
                allocation = next((word(i) & 65535 for i in range(callback, callback+48, 4)
                    if word(i) >> 16 == 0x3860), None)
                row['creation_callback_candidates'].append(dict(section=pair[1], section_offset=pair[2],
                    file_offset=callback, registration_file_offset=at,
                    initial_li_r3_candidate=allocation, first_48_bytes=data[callback:callback+48].hex()))
    return dict(scope='Original unmodified REL metadata/references only; NOT loaded addresses, linked components or gameplay proof',
        module=module, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), sections=sections,
        imports=[m for m, _ in imports], relocations=relocation_count, strings=list(strings.values()),
        **({'relocation_sites': sites} if include_sites else {}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('--name', action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.module.read_bytes(), args.name)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    for row in result['strings']:
        print(f"{row['name']} original section{row['section']}+{row['section_offset']:X}: {len(row['references'])} relocation references")
