/*
 * d3d11tri: D3D11 clear + triangle through a chosen UMD, with a pixel check.
 *
 *   d3d11tri [--sw DLL | --hw] [--window] [--frames N] [--out FILE.bmp]
 *
 * --sw DLL  D3D_DRIVER_TYPE_SOFTWARE with that DLL (default: g18d3d10.dll next to the exe)
 * --hw      D3D_DRIVER_TYPE_HARDWARE on the default adapter (the KMD's UserModeDriverName)
 * --window  also present through a DXGI swap chain into a window for N frames and report fps
 *
 * Exit 0 only when the offscreen 256x256 render has the clear color in the corner and the
 * triangle color at the center. Lines start with "D3D11TRI " for the host judge.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include <windows.h>
#include <d3d11.h>

#include "tri_vs_4_0.h"
#include "tri_ps_4_0.h"

#define LOG(...) do { printf("D3D11TRI " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

struct Vertex {
   float position[4];
   float color[4];
};

static const Vertex vertices[] = {
   { { -0.9f, -0.9f, 0.5f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
   { {  0.9f, -0.9f, 0.5f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
   { {  0.0f,  0.9f, 0.5f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
};

static const float clear_color[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

static void
print_addr(const char *what, DWORD64 a)
{
   HMODULE m = NULL;
   char name[MAX_PATH] = "?";
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)(uintptr_t)a, &m) && m)
      GetModuleFileNameA(m, name, sizeof name);
   const char *base = strrchr(name, '\\');
   printf("D3D11TRI %s 0x%llx = %s+0x%llx\n", what, (unsigned long long)a, base ? base + 1: name,
          (unsigned long long)(a - (uintptr_t)m));
}

static LONG WINAPI
crash_filter(EXCEPTION_POINTERS *ep)
{
   printf("D3D11TRI CRASH code 0x%08lx\n", ep->ExceptionRecord->ExceptionCode);
#if defined(_M_ARM64) || defined(__aarch64__)
   CONTEXT *c = ep->ContextRecord;
   print_addr("pc", c->Pc);
   print_addr("lr", c->Lr);
   /* frame-pointer walk */
   DWORD64 fp = c->Fp;
   for (int i = 0; i < 24 && fp; i++) {
      DWORD64 *f = (DWORD64 *)(uintptr_t)fp;
      if (IsBadReadPtr(f, 16))
         break;
      print_addr("frame", f[1]);
      fp = f[0];
   }
#endif
   fflush(stdout);
   return EXCEPTION_EXECUTE_HANDLER;
}

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
   if (m == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
   }
   return DefWindowProcA(h, m, w, l);
}

struct Pipeline {
   ID3D11VertexShader *vs;
   ID3D11PixelShader *ps;
   ID3D11InputLayout *il;
   ID3D11Buffer *vb;
   ID3D11RasterizerState *rs;
};

static HRESULT
make_pipeline(ID3D11Device *dev, Pipeline *p)
{
   HRESULT hr;
   memset(p, 0, sizeof *p);
   hr = dev->CreateVertexShader(g_VS, sizeof g_VS, NULL, &p->vs);
   if (FAILED(hr)) { LOG("CreateVertexShader hr=0x%08lx", hr); return hr; }
   hr = dev->CreatePixelShader(g_PS, sizeof g_PS, NULL, &p->ps);
   if (FAILED(hr)) { LOG("CreatePixelShader hr=0x%08lx", hr); return hr; }
   static const D3D11_INPUT_ELEMENT_DESC ied[] = {
      { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, color),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
   };
   hr = dev->CreateInputLayout(ied, 2, g_VS, sizeof g_VS, &p->il);
   if (FAILED(hr)) { LOG("CreateInputLayout hr=0x%08lx", hr); return hr; }
   D3D11_BUFFER_DESC bd = {};
   bd.Usage = D3D11_USAGE_DEFAULT;
   bd.ByteWidth = sizeof vertices;
   bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
   D3D11_SUBRESOURCE_DATA sd = { vertices, 0, 0 };
   hr = dev->CreateBuffer(&bd, &sd, &p->vb);
   if (FAILED(hr)) { LOG("CreateBuffer hr=0x%08lx", hr); return hr; }
   D3D11_RASTERIZER_DESC rd = {};
   rd.CullMode = D3D11_CULL_NONE;
   rd.FillMode = D3D11_FILL_SOLID;
   rd.DepthClipEnable = TRUE;
   hr = dev->CreateRasterizerState(&rd, &p->rs);
   if (FAILED(hr)) { LOG("CreateRasterizerState hr=0x%08lx", hr); return hr; }
   return S_OK;
}

static void
draw(ID3D11DeviceContext *ctx, Pipeline *p, ID3D11RenderTargetView *rtv, UINT w, UINT h)
{
   ctx->OMSetRenderTargets(1, &rtv, NULL);
   ctx->ClearRenderTargetView(rtv, clear_color);
   D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
   ctx->RSSetViewports(1, &vp);
   ctx->RSSetState(p->rs);
   ctx->IASetInputLayout(p->il);
   UINT stride = sizeof(Vertex), offset = 0;
   ctx->IASetVertexBuffers(0, 1, &p->vb, &stride, &offset);
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ctx->VSSetShader(p->vs, NULL, 0);
   ctx->PSSetShader(p->ps, NULL, 0);
   ctx->Draw(3, 0);
}

static void
write_bmp(const char *path, const unsigned char *bgra, UINT w, UINT h, UINT pitch)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   BITMAPFILEHEADER fh = {};
   BITMAPINFOHEADER ih = {};
   fh.bfType = 0x4d42;
   fh.bfOffBits = sizeof fh + sizeof ih;
   fh.bfSize = fh.bfOffBits + w * h * 4;
   ih.biSize = sizeof ih;
   ih.biWidth = w;
   ih.biHeight = -(LONG)h;
   ih.biPlanes = 1;
   ih.biBitCount = 32;
   fwrite(&fh, sizeof fh, 1, f);
   fwrite(&ih, sizeof ih, 1, f);
   for (UINT y = 0; y < h; y++)
      fwrite(bgra + y * pitch, 4, w, f);
   fclose(f);
}

int
main(int argc, char **argv)
{
   SetUnhandledExceptionFilter(crash_filter);
   const char *dll = NULL;
   const char *out = "d3d11tri.bmp";
   bool hw = false, window = false;
   int frames = 300;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--sw") && i + 1 < argc) dll = argv[++i];
      else if (!strcmp(argv[i], "--hw")) hw = true;
      else if (!strcmp(argv[i], "--window")) window = true;
      else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
   }

   HMODULE sw = NULL;
   if (!hw) {
      char path[MAX_PATH];
      if (!dll) {
         GetModuleFileNameA(NULL, path, sizeof path);
         char *s = strrchr(path, '\\');
         strcpy(s ? s + 1: path, "g18d3d10.dll");
         dll = path;
      }
      sw = LoadLibraryA(dll);
      LOG("LoadLibrary %s -> %p (err %lu)", dll, (void *)sw, sw ? 0: GetLastError());
      if (!sw)
         return 2;
      LOG("OpenAdapter10 %p OpenAdapter10_2 %p", (void *)GetProcAddress(sw, "OpenAdapter10"),
          (void *)GetProcAddress(sw, "OpenAdapter10_2"));
   }

   static const D3D_FEATURE_LEVEL fls[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
   };
   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *ctx = NULL;
   D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
   HRESULT hr = E_FAIL;
   for (unsigned first = 0; first < 4; first++) {
      hr = D3D11CreateDevice(NULL, hw ? D3D_DRIVER_TYPE_HARDWARE: D3D_DRIVER_TYPE_SOFTWARE, sw, 0,
                             fls + first, 4 - first, D3D11_SDK_VERSION, &dev, &fl, &ctx);
      LOG("D3D11CreateDevice(max FL 0x%x) hr=0x%08lx", fls[first], hr);
      if (SUCCEEDED(hr))
         break;
   }
   if (FAILED(hr))
      return 3;
   LOG("feature level 0x%x", fl);

   IDXGIDevice *xdev = NULL;
   IDXGIAdapter *xad = NULL;
   if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xdev)) &&
       SUCCEEDED(xdev->GetAdapter(&xad))) {
      DXGI_ADAPTER_DESC ad;
      xad->GetDesc(&ad);
      LOG("adapter \"%ls\" vendor 0x%04x device 0x%04x", ad.Description, ad.VendorId, ad.DeviceId);
   }

   Pipeline p;
   if (FAILED(make_pipeline(dev, &p)))
      return 4;

   const UINT W = 256, H = 256;
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = W;
   td.Height = H;
   td.MipLevels = 1;
   td.ArraySize = 1;
   td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_DEFAULT;
   td.BindFlags = D3D11_BIND_RENDER_TARGET;
   ID3D11Texture2D *rt = NULL, *st = NULL;
   hr = dev->CreateTexture2D(&td, NULL, &rt);
   if (FAILED(hr)) { LOG("CreateTexture2D(rt) hr=0x%08lx", hr); return 5; }
   td.Usage = D3D11_USAGE_STAGING;
   td.BindFlags = 0;
   td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   hr = dev->CreateTexture2D(&td, NULL, &st);
   if (FAILED(hr)) { LOG("CreateTexture2D(staging) hr=0x%08lx", hr); return 5; }
   ID3D11RenderTargetView *rtv = NULL;
   hr = dev->CreateRenderTargetView(rt, NULL, &rtv);
   if (FAILED(hr)) { LOG("CreateRenderTargetView hr=0x%08lx", hr); return 5; }

   draw(ctx, &p, rtv, W, H);
   {
      /* track H: what the GPU sees in the vertex buffer (a staging copy of it, read back) */
      D3D11_BUFFER_DESC sbd = {};
      sbd.ByteWidth = sizeof vertices;
      sbd.Usage = D3D11_USAGE_STAGING;
      sbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Buffer *svb = NULL;
      if (SUCCEEDED(dev->CreateBuffer(&sbd, NULL, &svb))) {
         ctx->CopyResource(svb, p.vb);
         D3D11_MAPPED_SUBRESOURCE vm;
         if (SUCCEEDED(ctx->Map(svb, 0, D3D11_MAP_READ, 0, &vm))) {
            const unsigned *f = (const unsigned *)vm.pData;
            LOG("vb readback: %08x %08x %08x %08x | %08x %08x %08x %08x | v1 %08x %08x v2 %08x %08x (want bf666666 bf666666 3f000000 3f800000)",
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[16], f[17]);
            ctx->Unmap(svb, 0);
         } else
            LOG("vb readback: Map failed");
         svb->Release();
      }
   }
   ctx->CopyResource(st, rt);
   D3D11_MAPPED_SUBRESOURCE m;
   hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
   if (FAILED(hr)) { LOG("Map hr=0x%08lx", hr); return 6; }
   const unsigned char *px = (const unsigned char *)m.pData;
   unsigned corner = *(const unsigned *)(px + 2 * m.RowPitch + 2 * 4);
   unsigned center = *(const unsigned *)(px + (H / 2) * m.RowPitch + (W / 2) * 4);
   write_bmp(out, px, W, H, m.RowPitch);
   {
      /* census of the whole target: pixels that are not the clear colour, their bounding box, the
       * distinct colours among them (first 4) and a coarse 16x16 coverage map (track H, M6) */
      unsigned other = 0, x0 = W, y0 = H, x1 = 0, y1 = 0, ncol = 0;
      unsigned cols[4] = {0}, cnt[4] = {0};
      char row[17];
      for (UINT y = 0; y < H; y++)
         for (UINT x = 0; x < W; x++) {
            unsigned v = *(const unsigned *)(px + y * m.RowPitch + x * 4);
            if ((v & 0xffffff) == 0x0000ff)
               continue;
            other++;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
            unsigned k;
            for (k = 0; k < ncol; k++)
               if (cols[k] == v)
                  break;
            if (k < ncol)
               cnt[k]++;
            else if (ncol < 4) {
               cols[ncol] = v;
               cnt[ncol++] = 1;
            }
         }
      LOG("census: %u of %u pixels not the clear colour; bbox (%u,%u)-(%u,%u); colours %08x x%u %08x x%u %08x x%u %08x x%u",
          other, W * H, other ? x0: 0, other ? y0: 0, x1, y1, cols[0], cnt[0], cols[1], cnt[1], cols[2], cnt[2],
          cols[3], cnt[3]);
      for (UINT by = 0; by < 16; by++) {
         for (UINT bx = 0; bx < 16; bx++) {
            unsigned n = 0;
            for (UINT y = by * (H / 16); y < (by + 1) * (H / 16); y++)
               for (UINT x = bx * (W / 16); x < (bx + 1) * (W / 16); x++)
                  if ((*(const unsigned *)(px + y * m.RowPitch + x * 4) & 0xffffff) != 0x0000ff)
                     n++;
            row[bx] = n == 0 ? '.': n == (W / 16) * (H / 16) ? '#': '+';
         }
         row[16] = 0;
         LOG("map %02u %s", by, row);
      }
   }
   ctx->Unmap(st, 0);
   bool ok = (corner & 0xffffff) == 0x0000ff && (center & 0xffffff) == 0xff0000;
   LOG("offscreen corner=0x%08x center=0x%08x -> %s (bmp %s)", corner, center, ok ? "PASS": "FAIL", out);

   if (window) {
      WNDCLASSA wc = {};
      wc.lpfnWndProc = wndproc;
      wc.hInstance = GetModuleHandleA(NULL);
      wc.lpszClassName = "d3d11tri";
      wc.hCursor = LoadCursor(NULL, IDC_ARROW);
      RegisterClassA(&wc);
      RECT r = { 0, 0, 512, 512 };
      AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
      HWND hwnd = CreateWindowA("d3d11tri", "d3d11tri", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                                r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
      IDXGIFactory *fac = NULL;
      xad->GetParent(__uuidof(IDXGIFactory), (void **)&fac);
      DXGI_SWAP_CHAIN_DESC scd = {};
      scd.BufferDesc.Width = 512;
      scd.BufferDesc.Height = 512;
      scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      scd.SampleDesc.Count = 1;
      scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scd.BufferCount = 1;
      scd.OutputWindow = hwnd;
      scd.Windowed = TRUE;
      scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      IDXGISwapChain *sc = NULL;
      hr = fac ? fac->CreateSwapChain(dev, &scd, &sc): E_FAIL;
      LOG("CreateSwapChain hr=0x%08lx", hr);
      if (SUCCEEDED(hr)) {
         ID3D11Texture2D *bb = NULL;
         ID3D11RenderTargetView *bbv = NULL;
         sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
         dev->CreateRenderTargetView(bb, NULL, &bbv);
         LARGE_INTEGER f, t0, t1;
         QueryPerformanceFrequency(&f);
         QueryPerformanceCounter(&t0);
         int n = 0;
         HRESULT phr = S_OK;
         for (; n < frames; n++) {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
               TranslateMessage(&msg);
               DispatchMessageA(&msg);
            }
            draw(ctx, &p, bbv, 512, 512);
            phr = sc->Present(1, 0);
            if (FAILED(phr))
               break;
         }
         QueryPerformanceCounter(&t1);
         double s = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
         LOG("presented %d frames in %.3f s = %.1f fps (last Present hr=0x%08lx)", n, s, n / s, phr);
         bbv->Release();
         bb->Release();
         sc->Release();
      }
      DestroyWindow(hwnd);
   }

   LOG("RESULT %s", ok ? "PASS": "FAIL");
   return ok ? 0: 1;
}
