"""Package an exact rebuilt R49 kernel with operator-owned pinned R42 resources.

This writes one NEW private image, never hardware or the public source tree.
The parent image/resource payload is intentionally absent from this release.
"""
from __future__ import annotations
import argparse,hashlib,json,struct
from pathlib import Path
RELEASE=Path(__file__).resolve().parents[1];ROOT=RELEASE.parents[1]
def sha(b):return hashlib.sha256(b).hexdigest()
def pad(b):return b+bytes(-len(b)%2048)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--kernel',type=Path,required=True);p.add_argument('--parent-image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        out=a.output.resolve()
        if out.exists() or out.is_relative_to(RELEASE) or out.is_relative_to(Path('/home/swl/openvela').resolve()) or not(out.is_relative_to(ROOT/'private') or out.is_relative_to(Path('/dev/shm').resolve())):raise ValueError('new_private_output_required_outside_SDK_and_release')
        lock=json.loads((RELEASE/'dependencies/packaging-inputs.lock.json').read_text(encoding='utf-8'));parent=a.parent_image.read_bytes();k=a.kernel.read_bytes()
        if sha(parent)!=lock['parent_image_sha256'] or sha(k)!=lock['kernel_sha256']:raise ValueError('packaging_input_identity_mismatch')
        h=bytearray(parent[:2048]);oldlen=struct.unpack_from('<I',h,8)[0];length=struct.unpack_from('<I',h,24)[0];start=2048+len(pad(parent[2048:2048+oldlen]));resource=parent[start:start+length]
        if len(resource)!=lock['parent_resource_bytes'] or sha(resource)!=lock['parent_resource_sha256']:raise ValueError('resource_identity_mismatch')
        struct.pack_into('<I',h,8,len(k));s=hashlib.sha1()
        for b in (k,b'',resource,b'',b''):s.update(b);s.update(struct.pack('<I',len(b)))
        h[576:608]=s.digest()+bytes(12);image=bytes(h)+pad(k)+pad(resource)
        if sha(image)!=lock['rebuilt_image_sha256'] or len(image)!=lock['rebuilt_image_bytes']:raise ValueError('result_identity_mismatch')
        # No output or partial image is created before complete identity checks.
        with out.open('xb') as f:f.write(image)
        print(json.dumps({'passed':True,'image_sha256':sha(image),'image_bytes':len(image),'private_output':True,'hardware_operation':False,'whole_source_clean_build':False}));return 0
    except (OSError,ValueError,KeyError,struct.error) as e:
        print(json.dumps({'passed':False,'reason':str(e)}));return 1
if __name__=='__main__':raise SystemExit(main())
