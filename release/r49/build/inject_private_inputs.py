"""Restore locked private headers/assets to a NEW private overlay, never release.

--input-map is a JSON object mapping each excluded release interface path to an
operator-owned exact file. All hashes are checked before creating output. No
credentials, target addresses or asset payloads are printed or fabricated.
"""
from __future__ import annotations
import argparse,hashlib,json,shutil
from pathlib import Path
RELEASE=Path(__file__).resolve().parents[1]
ROOT=RELEASE.parents[1]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input-map',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        supplied=json.loads(a.input_map.read_text(encoding='utf-8-sig'));lock=json.loads((RELEASE/'dependencies/excluded-inputs.lock.json').read_text(encoding='utf-8'));plan=[];missing=[]
        out=a.output.resolve()
        if out.is_relative_to(RELEASE) or out.is_relative_to(Path('/home/swl/openvela').resolve()) or not(out.is_relative_to(ROOT/'private') or out.is_relative_to(Path('/dev/shm').resolve())):raise ValueError('private_overlay_requires_separate_project_private_or_dev_shm_directory_outside_SDK')
        if out.exists():raise ValueError('output_already_exists')
        for r in lock['items']:
            rel=r['release_interface_path'];value=supplied.get(rel)
            if not value:missing.append({'path':rel,'reason':'input_not_supplied'});continue
            src=Path(value)
            if not src.is_file() or digest(src)!=r['sha256']:missing.append({'path':rel,'reason':'exact_file_identity_mismatch'});continue
            dest=(out/rel).resolve()
            if not dest.is_relative_to(out):raise ValueError('invalid_interface_path')
            plan.append((src,dest,r['sha256']))
        if missing:
            print(json.dumps({'passed':False,'output_created':False,'missing_inputs':missing},ensure_ascii=False,indent=2));return 2
        out.mkdir(mode=0o700,parents=True,exist_ok=False)
        for src,dest,h in plan:
            dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
            if digest(src)!=h or digest(dest)!=h:raise ValueError('input_changed_during_copy')
        (out/'private-input-receipt.json').write_text(json.dumps({'passed':True,'files':len(plan),'payload_values_printed':False,'original_R49_file_identity_preserved':True})+'\n',encoding='utf-8')
        print(json.dumps({'passed':True,'restored_files':len(plan),'private_output':True,'payload_values_printed':False}));return 0
    except (OSError,ValueError,KeyError,TypeError) as e:
        print(json.dumps({'passed':False,'reason':str(e)}));return 1
if __name__=='__main__':raise SystemExit(main())
