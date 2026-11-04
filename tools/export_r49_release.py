"""Export the version-bound R49 source evidence; never use main source50 as input.

The default action is a read-only verification. --export writes only the isolated
release/r49/source and release/r49/dependencies source locks. Models, recordings,
generated audio/weight arrays and private configuration are excluded, regardless
of their file extension. Parent archives remain private verification inputs.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
AUDIT=ROOT/'private/r49-source-delivery-20261104'
STAGE=ROOT/'private/microcrystal-r49-20261103/stage'
OUT=ROOT/'release/r49'
EXPECTED_PINS='8028ff80669b855e017ee73b17ee9b6856d508dd8ee7db1249597bbce47a88f2'
EXPECTED_KERNEL='2c41598a36403d4a41d6dfacd762b3ead83662d078e7d0679f031c10adb5bfc3'
EXPECTED_IMAGE='3479d1bd521ff143e44b6f2758558826bda6bf2f3148d1bfba21af7258c8689e'

def sha(blob):return hashlib.sha256(blob).hexdigest()
def read_json(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def json_file(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def known_values():
    values=[];checked=0
    def walk(v,key=''):
        if isinstance(v,dict):
            for k,x in v.items():walk(x,k)
        elif isinstance(v,list):
            for x in v:walk(x,key)
        elif isinstance(v,str) and len(v)>=4 and (key=='redaction_values' or re.search(r'password|credential|proof|token|secret|api.?key',key,re.I)):
            values.append(v.encode())
    for p in [ROOT.parent/'本地私密配置/wireless-auth.json',ROOT/'private/gimbal-device-auth.cloud.json',AUDIT/'private-secret-extension.json']:
        if not p.is_file():raise ValueError('Required private redaction configuration unavailable; export refused')
        item=read_json(p);before=len(values);walk(item)
        if len(values)==before:raise ValueError('Private redaction configuration has no usable known values; export refused')
        checked+=1
    if checked!=3 or not values:raise ValueError('Known-value privacy preflight unavailable')
    return values

def exclusion(path,blob,secrets):
    n=path.name.lower()
    if n in {'vv_microcrystal_target_private.h','k7cloud_auth_private.h','business_cloud_boot_config.inc','real_shuiguang_business_boot_config.inc'}:return 'private_configuration'
    if any(x in n for x in ['asset','weights','weight_data','model_data','voice_prompts','recorded_reference','recorded_mic','recorded_residual']):return 'model_audio_or_recorded_asset'
    if path.suffix.lower() in {'.bin','.onnx','.ort','.wav','.pcm','.jpg','.jpeg','.png','.mp4','.a','.o','.tgz'}:return 'binary_or_archive_not_source'
    if any(s in blob for s in secrets):return 'known_private_value'
    text=blob.decode('utf-8',errors='replace')
    if re.search(r'^\s*(?:(?:static|const)\s+)*(?:char|unsigned\s+char)\s+[\*\s]*\w*(?:credential|password|access_token|api_key|private_key|business_proof)\w*\s*(?:\[[^\]]*\])?\s*=\s*"[^"\n]{4,}"',text,re.I|re.M):return 'private_configuration_literal'
    numbers=re.findall(r'(?<!\w)(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?[fFuUlL]*)(?=\s*[,}])',text)
    if len(numbers)>2048 and (path.suffix.lower()=='.inc' or re.search(r'\b(?:static|const|unsigned\s+char|uint8_t|int8_t|int16_t|float)\b[^;=]{0,180}\[[^\]]*\]\s*=',text)):return 'large_generated_literal_array'
    return None

def project_path(original):
    s=original.replace('\\','/')
    if '/apps/examples/' in s:return 'app/'+s.split('/apps/examples/',1)[1]
    if '/app/' in s:return 'app/'+s.split('/app/',1)[1]
    if '/port/' in s:return 'port/'+s.split('/port/',1)[1]
    if '/nuttx/' in s:return 'port/nuttx/'+s.split('/nuttx/',1)[1]
    name=Path(s).name
    if name.startswith(('k7boot','native_boot_')):return 'app/k7boot/'+name
    if name=='k7_yunet.c':return 'app/k7host/'+name
    return 'retained-unmapped/'+name

def mapping(project):
    if project.startswith('app/'):return 'apps/examples/'+project[4:]
    if project.startswith('board/'):return 'nuttx/boards/arm64/rk3576/'+project[6:]
    if project.startswith('port/nuttx/'):return 'nuttx/'+project[len('port/nuttx/'):]
    return None

def verify():
    manifest=OUT/'source/source-files.json'
    if not manifest.is_file():return {'passed':False,'reason':'release_not_exported'}
    rows=read_json(manifest)['files'];problems=[]
    for r in rows:
        p=OUT/'source'/r['path']
        if not p.is_file() or sha(p.read_bytes())!=r['sha256']:problems.append(r['path'])
    expected={r['path'] for r in rows}|{'source-files.json'}
    problems.extend(str(p.relative_to(OUT/'source')) for p in (OUT/'source').rglob('*') if p.is_file() and p.relative_to(OUT/'source').as_posix() not in expected)
    return {'passed':not problems,'read_only':True,'files':len(rows),'problems':problems,'clean_build_proven':False}

def export():
    pins_path=STAGE/'pins.json'
    if sha(pins_path.read_bytes())!=EXPECTED_PINS:raise ValueError('R49 source pins identity mismatch')
    pins=read_json(pins_path)
    if any(sha((STAGE/r).read_bytes())!=h for r,h in pins.items()):raise ValueError('Frozen source content changed')
    actual={p.relative_to(STAGE).as_posix() for p in STAGE.rglob('*') if p.is_file()}
    if actual!=set(pins)|{'pins.json'}:raise ValueError('Unknown or missing file in frozen R49 stage; export refused')
    secrets=known_values();files=[];excluded=[];seen={};map_rows=[]
    def add(source,relative,role,provenance,equivalent=False):
        blob=source.read_bytes();reason=exclusion(source,blob,secrets)
        if reason:
            excluded.append({'input_role':role,'file':source.name,'release_interface_path':Path(relative).as_posix(),'reason':reason,'sha256':sha(blob),'bytes':len(blob)})
            return None
        rel=Path(relative).as_posix()
        if rel in seen:
            if seen[rel]==sha(blob):
                if equivalent:
                    for old in files:
                        if old['path']==rel:old['object_equivalence_verified']=True
                return rel
            rel='retained-variants/'+sha(blob)[:12]+'/'+Path(relative).as_posix()
        target=OUT/'source'/rel
        if target.exists() and target.read_bytes()!=blob:raise ValueError('Refuse unknown release source replacement: '+rel)
        target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(blob)
        seen[rel]=sha(blob)
        files.append({'path':rel,'sha256':sha(blob),'bytes':len(blob),'role':role,'provenance':provenance,'frozen_pin_identity_verified':source.is_relative_to(STAGE),'object_equivalence_verified':equivalent})
        return rel
    # Highest precedence is the actual R49 frozen overlay, not the active main tree.
    exact_replay_path=AUDIT/'r3-member-correspondence.json'
    replay_members=read_json(exact_replay_path)['rows'] if exact_replay_path.is_file() else []
    exact_replay_names={x['member'] for x in replay_members if x['full_object_bytes_equal']}
    compiled_final_names={'vv_native_runtime.c','vv_native_platform.c','vv_native_service.c','vv_microcrystal.c','vv_microcrystal_nuttx.c','vv_aliyun_provider.c','vv_near_experiment.c','echo_diag_runtime.c','bt_hcicore.c'}
    for p in sorted((STAGE/'after').rglob('*')):
        if not p.is_file():continue
        rel=p.relative_to(STAGE/'after').as_posix()
        if rel.startswith(('app/','port/')):
            equivalent=p.name in compiled_final_names and p.name+'.o' in exact_replay_names
            result=add(p,'project/'+rel,'r49_frozen_overlay','R49 stage pin; only explicitly rebuilt final C objects carry object equivalence',equivalent)
            if result:map_rows.append({'source':result,'sdk_relative_path':mapping(rel),'project_relative_path':rel})
        else:add(p,'frozen-closures/'+p.relative_to(STAGE).as_posix(),'r49_header_closure','R49 stage pin')
    for folder in ['echo-private-deps','near-private-deps']:
        for p in sorted((STAGE/folder).rglob('*')):
            if p.is_file():add(p,'frozen-closures/'+p.relative_to(STAGE).as_posix(),'r49_private_header_closure','R49 stage pin')
    for p in sorted(STAGE.glob('baseline-*.c')):add(p,'baselines/'+p.name,'comparison_baseline','R49 baseline/object checks')
    # Retained sources are a separate evidence class unless recompiled and compared.
    units=read_json(AUDIT/'parent-dwarf-units.json')
    proof_path=AUDIT/'retained-source-equivalence-r3.json'
    proofs={(x['archive'],x['member']):x for x in read_json(proof_path)['rows']} if proof_path.is_file() else {}
    object_rows=[]
    for u in units:
        row={'archive':u['archive'],'member':u['member'],'source_basename':Path(u.get('name','')).name,'original_path_present_at_capture':u['source_present'],'source_rebuild_equivalent':False}
        if u['source_present']:
            p=AUDIT/'sdk-parent-source-capture'/u['source_path'].lstrip('/')
            if p.is_file():
                if sha(p.read_bytes())!=u['source_sha256']:raise ValueError('Captured retained source changed')
                q=proofs.get((u['archive'],u['member']),{})
                equivalent=bool(q.get('allocated_equal') and q.get('disassembly_relocations_equal') and q.get('allocated_relocations_equal'))
                rel=project_path(u['source_path'])
                destination='project/'+rel if equivalent else 'unverified-snapshots/'+u['archive']+'/'+u['member']+'/'+p.name
                result=add(p,destination,'retained_source_snapshot' if equivalent else 'unverified_original_path_snapshot','Original parent-object DWARF path; current captured source bytes are not assumed to be the historical original',equivalent)
                row.update(release_source=result,source_rebuild_equivalent=equivalent)
                if result and result.startswith('project/'):map_rows.append({'source':result,'sdk_relative_path':mapping(rel),'project_relative_path':rel})
        object_rows.append(row)
    diag=AUDIT/'diagnostic-source-correspondence.json'
    if diag.is_file():
        for x in read_json(diag)['objects']:
            p=Path(x['source'])
            if sha(p.read_bytes())!=x['source_sha256']:raise ValueError('Historical diagnostic source changed')
            rel=add(p,'project/app/k7agent/cloud/src/'+p.name,'historical_source_object_correspondence','Source SHA and complete original object SHA match historical native receipt',True)
            for row in object_rows:
                if row['member']==x['member']:row.update(release_source=rel,historical_full_object_correspondence=True)
            for header in sorted(p.parent.glob('*.h')):
                digest=sha(header.read_bytes())
                if digest in x['dependencies'].values():add(header,'historical-closures/diagnostics/'+p.stem+'/'+header.name,'historical_diagnostic_header','Historical exact dependency SHA')
    for component in ['cloud','sound','persistent']:
        proof_file=AUDIT/(component+'-source-equivalence.json')
        if not proof_file.is_file():continue
        c=read_json(proof_file)
        if not c['all_ALLOC_and_disassembly_and_ALLOC_relocations_equal']:raise ValueError('Cloud source correspondence is incomplete')
        for x in c['rows']:
            p=ROOT/x['prepared_source_path']
            if sha(p.read_bytes())!=x['source_sha256']:raise ValueError('Component source proof changed')
            rel=project_path(x['old_DWARF_source_path'])
            if rel.startswith('retained-unmapped/'):
                group='k7boot' if x['archive']=='parent-boot.a' else 'k7host' if x['archive']=='parent-host.a' else 'k7sound' if x['archive']=='parent-sound.a' else 'k7agent/cloud/src';rel='app/'+group+'/'+p.name
            result=add(p,'project/'+rel,'verified_retained_source','Full historical object identity plus actual isolated ALLOC/code/relocations recompilation',True)
            if result and result.startswith('project/'):map_rows.append({'source':result,'sdk_relative_path':mapping(rel),'project_relative_path':rel})
            for row in object_rows:
                if (row['archive'],row['member'])==(x['archive'],x['member']):row.update(release_source=result,source_rebuild_equivalent=True,historical_full_object_correspondence=True)
        header_index=read_json(AUDIT/(component+'-source-equivalence/source-header-index.json'))
        for h,row in header_index['contents'].items():
            p=ROOT/row['local_paths'][0]
            if sha(p.read_bytes())!=h:raise ValueError('Component header closure changed')
            add(p,'historical-closures/'+component+'/'+h[:16]+'/'+p.name,'verified_historical_per_TU_closure','Actual historical used header/source hash; keep per-TU include domains separate')
    remaining=AUDIT/'remaining-source-equivalence.json'
    if remaining.is_file():
        c=read_json(remaining)
        if not c.get('input_content_stable'):raise ValueError('Last source inputs are not stable')
        for x in c['rows']:
            p=ROOT/x['prepared_source_path']
            if sha(p.read_bytes())!=x['source_sha256']:raise ValueError('Remaining source input changed')
            row=next(r for r in object_rows if (r['archive'],r['member'])==(x['archive'],x['member']))
            result=row.get('release_source')
            if not result or sha((OUT/'source'/result).read_bytes())!=x['source_sha256']:raise ValueError('Stable source differs from existing captured bytes; refuse implicit replacement')
            equivalent=x.get('equivalent') is True
            row.update(source_rebuild_equivalent=equivalent,original_argv_available=True,single_attempt_pending=not equivalent,comparison_evidence='remaining-source-equivalence.json')
            for f in files:
                if f['path']==result:
                    f['object_equivalence_verified']=equivalent
                    f['role']='verified_original_path_counterpart' if equivalent else 'unverified_original_path_snapshot'
            if equivalent:
                logical=project_path(next(u['source_path'] for u in units if (u['archive'],u['member'])==(x['archive'],x['member'])))
                map_rows.append({'source':result,'sdk_relative_path':mapping(logical),'project_relative_path':logical,'mapping_scope':'component counterpart; preserve exact original per-TU flags/header domain'})
    upstream=AUDIT/'license-audit/historical-upstream-correspondence.json'
    if upstream.is_file():
        for x in read_json(upstream)['PFFFT']['selected_metadata']:
            p=Path(x['path'])
            if p.name not in ['pffft.c','pffft.h']:continue
            if sha(p.read_bytes())!=x['sha256']:raise ValueError('PFFFT source proof changed')
            add(p,'third-party-overlays/webrtc/third_party/pffft/src/'+p.name,'verified_upstream_source_overlay','Historical PFFFT repair source proof; upstream version and archive pinned separately')
    sdk_snapshot=AUDIT/'sdk-matched-project-source.json'
    if sdk_snapshot.is_file():
        snapshot=read_json(sdk_snapshot);snapshot_map=[]
        for x in snapshot['rows']:
            if not x.get('SDK_present'):continue
            p=AUDIT/'sdk-matched-project-source'/x['SDK_path']
            if not p.is_file() or sha(p.read_bytes())!=x['SDK_sha256']:raise ValueError('Additional SDK source capture changed')
            rel=add(p,'sdk-baseline-snapshot/'+x['SDK_path'],'SDK_source_snapshot_not_yet_object_rebuilt','Actual SDK source bytes; tracked SDK versions and dirty-file hashes pinned separately')
            if rel:snapshot_map.append({'release_source':rel,'sdk_relative_path':x['SDK_path'],'SDK_source_sha256':x['SDK_sha256'],'active_project_file_was_same':x.get('source_exactly_matches_SDK_snapshot',False),'object_equivalence_verified':False})
        json_file(OUT/'dependencies/sdk-source-snapshot-map.json',{'scope':'Additional board/BSP and non-cloud application source snapshot; do not replace final R49 overlay or claim whole clean build','files':snapshot_map,'missing_SDK_paths':[x['SDK_path'] for x in snapshot['rows'] if not x.get('SDK_present')]})
    config=ROOT/'private/aliyun-candidates/aliyun-speech19x2-r49-typed-tail-recovery-r1/.config'
    add(config,'config/nuttx.config','actual_r49_configuration','Actual R49 candidate .config hash')
    # No private target or asset data are replaced with fake functional payloads.
    text='R49 private inputs\n\nExcluded target/credential headers and generated model/audio/recorded arrays must be provided by the authorized operator after license and provenance review. No output-capable target address or placeholder PCM is embedded here. Excluded items are recorded in the dependency lock.\n'
    p=OUT/'source/config/PRIVATE_INPUTS.txt';p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text,encoding='utf-8')
    files.append({'path':'config/PRIVATE_INPUTS.txt','sha256':sha(p.read_bytes()),'bytes':p.stat().st_size,'role':'private_input_instructions'})
    json_file(OUT/'source/source-files.json',{'schema_version':1,'candidate':'R49/source49-typed-tail-recovery-r1','files':files})
    map_rows=list({(x['source'],x['sdk_relative_path'],x['project_relative_path']):x for x in map_rows}.values())
    json_file(OUT/'build/project-to-sdk.json',{'schema_version':1,'entry':'release/r49/build','old_contest_build_is_not_r49_entry':True,'mappings':map_rows,'unmapped_requires_explicit_build_rule':True})
    inv=read_json(AUDIT/'sdk-source-inventory-r1.json');meta=read_json(AUDIT/'sdk-inventory-r1.json')
    repos=[]
    for name,v in inv['repos'].items():
        urls=[line.split()[1] for line in v['remotes']['value'].splitlines() if '(fetch)' in line]
        repos.append({'name':name,'url':urls[0] if urls else None,'revision':v['head']['value'].strip(),'tracked_dirty_paths':[line[3:] for line in v['status']['value'].splitlines()]})
    toolchain_git=AUDIT/'toolchain-git-source.json'
    if toolchain_git.is_file():
        g=read_json(toolchain_git);urls=[line.split()[1] for line in g['remotes']['out'].splitlines() if '(fetch)' in line]
        if not urls or not urls[0].startswith('https://github.com/open-vela/'):raise ValueError('Unapproved toolchain repository URL')
        repos.append({'name':g['root']['out'].strip().removeprefix('/home/swl/openvela/'),'url':urls[0],'revision':g['head']['out'].strip(),'tracked_dirty_paths':[line[3:] for line in g['status']['out'].splitlines()]})
    json_file(OUT/'dependencies/sdk.lock.json',{'schema_version':1,'repositories':repos,'compiler':{'name':'aarch64-none-elf-gcc','version':meta['compiler']['version'].splitlines()[0],'executable_sha256':meta['compiler']['sha256']},'sdk_modifications_not_silently_accepted':True})
    for row in object_rows:
        if row.get('release_source') is None:
            matches=[x for x in excluded if x['file']==row['source_basename']]
            if matches:
                row.update(excluded_reason='asset_related_source_input_excluded_by_export_policy_not_a_source_correspondence_failure',excluded_input_lock='excluded-inputs.lock.json',excluded_input_interfaces=[x['release_interface_path'] for x in matches],private_exact_restore_available=True)
    json_file(OUT/'dependencies/source-object-map.json',{'schema_version':1,'scope':'Five parent application/wireless archives, not all 62 system link inputs','objects':object_rows})
    json_file(OUT/'dependencies/excluded-inputs.lock.json',{'schema_version':1,'items':excluded,'reason':'No private configuration, unknown models/audio/recordings or generated literal arrays copied to public source tree.'})
    exact_kernel=AUDIT/'r3-artifacts/nuttx.bin'
    replay_passed=exact_kernel.is_file() and sha(exact_kernel.read_bytes())==EXPECTED_KERNEL
    image_proof=AUDIT/'r3-image-reproduction.json'
    image_passed=image_proof.is_file() and read_json(image_proof).get('rebuilt_image_sha256')==EXPECTED_IMAGE and read_json(image_proof).get('byte_reproduction_passed') is True
    equivalent_count=sum(x['source_rebuild_equivalent'] for x in object_rows)
    historical_only=sum(not x['source_rebuild_equivalent'] and x.get('historical_full_object_correspondence',False) for x in object_rows)
    json_file(OUT/'dependencies/source-delivery-status.json',{'schema_version':1,'candidate':'R49/source49-typed-tail-recovery-r1','frozen_pins_verified':len(pins),'source_files_exported':len(files),'parent_dwarf_units':len(units),'retained_equivalence_count':equivalent_count,'historical_source_object_identity_only':historical_only,'unproven_parent_application_units':len(units)-equivalent_count-historical_only,'clean_source_dependency_closure_complete':False,'whole_source_clean_build_passed':False,'frozen_archive_overlay_kernel_byte_reproduction_passed':replay_passed,'frozen_archive_overlay_image_byte_reproduction_passed':image_passed,'kernel_sha256':EXPECTED_KERNEL,'original_image_sha256':EXPECTED_IMAGE,'public_distribution_ready':False,'asset_and_private_injection_required':True,'main_source50_untouched':True,'SDK_untouched':True})
    return {'passed':True,'source_files':len(files),'excluded_inputs':len(excluded),'known_secret_values_never_echoed':True,'clean_source_dependency_closure_complete':False}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--export',action='store_true');a=p.parse_args()
    result=export() if a.export else verify();print(json.dumps(result,ensure_ascii=False,indent=2));return 0 if result['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
