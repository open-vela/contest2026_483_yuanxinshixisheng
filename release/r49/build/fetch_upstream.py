"""Acquire only the SHA-pinned official APM/Abseil archives into a new cache.

Without --download this copies existing archives from --from-dir. No SDK, main
source, build cache, hardware or remote repository is changed.
"""
from __future__ import annotations
import argparse,hashlib,json,shutil,tarfile,urllib.request
from pathlib import Path
RELEASE=Path(__file__).resolve().parents[1]
ROOT=RELEASE.parents[1]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def extract(p,d):
    with tarfile.open(p) as t:
        ms=t.getmembers()
        if len(ms)>30000 or sum(m.size for m in ms)>512*1024*1024:raise ValueError('archive_limits')
        for m in ms:
            q=(d/m.name).resolve()
            if m.name.startswith('/') or not q.is_relative_to(d) or m.issym() or m.islnk() or not(m.isfile() or m.isdir()):raise ValueError('unsafe_archive_member')
        t.extractall(d)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cache',type=Path,required=True);p.add_argument('--from-dir',type=Path);p.add_argument('--download',action='store_true');p.add_argument('--extract',action='store_true');a=p.parse_args()
    try:
        if not(a.from_dir or a.download):raise ValueError('choose_explicit_local_source_or_download')
        out=a.cache.resolve()
        if out.is_relative_to(RELEASE) or out.is_relative_to(Path('/home/swl/openvela').resolve()) or not(out.is_relative_to(ROOT/'private') or out.is_relative_to(Path('/dev/shm').resolve())):raise ValueError('dependency_cache_requires_separate_project_private_or_dev_shm_directory_outside_SDK')
        lock=json.loads((RELEASE/'dependencies/upstream.lock.json').read_text(encoding='utf-8'));out.mkdir(parents=True,exist_ok=False);rows=[]
        for r in lock['source_archives']:
            dest=(out/r['filename']).resolve()
            if dest==out or not dest.is_relative_to(out):raise ValueError('archive_destination_outside_cache')
            if a.from_dir:
                src=a.from_dir/r['filename']
                if digest(src)!=r['sha256']:raise ValueError('local_archive_identity_mismatch:'+r['name'])
                shutil.copy2(src,dest)
            else:
                if not r['url'].startswith('https://'):raise ValueError('HTTPS_required')
                with urllib.request.urlopen(r['url'],timeout=60) as response,dest.open('xb') as f:shutil.copyfileobj(response,f)
            if dest.stat().st_size!=r['bytes'] or digest(dest)!=r['sha256']:raise ValueError('archive_identity_mismatch:'+r['name'])
            if a.extract:extract(dest,out)
            rows.append({'name':r['name'],'sha256':r['sha256'],'verified':True})
        if a.extract:
            for r in lock.get('source_overlays',[]):
                src=(RELEASE/r['release_path']).resolve();dest=(out/r['upstream_relative_path']).resolve()
                if not src.is_relative_to((RELEASE/'source/third-party-overlays').resolve()) or not src.is_file():raise ValueError('overlay_source_outside_declared_source_root')
                if dest==out or not dest.is_relative_to(out):raise ValueError('overlay_destination_outside_cache')
                if digest(src)!=r['sha256'] or digest(dest)!=r['upstream_sha256']:raise ValueError('overlay_predecessor_mismatch')
                shutil.copy2(src,dest)
        report={'passed':True,'inputs':rows,'source_overlays_applied':bool(a.extract),'native_build_performed':False,'SDK_modified':False}
        (out/'prepared-inputs.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report,indent=2));return 0
    except (OSError,ValueError,KeyError,tarfile.TarError) as e:
        print(json.dumps({'passed':False,'reason':str(e)}));return 1
if __name__=='__main__':raise SystemExit(main())
