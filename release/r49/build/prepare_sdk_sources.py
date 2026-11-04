"""Plan or explicitly clone pinned upstream SDK repositories into a NEW tree.

Default --plan is read-only and offline. --clone creates separate detached SDK
source checkouts; it never applies an overlay to the user's SDK and does not
claim those upstream sources alone are a complete R49 clean-build workspace.
"""
from __future__ import annotations
import argparse,json,re,subprocess
from pathlib import Path
RELEASE=Path(__file__).resolve().parents[1]
ROOT=RELEASE.parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);mode=p.add_mutually_exclusive_group();mode.add_argument('--clone',action='store_true');mode.add_argument('--plan',action='store_true');p.add_argument('--destination',type=Path);p.add_argument('--from-sdk',type=Path);a=p.parse_args()
    lock=json.loads((RELEASE/'dependencies/sdk.lock.json').read_text(encoding='utf-8'))
    if not a.clone:
        print(json.dumps({'read_only':True,'repositories':lock['repositories'],'R49_overlay_and_private_inputs_still_required':True},ensure_ascii=False,indent=2));return 0
    try:
        if a.destination is None:raise ValueError('new_destination_required')
        dest=a.destination.resolve()
        if dest.exists() or dest.is_relative_to(RELEASE) or dest.is_relative_to(Path('/home/swl/openvela').resolve()) or not(dest.is_relative_to(ROOT/'private') or dest.is_relative_to(Path('/dev/shm').resolve())):raise ValueError('new_separate_project_private_or_dev_shm_destination_required_outside_existing_SDK')
        for r in lock['repositories']:
            if not re.fullmatch('[0-9a-f]{40}',r['revision']) or not r['url'].startswith('https://github.com/open-vela/'):raise ValueError('invalid_pinned_repository')
        dest.mkdir(mode=0o700,parents=True,exist_ok=False)
        for r in lock['repositories']:
            source=str(a.from_sdk/r['name']) if a.from_sdk else r['url']
            subprocess.run(['git','clone','--no-checkout','--no-hardlinks',source,str(dest/r['name'])],check=True)
            subprocess.run(['git','-C',str(dest/r['name']),'checkout','--detach',r['revision']],check=True)
            head=subprocess.check_output(['git','-C',str(dest/r['name']),'rev-parse','HEAD'],text=True).strip()
            if head!=r['revision']:raise ValueError('checkout_identity_mismatch')
        print(json.dumps({'passed':True,'scope':'pinned_upstream_source_acquisition_only','R49_complete_workspace':False,'existing_SDK_changed':False}));return 0
    except (OSError,ValueError,subprocess.CalledProcessError) as e:
        print(json.dumps({'passed':False,'reason':str(e)}));return 1
if __name__=='__main__':raise SystemExit(main())
