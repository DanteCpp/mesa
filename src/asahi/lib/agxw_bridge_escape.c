/* SPDX-License-Identifier: MIT */
#include "agxw_bridge_escape.h"
#include "agxw_contract.h"

#define INVALID_PARAMETER ((agx_kmt_status)0xc000000d)
#define NOT_IMPLEMENTED ((agx_kmt_status)0xc0000002)

agx_kmt_status
agxw_bridge_escape_dispatch(const struct agxw_bridge_provider_core *p,
                           agx_kmt_handle adapter, agxw_bridge_raw_escape raw,
                           pfn_agxw_escape callback,
                           const struct agx_kmt_escape *a)
{
   if (a == NULL || adapter == 0 || a->hAdapter != adapter)
      return INVALID_PARAMETER;

   if (a->hDevice == 0) {
      /* No context token or device sentinel may reach the raw adapter path. */
      if (a->hContext != 0 || raw == NULL)
         return INVALID_PARAMETER;
      return raw(a);
   }

   if (p == NULL || !p->in_use || p->slot >= AGXW_BRIDGE_MAX_DEVICES ||
       a->hDevice != agxw_bridge_sentinel_for_slot(p->slot) ||
       p->hRTAdapter == NULL || p->hRTDevice == NULL || callback == NULL ||
       a->Type != 0 || a->Flags != 0 || a->hContext != 0 ||
       a->pPrivateDriverData == NULL ||
       a->PrivateDriverDataSize < sizeof(struct agxw_escape_header))
      return INVALID_PARAMETER;

   const struct agxw_escape_header *header = a->pPrivateDriverData;
   if (header->op != AGXW_ESC_QUERY_ALLOCATION)
      return NOT_IMPLEMENTED;
   uint32_t required;
   if (a->PrivateDriverDataSize != sizeof(*header) + sizeof(struct agxw_allocation_info) ||
       agxw_escape_validate(header, a->PrivateDriverDataSize, &required) != 0 ||
       required != sizeof(struct agxw_allocation_info))
      return INVALID_PARAMETER;

   const struct agxw_ddi_escape escape = {
      .hDevice = p->hRTDevice,
      .Flags = 0,
      .pPrivateDriverData = a->pPrivateDriverData,
      .PrivateDriverDataSize = a->PrivateDriverDataSize,
      .hContext = NULL,
   };
   return callback(p->hRTAdapter, &escape);
}
