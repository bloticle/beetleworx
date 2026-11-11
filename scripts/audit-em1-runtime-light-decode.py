"""Compare native RVZ light decoding against the independent scene-property audit."""
import argparse
import json
from pathlib import Path
import struct


def audit(source, native):
    reference=json.loads(source.read_text())
    expected={}
    for row in reference['lights']:
        if not row['generate_runtime_light']:
            continue
        c=row['components']
        light=c['JPSLightComponent']['properties']
        bake=c['JPSLightBakingComponent']['properties']
        transform=c['JPSTransformationComponent']['properties']
        values=transform['Translation']+[transform['Rotation'][i] for i in (2,5,8)]
        values+=light['Color (Ambient)']+light['Color (Diffuse)']
        values += [light[key] for key in ('Attenuation (Constant)','Attenuation (Linear)','Attenuation (Quadratic)','Dimmer')]
        values += [bake['Min Falloff Range'],bake['Max Falloff Range']]
        values += [light[key] for key in ('Spot Angle (Inner)','Spot Angle (Outer)','Spot Exponent')]
        expected[row['id']]=[['Ambient','Directional','Point','Spot'].index(light['Light Type']),
            int(bake['LightIsSubtractive']),int(bake['RuntimeLightRayCast'])]+[struct.pack('>f',v).hex() for v in values]
    actual={}
    for line in native.read_text().splitlines():
        fields=line.split('\t')
        if len(fields)!=25:
            raise ValueError('Incomplete native light record')
        identity=int(fields[0])
        if identity in actual:
            raise ValueError('Duplicate native light identity')
        actual[identity]=list(map(int,fields[1:4]))+fields[4:]
    if not expected or actual!=expected:
        raise ValueError('Native light records differ from authored scene properties')
    if list(actual)!=list(expected):
        raise ValueError('Native lights changed authored placement order')
    return dict(runtime_lights=len(actual),float_fields_bit_exact=sum(len(v)-3 for v in actual.values()),
                identity_kind_subtractive_and_raycast_exact=True,
                baked_only_lights_not_admitted=reference['disabled_runtime_light_count'],
                source=str(source),native=str(native),render_binding='Not verified by this test')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('native',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=audit(a.source,a.native)
    a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(f"PASS: {r['runtime_lights']} lights; {r['float_fields_bit_exact']} float fields bit-exact; baked-only lights excluded")
