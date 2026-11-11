"""Inspect authored EM1 lighting after resolving entity/component inheritance.

Accepts original scenes or losslessly structurally converted scene fixtures.
This inventories source intent; it does not establish EM2 light compatibility.
"""
import argparse
import collections
import copy
import hashlib
import json
from pathlib import Path
import runpy
import struct


def audit(path):
    data = path.read_bytes()
    inspect = runpy.run_path(str(Path(__file__).with_name('em1_scene_upgrade.py')))['inspect']
    scene = inspect(data)
    entities = {}
    for name, ids, _, components in scene['entities']:
        identity, parent, flags = struct.unpack('>3I', bytes.fromhex(ids))
        if not identity or identity in entities:
            raise ValueError('Invalid or duplicate entity identity')
        entities[identity] = (name, parent, components)
    refs = [v[0] for v in struct.iter_unpack('>I', bytes.fromhex(scene['refs']))]
    if len(refs) != len(set(refs)):
        raise ValueError('Duplicate placed entity')

    def effective(identity):
        chain, seen = [], set()
        while identity:
            if identity in seen or identity not in entities or len(chain) >= 256:
                raise ValueError('Invalid entity inheritance')
            seen.add(identity)
            chain.append(entities[identity])
            identity = entities[identity][1]
        merged = []
        for _, _, components in reversed(chain):
            for klass, version, links, properties in components:
                cid, parent = struct.unpack('>2I', bytes.fromhex(links))
                matches = [c for c in merged if cid in c['lineage'] or (parent and parent in c['lineage'])]
                if len(matches) > 1:
                    raise ValueError('Ambiguous component inheritance')
                if matches:
                    c = matches[0]
                    if c['class'] != klass:
                        raise ValueError('Component ancestor changes class')
                    c['lineage'].add(cid)
                    c['version'] = version
                else:
                    if parent:
                        raise ValueError('Unresolved component ancestor')
                    c = dict(class_=klass, version=version, lineage={cid}, properties={})
                    c['class'] = c.pop('class_')
                    merged.append(c)
                for name, typ, mode, amount, value in properties:
                    c['properties'][name] = dict(type=typ, mode=mode, count=amount, raw=copy.deepcopy(value))
        return merged

    def values(properties):
        result = {}
        for name, p in properties.items():
            typ, raw = p['type'], p['raw']
            if typ == 'String':
                value = raw
            else:
                b = bytes.fromhex(raw)
                code = 'f' if typ in ('Float', 'Point2', 'Point3', 'Matrix3', 'Color (RGB)', 'Color (RGBA)') else 'i' if typ in ('Integer', 'Short') else 'I'
                value = [v[0] for v in struct.iter_unpack('>' + code, b)]
                if typ == 'Boolean':
                    value = [bool(v) for v in value]
            result[name] = value[0] if len(value) == 1 and not p['mode'] & 1 else value
        return result

    lights, relevant = [], collections.Counter()
    for identity in refs:
        components = effective(identity)
        by_class = collections.defaultdict(list)
        for c in components:
            by_class[c['class']].append(c)
            if 'light' in c['class'].lower() or c['class'] == 'DynamicallyLitComponent':
                relevant[c['class']] += 1
        if 'JPSLightComponent' not in by_class:
            continue
        row = dict(id=identity, name=entities[identity][0], components={})
        for klass in ('JPSTransformationComponent', 'JPSLightComponent', 'JPSLightBakingComponent'):
            selected = by_class[klass]
            if len(selected) > 1:
                raise ValueError('Multiple effective lighting/transform components')
            if selected:
                row['components'][klass] = dict(version=selected[0]['version'], properties=values(selected[0]['properties']))
        baking = row['components'].get('JPSLightBakingComponent', {}).get('properties', {})
        row['generate_runtime_light'] = baking.get('GenerateRuntimeLight')
        row['bake_light'] = baking.get('Bake Light')
        lights.append(row)
    return dict(source=str(path), sha256=hashlib.sha256(data).hexdigest(),
                placed_entities=len(refs), lighting_components=dict(relevant), lights=lights,
                runtime_light_count=sum(r['generate_runtime_light'] is True for r in lights),
                disabled_runtime_light_count=sum(r['generate_runtime_light'] is False for r in lights),
                unspecified_runtime_light_count=sum(r['generate_runtime_light'] is None for r in lights),
                scope='Authored source intent after inheritance resolution; native EM2 behavior remains unverified.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scene', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = audit(args.scene)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'lights'}, indent=2))
