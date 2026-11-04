"""R49-only source verification and explicit private frozen-overlay replay.

Default verification is offline/read-only. Replay creates a new private build
directory and invokes only the SHA-pinned historical compile/link recipe. It does
not operate hardware, publish, change the SDK, or claim a whole-source clean build.
"""
from __future__ import annotations
import argparse,hashlib,json,os
from pathlib import Path
import runpy,subprocess,sys,tarfile

RELEASE=Path(__file__).resolve().parents[1]
ROOT=RELEASE.parents[1]
KERNEL='2c41598a36403d4a41d6dfacd762b3ead83662d078e7d0679f031c10adb5bfc3'
STAGE='697072627c44b8d153fefe9d793e3f4e910ad3bc5a41966a46f0e64631c431f1'
PINS='8028ff80669b855e017ee73b17ee9b6856d508dd8ee7db1249597bbce47a88f2'
CANONICAL='/dev/shm/velavision-microcrystal-r49-r1-20261103'
SDK_CANONICAL='/home/swl/openvela'
BASE_REL='work/velavision-project/private/aliyun-candidates/aliyun-speech19x2/build'

def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def verify_sources():
    m=load(RELEASE/'source/source-files.json');problems=[]
    for r in m['files']:
        p=RELEASE/'source'/r['path']
        if not p.is_file() or digest(p)!=r['sha256']:problems.append(r['path'])
    expected={r['path'] for r in m['files']}|{'source-files.json'}
    problems.extend(p.relative_to(RELEASE/'source').as_posix() for p in (RELEASE/'source').rglob('*') if p.is_file() and p.relative_to(RELEASE/'source').as_posix() not in expected)
    return problems
def sdk_problems(sdk):
    problems=[];lock=load(RELEASE/'dependencies/sdk.lock.json')
    for r in lock['repositories']:
        p=subprocess.run(['git','--no-optional-locks','-C',str(sdk/r['name']),'rev-parse','HEAD'],capture_output=True,text=True)
        if p.returncode or p.stdout.strip()!=r['revision']:problems.append('sdk_revision:'+r['name'])
        p=subprocess.run(['git','--no-optional-locks','-C',str(sdk/r['name']),'status','--porcelain=v1','--untracked-files=no'],capture_output=True,text=True)
        if p.returncode:problems.append('SDK_status_unreadable:'+r['name'])
        elif {line[3:] for line in p.stdout.splitlines()}!=set(r['tracked_dirty_paths']):problems.append('unknown_tracked_sdk_modification:'+r['name'])
    tool=load(RELEASE/'dependencies/toolchain-and-config.lock.json')
    for r in tool['tools']+tool['sdk_dirty_files']:
        p=sdk/r['path']
        if not p.is_file() or digest(p)!=r['sha256']:problems.append('sdk_file:'+r['path'])
    expected={r['path'][len('nuttx/'):] for r in tool['sdk_dirty_files'] if r['path'].startswith('nuttx/')}
    p=subprocess.run(['git','--no-optional-locks','-C',str(sdk/'nuttx'),'status','--porcelain=v1','--untracked-files=no'],capture_output=True,text=True)
    actual={line[3:] for line in p.stdout.splitlines()}
    if p.returncode or actual!=expected:problems.append('unknown_or_missing_tracked_sdk_modification')
    for r in tool['generated']:
        p=sdk/BASE_REL/r['path'].split('{BASE_BUILD}/',1)[1]
        if not p.is_file() or digest(p)!=r['sha256']:problems.append('generated_config:'+r['path'])
    for r in tool.get('source30_helpers',[]):
        p=sdk/r['path']
        if not p.is_file() or digest(p)!=r['sha256']:problems.append('historical_helper:'+r['path'])
    for r in load(RELEASE/'dependencies/frozen-link-inputs.lock.json')['inputs']:
        p=Path(r['resolver'].replace('{SDK_ROOT}',str(sdk)))
        if not p.is_file() or digest(p)!=r['sha256']:problems.append('frozen_link_input:'+r['name'])
    return problems
def safe_extract(archive,target):
    with tarfile.open(archive) as t:
        members=t.getmembers()
        if len(members)>10000 or sum(x.size for x in members)>1024**3:raise ValueError('archive_limits')
        for m in members:
            p=(target/m.name).resolve()
            if m.name.startswith('/') or not p.is_relative_to(target) or m.issym() or m.islnk() or not(m.isfile() or m.isdir()):raise ValueError('unsafe_archive_member')
        t.extractall(target)
def replay(a):
    sdk=a.sdk.resolve()
    if str(sdk)!=SDK_CANONICAL:raise ValueError('historical_recipe_requires_canonical_SDK_path; no alias or SDK overwrite will be created')
    problems=sdk_problems(sdk)
    if problems:return 1,{'passed':False,'problems':problems}
    if digest(a.stage)!=STAGE:raise ValueError('private_stage_sha256_mismatch')
    out=a.output.resolve()
    if out.is_relative_to(RELEASE) or out.is_relative_to(sdk) or not out.is_relative_to(Path('/dev/shm').resolve()):raise ValueError('private_build_output_requires_new_dev_shm_directory_outside_SDK_and_release')
    out.mkdir(mode=0o700,parents=True,exist_ok=False);stage=out/'stage';stage.mkdir();safe_extract(a.stage,stage)
    if digest(stage/'pins.json')!=PINS:raise ValueError('source_pin_manifest_mismatch')
    for rel,h in load(stage/'pins.json').items():
        if digest(stage/rel)!=h:raise ValueError('frozen_source_pin_mismatch')
    # Compile parameters only: restore canonical debug paths. No source or output
    # bytes are patched; the original stage build.py and pins remain untouched.
    prefix='-fdebug-prefix-map='+str(stage)+'='+CANONICAL
    launcher="import runpy,subprocess; old=subprocess.run; prefix="+repr(prefix)+"; subprocess.run=lambda args,*a,**k: old(([args[0],prefix,*args[1:]] if isinstance(args,list) and str(args[0]).endswith(('aarch64-none-elf-gcc','aarch64-none-elf-g++')) else args),*a,**k); runpy.run_path("+repr(str(stage/'build.py'))+",run_name='__main__')"
    p=subprocess.run([sys.executable,'-B','-c',launcher],cwd=stage,capture_output=True,env=dict(os.environ,TMPDIR=str(out)))
    (out/'build.stdout').write_bytes(p.stdout);(out/'build.stderr').write_bytes(p.stderr)
    kernel=stage/'nuttx.bin';actual=digest(kernel) if kernel.is_file() else None
    post=sdk_problems(sdk)
    report={'passed':p.returncode==0 and actual==KERNEL and not post,'mode':'frozen_archive_overlay_replay','kernel_sha256':actual,'expected_kernel_sha256':KERNEL,'whole_source_clean_build':False,'hardware_operation':False,'SDK_modified':bool(post),'SDK_postcheck_issues':post}
    (out/'replay-receipt.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return (0 if report['passed'] else 1),report
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('command',nargs='?',choices=['verify','preflight','replay','clean'],default='verify');p.add_argument('--sdk',type=Path);p.add_argument('--stage',type=Path);p.add_argument('--output',type=Path);p.add_argument('--private-overlay',type=Path);a=p.parse_args()
    try:
        issues=verify_sources()
        if issues:code,report=1,{'passed':False,'reason':'source_identity_mismatch','paths':issues}
        elif a.command=='replay':
            if not(a.sdk and a.stage and a.output):raise ValueError('replay_requires_sdk_stage_and_new_private_output')
            code,report=replay(a)
        else:
            missing=[]
            for x in load(RELEASE/'dependencies/excluded-inputs.lock.json')['items']:
                rel=x['release_interface_path'];q=a.private_overlay/rel if a.private_overlay else None
                if q is None or not q.is_file() or digest(q)!=x['sha256']:missing.append({'path':rel,'reason':x['reason']})
            sdk_issues=sdk_problems(a.sdk.resolve()) if a.sdk else ['SDK_not_supplied']
            status=load(RELEASE/'dependencies/source-delivery-status.json')
            code=2 if a.command=='clean' or missing or sdk_issues or not status['whole_source_clean_build_passed'] else 0
            report={'source_identity_passed':True,'read_only':True,'source_files_checked':len(load(RELEASE/'source/source-files.json')['files']),'missing_private_or_asset_inputs':missing,'SDK_preflight_issues':sdk_issues,'dependency_closure_complete':status['clean_source_dependency_closure_complete'],'whole_source_clean_build_passed':False,'clean_build_refused':a.command=='clean','reason':'source_organization_package_with_explicit_unclosed_full_build_inputs','next_entry':'fetch_upstream.py / inject_private_inputs.py / explicit replay with original private bundle'}
        print(json.dumps(report,ensure_ascii=False,indent=2));return code
    except (OSError,ValueError,KeyError,TypeError) as e:
        print(json.dumps({'passed':False,'reason':str(e)},ensure_ascii=False));return 1
if __name__=='__main__':raise SystemExit(main())
