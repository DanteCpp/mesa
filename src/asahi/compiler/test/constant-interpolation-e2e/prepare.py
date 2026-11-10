#!/usr/bin/env python3
"""Compile the exact production interpolation-placement block, not a mirrored if."""
from pathlib import Path
import hashlib,json
H=Path(__file__).resolve().parent;M=H.parents[4]
p=M/'src/asahi/vulkan/hk_shader.c';s=p.read_text();start=s.index('      /* G17P exposes the raster layer through the flat coefficient table.')
end=s.index('      if (agx_native_graphics(&dev->dev)) {',start);block=s[start:end]
# Source extraction includes any actual G17G caller added within this block;
# absence/omission leaves the numerical counterexample failing.
device=M/'src/asahi/lib/agx_device.h';ds=device.read_text()
a=ds.index('static inline bool\nagx_native_graphics(');z=ds.index('\n}',a)+2;helper=ds[a:z]
# The only accessed device field is chip. Copy the owning selector body, too;
# no whole-device/DRM runtime is claimed by this offline compiler workflow.
header='struct agx_device { enum agx_chip chip; };\n'+helper+'\nstatic void production_interpolation(nir_shader *nir, enum agx_chip chip)\n{\n struct {struct agx_device dev;} object={.dev.chip=chip};\n const __typeof__(object) *dev=&object;\n'
(H/'placement.inc').write_text(header+block+'}\n')
(H/'placement-binding.json').write_text(json.dumps(dict(source=str(p),source_sha256=hashlib.sha256(p.read_bytes()).hexdigest(),block_sha256=hashlib.sha256(block.encode()).hexdigest(),block=block,device_source=str(device),device_sha256=hashlib.sha256(device.read_bytes()).hexdigest(),selector=helper),indent=2)+'\n')
