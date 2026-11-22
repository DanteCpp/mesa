/* SPDX-License-Identifier: MIT */
#pragma once
#include "DriverIncludes.h"
void FillD3D11DeviceFuncs(D3D11DDI_DEVICEFUNCS *pDeviceFuncs);
void FillD3D11_1DeviceFuncs(D3D11_1DDI_DEVICEFUNCS *pDeviceFuncs);
void D3D11_1CreateGeometryShaderWithStreamOutput(D3D10DDI_HDEVICE hDevice,
                                                 const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pCreate,
                                                 D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader);
