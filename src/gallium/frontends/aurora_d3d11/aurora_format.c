/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * DXGI_FORMAT <-> Gallium pipe_format translation and Windows 11 25H2
 * WDDM 2.0 / Feature Level 11_1 CheckFormatSupport & MSAA contract.
 */
#include "aurora_format.h"

enum pipe_format
aurora_dxgi_to_pipe_format(DXGI_FORMAT fmt)
{
   switch (fmt) {
   case DXGI_FORMAT_UNKNOWN:                  return PIPE_FORMAT_NONE;

   /* 128-bit */
   case DXGI_FORMAT_R32G32B32A32_TYPELESS:
   case DXGI_FORMAT_R32G32B32A32_FLOAT:       return PIPE_FORMAT_R32G32B32A32_FLOAT;
   case DXGI_FORMAT_R32G32B32A32_UINT:        return PIPE_FORMAT_R32G32B32A32_UINT;
   case DXGI_FORMAT_R32G32B32A32_SINT:        return PIPE_FORMAT_R32G32B32A32_SINT;

   /* 96-bit */
   case DXGI_FORMAT_R32G32B32_TYPELESS:
   case DXGI_FORMAT_R32G32B32_FLOAT:          return PIPE_FORMAT_R32G32B32_FLOAT;
   case DXGI_FORMAT_R32G32B32_UINT:           return PIPE_FORMAT_R32G32B32_UINT;
   case DXGI_FORMAT_R32G32B32_SINT:           return PIPE_FORMAT_R32G32B32_SINT;

   /* 64-bit */
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
   case DXGI_FORMAT_R16G16B16A16_FLOAT:       return PIPE_FORMAT_R16G16B16A16_FLOAT;
   case DXGI_FORMAT_R16G16B16A16_UNORM:       return PIPE_FORMAT_R16G16B16A16_UNORM;
   case DXGI_FORMAT_R16G16B16A16_UINT:        return PIPE_FORMAT_R16G16B16A16_UINT;
   case DXGI_FORMAT_R16G16B16A16_SNORM:       return PIPE_FORMAT_R16G16B16A16_SNORM;
   case DXGI_FORMAT_R16G16B16A16_SINT:        return PIPE_FORMAT_R16G16B16A16_SINT;

   case DXGI_FORMAT_R32G32_TYPELESS:
   case DXGI_FORMAT_R32G32_FLOAT:             return PIPE_FORMAT_R32G32_FLOAT;
   case DXGI_FORMAT_R32G32_UINT:              return PIPE_FORMAT_R32G32_UINT;
   case DXGI_FORMAT_R32G32_SINT:              return PIPE_FORMAT_R32G32_SINT;

   case DXGI_FORMAT_R32G8X24_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:     return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
   case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
   case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:  return PIPE_FORMAT_X32_S8X24_UINT;

   /* 32-bit */
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
   case DXGI_FORMAT_R10G10B10A2_UNORM:        return PIPE_FORMAT_R10G10B10A2_UNORM;
   case DXGI_FORMAT_R10G10B10A2_UINT:         return PIPE_FORMAT_R10G10B10A2_UINT;
   case DXGI_FORMAT_R11G11B10_FLOAT:          return PIPE_FORMAT_R11G11B10_FLOAT;

   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
   case DXGI_FORMAT_R8G8B8A8_UNORM:           return PIPE_FORMAT_R8G8B8A8_UNORM;
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:      return PIPE_FORMAT_R8G8B8A8_SRGB;
   case DXGI_FORMAT_R8G8B8A8_UINT:            return PIPE_FORMAT_R8G8B8A8_UINT;
   case DXGI_FORMAT_R8G8B8A8_SNORM:           return PIPE_FORMAT_R8G8B8A8_SNORM;
   case DXGI_FORMAT_R8G8B8A8_SINT:            return PIPE_FORMAT_R8G8B8A8_SINT;

   case DXGI_FORMAT_R16G16_TYPELESS:
   case DXGI_FORMAT_R16G16_FLOAT:             return PIPE_FORMAT_R16G16_FLOAT;
   case DXGI_FORMAT_R16G16_UNORM:             return PIPE_FORMAT_R16G16_UNORM;
   case DXGI_FORMAT_R16G16_UINT:              return PIPE_FORMAT_R16G16_UINT;
   case DXGI_FORMAT_R16G16_SNORM:             return PIPE_FORMAT_R16G16_SNORM;
   case DXGI_FORMAT_R16G16_SINT:              return PIPE_FORMAT_R16G16_SINT;

   case DXGI_FORMAT_R32_TYPELESS:
   case DXGI_FORMAT_R32_FLOAT:                return PIPE_FORMAT_R32_FLOAT;
   case DXGI_FORMAT_D32_FLOAT:                return PIPE_FORMAT_Z32_FLOAT;
   case DXGI_FORMAT_R32_UINT:                 return PIPE_FORMAT_R32_UINT;
   case DXGI_FORMAT_R32_SINT:                 return PIPE_FORMAT_R32_SINT;

   /* Apple Silicon AGX supports Z32_FLOAT_S8X24_UINT natively (no native Z24S8);
    * map D24S8 cleanly onto Z32_FLOAT_S8X24_UINT so every depth+stencil surface works. */
   case DXGI_FORMAT_R24G8_TYPELESS:
   case DXGI_FORMAT_D24_UNORM_S8_UINT:
   case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:    return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
   case DXGI_FORMAT_X24_TYPELESS_G8_UINT:     return PIPE_FORMAT_X32_S8X24_UINT;

   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8A8_UNORM:           return PIPE_FORMAT_B8G8R8A8_UNORM;
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:      return PIPE_FORMAT_B8G8R8A8_SRGB;
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_UNORM:           return PIPE_FORMAT_B8G8R8X8_UNORM;
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:      return PIPE_FORMAT_B8G8R8X8_SRGB;

   case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:       return PIPE_FORMAT_R9G9B9E5_FLOAT;

   /* 16-bit */
   case DXGI_FORMAT_R8G8_TYPELESS:
   case DXGI_FORMAT_R8G8_UNORM:               return PIPE_FORMAT_R8G8_UNORM;
   case DXGI_FORMAT_R8G8_UINT:                return PIPE_FORMAT_R8G8_UINT;
   case DXGI_FORMAT_R8G8_SNORM:               return PIPE_FORMAT_R8G8_SNORM;
   case DXGI_FORMAT_R8G8_SINT:                return PIPE_FORMAT_R8G8_SINT;

   case DXGI_FORMAT_R16_TYPELESS:
   case DXGI_FORMAT_R16_FLOAT:                return PIPE_FORMAT_R16_FLOAT;
   case DXGI_FORMAT_D16_UNORM:                return PIPE_FORMAT_Z16_UNORM;
   case DXGI_FORMAT_R16_UNORM:                return PIPE_FORMAT_R16_UNORM;
   case DXGI_FORMAT_R16_UINT:                 return PIPE_FORMAT_R16_UINT;
   case DXGI_FORMAT_R16_SNORM:                return PIPE_FORMAT_R16_SNORM;
   case DXGI_FORMAT_R16_SINT:                 return PIPE_FORMAT_R16_SINT;

   case DXGI_FORMAT_B5G6R5_UNORM:             return PIPE_FORMAT_B5G6R5_UNORM;
   case DXGI_FORMAT_B5G5R5A1_UNORM:           return PIPE_FORMAT_B5G5R5A1_UNORM;
   case DXGI_FORMAT_B4G4R4A4_UNORM:           return PIPE_FORMAT_B4G4R4A4_UNORM;

   /* 8-bit */
   case DXGI_FORMAT_R8_TYPELESS:
   case DXGI_FORMAT_R8_UNORM:                 return PIPE_FORMAT_R8_UNORM;
   case DXGI_FORMAT_R8_UINT:                  return PIPE_FORMAT_R8_UINT;
   case DXGI_FORMAT_R8_SNORM:                 return PIPE_FORMAT_R8_SNORM;
   case DXGI_FORMAT_R8_SINT:                  return PIPE_FORMAT_R8_SINT;
   case DXGI_FORMAT_A8_UNORM:                 return PIPE_FORMAT_A8_UNORM;

   /* Block-compressed (BC1..BC7) */
   case DXGI_FORMAT_BC1_TYPELESS:
   case DXGI_FORMAT_BC1_UNORM:                return PIPE_FORMAT_DXT1_RGBA;
   case DXGI_FORMAT_BC1_UNORM_SRGB:           return PIPE_FORMAT_DXT1_SRGBA;
   case DXGI_FORMAT_BC2_TYPELESS:
   case DXGI_FORMAT_BC2_UNORM:                return PIPE_FORMAT_DXT3_RGBA;
   case DXGI_FORMAT_BC2_UNORM_SRGB:           return PIPE_FORMAT_DXT3_SRGBA;
   case DXGI_FORMAT_BC3_TYPELESS:
   case DXGI_FORMAT_BC3_UNORM:                return PIPE_FORMAT_DXT5_RGBA;
   case DXGI_FORMAT_BC3_UNORM_SRGB:           return PIPE_FORMAT_DXT5_SRGBA;
   case DXGI_FORMAT_BC4_TYPELESS:
   case DXGI_FORMAT_BC4_UNORM:                return PIPE_FORMAT_RGTC1_UNORM;
   case DXGI_FORMAT_BC4_SNORM:                return PIPE_FORMAT_RGTC1_SNORM;
   case DXGI_FORMAT_BC5_TYPELESS:
   case DXGI_FORMAT_BC5_UNORM:                return PIPE_FORMAT_RGTC2_UNORM;
   case DXGI_FORMAT_BC5_SNORM:                return PIPE_FORMAT_RGTC2_SNORM;
   case DXGI_FORMAT_BC6H_TYPELESS:
   case DXGI_FORMAT_BC6H_UF16:                return PIPE_FORMAT_BPTC_RGB_UFLOAT;
   case DXGI_FORMAT_BC6H_SF16:                return PIPE_FORMAT_BPTC_RGB_FLOAT;
   case DXGI_FORMAT_BC7_TYPELESS:
   case DXGI_FORMAT_BC7_UNORM:                return PIPE_FORMAT_BPTC_RGBA_UNORM;
   case DXGI_FORMAT_BC7_UNORM_SRGB:           return PIPE_FORMAT_BPTC_SRGBA;

   default:
      return PIPE_FORMAT_NONE;
   }
}

enum pipe_format
aurora_dxgi_to_pipe_format_dsv(DXGI_FORMAT fmt)
{
   switch (fmt) {
   case DXGI_FORMAT_R32_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT:            return PIPE_FORMAT_Z32_FLOAT;
   case DXGI_FORMAT_R16_TYPELESS:
   case DXGI_FORMAT_D16_UNORM:            return PIPE_FORMAT_Z16_UNORM;
   case DXGI_FORMAT_R32G8X24_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
   case DXGI_FORMAT_R24G8_TYPELESS:
   case DXGI_FORMAT_D24_UNORM_S8_UINT:    return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
   default:                               return aurora_dxgi_to_pipe_format(fmt);
   }
}

enum pipe_format
aurora_dxgi_to_pipe_format_srv(DXGI_FORMAT fmt)
{
   switch (fmt) {
   case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
   case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:    return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
   case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
   case DXGI_FORMAT_X24_TYPELESS_G8_UINT:     return PIPE_FORMAT_X32_S8X24_UINT;
   default:                                   return aurora_dxgi_to_pipe_format(fmt);
   }
}

bool
aurora_format_is_depth_stencil(DXGI_FORMAT fmt)
{
   switch (fmt) {
   case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
   case DXGI_FORMAT_D32_FLOAT:
   case DXGI_FORMAT_D24_UNORM_S8_UINT:
   case DXGI_FORMAT_D16_UNORM:
      return true;
   default:
      return false;
   }
}

bool
aurora_format_is_typeless(DXGI_FORMAT fmt)
{
   switch (fmt) {
   case DXGI_FORMAT_R32G32B32A32_TYPELESS:
   case DXGI_FORMAT_R32G32B32_TYPELESS:
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
   case DXGI_FORMAT_R32G32_TYPELESS:
   case DXGI_FORMAT_R32G8X24_TYPELESS:
   case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
   case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
   case DXGI_FORMAT_R16G16_TYPELESS:
   case DXGI_FORMAT_R32_TYPELESS:
   case DXGI_FORMAT_R24G8_TYPELESS:
   case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
   case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
   case DXGI_FORMAT_R8G8_TYPELESS:
   case DXGI_FORMAT_R16_TYPELESS:
   case DXGI_FORMAT_R8_TYPELESS:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
   case DXGI_FORMAT_BC1_TYPELESS:
   case DXGI_FORMAT_BC2_TYPELESS:
   case DXGI_FORMAT_BC3_TYPELESS:
   case DXGI_FORMAT_BC4_TYPELESS:
   case DXGI_FORMAT_BC5_TYPELESS:
   case DXGI_FORMAT_BC6H_TYPELESS:
   case DXGI_FORMAT_BC7_TYPELESS:
      return true;
   default:
      return false;
   }
}

bool
aurora_format_is_srgb(DXGI_FORMAT fmt)
{
   switch (fmt) {
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
   case DXGI_FORMAT_BC1_UNORM_SRGB:
   case DXGI_FORMAT_BC2_UNORM_SRGB:
   case DXGI_FORMAT_BC3_UNORM_SRGB:
   case DXGI_FORMAT_BC7_UNORM_SRGB:
      return true;
   default:
      return false;
   }
}

uint32_t
aurora_dxgi_to_d3dddi_format(DXGI_FORMAT fmt)
{
   switch (fmt) {
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return (uint32_t)D3DDDIFMT_A8R8G8B8;
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return (uint32_t)D3DDDIFMT_X8R8G8B8;
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return (uint32_t)D3DDDIFMT_A8B8G8R8;
   case DXGI_FORMAT_R10G10B10A2_UNORM:   return (uint32_t)D3DDDIFMT_A2B10G10R10;
   case DXGI_FORMAT_R16G16B16A16_FLOAT:  return (uint32_t)D3DDDIFMT_A16B16G16R16F;
   default:                              return 0u;
   }
}

DXGI_FORMAT
aurora_d3dddi_to_dxgi_format(uint32_t d3d_fmt)
{
   switch ((D3DDDIFORMAT)d3d_fmt) {
   case D3DDDIFMT_A8R8G8B8:       return DXGI_FORMAT_B8G8R8A8_UNORM;
   case D3DDDIFMT_X8R8G8B8:       return DXGI_FORMAT_B8G8R8X8_UNORM;
   case D3DDDIFMT_A8B8G8R8:       return DXGI_FORMAT_R8G8B8A8_UNORM;
   case D3DDDIFMT_A2B10G10R10:    return DXGI_FORMAT_R10G10B10A2_UNORM;
   case D3DDDIFMT_A16B16G16R16F:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
   default:                       return DXGI_FORMAT_UNKNOWN;
   }
}

/* =========================================================================
 * Windows 11 25H2 WDDM 2.0 / FL 11_1 CheckFormatSupport & MSAA Contract
 * ========================================================================= */

static bool
fmt_is_integer_rt(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
   case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT:
   case DXGI_FORMAT_R32G32_UINT:       case DXGI_FORMAT_R32G32_SINT:
   case DXGI_FORMAT_R10G10B10A2_UINT:
   case DXGI_FORMAT_R8G8B8A8_UINT:     case DXGI_FORMAT_R8G8B8A8_SINT:
   case DXGI_FORMAT_R16G16_UINT:       case DXGI_FORMAT_R16G16_SINT:
   case DXGI_FORMAT_R32_UINT:          case DXGI_FORMAT_R32_SINT:
   case DXGI_FORMAT_R8G8_UINT:         case DXGI_FORMAT_R8G8_SINT:
   case DXGI_FORMAT_R16_UINT:          case DXGI_FORMAT_R16_SINT:
   case DXGI_FORMAT_R8_UINT:           case DXGI_FORMAT_R8_SINT:
      return true;
   default:
      return false;
   }
}

static bool
fmt_carries_depth_msaa(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_R32G8X24_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
   case DXGI_FORMAT_R32_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT:
   case DXGI_FORMAT_R24G8_TYPELESS:
   case DXGI_FORMAT_D24_UNORM_S8_UINT:
   case DXGI_FORMAT_R16_TYPELESS:
   case DXGI_FORMAT_D16_UNORM:
      return true;
   default:
      return false;
   }
}

static bool
fmt_is_depth_stencil_view_only(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
   case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
   case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
   case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
      return true;
   default:
      return false;
   }
}

static bool
fmt_may_be_not_supported(DXGI_FORMAT f)
{
   /* Exact bounded optional/video set permitted to return NOT_SUPPORTED (0x80000000)
    * by the Windows 11 25H2 D3D11 runtime; other unsupported formats must return 0. */
   switch (f) {
   case DXGI_FORMAT_A8P8:
   case DXGI_FORMAT_AI44:
   case DXGI_FORMAT_AYUV:
   case DXGI_FORMAT_IA44:
   case DXGI_FORMAT_NV11:
   case DXGI_FORMAT_P010:
   case DXGI_FORMAT_P016:
   case DXGI_FORMAT_P8:
   case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
   case DXGI_FORMAT_Y210:
   case DXGI_FORMAT_Y216:
   case DXGI_FORMAT_Y410:
   case DXGI_FORMAT_Y416:
      return true;
   default:
      return false;
   }
}

static bool
fmt_supports_typed_uav(DXGI_FORMAT f)
{
   switch (f) {
   /* Required for FL 11_0/11_1 */
   case DXGI_FORMAT_R32_FLOAT:
   case DXGI_FORMAT_R32_UINT:
   case DXGI_FORMAT_R32_SINT:
   /* Common typed UAV formats supported on AGX */
   case DXGI_FORMAT_R32G32B32A32_FLOAT:
   case DXGI_FORMAT_R32G32B32A32_UINT:
   case DXGI_FORMAT_R32G32B32A32_SINT:
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16B16A16_UNORM:
   case DXGI_FORMAT_R16G16B16A16_UINT:
   case DXGI_FORMAT_R16G16B16A16_SNORM:
   case DXGI_FORMAT_R16G16B16A16_SINT:
   case DXGI_FORMAT_R32G32_FLOAT:
   case DXGI_FORMAT_R32G32_UINT:
   case DXGI_FORMAT_R32G32_SINT:
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10A2_UINT:
   case DXGI_FORMAT_R11G11B10_FLOAT:
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UINT:
   case DXGI_FORMAT_R8G8B8A8_SNORM:
   case DXGI_FORMAT_R8G8B8A8_SINT:
   case DXGI_FORMAT_R16G16_FLOAT:
   case DXGI_FORMAT_R16G16_UNORM:
   case DXGI_FORMAT_R16G16_UINT:
   case DXGI_FORMAT_R16G16_SNORM:
   case DXGI_FORMAT_R16G16_SINT:
   case DXGI_FORMAT_R8G8_UNORM:
   case DXGI_FORMAT_R8G8_UINT:
   case DXGI_FORMAT_R8G8_SNORM:
   case DXGI_FORMAT_R8G8_SINT:
   case DXGI_FORMAT_R16_FLOAT:
   case DXGI_FORMAT_R16_UNORM:
   case DXGI_FORMAT_R16_UINT:
   case DXGI_FORMAT_R16_SNORM:
   case DXGI_FORMAT_R16_SINT:
   case DXGI_FORMAT_R8_UNORM:
   case DXGI_FORMAT_R8_UINT:
   case DXGI_FORMAT_R8_SNORM:
   case DXGI_FORMAT_R8_SINT:
   case DXGI_FORMAT_B8G8R8A8_UNORM:
      return true;
   default:
      return false;
   }
}

static bool
fmt_is_color_rt(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_R32G32B32A32_FLOAT:
   case DXGI_FORMAT_R32G32B32A32_UINT:
   case DXGI_FORMAT_R32G32B32A32_SINT:
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16B16A16_UNORM:
   case DXGI_FORMAT_R16G16B16A16_UINT:
   case DXGI_FORMAT_R16G16B16A16_SNORM:
   case DXGI_FORMAT_R16G16B16A16_SINT:
   case DXGI_FORMAT_R32G32_FLOAT:
   case DXGI_FORMAT_R32G32_UINT:
   case DXGI_FORMAT_R32G32_SINT:
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10A2_UINT:
   case DXGI_FORMAT_R11G11B10_FLOAT:
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_UINT:
   case DXGI_FORMAT_R8G8B8A8_SNORM:
   case DXGI_FORMAT_R8G8B8A8_SINT:
   case DXGI_FORMAT_R16G16_FLOAT:
   case DXGI_FORMAT_R16G16_UNORM:
   case DXGI_FORMAT_R16G16_UINT:
   case DXGI_FORMAT_R16G16_SNORM:
   case DXGI_FORMAT_R16G16_SINT:
   case DXGI_FORMAT_R32_FLOAT:
   case DXGI_FORMAT_R32_UINT:
   case DXGI_FORMAT_R32_SINT:
   case DXGI_FORMAT_R8G8_UNORM:
   case DXGI_FORMAT_R8G8_UINT:
   case DXGI_FORMAT_R8G8_SNORM:
   case DXGI_FORMAT_R8G8_SINT:
   case DXGI_FORMAT_R16_FLOAT:
   case DXGI_FORMAT_R16_UNORM:
   case DXGI_FORMAT_R16_UINT:
   case DXGI_FORMAT_R16_SNORM:
   case DXGI_FORMAT_R16_SINT:
   case DXGI_FORMAT_R8_UNORM:
   case DXGI_FORMAT_R8_UINT:
   case DXGI_FORMAT_R8_SNORM:
   case DXGI_FORMAT_R8_SINT:
   case DXGI_FORMAT_A8_UNORM:
   case DXGI_FORMAT_B5G6R5_UNORM:
   case DXGI_FORMAT_B5G5R5A1_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return true;
   default:
      return false;
   }
}

static bool
fmt_is_typeless_color_msaa(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_R32G32B32A32_TYPELESS:
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
   case DXGI_FORMAT_R32G32_TYPELESS:
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
   case DXGI_FORMAT_R16G16_TYPELESS:
   case DXGI_FORMAT_R8G8_TYPELESS:
   case DXGI_FORMAT_R8_TYPELESS:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      return true;
   default:
      return false;
   }
}

static bool
fmt_is_vertex_buffer(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_R32G32B32A32_FLOAT:
   case DXGI_FORMAT_R32G32B32A32_UINT:
   case DXGI_FORMAT_R32G32B32A32_SINT:
   case DXGI_FORMAT_R32G32B32_FLOAT:
   case DXGI_FORMAT_R32G32B32_UINT:
   case DXGI_FORMAT_R32G32B32_SINT:
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16B16A16_UNORM:
   case DXGI_FORMAT_R16G16B16A16_UINT:
   case DXGI_FORMAT_R16G16B16A16_SNORM:
   case DXGI_FORMAT_R16G16B16A16_SINT:
   case DXGI_FORMAT_R32G32_FLOAT:
   case DXGI_FORMAT_R32G32_UINT:
   case DXGI_FORMAT_R32G32_SINT:
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10A2_UINT:
   case DXGI_FORMAT_R11G11B10_FLOAT:
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UINT:
   case DXGI_FORMAT_R8G8B8A8_SNORM:
   case DXGI_FORMAT_R8G8B8A8_SINT:
   case DXGI_FORMAT_R16G16_FLOAT:
   case DXGI_FORMAT_R16G16_UNORM:
   case DXGI_FORMAT_R16G16_UINT:
   case DXGI_FORMAT_R16G16_SNORM:
   case DXGI_FORMAT_R16G16_SINT:
   case DXGI_FORMAT_R32_FLOAT:
   case DXGI_FORMAT_R32_UINT:
   case DXGI_FORMAT_R32_SINT:
   case DXGI_FORMAT_R8G8_UNORM:
   case DXGI_FORMAT_R8G8_UINT:
   case DXGI_FORMAT_R8G8_SNORM:
   case DXGI_FORMAT_R8G8_SINT:
   case DXGI_FORMAT_R16_FLOAT:
   case DXGI_FORMAT_R16_UNORM:
   case DXGI_FORMAT_R16_UINT:
   case DXGI_FORMAT_R16_SNORM:
   case DXGI_FORMAT_R16_SINT:
   case DXGI_FORMAT_R8_UNORM:
   case DXGI_FORMAT_R8_UINT:
   case DXGI_FORMAT_R8_SNORM:
   case DXGI_FORMAT_R8_SINT:
   case DXGI_FORMAT_B8G8R8A8_UNORM:
      return true;
   default:
      return false;
   }
}

static bool
fmt_is_bc(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_BC1_TYPELESS:
   case DXGI_FORMAT_BC1_UNORM:
   case DXGI_FORMAT_BC1_UNORM_SRGB:
   case DXGI_FORMAT_BC2_TYPELESS:
   case DXGI_FORMAT_BC2_UNORM:
   case DXGI_FORMAT_BC2_UNORM_SRGB:
   case DXGI_FORMAT_BC3_TYPELESS:
   case DXGI_FORMAT_BC3_UNORM:
   case DXGI_FORMAT_BC3_UNORM_SRGB:
   case DXGI_FORMAT_BC4_TYPELESS:
   case DXGI_FORMAT_BC4_UNORM:
   case DXGI_FORMAT_BC4_SNORM:
   case DXGI_FORMAT_BC5_TYPELESS:
   case DXGI_FORMAT_BC5_UNORM:
   case DXGI_FORMAT_BC5_SNORM:
   case DXGI_FORMAT_BC6H_TYPELESS:
   case DXGI_FORMAT_BC6H_UF16:
   case DXGI_FORMAT_BC6H_SF16:
   case DXGI_FORMAT_BC7_TYPELESS:
   case DXGI_FORMAT_BC7_UNORM:
   case DXGI_FORMAT_BC7_UNORM_SRGB:
      return true;
   default:
      return false;
   }
}

static UINT
compute_format_support(struct pipe_screen *screen, DXGI_FORMAT Format)
{
   if (fmt_may_be_not_supported(Format))
      return D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;

   enum pipe_format pf = aurora_dxgi_to_pipe_format(Format);
   if (pf == PIPE_FORMAT_NONE)
      return 0u;

   UINT caps = 0u;
   const bool typeless = aurora_format_is_typeless(Format);
   const bool depth_carrier = fmt_carries_depth_msaa(Format);
   const bool depth_view_only = fmt_is_depth_stencil_view_only(Format);
   const bool color_rt = fmt_is_color_rt(Format);
   const bool int_rt = fmt_is_integer_rt(Format);
   const bool bc = fmt_is_bc(Format);

   /* Sample / Gather / Buffer / VB */
   if (!typeless && !aurora_format_is_depth_stencil(Format)) {
      if (screen->is_format_supported(screen, pf, PIPE_TEXTURE_2D, 0, 0,
                                      PIPE_BIND_SAMPLER_VIEW)) {
         if (!int_rt && Format != DXGI_FORMAT_R32G32B32_UINT &&
             Format != DXGI_FORMAT_R32G32B32_SINT &&
             Format != DXGI_FORMAT_X32_TYPELESS_G8X24_UINT &&
             Format != DXGI_FORMAT_X24_TYPELESS_G8_UINT) {
            caps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
         }
         caps |= D3D11_1DDI_FORMAT_SUPPORT_SHADER_GATHER;
      }
   } else if (Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
              Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) {
      caps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE |
              D3D11_1DDI_FORMAT_SUPPORT_SHADER_GATHER;
   }

   if (fmt_is_vertex_buffer(Format)) {
      caps |= D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER |
              D3D11_1DDI_FORMAT_SUPPORT_BUFFER;
   } else if (Format == DXGI_FORMAT_R32G32B32_FLOAT ||
              Format == DXGI_FORMAT_R32G32B32_UINT ||
              Format == DXGI_FORMAT_R32G32B32_SINT) {
      caps |= D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER |
              D3D11_1DDI_FORMAT_SUPPORT_BUFFER;
   }

   /* Render Target + Blendable + LogicOp */
   if (color_rt) {
      caps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
      if (!int_rt) {
         caps |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
      } else {
         /* D3D11.1 mandates OUTPUT_MERGER_LOGIC_OP on integer RTs ONLY (never float). */
         caps |= D3D11_1DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP;
      }
      if (Format != DXGI_FORMAT_A8_UNORM) {
         caps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET |
                 D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
      }
   }

   /* Depth MSAA carriers + typeless color MSAA carriers (NEVER view-only X-padded formats!) */
   if (!depth_view_only) {
      if (depth_carrier) {
         caps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
      } else if (fmt_is_typeless_color_msaa(Format)) {
         caps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET |
                 D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET |
                 D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
      }
   } else {
      /* X-padded depth/stencil view formats support multisample load (ld_ms) but NOT MSAA RT. */
      caps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
   }

   /* Typed UAV load/store (mandatory for R32_{FLOAT,UINT,SINT} at FL 11_1) */
   if (fmt_supports_typed_uav(Format)) {
      caps |= D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES |
              D3DWDDM2_0DDI_FORMAT_SUPPORT_UAV_READS;
   }

   /* BC formats: sample + gather, never RT/MSAA/VB */
   if (bc && !typeless) {
      caps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE |
              D3D11_1DDI_FORMAT_SUPPORT_SHADER_GATHER;
   }

   return caps;
}

void APIENTRY
aurora_CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                          UINT *pFormatCaps)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!pFormatCaps)
      return;
   if (!dev || !dev->screen) {
      *pFormatCaps = 0u;
      return;
   }

   UINT idx = (UINT)Format;
   if (idx < ARRAY_SIZE(dev->fmt_caps_cache) &&
       InterlockedCompareExchange(&dev->fmt_caps_valid[idx], 0, 0) != 0) {
      *pFormatCaps = dev->fmt_caps_cache[idx];
      return;
   }

   UINT caps = compute_format_support(dev->screen, Format);
   if (idx < ARRAY_SIZE(dev->fmt_caps_cache)) {
      dev->fmt_caps_cache[idx] = caps;
      InterlockedExchange(&dev->fmt_caps_valid[idx], 1);
   }
   *pFormatCaps = caps;
}

static UINT
msaa_quality_levels(struct aurora_device *dev, DXGI_FORMAT Format,
                    UINT SampleCount)
{
   if (SampleCount != 1u && SampleCount != 2u && SampleCount != 4u)
      return 0u;

   UINT caps = 0u;
   D3D10DDI_HDEVICE hdev = { .pDrvPrivate = dev };
   aurora_CheckFormatSupport(hdev, Format, &caps);
   if (caps & D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED)
      return 0u;

   if (SampleCount == 1u) {
      /* Sample count 1 is valid for any supported format with RT/depth/sample/buffer. */
      return (caps != 0u || aurora_format_is_typeless(Format)) ? 1u : 0u;
   }

   return (caps & D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) ? 1u : 0u;
}

void APIENTRY
aurora_CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                     DXGI_FORMAT Format, UINT SampleCount,
                                     UINT *pNumQualityLevels)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!pNumQualityLevels)
      return;
   *pNumQualityLevels = msaa_quality_levels(dev, Format, SampleCount);
}

void APIENTRY
aurora_CheckMultisampleQualityLevels_1_3(D3D10DDI_HDEVICE hDevice,
                                         DXGI_FORMAT Format, UINT SampleCount,
                                         UINT Flags, UINT *pNumQualityLevels)
{
   (void)Flags;
   aurora_CheckMultisampleQualityLevels(hDevice, Format, SampleCount,
                                        pNumQualityLevels);
}
