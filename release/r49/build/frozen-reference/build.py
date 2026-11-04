from pathlib import Path
import json,hashlib,shlex,subprocess,shutil,os,sys,re,struct
O=Path(__file__).resolve().parent;ENV=dict(os.environ,TMPDIR=str(O));sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
OLD=Path('/home/swl/openvela/private/camera-lateplug-source30-20261029-r1');sys.path.insert(0,str(OLD))
from elf_checks import elf_facts,elf_named_bytes,archive_table
P=json.loads((OLD/'plan.json').read_text());B=Path(P['build']);paths={}
for rel,h in json.loads((O/'pins.json').read_text()).items():assert sha(O/rel)==h,rel
for row in P['rows']:
 p=OLD/row['staged'] if 'staged' in row else Path(row['remote']);assert sha(p)==row['sha256'],str(p);paths[row['remote']]=str(p)
def run(label,args):
 p=subprocess.run(args,cwd=B,capture_output=True,env=ENV,timeout=240)
 (O/(label+'.stdout')).write_bytes(p.stdout);(O/(label+'.stderr')).write_bytes(p.stderr)
 if p.returncode:print(p.stderr.decode(errors='replace')[-4000:],flush=True)
 assert p.returncode==0,label;return p.stdout
original=json.loads((O/'vv_native_runtime-template.json').read_text());args=[];i=0
while i<len(original):
 a=original[i];i+=1
 if a in ['-o','-iquote','-MF']:i+=1;continue
 if a in ['-MD','-c'] or a.startswith('-I/dev/shm/') or (a.endswith('.c') and not a.startswith('-')):continue
 if a.startswith('/dev/shm/'):
  m=re.search(r'/after/0/(?:home/swl/openvela/)?(nuttx|apps|work)/(.+)',a);assert m,a;a='/home/swl/openvela/'+m.group(1)+'/'+m.group(2)
 args.append(a)
args[1:1]=['-I'+str(O/'after/deps'),'-I'+str(O/'after/app/k7agent/cloud/include'),'-I'+str(O/'after/app/k7host'),'-I'+str(O/'after/app/k7sound'),'-I/home/swl/openvela/nuttx/wireless/bluetooth','-iquote','/home/swl/openvela/apps/examples/k7agent/cloud/src']
args+=['-DK7_LOCAL_ASSESSMENT=1','-DK7_REAL_SHUIGUANG_PROFILE=1','-DK7_CLOUD_BUSINESS_PROFILE=1']
provider_original=json.loads((O/'vv_aliyun_provider-template.json').read_text());provider_args=[];i=0
while i<len(provider_original):
 a=provider_original[i];i+=1
 if a in ['-o','-iquote','-MF']:i+=1;continue
 if a in ['-MD','-c'] or a.startswith('-I/dev/shm/') or (a.endswith('.c') and not a.startswith('-')):continue
 if a.startswith('/dev/shm/'):
  m=re.search(r'/after/0/(?:home/swl/openvela/)?(nuttx|apps|work)/(.+)',a);assert m,a;a='/home/swl/openvela/'+m.group(1)+'/'+m.group(2)
 provider_args.append(a)
provider_args[1:1]=['-I'+str(O/'after/deps'),'-I'+str(O/'after/app/k7agent/cloud/include'),'-I'+str(O/'after/app/k7host'),'-I'+str(O/'after/app/k7sound'),'-I/home/swl/openvela/nuttx/wireless/bluetooth','-iquote','/home/swl/openvela/apps/examples/k7agent/cloud/src']
provider_args+=['-DK7_LOCAL_ASSESSMENT=1','-DK7_REAL_SHUIGUANG_PROFILE=1','-DK7_CLOUD_BUSINESS_PROFILE=1']
def allocated(path):
 b=path.read_bytes();off=struct.unpack_from('<Q',b,40)[0];es,num,sti=struct.unpack_from('<HHH',b,58)
 ss=[struct.unpack_from('<IIQQQQIIQQ',b,off+i*es) for i in range(num)];st=ss[sti];nb=b[st[4]:st[4]+st[5]]
 return {nb[s[0]:].split(b'\0',1)[0].decode():(s[1],s[2],s[5],s[8],b[s[4]:s[4]+s[5]] if s[1]!=8 else b'') for s in ss if s[2]&2}

def extra_template(name):
 original=json.loads((O/(name+'-template.json')).read_text());result=[];i=0
 while i<len(original):
  a=original[i];i+=1
  if a in ['-o','-iquote','-MF']:i+=1;continue
  if a in ['-MD','-c'] or a.startswith('-I/dev/shm/') or (a.endswith('.c') and not a.startswith('-')):continue
  if a.startswith('/dev/shm/'):
   m=re.search(r'/after/0/(?:home/swl/openvela/)?(nuttx|apps|work)/(.+)',a);assert m,a;a='/home/swl/openvela/'+m.group(1)+'/'+m.group(2)
  result.append(a)
 result[1:1]=['-I'+str(O/'after/deps'),'-I'+str(O/'after/app/k7agent/cloud/include'),'-I'+str(O/'after/app/k7host'),'-I'+str(O/'after/app/k7sound'),'-I/home/swl/openvela/nuttx/wireless/bluetooth','-iquote','/home/swl/openvela/apps/examples/k7agent/cloud/src']
 result+=['-DK7_LOCAL_ASSESSMENT=1','-DK7_REAL_SHUIGUANG_PROFILE=1','-DK7_CLOUD_BUSINESS_PROFILE=1']
 source_path=next(a for a in original if a.endswith('.c') and not a.startswith('-'))
 result+=['-fmacro-prefix-map='+str(O/'after/app/k7agent/cloud/src'/(name+'.c'))+'='+source_path]
 return result
platform_args=extra_template('vv_native_platform');service_args=extra_template('vv_native_service')
compiler=Path(args[0]);ar=str(compiler.with_name('aarch64-none-elf-ar'));objdump=str(compiler.with_name('aarch64-none-elf-objdump'))
run('baseline-runtime',args+['-c',str(O/'baseline-runtime.c'),'-o',str(O/'baseline-runtime.c.o')])
(O/'old-runtime.c.o').write_bytes(run('extract-old',[ar,'p',str(O/'parent-cloud.a'),'vv_native_runtime.c.o']))
old=run('dis-old',[objdump,'-dr',str(O/'old-runtime.c.o')]).split(b'Disassembly of section',1)[1]
base=run('dis-base',[objdump,'-dr',str(O/'baseline-runtime.c.o')]).split(b'Disassembly of section',1)[1]
assert old==base,'R42 baseline differs'
run('baseline-provider',provider_args+['-c',str(O/'baseline-provider.c'),'-o',str(O/'baseline-provider.c.o')])
(O/'old-provider.c.o').write_bytes(run('extract-old-provider',[ar,'p',str(O/'parent-cloud.a'),'vv_aliyun_provider.c.o']))
old_provider=run('dis-old-provider',[objdump,'-dr',str(O/'old-provider.c.o')]).split(b'Disassembly of section',1)[1]
baseline_provider=run('dis-base-provider',[objdump,'-dr',str(O/'baseline-provider.c.o')]).split(b'Disassembly of section',1)[1]
assert old_provider==baseline_provider,'R42 provider baseline differs'
for name,command in [('vv_native_platform',platform_args),('vv_native_service',service_args)]:
 base=O/('baseline-'+name+'.c.o');old_obj=O/('old-'+name+'.c.o')
 origin=next(a for a in json.loads((O/(name+'-template.json')).read_text()) if a.endswith('.c') and not a.startswith('-'))
 run('baseline-'+name,command+['-fmacro-prefix-map='+str(O/('baseline-'+name+'.c'))+'='+origin,'-c',str(O/('baseline-'+name+'.c')),'-o',str(base)])
 old_obj.write_bytes(run('extract-'+name,[ar,'p',str(O/'parent-cloud.a'),name+'.c.o']))
 old_dis=run('old-dis-'+name,[objdump,'-dr',str(old_obj)]).split(b'Disassembly of section',1)[1]
 base_dis=run('baseline-dis-'+name,[objdump,'-dr',str(base)]).split(b'Disassembly of section',1)[1]
 assert old_dis==base_dis and allocated(old_obj)==allocated(base),'Exact R42 baseline differs: '+name
new=O/'libapps_k7cloud.a';shutil.copyfile(O/'parent-cloud.a',new)
for name in ['vv_native_runtime','vv_native_platform','vv_native_service','vv_microcrystal','vv_microcrystal_nuttx','vv_aliyun_provider']:
 source=O/'after/app/k7agent/cloud/src'/(name+'.c');obj=O/(name+'.c.o')
 extra=['-DK7_MICROCRYSTAL=1','-DK7_MICROCRYSTAL_TARGET_HEADER="vv_microcrystal_target_private.h"']
 run('compile-'+name,(provider_args if name=='vv_aliyun_provider' else platform_args if name=='vv_native_platform' else service_args if name=='vv_native_service' else args)+extra+['-c',str(source),'-o',str(obj)])
 run('archive-'+name,[ar,'r',str(new),str(obj)])
near_template=json.loads((O/'vv_near_experiment-normalized-argv.json').read_text())
def near_args(source):return [a.replace('{STAGE}',str(O)).replace('{SOURCE}',str(source)) for a in near_template]
near_base_source=O/'baseline-vv_near_experiment.c';near_old=O/'old-vv_near_experiment.c.o';near_base=O/'baseline-vv_near_experiment.c.o'
near_old.write_bytes(run('extract-old-near',[ar,'p',str(O/'parent-cloud.a'),'vv_near_experiment.c.o']))
run('baseline-near',near_args(near_base_source)+['-c',str(near_base_source),'-o',str(near_base)])
assert allocated(near_old)==allocated(near_base),'Near allocated baseline differs'
assert run('old-near-dis',[objdump,'-dr',str(near_old)]).split(b'Disassembly of section',1)[1]==run('base-near-dis',[objdump,'-dr',str(near_base)]).split(b'Disassembly of section',1)[1],'Near disasm baseline differs'
near_source=O/'after/app/k7agent/cloud/src/vv_near_experiment.c';near_input=O/'near-input/vv_near_experiment.c';near_input.parent.mkdir();shutil.copyfile(near_source,near_input);near_obj=O/'vv_near_experiment.c.o'
run('compile-near',near_args(near_input)+['-c',str(near_input),'-o',str(near_obj)])
run('replace-near',[ar,'r',str(new),str(near_obj)])

run('index',[ar,'s',str(new)])
old_members=run('old-members',[ar,'t',str(O/'parent-cloud.a')]).decode().splitlines();new_members=run('new-members',[ar,'t',str(new)]).decode().splitlines()
allowed={n+'.c.o' for n in ['vv_native_runtime','vv_native_platform','vv_native_service','vv_microcrystal','vv_microcrystal_nuttx','vv_aliyun_provider','vv_near_experiment']}
assert len(old_members)==len(set(old_members)) and len(new_members)==len(set(new_members))
assert set(new_members)==set(old_members)|{'vv_microcrystal.c.o','vv_microcrystal_nuttx.c.o'}
retained_objects=0
for member in old_members:
 if member in allowed:continue
 assert run('retain-old-'+member,[ar,'p',str(O/'parent-cloud.a'),member])==run('retain-new-'+member,[ar,'p',str(new),member]),member
 retained_objects+=1

wireless_original=json.loads((O/'bt_hcicore-template.json').read_text());wireless_args=[];i=0
while i<len(wireless_original):
 a=wireless_original[i];i+=1
 if a in ['-o','-MF']:i+=1;continue
 if a in ['-c','-MD'] or (a.endswith('.c') and not a.startswith('-')):continue
 wireless_args.append(a)
wireless_args+=['-iquote','/home/swl/openvela/nuttx/wireless/bluetooth']
wireless_source=O/'after/port/nuttx/wireless/bluetooth/bt_hcicore.c'
wireless_header=wireless_source.with_name('vv_microcrystal_fast_connect.h')
wireless_baseline=O/'baseline-bt_hcicore.c';wireless_old=O/'old-bt_hcicore.c.o';wireless_base=O/'baseline-bt_hcicore.c.o'
wireless_old.write_bytes(run('extract-old-wireless',[ar,'p',str(O/'parent-wireless.a'),'bt_hcicore.c.o']))
run('baseline-wireless',wireless_args+['-fmacro-prefix-map='+str(wireless_baseline)+'=/wireless/bluetooth/bt_hcicore.c','-c',str(wireless_baseline),'-o',str(wireless_base)])
assert allocated(wireless_old)==allocated(wireless_base),'Wireless allocated baseline differs'
assert run('old-wireless-dis',[objdump,'-dr',str(wireless_old)]).split(b'Disassembly of section',1)[1]==run('base-wireless-dis',[objdump,'-dr',str(wireless_base)]).split(b'Disassembly of section',1)[1],'Wireless disasm baseline differs'
# Both default and explicit flagoff remain exactly the predecessor radio object.
for label,flags in [('default',[]),('flagoff',['-DK7_MICROCRYSTAL_FAST_CONNECT_GUARD=0'])]:
 obj=O/('wireless-'+label+'.c.o')
 run('compile-wireless-'+label,wireless_args+['-include',str(wireless_header),'-fmacro-prefix-map='+str(wireless_source)+'=/wireless/bluetooth/bt_hcicore.c']+flags+['-c',str(wireless_source),'-o',str(obj)])
 assert allocated(wireless_old)==allocated(obj),label
 assert run('wireless-'+label+'-dis',[objdump,'-dr',str(obj)]).split(b'Disassembly of section',1)[1]==run('old-wireless-dis',[objdump,'-dr',str(wireless_old)]).split(b'Disassembly of section',1)[1],label
wireless_obj=O/'bt_hcicore.c.o'
run('compile-wireless-fast',wireless_args+['-include',str(wireless_header),'-DK7_MICROCRYSTAL_FAST_CONNECT_GUARD=1','-fmacro-prefix-map='+str(wireless_source)+'=/wireless/bluetooth/bt_hcicore.c','-c',str(wireless_source),'-o',str(wireless_obj)])
old_sec=allocated(wireless_old);new_sec=allocated(wireless_obj);changed_sec=[k for k in set(old_sec)|set(new_sec) if old_sec.get(k)!=new_sec.get(k)]
assert changed_sec==['.text.hci_rx_work'],changed_sec
wireless_new=O/'libwireless.a';shutil.copyfile(O/'parent-wireless.a',wireless_new);run('replace-wireless',[ar,'r',str(wireless_new),str(wireless_obj)]);run('index-wireless',[ar,'s',str(wireless_new)])
wmembers=run('old-wireless-members',[ar,'t',str(O/'parent-wireless.a')]).decode().splitlines();assert len(wmembers)==len(set(wmembers))
assert run('new-wireless-members',[ar,'t',str(wireless_new)]).decode().splitlines()==wmembers
retained_wireless_objects=0
for member in wmembers:
 if member=='bt_hcicore.c.o':continue
 assert run('keep-wireless-old-'+member,[ar,'p',str(O/'parent-wireless.a'),member])==run('keep-wireless-new-'+member,[ar,'p',str(wireless_new),member]),member
 retained_wireless_objects+=1
for key in paths:
 if key.endswith('/wireless/libwireless.a'):
  assert sha(O/'parent-wireless.a')==sha(paths[key]);paths[key]=str(wireless_new)

sound_template=json.loads((O/'echo_diag_runtime-normalized-argv.json').read_text())
def sound_args(source):
 return [a.replace('{STAGE}',str(O)).replace('{SOURCE}',str(source)) for a in sound_template]
sound_source=O/'after/app/k7sound/echo_diag_runtime.c';sound_baseline=O/'baseline-echo_diag_runtime.c'
sound_old=O/'old-echo_diag_runtime.c.o';sound_base=O/'baseline-echo_diag_runtime.c.o';sound_obj=O/'echo_diag_runtime.c.o'
sound_old.write_bytes(run('extract-old-echo',[ar,'p',str(O/'parent-sound.a'),'echo_diag_runtime.c.o']))
run('baseline-echo',sound_args(sound_baseline)+['-c',str(sound_baseline),'-o',str(sound_base)])
assert allocated(sound_old)==allocated(sound_base),'Echo allocated baseline differs'
assert run('old-echo-dis',[objdump,'-dr',str(sound_old)]).split(b'Disassembly of section',1)[1]==run('base-echo-dis',[objdump,'-dr',str(sound_base)]).split(b'Disassembly of section',1)[1],'Echo disasm baseline differs'
sound_input=O/'echo-input/echo_diag_runtime.c';sound_input.parent.mkdir();shutil.copyfile(sound_source,sound_input)
run('compile-echo',sound_args(sound_input)+['-c',str(sound_input),'-o',str(sound_obj)])
sound_new=O/'libapps_k7sound.a';shutil.copyfile(O/'parent-sound.a',sound_new);run('replace-echo',[ar,'r',str(sound_new),str(sound_obj)]);run('index-sound',[ar,'s',str(sound_new)])
smembers=run('old-sound-members',[ar,'t',str(O/'parent-sound.a')]).decode().splitlines();assert len(smembers)==len(set(smembers))
assert run('new-sound-members',[ar,'t',str(sound_new)]).decode().splitlines()==smembers
retained_sound_objects=0
for member in smembers:
 if member=='echo_diag_runtime.c.o':continue
 assert run('keep-sound-old-'+member,[ar,'p',str(O/'parent-sound.a'),member])==run('keep-sound-new-'+member,[ar,'p',str(sound_new),member]),member
 retained_sound_objects+=1
sound_paths=[key for key in paths if key.endswith('/libapps_k7sound.a')];assert len(sound_paths)==1,sound_paths
for key in sound_paths:
 assert sha(O/'parent-sound.a')==sha(paths[key]);paths[key]=str(sound_new)

paths.update({k:str(new if k.endswith('/libapps_k7cloud.a') else O/('parent-'+('boot' if k.endswith('/libapps_k7boot.a') else 'host')+'.a')) for k in paths if any(k.endswith('/libapps_k7'+g+'.a') for g in ['cloud','boot','host'])})
argv=[]
for a in P['argv']:
 if a in paths:a=paths[a]
 elif a.startswith('-Wl,-cref,-Map='):a='-Wl,-cref,-Map='+str(O/'nuttx.map')
 elif a.endswith('/nuttx'):a=str(O/'nuttx')
 argv.append(a)
run('link',argv);run('objcopy',[str(compiler.with_name('aarch64-none-elf-objcopy')),'-O','binary',str(O/'nuttx'),str(O/'nuttx.bin')])
assert not run('undefined',[str(compiler.with_name('aarch64-none-elf-nm')),'-u',str(O/'nuttx')]).strip()
symbols=run('check-fast-hook-symbol',[str(compiler.with_name('aarch64-none-elf-nm')),'--defined-only',str(O/'nuttx')]).decode();assert re.search(r'(?m)^[0-9a-fA-F]+ T vv_microcrystal_manual_fast_connect_target$',symbols),'Strong manual target predicate missing'
facts=elf_facts(O/'nuttx',O/'nuttx.bin');assert json.loads(json.dumps(facts['initializer_arrays']))==json.loads((O/'ELF-facts.json').read_text())['initializer_arrays']
retained=json.loads((O/'retention-recheck.json').read_text())['assets']
for x in retained:
 b=elf_named_bytes(O/'nuttx',x['name'],end_name='k7_vad_silero51_end' if x['name']=='k7_vad_silero51_data' else None);assert len(b)==x['bytes'] and hashlib.sha256(b).hexdigest()==x['sha256'],x['name']
for label in ['default','firefly']:
 data=elf_named_bytes(O/'nuttx','vv_microcrystal_cue_'+label+'_complete');expected=json.loads((O/'micro-audio.json').read_text());row=next(r for r in expected['rows'] if r['label']==label);assert len(data)==row['bytes'] and hashlib.sha256(data).hexdigest()==row['sha256']
for row in json.loads((O/'micro-voice-audio.json').read_text())['rows']:
 data=elf_named_bytes(O/'nuttx','vv_micro_voice_'+row['label']+'_'+row['code']);assert len(data)==row['packed_bytes'] and hashlib.sha256(data).hexdigest()==row['packed_sha256']
result=dict(passed=True,parent='R48 + typed exactoldcancel0 tail handoff repair',runtime_baseline_disassembly_equal=True,platform_baseline_disassembly_equal=True,service_baseline_disassembly_equal=True,retained_cloud_objects=retained_objects,wireless_original_baseline_disassembly_and_allocated_equal=True,wireless_default_and_flagoff_equal=True,wireless_changed_allocated_sections=changed_sec,retained_wireless_objects=retained_wireless_objects,SDK_private_overlay=True,SDK_workspace_modified=False,echo_original_baseline_disassembly_and_allocated_equal=True,retained_sound_objects=retained_sound_objects,near_original_baseline_disassembly_and_allocated_equal=True,kernel_sha256=sha(O/'nuttx.bin'),kernel_bytes=(O/'nuttx.bin').stat().st_size,retained_assets=len(retained),storage_written=False,SDK_modified=False,hardware_tested=False)
(O/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
