/*
 * G6 r7 (track H): per-frame profile of a WDDM process on this UMD (DWM on zink/hk).
 *
 * Every DDI in the device tables goes through DdiWrap (DDIW(fn) in the table fills): the outermost call per
 * thread is timed, so "in the UMD" is the wall time the runtime spent inside this driver, and each DDI keeps
 * its own time and count. The kernel-thunk callbacks the UMD calls (RenderCb, LockCb, ...) go through CbWrap
 * (PerfWrapCallbacks at CreateDevice). PerfFrame() at each Present prints every 120 frames: wall, in the UMD,
 * the presenting thread's CPU, the process's CPU, the machine's idle share, the top DDIs and the callbacks.
 * Every 1200 frames one frame's anatomy is logged: each DDI call over 0.3 ms and each gap between DDIs over
 * 0.5 ms (time the runtime/DWM spent outside the UMD) with the thread's CPU in that gap.
 * Only while the UMD trace is on (C:\g6u\umdlog.on); otherwise one flag test per DDI.
 */
#pragma once

#include <windows.h>

struct PerfSite {
   const char *name;
   volatile LONG64 ticks, calls;
   PerfSite *next;
   volatile LONG reg;
   int kind; /* 0 DDI, 1 callback */
};

extern volatile LONG g_perfOn; /* 1: the trace is on */
extern volatile LONG g_perfAnatomy; /* 1: this frame's calls are logged */

void PerfSiteRegister(PerfSite *s, const char *name, int kind);
UINT64 PerfQpc(void);
void PerfDdiEnter(PerfSite *s, UINT64 *t0);
void PerfDdiLeave(PerfSite *s, UINT64 t0);
void PerfCbDone(PerfSite *s, UINT64 t0, UINT64 t1);
void PerfFrame(void); /* once per present of the presenting device */
void PerfInit(void);
void PerfAdd(int which, UINT64 ticks); /* the Present split below */
enum { PERF_P_FLUSH, PERF_P_CB, PERF_P_FLIP, PERF_F_COPYOUT, PERF_F_PIPE, PERF_R_CREATE, PERF_R_COPY, PERF_F_COPYOUTS, PERF_P_N };

struct PerfDdiScope {
   PerfSite *s;
   UINT64 t0;
   PerfDdiScope(PerfSite *site): s(site), t0(0) { if (g_perfOn) PerfDdiEnter(s, &t0); }
   ~PerfDdiScope() { if (t0) PerfDdiLeave(s, t0); }
};

template <typename F, F fn> struct DdiWrap;
template <typename R, typename... A, R (APIENTRY *fn)(A...)>
struct DdiWrap<R (APIENTRY *)(A...), fn> {
   static PerfSite site;
   static R APIENTRY call(A... a)
   {
      PerfDdiScope sc(&site);
      return fn(a...);
   }
   static R (APIENTRY *make(const char *name))(A...)
   {
      PerfSiteRegister(&site, name, 0);
      return &call;
   }
};
template <typename R, typename... A, R (APIENTRY *fn)(A...)>
PerfSite DdiWrap<R (APIENTRY *)(A...), fn>::site;

#define DDIW(fn) (DdiWrap<decltype(&fn), &fn>::make(#fn))

template <int Tag, typename F> struct CbWrap;
template <int Tag, typename R, typename... A>
struct CbWrap<Tag, R (APIENTRY *)(A...)> {
   static R (APIENTRY *orig)(A...);
   static PerfSite site;
   static R APIENTRY call(A... a)
   {
      if (!g_perfOn)
         return orig(a...);
      UINT64 t0 = PerfQpc();
      R r = orig(a...);
      PerfCbDone(&site, t0, PerfQpc());
      return r;
   }
   static void wrap(R (APIENTRY **slot)(A...), const char *name)
   {
      if (!*slot || *slot == &call)
         return;
      if (!orig)
         orig = *slot;
      if (orig != *slot)
         return; /* another runtime table: leave it */
      PerfSiteRegister(&site, name, 1);
      *slot = &call;
   }
};
template <int Tag, typename R, typename... A>
R (APIENTRY *CbWrap<Tag, R (APIENTRY *)(A...)>::orig)(A...);
template <int Tag, typename R, typename... A>
PerfSite CbWrap<Tag, R (APIENTRY *)(A...)>::site;

#define CBW(tag, cbs, member) CbWrap<tag, decltype((cbs).member)>::wrap(&(cbs).member, #member)
