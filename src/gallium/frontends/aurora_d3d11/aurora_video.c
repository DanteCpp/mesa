/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Truthful zero-capability D3D11.1 / WDDM 2.0 / WDDM 2.1 video subobject.
 *
 * D3D11.1 / WDDM 2.x requires drivers exposing the 11.1+ graphics DDI to make
 * the video subobject retrievable via RetrieveSubObject so callers (including
 * dwmcore.dll and d2d1.dll) can complete capability discovery rather than
 * aborting on E_NOINTERFACE. All methods truthfully report zero hardware video
 * decode/processing capabilities.
 */
#include "aurora_d3d11.h"

#include <string.h>

struct aurora_video_processor_enum {
   UINT32 marker;
};

static void APIENTRY
video_GetDecoderProfileCount(D3D10DDI_HDEVICE hDevice, UINT *count)
{
   (void)hDevice;
   if (count) *count = 0;
}

static void APIENTRY
video_GetDecoderProfile(D3D10DDI_HDEVICE hDevice, UINT index, GUID *profile)
{
   (void)hDevice; (void)index;
   if (profile) memset(profile, 0, sizeof(*profile));
}

static void APIENTRY
video_CheckDecoderFormat(D3D10DDI_HDEVICE hDevice, const GUID *profile,
                         DXGI_FORMAT format, BOOL *supported)
{
   (void)hDevice; (void)profile; (void)format;
   if (supported) *supported = FALSE;
}

static void APIENTRY
video_GetDecoderConfigCount(D3D10DDI_HDEVICE hDevice,
                            const D3D11_1DDI_VIDEO_DECODER_DESC *desc,
                            UINT *count)
{
   (void)hDevice; (void)desc;
   if (count) *count = 0;
}

static void APIENTRY
video_GetDecoderConfig(D3D10DDI_HDEVICE hDevice,
                       const D3D11_1DDI_VIDEO_DECODER_DESC *desc, UINT index,
                       D3D11_1DDI_VIDEO_DECODER_CONFIG *config)
{
   (void)hDevice; (void)desc; (void)index;
   if (config) memset(config, 0, sizeof(*config));
}

static void APIENTRY
video_GetDecoderBufferTypeCount(D3D10DDI_HDEVICE hDevice,
                                const D3D11_1DDI_VIDEO_DECODER_DESC *desc,
                                UINT *count)
{
   (void)hDevice; (void)desc;
   if (count) *count = 0;
}

static void APIENTRY
video_GetDecoderBufferInfo(D3D10DDI_HDEVICE hDevice,
                           const D3D11_1DDI_VIDEO_DECODER_DESC *desc,
                           UINT index,
                           D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO *info)
{
   (void)hDevice; (void)desc; (void)index;
   if (info) memset(info, 0, sizeof(*info));
}

static SIZE_T APIENTRY
video_CalcDecoderSize(D3D10DDI_HDEVICE hDevice,
                      const D3D11_1DDIARG_CREATEVIDEODECODER *args)
{
   (void)hDevice; (void)args;
   return 0;
}

static HRESULT APIENTRY
video_CreateDecoder(D3D10DDI_HDEVICE hDevice,
                    const D3D11_1DDIARG_CREATEVIDEODECODER *args,
                    D3D11_1DDI_HDECODE hDecoder,
                    D3D11_1DDI_HRTDECODE hRuntimeDecoder)
{
   (void)hDevice; (void)args; (void)hDecoder; (void)hRuntimeDecoder;
   return E_NOTIMPL;
}

static void APIENTRY
video_DestroyDecoder(D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecoder)
{
   (void)hDevice; (void)hDecoder;
}

static HRESULT APIENTRY
video_DecoderExtension(D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecoder,
                       const D3D11_1DDIARG_VIDEODECODEREXTENSION *args)
{
   (void)hDevice; (void)hDecoder; (void)args;
   return E_NOTIMPL;
}

static HRESULT APIENTRY
video_DecoderBeginFrame(D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecoder,
                        const D3D11_1DDIARG_VIDEODECODERBEGINFRAME *args)
{
   (void)hDevice; (void)hDecoder; (void)args;
   return E_NOTIMPL;
}

static void APIENTRY
video_DecoderEndFrame(D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecoder)
{
   (void)hDevice; (void)hDecoder;
}

static HRESULT APIENTRY
video_DecoderSubmitBuffers(D3D10DDI_HDEVICE hDevice,
                           D3D11_1DDI_HDECODE hDecoder, UINT bufferCount,
                           const D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *buffers)
{
   (void)hDevice; (void)hDecoder; (void)bufferCount; (void)buffers;
   return E_NOTIMPL;
}

static SIZE_T APIENTRY
video_CalcProcessorEnumSize(D3D10DDI_HDEVICE hDevice,
                            const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *args)
{
   (void)hDevice; (void)args;
   return sizeof(struct aurora_video_processor_enum);
}

static HRESULT APIENTRY
video_CreateProcessorEnum(D3D10DDI_HDEVICE hDevice,
                          const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *args,
                          D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
                          D3D11_1DDI_HRTVIDEOPROCESSORENUM hRuntimeEnum)
{
   (void)hDevice; (void)hRuntimeEnum;
   if (!args || !hEnum.pDrvPrivate)
      return E_INVALIDARG;
   ((struct aurora_video_processor_enum *)hEnum.pDrvPrivate)->marker = 0;
   return S_OK;
}

static void APIENTRY
video_DestroyProcessorEnum(D3D10DDI_HDEVICE hDevice,
                           D3D11_1DDI_HVIDEOPROCESSORENUM hEnum)
{
   (void)hDevice;
   if (hEnum.pDrvPrivate)
      ((struct aurora_video_processor_enum *)hEnum.pDrvPrivate)->marker = 0;
}

static void APIENTRY
video_CheckProcessorFormat(D3D10DDI_HDEVICE hDevice,
                           D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
                           DXGI_FORMAT format, UINT *flags)
{
   (void)hDevice; (void)hEnum; (void)format;
   if (flags) *flags = 0;
}

static void APIENTRY
video_GetProcessorCaps(D3D10DDI_HDEVICE hDevice,
                       D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
                       D3D11_1DDI_VIDEO_PROCESSOR_CAPS *caps)
{
   (void)hDevice; (void)hEnum;
   if (caps) memset(caps, 0, sizeof(*caps));
}

static void APIENTRY
video_GetRateConversionCaps(D3D10DDI_HDEVICE hDevice,
                            D3D11_1DDI_HVIDEOPROCESSORENUM hEnum, UINT index,
                            D3D11_1DDI_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *caps)
{
   (void)hDevice; (void)hEnum; (void)index;
   if (caps) memset(caps, 0, sizeof(*caps));
}

static void APIENTRY
video_GetCustomRate(D3D10DDI_HDEVICE hDevice,
                    D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
                    UINT rateIndex, UINT customRateIndex,
                    D3D11_1DDI_VIDEO_PROCESSOR_CUSTOM_RATE *rate)
{
   (void)hDevice; (void)hEnum; (void)rateIndex; (void)customRateIndex;
   if (rate) memset(rate, 0, sizeof(*rate));
}

static void APIENTRY
video_GetFilterRange(D3D10DDI_HDEVICE hDevice,
                     D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
                     D3D11_1DDI_VIDEO_PROCESSOR_FILTER filter,
                     D3D11_1DDI_VIDEO_PROCESSOR_FILTER_RANGE *range)
{
   (void)hDevice; (void)hEnum; (void)filter;
   if (range) memset(range, 0, sizeof(*range));
}

static SIZE_T APIENTRY
video_CalcProcessorSize(D3D10DDI_HDEVICE hDevice,
                        const D3D11_1DDIARG_CREATEVIDEOPROCESSOR *args)
{
   (void)hDevice; (void)args;
   return 0;
}

static HRESULT APIENTRY
video_CreateProcessor(D3D10DDI_HDEVICE hDevice,
                      const D3D11_1DDIARG_CREATEVIDEOPROCESSOR *args,
                      D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
                      D3D11_1DDI_HRTVIDEOPROCESSOR hRuntimeProcessor)
{
   (void)hDevice; (void)args; (void)hProcessor; (void)hRuntimeProcessor;
   return E_NOTIMPL;
}

static void APIENTRY
video_DestroyProcessor(D3D10DDI_HDEVICE hDevice,
                       D3D11_1DDI_HVIDEOPROCESSOR hProcessor)
{
   (void)hDevice; (void)hProcessor;
}

static void
fill_video_11_1(D3D11_1DDI_VIDEODEVICEFUNCS *f)
{
   memset(f, 0, sizeof(*f));
   f->pfnGetVideoDecoderProfileCount        = video_GetDecoderProfileCount;
   f->pfnGetVideoDecoderProfile             = video_GetDecoderProfile;
   f->pfnCheckVideoDecoderFormat            = video_CheckDecoderFormat;
   f->pfnGetVideoDecoderConfigCount         = video_GetDecoderConfigCount;
   f->pfnGetVideoDecoderConfig              = video_GetDecoderConfig;
   f->pfnGetVideoDecoderBufferTypeCount     = video_GetDecoderBufferTypeCount;
   f->pfnGetVideoDecoderBufferInfo          = video_GetDecoderBufferInfo;
   f->pfnCalcPrivateVideoDecoderSize        = video_CalcDecoderSize;
   f->pfnCreateVideoDecoder                 = video_CreateDecoder;
   f->pfnDestroyVideoDecoder                = video_DestroyDecoder;
   f->pfnVideoDecoderExtension              = video_DecoderExtension;
   f->pfnVideoDecoderBeginFrame             = video_DecoderBeginFrame;
   f->pfnVideoDecoderEndFrame               = video_DecoderEndFrame;
   f->pfnVideoDecoderSubmitBuffers          = video_DecoderSubmitBuffers;
   f->pfnCalcPrivateVideoProcessorEnumSize  = video_CalcProcessorEnumSize;
   f->pfnCreateVideoProcessorEnum           = video_CreateProcessorEnum;
   f->pfnDestroyVideoProcessorEnum          = video_DestroyProcessorEnum;
   f->pfnCheckVideoProcessorFormat          = video_CheckProcessorFormat;
   f->pfnGetVideoProcessorCaps              = video_GetProcessorCaps;
   f->pfnGetVideoProcessorRateConversionCaps = video_GetRateConversionCaps;
   f->pfnGetVideoProcessorCustomRate        = video_GetCustomRate;
   f->pfnGetVideoProcessorFilterRange       = video_GetFilterRange;
   f->pfnCalcPrivateVideoProcessorSize      = video_CalcProcessorSize;
   f->pfnCreateVideoProcessor               = video_CreateProcessor;
   f->pfnDestroyVideoProcessor              = video_DestroyProcessor;
}

#if D3D11DDI_MINOR_HEADER_VERSION >= 5
static void APIENTRY
video_CheckProcessorFormatConversion(
   D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
   D3DWDDM2_0DDI_CHECK_VIDEO_PROCESSOR_FORMAT_CONVERSION *conv)
{
   (void)hDevice; (void)hEnum;
   if (conv) conv->Supported = FALSE;
}

static void APIENTRY
video_QueryCapabilities(D3D10DDI_HDEVICE hDevice,
                        D3DWDDM2_0DDI_VIDEO_CAPABILITY_QUERY query,
                        UINT dataSize, void *data)
{
   (void)hDevice;
   if (!data)
      return;
   switch (query) {
   case D3DWDDM2_0DDI_VIDEO_CAPABILITY_QUERY_DECODER_DOWNSAMPLING:
      if (dataSize >= sizeof(D3DWDDM2_0DDI_VIDEO_CAPABILITY_DECODER_DOWNSAMPLING)) {
         D3DWDDM2_0DDI_VIDEO_CAPABILITY_DECODER_DOWNSAMPLING *c = data;
         c->Supported = FALSE;
         c->RealTime = FALSE;
      }
      break;
   case D3DWDDM2_0DDI_VIDEO_CAPABILITY_QUERY_DECODER_CAPS:
      if (dataSize >= sizeof(D3DWDDM2_0DDI_VIDEO_CAPABILITY_DECODER_CAPS)) {
         D3DWDDM2_0DDI_VIDEO_CAPABILITY_DECODER_CAPS *c = data;
         c->Caps = D3DWDDM2_0DDI_VIDEO_DECODER_CAP_UNSUPPORTED;
      }
      break;
   case D3DWDDM2_0DDI_VIDEO_CAPABILITY_QUERY_DECODER_DOWNSAMPLE_OUTPUT_FORMAT:
      if (dataSize >= sizeof(D3DWDDM2_0DDI_VIDEO_CAPABILITY_DECODER_DOWNSAMPLE_OUTPUT_FORMAT)) {
         D3DWDDM2_0DDI_VIDEO_CAPABILITY_DECODER_DOWNSAMPLE_OUTPUT_FORMAT *c = data;
         c->Supported = FALSE;
      }
      break;
   default:
      break;
   }
}

static void
fill_video_wddm2_0(D3DWDDM2_0DDI_VIDEODEVICEFUNCS *f)
{
   memset(f, 0, sizeof(*f));
   fill_video_11_1((D3D11_1DDI_VIDEODEVICEFUNCS *)f);
   f->pfnQueryVideoCapabilities = video_QueryCapabilities;
   f->pfnCheckVideoProcessorFormatConversion = video_CheckProcessorFormatConversion;
}
#endif

HRESULT APIENTRY
aurora_RetrieveSubObject(D3D10DDI_HDEVICE hDevice, UINT32 SubDeviceID,
                         SIZE_T ParamSize, void *pParams,
                         SIZE_T OutputParamSize, void *pOutputParamsBuffer)
{
   static volatile LONG subobj_logs = 0;
   (void)hDevice;

   LONG n = InterlockedIncrement(&subobj_logs);
   if (n <= 16) {
      aurora_log("RetrieveSubObject: id=%u param=%zu out=%zu",
                 (unsigned)SubDeviceID, ParamSize, OutputParamSize);
   }
   if (!pParams || !pOutputParamsBuffer)
      return E_INVALIDARG;

   switch (SubDeviceID) {
   case D3D11_1DDI_VIDEO_FUNCTIONS: {
      D3D11_1DDI_VIDEO_INPUT *in = (D3D11_1DDI_VIDEO_INPUT *)pParams;
      if (ParamSize < sizeof(*in) ||
          OutputParamSize < sizeof(D3D11_1DDI_VIDEO_OUTPUT) ||
          !in->p11VideoDeviceFuncs)
         return E_INVALIDARG;
      fill_video_11_1(in->p11VideoDeviceFuncs);
      memset(pOutputParamsBuffer, 0, sizeof(D3D11_1DDI_VIDEO_OUTPUT));
      return S_OK;
   }
#if D3D11DDI_MINOR_HEADER_VERSION >= 5
   case D3DWDDM2_0DDI_VIDEO_FUNCTIONS: {
      D3DWDDM2_0DDI_VIDEO_INPUT *in = (D3DWDDM2_0DDI_VIDEO_INPUT *)pParams;
      if (ParamSize < sizeof(*in) ||
          OutputParamSize < sizeof(D3DWDDM2_0DDI_VIDEO_OUTPUT) ||
          !in->pWDDM2_0VideoDeviceFuncs)
         return E_INVALIDARG;
      fill_video_wddm2_0(in->pWDDM2_0VideoDeviceFuncs);
      memset(pOutputParamsBuffer, 0, sizeof(D3DWDDM2_0DDI_VIDEO_OUTPUT));
      return S_OK;
   }
#endif
#if D3D11DDI_MINOR_HEADER_VERSION >= 10
   case D3DWDDM2_1DDI_VIDEO_FUNCTIONS: {
      D3DWDDM2_1DDI_VIDEO_INPUT *in = (D3DWDDM2_1DDI_VIDEO_INPUT *)pParams;
      if (ParamSize < sizeof(*in) ||
          OutputParamSize < sizeof(D3DWDDM2_1DDI_VIDEO_OUTPUT) ||
          !in->pWDDM2_1VideoDeviceFuncs)
         return E_INVALIDARG;
      memset(in->pWDDM2_1VideoDeviceFuncs, 0, sizeof(*in->pWDDM2_1VideoDeviceFuncs));
      fill_video_wddm2_0((D3DWDDM2_0DDI_VIDEODEVICEFUNCS *)in->pWDDM2_1VideoDeviceFuncs);
      memset(pOutputParamsBuffer, 0, sizeof(D3DWDDM2_1DDI_VIDEO_OUTPUT));
      return S_OK;
   }
#endif
   default:
      aurora_log_err("RetrieveSubObject: unhandled id=%u param=%zu out=%zu",
                     (unsigned)SubDeviceID, ParamSize, OutputParamSize);
      return E_NOINTERFACE;
   }
}
