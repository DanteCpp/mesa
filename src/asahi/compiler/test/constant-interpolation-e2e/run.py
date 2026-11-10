#!/usr/bin/env python3
"""Replay actual Honeykrisp placement through G17G AGX; fail targeted mutations."""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess,sys
H=Path(__file__).resolve().parent;M=H.parents[4]
T=Path(os.environ.get('AURORA_TOOLCHAINS') or sys.exit('run.py: set AURORA_TOOLCHAINS to the toolchains folder that holds spirv-llvm-translator-23.1.3'))
P=argparse.ArgumentParser();P.add_argument('--build',type=Path,required=True);P.add_argument('--out',type=Path,required=True);P.add_argument('--mutations',action='store_true');a=P.parse_args();a.build=a.build.resolve();a.out=a.out.resolve();a.out.mkdir()
env=os.environ.copy();env['PATH']='/opt/homebrew/opt/llvm/bin:/opt/homebrew/Caskroom/miniforge/base/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin';env['PKG_CONFIG_PATH']=str(T/'spirv-llvm-translator-23.1.3/lib/pkgconfig')+':/opt/homebrew/opt/spirv-tools/lib/pkgconfig'
ninja='/opt/homebrew/Caskroom/miniforge/base/bin/ninja';target='src/asahi/compiler/g17g_constant_interpolation_e2e';exe=a.build/target
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def file(p):return {'sha256':sha(p),'bytes':p.stat().st_size}
def command(argv,d,name):
 r=subprocess.run(list(map(str,argv)),env=env,cwd=M,capture_output=True)
 (d/(name+'.stdout')).write_bytes(r.stdout);(d/(name+'.stderr')).write_bytes(r.stderr)
 return dict(argv=list(map(str,argv)),returncode=r.returncode)
def step(name,expect_failure=False,legacy=False):
 d=a.out/name;d.mkdir();assert command([sys.executable,'-I','-B',H/'prepare.py'],d,'prepare')['returncode']==0
 build=command([ninja,'-C',a.build,target],d,'build');assert build['returncode']==0,build
 (d/'placement.inc').write_bytes((H/'placement.inc').read_bytes());(d/'placement-binding.json').write_bytes((H/'placement-binding.json').read_bytes())
 r=command([exe,d],d,'replay');text=(d/'replay.stdout').read_text();m=re.search(r'RESULT checks=(\d+) failures=(\d+)',text);assert m,text
 failures=int(m[2]);assert int(m[1])==31
 assert (r['returncode']==1 and failures>0 and 'marker=1 passed=0'in text) if expect_failure else (r['returncode']==0 and failures==0)
 if legacy:assert command([exe,'--legacy-compiler'],d,'legacy')['returncode']==0
 snapshots=[]
 for p in [M/'src/asahi/vulkan/hk_shader.c',M/'src/asahi/compiler/agx_nir_lower_interpolation.c',exe]:
  q=d/p.name;q.write_bytes(p.read_bytes());snapshots.append(q)
 pins={str(p):file(p) for p in snapshots+[d/'placement.inc']}
 return dict(name=name,passed=True,expected_failure=expect_failure,checks=int(m[1]),failures=failures,build=build,replay=r,files=pins)
results=[];hk=M/'src/asahi/vulkan/hk_shader.c';lower=M/'src/asahi/compiler/agx_nir_lower_interpolation.c';original={hk:hk.read_bytes(),lower:lower.read_bytes()}
try:
 results.append(step('healthy',legacy=True))
 if a.mutations:
  s=hk.read_text();needle='         NIR_PASS(_, nir, agx_nir_fix_g17g_constant_interpolation);';assert s.count(needle)==1
  hk.write_text(s.replace(needle,'         ; /* targeted omitted production caller */'))
  results.append(step('omit-caller',True));hk.write_bytes(original[hk])
  s=lower.read_text();needle='b, cf_constant_marker(b, cf),\n          nir_f2fN';assert s.count(needle)==1
  lower.write_text(s.replace(needle,'b, nir_inot(b, cf_valid(b, cf)),\n          nir_f2fN'))
  results.append(step('legacy-predicate',True));lower.write_bytes(original[lower])
  s=hk.read_text();needle='if (dev->dev.chip == AGX_CHIP_G17G)';assert s.count(needle)==1
  hk.write_text(s.replace(needle,'if (dev->dev.chip == AGX_CHIP_G17P)'))
  results.append(step('wrong-chip',True));hk.write_bytes(original[hk])
finally:
 for p,v in original.items():p.write_bytes(v)
 results.append(step('restored',legacy=True))
files={str(p):file(p) for p in sorted(a.out.rglob('*')) if p.is_file()}
source=[H/'run.py',H/'prepare.py',H/'FAILURE-MODEL.md',H.parent/'test-g17g-constant-interpolation.c',H.parent/'test-agx3-graphics-compile.c',M/'src/asahi/vulkan/hk_shader.c',M/'src/asahi/compiler/agx_nir_lower_interpolation.c',M/'src/asahi/compiler/agx_compile.c',M/'src/asahi/lib/agx_device.h',M/'src/asahi/lib/agx_chip.h']
(a.out/'result.json').write_text(json.dumps(dict(schema=1,result='PASS',scope='Actual Honeykrisp placement and G17G compiler; modeled native coefficients; no ISA execution or hardware qualification',cases=results,source_files={str(p):file(p)for p in source},files=files),indent=2)+'\n')
print(json.dumps({'result':'PASS','cases':len(results),'healthy_checks':31,'mutations':sum(x['expected_failure'] for x in results),'artifacts':len(files)}))
