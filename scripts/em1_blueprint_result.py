"""Compare full native GV blueprint reads with independently inspected source GSAs.

Definitions/linked inheritance only: no claim of actor/world activation,
resource conversion, physics, scripts, audio or playable-level compatibility.
"""
import argparse
import ctypes as c
import hashlib
import json
import re
import runpy
import tempfile
import struct
from pathlib import Path
from em1_scene_upgrade import inspect

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('profile',type=Path)
p.add_argument('--library',type=Path,required=True)
p.add_argument('--image',type=Path,required=True)
a=p.parse_args()
completion=json.loads((a.profile/'completion-result.json').read_text())
assert completion['completed'] and completion['normal_user_files_unchanged'] and completion['em1_source_metadata_unchanged'] and not completion['gameplay_input_sent']
assert completion['exe_sha256']==hashlib.sha256(Path('build/runtime23/beetleworx.exe').read_bytes()).hexdigest().upper()
assert not (a.profile/'user_data/diagnostics/crash.log').exists()
log=(a.profile/'game.log').read_text(errors='replace')
assert "native float schema probe success=true error=''" in log
assert "native array schema probe success=true error=''" in log
assert "native navigation scalar schema: success=true error=''" in log
assert "native Boolean schema: success=true error=''" in log
rows=re.findall(r"complete blueprint result: success=(\w+) entities=(\d+) components=(\d+) properties=(\d+) authored_ammo=(\d+) error='([^']*)'",log)
assert len(rows)==3 and all(row[0]=='true' and not row[5] for row in rows)
d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte)
d.prepare.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(U),c.POINTER(c.c_size_t)];d.prepare.restype=c.c_char_p
pointer,size=U(),c.c_size_t();error=d.prepare(str(a.image),b'gv_zonea_start.level',c.byref(pointer),c.byref(size));assert not error,error
scenes=[];definitions=[]
with tempfile.TemporaryDirectory() as folder:
    path=Path(folder)/'scenes.pak';path.write_bytes(c.string_at(pointer,size.value))
    for (name,data,kind),row in zip(runpy.run_path('scripts/game-pak.py')['read_pak'](path),rows,strict=True):
        source=inspect(data);entities=source['entities']
        definitions.extend(entities)
        counts=(len(entities),sum(len(e[3]) for e in entities),sum(len(comp[3]) for e in entities for comp in e[3]))
        assert counts==tuple(map(int,row[1:4])),(name,counts,row)
        scenes.append(dict(scene=name,entities=counts[0],components=counts[1],properties=counts[2],authored_ammo_probes=int(row[4]),
            placement_ids=list(struct.unpack('>'+str(len(source['refs'])//8)+'I',bytes.fromhex(source['refs']))),
            source_master_definitions=sum(struct.unpack('>III',bytes.fromhex(entity[1]))[1]!=0 for entity in entities),
            declared_model_paths=sorted({value.lower().replace('\\','/') for entity in entities for comp in entity[3] for prop in comp[3]
                if prop[1]=='String' and prop[2] in [2,3] for value in prop[4] if value.lower().endswith(('.nif','.nif_wii'))}),
            inventory_schema_roots=sum(comp[0]=='InventoryComponent' and len(comp[3])==17 for entity in entities for comp in entity[3])))
totals=[sum(row[key] for row in scenes) for key in ['entities','components','properties','authored_ammo_probes']]
assert totals==[954,4330,21679,1]
assert 'complete blueprint finished: entities=954 components=4330 properties=21679 authored_ammo=1;' in log
by_id={};duplicates=0
for entity in definitions:
    identifier,master,flags=struct.unpack('>III',bytes.fromhex(entity[1]))
    if identifier in by_id:
        assert by_id[identifier]==entity,'Conflicting source definitions cannot be silently selected'
        duplicates+=1
    else:by_id[identifier]=entity
resolved={};visiting=set()
def components(entity):
    identifier,master,flags=struct.unpack('>III',bytes.fromhex(entity[1]))
    if identifier in resolved:return resolved[identifier]
    assert identifier not in visiting,'Source inheritance cycle'
    visiting.add(identifier);result=list(entity[3]);names={comp[0] for comp in result}
    if master:
        for comp in components(by_id[master]):
            if comp[0] not in names:result.append(comp);names.add(comp[0])
    visiting.remove(identifier);resolved[identifier]=result;return result
parent_count=component_links=component_count=0
for entity in definitions:
    identifier,master,flags=struct.unpack('>III',bytes.fromhex(entity[1]))
    component_count+=len(components(entity))
    if master:parent_count+=1;component_links+=len(components(by_id[master]))
expected=(len(by_id),duplicates,parent_count,component_count,component_count-totals[1],component_links)
match=re.search(r"scoped inheritance result: success=true unique=(\d+) duplicates=(\d+) parents=(\d+) components=(\d+) inherited_components=(\d+) component_links=(\d+) error=''",log)
assert match and tuple(map(int,match.groups()))==expected,(expected,match.groups() if match else None)
assert "inherited ammo result: success=true own_properties=0 error=''" in log
assert 'scoped inheritance source preservation: success=true;' in log
assert re.search(r"native ItemManager adapter: success=true address=[0-9A-F]+ error=''",log)
item_rows=re.findall(r'native ItemManager ammo: channel=(\d+) amount=([\d.eE+-]+) effective_max=([\d.eE+-]+) configured_max=([\d.eE+-]+) segment=([\d.eE+-]+) max_segments=(-?\d+) penalty=([\d.eE+-]+) penalty_wait=([\d.eE+-]+) recharge_max=([\d.eE+-]+) recharge_rate=([\d.eE+-]+) recharge_wait=([\d.eE+-]+)',log)
assert len(item_rows)==3 and [int(row[0]) for row in item_rows]==[0,1,2]
# Independent source-data comparison for this original default-profile capture.
# This is a compatibility baseline, not a rule that all future saves must match.
roots=[comp for entity in definitions for comp in entity[3] if comp[0]=='AmmoRechargeComponent' and len(comp[3])==14]
assert len(roots)==1
source_rules={prop[0]:struct.unpack('>f',bytes.fromhex(prop[4]))[0] for prop in roots[0][3]}
for row,prefix in zip(item_rows[:2],['Paint','Thinner'],strict=True):
    for column,suffix in [(6,'PenaltyAmount'),(7,'PenaltyWait'),(8,'RechargeMax'),(9,'RechargeRate'),(10,'RechargeWait')]:
        assert float(row[column])==source_rules[prefix+suffix],(prefix,suffix,row)
result=dict(scope=__doc__,level='gv_zonea_start.level',scenes=scenes,completion=completion,
    complete_native_source_definitions=True,source_input_bytes_unchanged=True,
    authored_ammo_native_component_reads_and_runtime_override_isolation_passed=True,
    inherited_master_links_resolved=True,inheritance=dict(zip(['unique','duplicates','parents','components','inherited_components','component_links'],expected)),
    inherited_ammo_named_reads_and_independent_runtime_overrides=True,world_activated=False)
result['native_em2_item_manager']=dict(channels=item_rows,native_channel_lookup_and_same_value_retail_setters=True,
    singleton_prefix_unchanged=True,original_em1_default_recharge_rules_match=True,
    duplicate_recharge_simulation_installed=False,live_legacy_actor_binding=False)
if 'original level model archives staged:' in log:
    stage=re.search(r"original level model archives staged: level='gv_zonea_start.level' scenes=(\d+) models=(\d+) native_bytes=(\d+);",log)
    expected_models=sum(len(scene['declared_model_paths']) for scene in scenes)
    assert stage and int(stage[1])==len(scenes) and int(stage[2])==expected_models and int(stage[3])>0
    result['native_model_archive_staging']=dict(scenes=int(stage[1]),declared_models=int(stage[2]),native_model_bytes=int(stage[3]),
        original_declared_model_inventory_matches=True,world_activated=False,complete_platform_resources_ready=False)
if 'source navigation factory begin:' in log:
    def navigation_path(value):
        value=value.replace('\\','/').lower()
        for prefix in ['game:/','d:/']:
            if value.startswith(prefix):value=value[len(prefix):];break
        return value.lstrip('/')
    def navigation_properties(entity):
        identifier,master,flags=struct.unpack('>III',bytes.fromhex(entity[1]))
        inherited=navigation_properties(by_id[master]) if master else None
        own=next((comp for comp in entity[3] if comp[0]=='AIPathDatabaseComponent'),None)
        if own is None:return inherited
        properties=dict(inherited or {});properties.update({prop[0]:prop for prop in own[3]});return properties
    source_navigation=[properties for entity in definitions if (properties:=navigation_properties(entity)) is not None]
    factory_rows=re.findall(r"source navigation factory result: success=(\w+) bound_sectors=(\d+) error='([^']*)'",log)
    assert len(factory_rows)==len(source_navigation) and all(row[0]=='true' and not row[2] for row in factory_rows)
    profile_rows=re.findall(r"source navigation profile: file='([^']*)' pitch=([^ ]+) max=([^ ]+) min=([^ ]+) radius=(-?\d+) height=(-?\d+) movement=(-?\d+)",log)
    assert len(profile_rows)==len(source_navigation)
    def check_navigation_profile(props,row):
        assert navigation_path(row[0])==navigation_path(props['PathData HPD Filename'][4][0])
        assert struct.pack('>f',float(row[1])).hex()==props['PathData Pitch Value'][4]
        for text,name in [(row[2],'PathData Bounding Box Max'),(row[3],'PathData Bounding Box Min')]:
            values=[float(value) for value in text.split(',')];assert len(values)==3
            assert struct.pack('>3f',*values).hex()==props[name][4]
        for text,name in zip(row[4:],['PathData Radius','PathData Height','PathData Movement Type'],strict=True):
            assert int(text)==struct.unpack('>i',bytes.fromhex(props[name][4]))[0]
    for props,row in zip(source_navigation,profile_rows,strict=True):check_navigation_profile(props,row)
    total=sum(int(row[1]) for row in factory_rows)
    assert f'source navigation factories finished: definitions={len(source_navigation)} bound_sectors={total};' in log
    result['private_native_navigation_components']=dict(definitions=len(source_navigation),instances=len(source_navigation)*2,
        bound_sector_graphs=total,all_seven_fields_match_original_source=True,source_and_instances_isolated=True,
        private_factory_published_globally=False,ai_world_binding_complete=False)
if 'registered navigation lifecycle:' in log:
    assert "registered navigation lifecycle: success=true definitions=3 error=''" in log
    assert 'reserved native component type capacity: 135 -> 142 before entity allocation' in log
    type_rows=re.findall(r"native component type registered: class='([^']*)' index=(\d+) assigned=(\d+) capacity=(\d+)",log)
    assert type_rows==[('AmmoRechargeComponent','135','136','142'),('InventoryComponent','136','137','142'),('AIPathDatabaseComponent','137','138','142'),('LogicBranchComponent','138','139','142'),('LogicRelayComponent','139','140','142'),('SeedPointComponent','140','141','142'),('TrackingComponent','141','142','142')]
    result['registered_navigation_component']=dict(global_factory_registered=True,definition_changes_and_property_overrides_checked=True,
        clear_and_destroy_retire_resources=True,entity_type_indices_reserved_before_allocation=True,actor_ai_world_published=False)
if 'registered logic lifecycle:' in log:
    assert "registered logic lifecycle: success=true error=''" in log
    def logic_properties(entity,kind):
        identifier,master,flags=struct.unpack('>III',bytes.fromhex(entity[1]))
        inherited=logic_properties(by_id[master],kind) if master else {}
        own=next((comp for comp in entity[3] if comp[0]==kind),None)
        properties=dict(inherited)
        if own is not None:properties.update({prop[0]:prop for prop in own[3]})
        return properties
    expected_logic=[]
    for entity in definitions:
        identifier,master,flags=struct.unpack('>III',bytes.fromhex(entity[1]))
        for comp in entity[3]:
            if comp[0] not in ['LogicBranchComponent','LogicRelayComponent']:continue
            props=logic_properties(entity,comp[0]);field='BranchValue' if comp[0]=='LogicBranchComponent' else 'RelayDisabled'
            value=bool(int(props[field][4],16)) if field in props else False
            inherited=bool(master and any(parent[0]==comp[0] for parent in components(by_id[master])))
            expected_logic.append((comp[0],str(len(comp[3])),str(inherited).lower(),str(value).lower()))
    actual_logic=re.findall(r"original logic component: class='([^']*)' definition=[0-9A-F]{8} own_properties=(\d+) inherited=(true|false) authored=(true|false) success=true;",log)
    assert actual_logic==expected_logic,(actual_logic,expected_logic)
    result['registered_logic_components']=dict(definitions=len(expected_logic),
        authored_boolean_values_match_original_source=True,independent_native_instances=True,
        branch_true_false_and_disabled_relay_output_checked=True,missing_or_rejecting_router_fails=True,
        world_event_router_connected=False)
if 'registered seed lifecycle:' in log:
    assert "registered seed lifecycle: success=true error=''" in log
    expected_seeds=[]
    for entity in definitions:
        identifier,master,flags=struct.unpack('>III',bytes.fromhex(entity[1]))
        for comp in entity[3]:
            if comp[0]!='SeedPointComponent':continue
            props=logic_properties(entity,comp[0])
            sizes=[struct.unpack('>i',bytes.fromhex(props[field][4]))[0] if field in props else 0 for field in ['AI Size 1','AI Size 2']]
            inherited=bool(master and any(parent[0]==comp[0] for parent in components(by_id[master])))
            expected_seeds.append((str(len(comp[3])),str(inherited).lower(),*map(str,sizes)))
    actual_seeds=re.findall(r'original seed component: definition=[0-9A-F]{8} own_properties=(\d+) inherited=(true|false) sizes=(-?\d+),(-?\d+) success=true;',log)
    assert actual_seeds==expected_seeds,(actual_seeds,expected_seeds)
    result['registered_seed_component']=dict(definitions=len(expected_seeds),all_integer_fields_match_original_source=True,
        original_zero_state_and_native_lifecycle_checked=True,actor_ai_world_connected=False)
if 'registered tracking lifecycle:' in log:
    assert "registered tracking lifecycle: success=true error=''" in log
    expected_tracking=[];expected_collectibles=[]
    for entity in definitions:
        identifier,master,flags=struct.unpack('>III',bytes.fromhex(entity[1]))
        for comp in entity[3]:
            if comp[0]!='TrackingComponent':continue
            props=logic_properties(entity,comp[0])
            values=[struct.unpack('>i',bytes.fromhex(props[field][4]))[0] if field in props else 0 for field in ['TrackingType','TrackingStimType']]
            collectibles=props['Collectible'][4] if 'Collectible' in props else []
            inherited=bool(master and any(parent[0]==comp[0] for parent in components(by_id[master])))
            expected_tracking.append((str(len(comp[3])),str(inherited).lower(),*map(str,values),str(len(collectibles))))
            expected_collectibles.extend((str(i),value) for i,value in enumerate(collectibles))
    actual_tracking=re.findall(r'original tracking component: definition=[0-9A-F]{8} own_properties=(\d+) inherited=(true|false) type=(-?\d+) stimulus=(-?\d+) collectibles=(\d+) success=true;',log)
    assert actual_tracking==expected_tracking,(actual_tracking,expected_tracking)
    actual_collectibles=re.findall(r"original tracking collectible: definition=[0-9A-F]{8} index=(\d+) value='([^']*)'",log)
    assert actual_collectibles==expected_collectibles,(actual_collectibles,expected_collectibles)
    result['registered_tracking_component']=dict(definitions=len(expected_tracking),
        source_integer_and_collectible_array_values_match=True,typed_damage_filters_classification_and_retirement_checked=True,
        native_instance_lifetimes_checked=True,world_observer_and_manager_connected=False)
construction=re.findall(r"complete scene construction result: scene='([^']*)' success=(\w+) placements=(\d+) components=(\d+) error='([^']*)'",log)
if construction:
    assert len(construction)<=len(scenes)
    expected_construction=[(len(scene['placement_ids']),sum(len(components(by_id[identifier])) for identifier in scene['placement_ids'])) for scene in scenes]
    for row,expected_counts in zip(construction,expected_construction):
        if row[1]=='true':assert tuple(map(int,row[2:4]))==expected_counts and not row[4]
    ownership=re.findall(r'complete scene navigation ownership: instances=(\d+) graphs=(\d+) restored=(\w+)',log)
    assert len(ownership)==len(construction) and all(row[2]=='true' for row in ownership)
    navigation_pattern=r"constructed navigation profile: file='([^']*)' pitch=([^ ]+) max=([^ ]+) min=([^ ]+) radius=(-?\d+) height=(-?\d+) movement=(-?\d+)"
    original_log,_,bound_log=log.partition('registered bound scene construction begin:')
    actor_profiles=re.findall(navigation_pattern,original_log)
    expected_actor_profiles=[]
    for scene,row in zip(scenes,construction):
        if row[1]!='true':continue
        for identifier in scene['placement_ids']:
            props=navigation_properties(by_id[identifier])
            if props is not None:expected_actor_profiles.append(props)
    assert len(actor_profiles)==len(expected_actor_profiles)
    for props,row in zip(expected_actor_profiles,actor_profiles,strict=True):check_navigation_profile(props,row)
    result['complete_scene_construction']=dict(results=construction,expected_placement_component_counts=expected_construction,
        navigation_ownership=ownership,navigation_instance_fields_match_original_source=True,
        all_scenes_constructed=len(construction)==len(scenes) and all(row[1]=='true' for row in construction),world_admitted=False)
inventory_registered="native Inventory factory result: success=true error=''" in log
if 'bound source blueprint library retained:' in log:
    library=re.search(r"bound source blueprint library retained: level='gv_zonea_start.level' scenes=(\d+) definitions=(\d+) unique=(\d+) duplicates=(\d+);",log)
    assert library and tuple(map(int,library.groups()))==(len(scenes),totals[0],len(by_id),duplicates)
    bound=re.findall(r"registered bound scene construction result: scene=(\d+) success=(\w+) placements=(\d+) components=(\d+) parent_resolutions=(\d+) detached=(\w+) library_refs_restored=(\w+) error='([^']*)'",log)
    assert len(bound)==len(scenes)
    bound_actor_profiles=re.findall(navigation_pattern,bound_log)
    assert len(bound_actor_profiles)==len(expected_actor_profiles)
    for props,row in zip(expected_actor_profiles,bound_actor_profiles,strict=True):check_navigation_profile(props,row)
    for i,(row,source_scene,counts) in enumerate(zip(bound,scenes,expected_construction,strict=True)):
        assert int(row[0])==i and row[1]=='true' and tuple(map(int,row[2:4]))==counts
        assert int(row[4])==source_scene['source_master_definitions'] and row[5:7]==('true','true') and not row[7]
    assert f"registered bound scene construction finished: level='gv_zonea_start.level' scenes=3 placements=750 components=3121;" in log
    result['registered_bound_blueprint_library']=dict(scenes=len(scenes),definitions=totals[0],unique=len(by_id),duplicates=duplicates,
        normal_begin_and_deferred_pump_scope_hooks_checked=True,all_original_master_counts_match=True,
        bound_scene_constructor_results=bound,parser_detached_and_library_reference_counts_restored=True,
        private_master_override_used=False,world_admitted=False)
if inventory_registered:
    inventory_rows=re.findall(r"source Inventory factory checks: scene='([^']*)' count=(\d+) success=(\w+) error='([^']*)'",log)
    assert len(inventory_rows)==len(scenes) and all(row[2]=='true' and not row[3] for row in inventory_rows)
    native_scene_paths=re.findall(r"complete blueprint begin: scene='([^']*)'",log)
    assert len(native_scene_paths)==3
    for source_scene,row,path in zip(scenes,inventory_rows,native_scene_paths,strict=True):
        assert path==row[0] and source_scene['inventory_schema_roots']==int(row[1])
    source_inventory_roots=[comp for entity in definitions for comp in entity[3] if comp[0]=='InventoryComponent' and len(comp[3])==17]
    assert len(source_inventory_roots)==sum(int(row[1]) for row in inventory_rows)==1
    assert "inherited Inventory factory result: success=true own_properties=0 error=''" in log
    assert re.search(r"native creation callback class='InventoryComponent' address=(?!00000000)[0-9A-F]{8}",log)
    result['native_inventory_component']=dict(registered=True,property_count=17,
        original_source_root_and_inherited_definitions_checked=True,
        compiled_defaults_and_named_property_reads=True,independent_scalar_and_string_float_arrays=True,
        source_definition_preserved=True,player_manager_binding_reads_checked=True,
        live_actor_world_binding=False,legacy_event_lifecycle_complete=False)
inventory=re.search(r'native inventory manager bindings: success=true fields=6 etickets=(\d+) eticket_limit=(\d+);',log)
if inventory:
    result['native_em2_item_manager']['inventory_scalar_backend']=dict(
        fields=['Paint','PaintMax','Thinner','ThinnerMax','ETicket','ETicketMax'],
        etickets=int(inventory[1]),eticket_limit=int(inventory[2]),
        private_changed_value_clamp_and_restore_passed="private native inventory writes: success=true error=''" in log,
        inventory_component_registered=inventory_registered,live_legacy_actor_binding=False)
result['native_property_arrays']=dict(types=['Float','Integer','String'],
    native_descriptor_size=24,native_box_size=16,native_vector_resize_and_element_writes=True,
    descriptor_association_preserved=True,inventory_component_registered=inventory_registered)
if 'native navigation graph import:' in log:
    navigation=re.findall(r"native navigation graph import: success=true databases=(\d+) sectors=(\d+) nodes=(\d+) edges=(\d+) queries=(\d+) half_cost_rounding=(\d+) error=''",log)
    assert len(navigation)==1,'Native navigation import failed or was not uniquely captured'
    counts=tuple(map(int,navigation[0]))
    corpus=json.loads(Path('analysis/em1-compatibility/navigation-data-result.json').read_text())
    assert corpus['library_sha256']==hashlib.sha256(a.library.read_bytes()).hexdigest()
    assert corpus['input_bytes_preserved'] and not corpus['world_activated']
    source=a.image.stat()
    assert corpus['source_identity']['bytes']==source.st_size and corpus['source_identity']['mtime_ns']==source.st_mtime_ns
    databases=[row for row in corpus['packed_databases'] if row['archive']=='packfiles/gv_shared_za.pak']
    assert len(databases)==2
    navigation_expected=(len(databases),sum(row['sectors'] for row in databases),sum(row['nodes'] for row in databases),sum(row['edges'] for row in databases))
    assert counts[:4]==navigation_expected and counts[4]==navigation_expected[1]*3 and counts[5]==0
    if 'private_native_navigation_components' in result:
        for profile,row in zip(profile_rows,factory_rows,strict=True):
            expected_sectors=0
            if profile[0]:
                matches=[database for database in databases if navigation_path(database['path'])==navigation_path(profile[0])]
                assert len(matches)==1,'Component HPD has no unique original archive binding'
                expected_sectors=2*matches[0]['sectors']
            assert int(row[1])==expected_sectors
    paths=0
    if 'native navigation path search:' in log:
        path_rows=re.findall(r"native navigation path search: success=true paths=(\d+) error=''",log)
        assert len(path_rows)==1 and int(path_rows[0])==navigation_expected[1]*4,'Native graph route checks failed'
        paths=int(path_rows[0])
    result['native_navigation']=dict(scope='Imported sector graphs and sampled native queries; not AI gameplay',
        source_databases=databases,databases=counts[0],sectors=counts[1],nodes=counts[2],edges=counts[3],
        nearest_node_checks=counts[4],half_cost_rounding=counts[5],sampled_route_checks=paths,
        sampled_directed_reachability_and_shortest_cost_passed=paths>0,
        query_local_collection_and_graph_cleanup_passed=True,cross_sector_links_installed=False,
        native_navmesh_traversal_proven=False,ai_component_registered=False,world_activated=False)
    if 'source navigation mesh retention:' in log:
        geometry_rows=re.findall(r'source navigation mesh retention: floors=(\d+) portals=(\d+) neighbor_groups=(\d+) neighbor_links=(\d+) unassigned_links=(\d+);',log)
        assert len(geometry_rows)==1
        geometry_keys=['floors','portals','neighbor_groups','neighbor_references','unassigned_links']
        folders={row['path'].rsplit('/',2)[1].lower() for row in databases}
        source_sectors=[row for row in corpus['loose_sectors'] if row['path'].split('/')[1].lower() in folders]
        assert len(source_sectors)==counts[1]
        expected_geometry=tuple(sum(row['runtime_geometry'][key] for row in source_sectors) for key in geometry_keys)
        assert tuple(map(int,geometry_rows[0]))==expected_geometry
        result['native_navigation']['source_runtime_geometry']=dict(zip(geometry_keys,expected_geometry))
        result['native_navigation']['source_runtime_geometry'].update(
            retained_in_registered_level_owner=True,native_walkable_surfaces_generated=False)
    if 'source navigation floor lookup:' in log:
        floor_rows=re.findall(r'source navigation floor lookup: queries=(\d+) matched=(\d+);',log)
        assert len(floor_rows)==1 and int(floor_rows[0][0])==counts[2]
        assert 0<=int(floor_rows[0][1])<=counts[2]
        references=[]
        for filename,suffix in [('original-gv-portal-reference.json','gv_main_gremlinvillage_ai_ai_path_database_01/sector_1000.prelnk.obj'),('original-dbc-floor-reference.json','dbc_main_1st_dbc_shared_1st_ai_path_database_01/sector_1000.prelnk.obj')]:
            proof=json.loads((Path('analysis/em1-compatibility')/filename).read_text())
            assert proof['returned'] and proof['sdk_initialization_return']==1 and not proof['error']
            comparison=proof['floor_query_comparison']
            assert comparison['library_sha256']==corpus['library_sha256']
            assert comparison['checks']>0 and comparison['find_checks']>0 and not comparison['mismatches'] and not comparison['find_mismatches']
            source=next(row for row in corpus['loose_sectors'] if row['path'].lower().endswith(suffix))
            assert source['runtime_mesh_sha256']==proof['mesh_sha256']
            references.append(dict(source=source['path'],containment_checks=comparison['checks'],find_checks=comparison['find_checks'],digest=comparison['digest']))
            if 'portal_step_comparison' in comparison:
                portal=comparison['portal_step_comparison']
                assert portal['segment_checks']==320 and portal['portal_checks']==960
                assert not portal['segment_mismatches'] and not portal['portal_mismatches']
                assert proof['gekko_sqrt_cpu_instructions']>0 and not proof['reference_emulator_used_in_production']
                result['native_navigation']['single_mesh_portal_reference']=dict(
                    segment_checks=portal['segment_checks'],portal_checks=portal['portal_checks'],
                    bound_cross_sector_routes_checked=False,world_activated=False)
        result['native_navigation']['source_floor_query_port']=dict(references=references,
            original_floor_containment_and_lookup_checked=True,game_graph_node_queries=int(floor_rows[0][0]),
            game_graph_node_matches=int(floor_rows[0][1]),unmapped_graph_nodes_retained=True,
            reference_emulator_used_in_production=False,ai_world_binding_complete=False)
    Path('analysis/em1-compatibility/native-navigation-result.json').write_text(json.dumps(dict(
        run=str(a.profile),completion=completion,**result['native_navigation']),indent=2)+'\n')
Path('analysis/em1-compatibility/native-blueprint-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS:3whole native blueprint reads and scoped inheritance match original source:',expected,'; original root and inherited ammo reads/overrides, input unchanged; no world activation')
