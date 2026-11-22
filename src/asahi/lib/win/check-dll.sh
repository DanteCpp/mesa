#!/bin/sh
# Copyright 2026 The Mesa Authors
# SPDX-License-Identifier: MIT
#
# Link gates for the Windows ICD (G4-PLAN M5), run on the built DLL:
#  1. no C++ runtime, unwinder or pthreads DLL (libc++, libunwind,
#     libwinpthread: everything is linked statically);
#  2. every imported DLL is on the WinPE list below: KERNEL32 and the UCRT
#     (ucrtbase.dll behind the api-ms-win-crt-* API sets, both present in
#     WinPE ARM64 10.0.26100) and the core synch API set (WaitOnAddress, used
#     by Mesa's futex); nothing graphical (no dxgi, d3d12, user32, gdi32);
#  3. the exports are exactly the three ICD entry points of vulkan_api.def.
#
# usage: check-dll.sh vulkan_asahi.dll   (needs llvm-objdump on PATH)
set -eu
dll=${1:?usage: check-dll.sh vulkan_asahi.dll}
dump=$(llvm-objdump -p "$dll")
fail=0

imports=$(printf '%s\n' "$dump" | sed -n 's/^ *DLL Name: //p')
for d in $imports; do
	lc=$(printf '%s' "$d" | tr 'A-Z' 'a-z')
	case "$lc" in
	kernel32.dll|api-ms-win-crt-*-l1-1-0.dll|api-ms-win-core-synch-l1-2-0.dll)
		echo "ok      import $d" ;;
	*)
		echo "FAIL    import $d (not on the WinPE list)"; fail=1 ;;
	esac
done
[ -n "$imports" ] || { echo "FAIL    no import table"; fail=1; }

exports=$(printf '%s\n' "$dump" | sed -n '/^Export Table:/,/^$/p' |
	awk '$1 ~ /^[0-9]+$/ && NF == 3 {print $3}' | sort)
want=$(printf '%s\n' vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr \
	vk_icdNegotiateLoaderICDInterfaceVersion | sort)
if [ "$exports" = "$want" ]; then
	echo "ok      exports: $(echo $exports)"
else
	echo "FAIL    exports: $(echo $exports) (want: $(echo $want))"; fail=1
fi

[ $fail = 0 ] && echo "check-dll: PASS ($dll)" || echo "check-dll: FAIL ($dll)"
exit $fail
