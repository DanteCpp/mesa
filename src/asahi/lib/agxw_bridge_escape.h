/* SPDX-License-Identifier: MIT */
#pragma once

#include "agxw_bridge_core.h"
#include "agxw_bridge_ddi.h"

typedef agx_kmt_status(AGX_KMT_API *agxw_bridge_raw_escape)(
   const struct agx_kmt_escape *);

/* The device path requires the publisher lease held across this whole call.
 * Adapter-only escapes retain their raw KMT adapter identity. */
agx_kmt_status agxw_bridge_escape_dispatch(
   const struct agxw_bridge_provider_core *provider, agx_kmt_handle adapter,
   agxw_bridge_raw_escape raw, pfn_agxw_escape callback,
   const struct agx_kmt_escape *escape);
