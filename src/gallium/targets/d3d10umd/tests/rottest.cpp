/*
 * rottest: DXGI swap-chain buffer rotation (RotateResourceIdentities) through the D3D11 runtime on the default
 * (hardware) adapter, with pixel checks.
 *
 *   rottest [--log FILE] [--frames N] [--then EXE]
 *
 * For blt model (SEQUENTIAL) and flip model (FLIP_SEQUENTIAL), 2 and 3 buffers, two sizes (256x256 and an odd
 * 333x217), two binding modes:
 *   rt   the RTV on buffer 0 is made and bound ONCE (blt model) and never rebound: each frame clears through it and
 *        draws a quad through the bound framebuffer, presents, then reads every buffer back by CopyResource.
 *        After present k, buffer i must hold frame k-(N-1)+i (clear colour and quad colour).
 *   srv  an SRV on buffer 1 is made and bound to PS slot 0 ONCE: each frame also renders a pass that Loads it into
 *        an offscreen target: it must show what buffer 1 holds (frame k-N+2).
 * Lines "ROTTEST ..." go to stdout and the log; the last one is "ROTTEST RESULT PASS|FAIL checks c failures f".
 * Shaders are compiled at run time with d3dcompiler_47.dll (in System32); without it only clears are checked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

static FILE *g_log;
static unsigned g_checks, g_fail;

static void
LOG(const char *fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof buf, fmt, ap);
   va_end(ap);
   printf("ROTTEST %s\n", buf);
   fflush(stdout);
   if (g_log) {
      SYSTEMTIME t;
      GetLocalTime(&t);
      fprintf(g_log, "%02u:%02u:%02u.%03u ROTTEST %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
      fflush(g_log);
   }
}

typedef HRESULT(WINAPI *PFN_D3DCOMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                        LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static PFN_D3DCOMPILE g_compile;

static const char *vs_src =
   "float4 main(uint id: SV_VertexID): SV_Position {\n"
   "  float2 p = float2((id << 1) & 2, id & 2);\n"
   "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
   "}\n";
static const char *ps_solid_src =
   "cbuffer c: register(b0) { float4 col; };\n"
   "float4 main(float4 p: SV_Position): SV_Target { return col; }\n";
static const char *ps_load_src =
   "Texture2D t: register(t0);\n"
   "float4 main(float4 p: SV_Position): SV_Target { return t.Load(int3(int2(p.xy), 0)); }\n";

static ID3DBlob *
compile(const char *src, const char *target)
{
   ID3DBlob *code = NULL, *err = NULL;
   HRESULT hr = g_compile(src, strlen(src), "rottest", NULL, NULL, "main", target, 0, 0, &code, &err);
   if (FAILED(hr)) {
      LOG("compile %s hr=0x%08lx %s", target, hr, err ? (const char *)err->GetBufferPointer(): "");
      if (err)
         err->Release();
      return NULL;
   }
   if (err)
      err->Release();
   return code;
}

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
   return DefWindowProcA(h, m, w, l);
}

static void
pump(void)
{
   MSG msg;
   while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
   }
}

/* frame k's clear and quad colours (B8G8R8A8 in memory: 0xAARRGGBB as a little-endian uint32) */
static void
colour(int k, bool quad, float f[4], UINT32 *px)
{
   UINT r = (k * 37 + 11) & 0xff, g = (k * 71 + 5) & 0xff, b = (k * 113 + 90) & 0xff;
   if (quad) {
      r ^= 0xff;
      g ^= 0x80;
   }
   f[0] = r / 255.0f;
   f[1] = g / 255.0f;
   f[2] = b / 255.0f;
   f[3] = 1.0f;
   *px = 0xff000000u | (r << 16) | (g << 8) | b;
}

struct Ctx {
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   ID3D11VertexShader *vs;
   ID3D11PixelShader *psSolid, *psLoad;
   ID3D11Buffer *cb;
};

static UINT32
read_px(Ctx *c, ID3D11Texture2D *staging, ID3D11Resource *src, UINT x, UINT y)
{
   c->ctx->CopyResource(staging, src);
   D3D11_MAPPED_SUBRESOURCE m;
   HRESULT hr = c->ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
   if (FAILED(hr)) {
      LOG("Map hr=0x%08lx", hr);
      return 0xdeadbeef;
   }
   UINT32 v = *(const UINT32 *)((const char *)m.pData + (size_t)y * m.RowPitch + x * 4);
   c->ctx->Unmap(staging, 0);
   return v;
}

static void
check(const char *what, UINT32 got, UINT32 want)
{
   g_checks++;
   if ((got & 0xffffff) != (want & 0xffffff)) {
      g_fail++;
      LOG("  FAIL %s: got %08x want %08x", what, got, want);
   }
}

static void
run(Ctx *c, IDXGIFactory *fac, DXGI_SWAP_EFFECT eff, UINT n, UINT w, UINT h, bool srvMode, int frames)
{
   const char *effName = eff == DXGI_SWAP_EFFECT_SEQUENTIAL ? "blt-sequential":
                         eff == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ? "flip-sequential": "other";
   bool flip = eff == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
   unsigned f0 = g_fail, c0 = g_checks;
   LOG("config %s buffers %u %ux%u mode %s", effName, n, w, h, srvMode ? "srv": "rt");

   RECT rc = { 0, 0, (LONG)w, (LONG)h };
   AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
   char title[96];
   snprintf(title, sizeof title, "rottest %s %u %s", effName, n, srvMode ? "srv": "rt");
   HWND hwnd = CreateWindowA("rottest", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40 + 30 * n, 40 + 20 * n,
                             rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
   pump();

   DXGI_SWAP_CHAIN_DESC sd = {};
   sd.BufferDesc.Width = w;
   sd.BufferDesc.Height = h;
   sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   sd.SampleDesc.Count = 1;
   sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
   sd.BufferCount = n;
   sd.OutputWindow = hwnd;
   sd.Windowed = TRUE;
   sd.SwapEffect = eff;
   IDXGISwapChain *sc = NULL;
   HRESULT hr = fac->CreateSwapChain(c->dev, &sd, &sc);
   if (FAILED(hr)) {
      LOG("  CreateSwapChain hr=0x%08lx: config skipped", hr);
      DestroyWindow(hwnd);
      return;
   }
   ID3D11Texture2D *b[4] = {};
   for (UINT i = 0; i < n; i++) {
      hr = sc->GetBuffer(i, __uuidof(ID3D11Texture2D), (void **)&b[i]);
      if (FAILED(hr)) {
         LOG("  GetBuffer(%u) hr=0x%08lx", i, hr);
         g_fail++;
      }
   }
   ID3D11RenderTargetView *rtv0 = NULL;
   hr = c->dev->CreateRenderTargetView(b[0], NULL, &rtv0);
   if (FAILED(hr))
      LOG("  CreateRenderTargetView hr=0x%08lx", hr);
   ID3D11ShaderResourceView *srv1 = NULL;
   if (srvMode && b[1]) {
      hr = c->dev->CreateShaderResourceView(b[1], NULL, &srv1);
      if (FAILED(hr))
         LOG("  CreateShaderResourceView(buffer 1) hr=0x%08lx", hr);
   }
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = w;
   td.Height = h;
   td.MipLevels = 1;
   td.ArraySize = 1;
   td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_STAGING;
   td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ID3D11Texture2D *staging = NULL, *off = NULL;
   c->dev->CreateTexture2D(&td, NULL, &staging);
   td.Usage = D3D11_USAGE_DEFAULT;
   td.CPUAccessFlags = 0;
   td.BindFlags = D3D11_BIND_RENDER_TARGET;
   c->dev->CreateTexture2D(&td, NULL, &off);
   ID3D11RenderTargetView *rtvOff = NULL;
   if (off)
      c->dev->CreateRenderTargetView(off, NULL, &rtvOff);
   if (!rtv0 || !staging || !off || !rtvOff) {
      LOG("  setup failed");
      g_fail++;
      goto out;
   }

   if (c->vs) {
      c->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      c->ctx->IASetInputLayout(NULL);
      c->ctx->VSSetShader(c->vs, NULL, 0);
      c->ctx->PSSetConstantBuffers(0, 1, &c->cb);
   }
   if (srv1)
      c->ctx->PSSetShaderResources(0, 1, &srv1); /* once */
   c->ctx->OMSetRenderTargets(1, &rtv0, NULL);    /* once (rt mode, blt model) */

   for (int k = 0; k < frames; k++) {
      float cf[4], qf[4];
      UINT32 cpx, qpx;
      colour(k, false, cf, &cpx);
      colour(k, true, qf, &qpx);
      /* flip model: the runtime unbinds the back buffer at Present; the same RTV object is bound again */
      if (flip || srvMode)
         c->ctx->OMSetRenderTargets(1, &rtv0, NULL);
      c->ctx->ClearRenderTargetView(rtv0, cf);
      if (c->vs) {
         c->ctx->UpdateSubresource(c->cb, 0, NULL, qf, 0, 0);
         D3D11_VIEWPORT vp = { 0, 0, (float)(w / 4), (float)(h / 4), 0, 1 };
         c->ctx->RSSetViewports(1, &vp);
         c->ctx->PSSetShader(c->psSolid, NULL, 0);
         c->ctx->Draw(3, 0);
      }
      hr = sc->Present(0, 0);
      if (FAILED(hr)) {
         LOG("  Present %d hr=0x%08lx", k, hr);
         g_fail++;
         break;
      }
      pump();
      char what[96];
      for (UINT i = 0; i < n; i++) {
         int fk = k - (int)(n - 1) + (int)i;
         if (fk < 0 || !b[i])
            continue;
         float t[4];
         UINT32 wc, wq;
         colour(fk, false, t, &wc);
         colour(fk, true, t, &wq);
         snprintf(what, sizeof what, "frame %d buffer %u clear", k, i);
         check(what, read_px(c, staging, b[i], w * 3 / 4, h * 3 / 4), wc);
         if (c->vs) {
            snprintf(what, sizeof what, "frame %d buffer %u quad", k, i);
            check(what, read_px(c, staging, b[i], w / 8, h / 8), wq);
         }
      }
      if (srv1 && c->psLoad) {
         int fk = k - (int)n + 2;
         c->ctx->OMSetRenderTargets(1, &rtvOff, NULL);
         D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
         c->ctx->RSSetViewports(1, &vp);
         c->ctx->PSSetShader(c->psLoad, NULL, 0);
         c->ctx->Draw(3, 0);
         if (fk >= 0) {
            float t[4];
            UINT32 wc, wq;
            colour(fk, false, t, &wc);
            colour(fk, true, t, &wq);
            snprintf(what, sizeof what, "frame %d srv(buffer 1) clear", k);
            check(what, read_px(c, staging, off, w * 3 / 4, h * 3 / 4), wc);
            snprintf(what, sizeof what, "frame %d srv(buffer 1) quad", k);
            check(what, read_px(c, staging, off, w / 8, h / 8), wq);
         }
      }
   }
   LOG("  %s buffers %u %ux%u mode %s: %u checks, %u failures", effName, n, w, h, srvMode ? "srv": "rt",
       g_checks - c0, g_fail - f0);
out:
   c->ctx->ClearState();
   c->ctx->Flush();
   if (rtvOff) rtvOff->Release();
   if (off) off->Release();
   if (staging) staging->Release();
   if (srv1) srv1->Release();
   if (rtv0) rtv0->Release();
   for (UINT i = 0; i < n; i++)
      if (b[i])
         b[i]->Release();
   sc->Release();
   c->ctx->Flush();
   DestroyWindow(hwnd);
   pump();
}

int
main(int argc, char **argv)
{
   const char *logPath = "C:\\g6u\\out\\rottest.txt";
   const char *then = NULL;
   int frames = 8;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--log") && i + 1 < argc)
         logPath = argv[++i];
      else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
         frames = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--then") && i + 1 < argc)
         then = argv[++i];
   }
   if (!then) {
      /* run as C:\g6d\g6anim.exe by the Run key: the real g6anim afterwards */
      static char next[MAX_PATH];
      GetModuleFileNameA(NULL, next, sizeof next);
      char *s = strrchr(next, '\\');
      if (s) {
         strcpy(s + 1, "g6anim0.exe");
         if (GetFileAttributesA(next) != INVALID_FILE_ATTRIBUTES)
            then = next;
      }
   }
   g_log = fopen(logPath, "a");
   LOG("start pid %lu frames %d", GetCurrentProcessId(), frames);

   WNDCLASSA wc = {};
   wc.lpfnWndProc = wndproc;
   wc.hInstance = GetModuleHandleA(NULL);
   wc.lpszClassName = "rottest";
   wc.hCursor = LoadCursor(NULL, IDC_ARROW);
   RegisterClassA(&wc);

   Ctx c = {};
   D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got;
   HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fls, 2, D3D11_SDK_VERSION, &c.dev, &got,
                                  &c.ctx);
   if (FAILED(hr)) {
      LOG("D3D11CreateDevice hr=0x%08lx", hr);
      LOG("RESULT FAIL checks 0 failures 1");
      return 1;
   }
   IDXGIDevice *dd = NULL;
   IDXGIAdapter *ad = NULL;
   IDXGIFactory *fac = NULL;
   c.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd);
   dd->GetAdapter(&ad);
   ad->GetParent(__uuidof(IDXGIFactory), (void **)&fac);
   DXGI_ADAPTER_DESC adesc;
   ad->GetDesc(&adesc);
   char an[128];
   WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, an, sizeof an, NULL, NULL);
   LOG("device fl 0x%x adapter \"%s\" vendor 0x%04x device 0x%04x", got, an, adesc.VendorId, adesc.DeviceId);

   HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
   g_compile = dc ? (PFN_D3DCOMPILE)GetProcAddress(dc, "D3DCompile"): NULL;
   if (g_compile) {
      ID3DBlob *v = compile(vs_src, "vs_4_0"), *p1 = compile(ps_solid_src, "ps_4_0"), *p2 = compile(ps_load_src, "ps_4_0");
      if (v && p1 && p2) {
         c.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), NULL, &c.vs);
         c.dev->CreatePixelShader(p1->GetBufferPointer(), p1->GetBufferSize(), NULL, &c.psSolid);
         c.dev->CreatePixelShader(p2->GetBufferPointer(), p2->GetBufferSize(), NULL, &c.psLoad);
         D3D11_BUFFER_DESC bd = {};
         bd.ByteWidth = 16;
         bd.Usage = D3D11_USAGE_DEFAULT;
         bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         c.dev->CreateBuffer(&bd, NULL, &c.cb);
      }
      if (!c.vs || !c.psSolid || !c.psLoad || !c.cb) {
         LOG("shader setup failed: clears only");
         c.vs = NULL;
         c.psLoad = NULL;
      }
   } else {
      LOG("no d3dcompiler_47.dll: clears only, no srv mode");
   }

   struct { DXGI_SWAP_EFFECT e; UINT n; UINT w, h; } cfg[] = {
      { DXGI_SWAP_EFFECT_SEQUENTIAL, 2, 256, 256 },
      { DXGI_SWAP_EFFECT_SEQUENTIAL, 3, 333, 217 },
      { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2, 256, 256 },
      { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 3, 333, 217 },
   };
   for (unsigned mode = 0; mode < 2; mode++)
      for (unsigned i = 0; i < ARRAYSIZE(cfg); i++) {
         if (mode && !c.psLoad)
            continue;
         run(&c, fac, cfg[i].e, cfg[i].n, cfg[i].w, cfg[i].h, mode == 1, frames);
      }
   LOG("RESULT %s checks %u failures %u", g_fail ? "FAIL": "PASS", g_checks, g_fail);
   if (g_log)
      fclose(g_log);
   g_log = NULL;
   if (then) {
      STARTUPINFOA si = { sizeof si };
      PROCESS_INFORMATION pi;
      if (CreateProcessA(then, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
         CloseHandle(pi.hThread);
         CloseHandle(pi.hProcess);
      }
   }
   return g_fail ? 1: 0;
}
