#!/usr/bin/env python3
"""Real SDK bitfields + actual bridge identity path. No Windows calls/hardware."""
from pathlib import Path
import argparse,hashlib,json,subprocess,sys,shutil,os,re
H=Path(__file__).resolve().parent;M=H.parents[3];L=H.parent
need=lambda v,what:Path(os.environ.get(v) or sys.exit('run.py: set '+v+' to '+what))
A=need('AURORA_SILICON_DIR','the AuroraSilicon checkout');T=need('AURORA_TOOLCHAINS','the toolchains folder that holds llvm-mingw');SDK=need('WINDOWS_SDK_SHARED_INCLUDE','the shared include folder of Windows SDK 10.0.26100 (c/Include/10.0.26100.0/shared)')
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();ent=lambda p:{'sha256':sha(p),'bytes':p.stat().st_size}
r={'result':'FAIL','hardware_accessed':False,'commands':[],'source_files':{},'tool_files':{},'sdk_masks':{},'limits':['OS calls/enumeration modeled; actual helper and identity parser compiled','Actual193 failure branch remains unknown until a source-bound hardware run']}
def run(c,name):
 r['commands'].append([str(x) for x in c])
 q=subprocess.run([str(x) for x in c],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'));(O/name).write_text(q.stdout);return q.returncode
try:
 iso=O/'sdk';iso.mkdir()
 for q in list((T/'llvm-mingw/bin').glob('*.cfg'))+[T/'llvm-mingw/bin/clang',T/'llvm-mingw/bin/clang-target-wrapper.sh']:
  r['tool_files'][str(q.resolve())]=ent(q.resolve())
 for name in ['d3dkmthk.h','d3dkmdt.h','d3dukmdt.h']:shutil.copy2(SDK/name,iso/name);r['source_files'][str(SDK/name)]=ent(SDK/name)
 code='#include <windows.h>\n#include <d3dkmthk.h>\n'
 fields={'render':'RenderSupported','display':'DisplaySupported','software':'SoftwareDevice','post':'PostDevice'}
 for name,field in fields.items():code+='extern "C" unsigned sdk_'+name+'(){D3DKMT_ADAPTERTYPE t={};t.'+field+'=1;return t.Value;}\n'
 (O/'sdk-bits.cpp').write_text(code)
 for triple in ['aarch64-w64-mingw32','x86_64-w64-mingw32']:
  cc=T/'llvm-mingw/bin'/(triple+'-clang++');r['tool_files'][str(cc.resolve())]=ent(cc.resolve())
  cmd=[cc,'-std=c++17','-w','-O2','-S','-emit-llvm','-I',iso,O/'sdk-bits.cpp','-o',O/(triple+'.ll')];assert run(cmd,triple+'.log')==0,'SDK compile '+triple
  ir=(O/(triple+'.ll')).read_text();values={}
  for name in fields:
   body=re.search(r'define[^@]*@sdk_'+name+r'\(\)[^{]*\{([^}]+)\}',ir).group(1);values[name]=int(re.search(r'ret i32 (\d+)',body).group(1))
  r['sdk_masks'][triple]=values
 assert len({json.dumps(v,sort_keys=True) for v in r['sdk_masks'].values()})==1,'architecture bitfield drift'
 for triple in ['aarch64-w64-mingw32','x86_64-w64-mingw32']:
  cc=T/'llvm-mingw/bin'/(triple+'-clang++')
  assert run([cc,'-std=c++17','-w','-fsyntax-only','-I',iso,'-I',L,'-I',A/'drivers/AppleAgxWddm/core',L/'agx_d3dkmt_conformance.cpp'],triple+'-conformance.log')==0,'full ABI conformance'
 values=r['sdk_masks']['aarch64-w64-mingw32'];assert values=={'render':1,'display':2,'software':4,'post':8},'SDK contract changed'
 (O/'sdk-masks.h').write_text('\n'.join('#define SDK_'+k.upper()+('_DEVICE' if k in ['software','post'] else '_SUPPORTED')+' '+str(v)+'u' for k,v in values.items())+'\n')
 source=(L/'agx_d3dkmt_bridge.c').read_text();start=source.index('bridge_adapter_matches(');end=source.index('\nbool\nagx_d3dkmt_bridge_install',start);body=source[start:end]
 (O/'production.c').write_text('#include "host-windows.h"\n#include "aurora_gpu_bridge.h"\n#include "agx_d3dkmt_core.h"\n#include "sdk-masks.h"\n#define AGXW_STATUS_DEVICE_REMOVED ((agx_kmt_status)0xC00002B6)\n#include "adapter.c"\nstatic bool\n'+body)
 files=[L/'agx_d3dkmt_bridge.c',L/'agx_d3dkmt.h',L/'agx_d3dkmt_core.c',L/'agx_d3dkmt_core.h',L/'agx_d3dkmt_conformance.cpp',L/'agxw_bridge_ddi.h',H/'adapter.c',H/'run.py',H/'FAILURE-MODEL.md',A/'integration/agx-runtime-bridge/host-windows.h',A/'umd/compat/aurora_gpu_bridge.h',A/'drivers/AppleAgxWddm/core/agxw_contract.c',A/'drivers/AppleAgxWddm/core/agxw_contract.h']
 r['source_files'].update({str(p):ent(p) for p in files})
 cc=Path(subprocess.check_output(['xcrun','--find','clang'],text=True).strip());sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();r['tool_files'][str(cc.resolve())]=ent(cc.resolve())
 cmd=[cc,'-isysroot',sdk,'-std=c11','-g','-fsanitize=address,undefined','-Wno-incompatible-pointer-types','-I',O,'-I',H,'-I',A/'integration/agx-runtime-bridge','-I',L,'-I',A/'umd/compat','-I',A/'drivers/AppleAgxWddm/core',O/'production.c',L/'agx_d3dkmt_core.c',A/'drivers/AppleAgxWddm/core/agxw_contract.c','-pthread','-o',O/'replay']
 assert run(cmd,'build.log')==0,'adapter replay compile';rc=run([O/'replay'],'run.log');r['exit']=rc;r['checks']=sum(x.startswith('PASS ') for x in (O/'run.log').read_text().splitlines());assert rc==0,'adapter identity E2E'
 assert r['checks']==22,'case count'
 segments=(O/'run.log').read_text().split('BEGIN scenario')[1:]
 assert len(segments)==22
 for segment in segments:
  number=int(segment.splitlines()[0]);diagnosed='agx_d3dkmt_bridge:' in segment
  assert diagnosed == (number not in [0,19,20]),('failure-only diagnostics',number)
 for section in ['source_files','tool_files']:
  assert all(ent(Path(p))==v for p,v in r[section].items()),'input drift'
 r['result']='PASS'
except Exception as e:r['error']=str(e)
r['artifacts']={str(p.relative_to(O)):ent(p) for p in O.rglob('*') if p.is_file()};(O/'result.json').write_text(json.dumps(r,indent=2)+'\n');print(r['result'],r.get('error'),r.get('checks'));sys.exit(r['result']!='PASS')
