/*
 * G6 r7 (track H): per-frame profile (see Perf.h).
 */
#include <stdio.h>
#include <string.h>

#include "Perf.h"
#include "Debug.h"

volatile LONG g_perfOn;
volatile LONG g_perfAnatomy;

static PerfSite *volatile g_sites;
static volatile LONG g_siteLock;
static LARGE_INTEGER g_qpf;
static double g_cyclesPerTick; /* QueryThreadCycleTime units per QPC tick (calibrated) */
static __thread int t_depth;
static __thread UINT64 t_lastLeave, t_lastLeaveCyc;

/* outermost DDI time (all threads), DDI calls, Present split */
static volatile LONG64 g_inUmd, g_ddiCalls, g_presentSplit[PERF_P_N];
static UINT64 g_frameT0, g_frames, g_framesTotal, g_threadCyc0, g_procCpu0, g_sysIdle0, g_sysAll0;
static DWORD g_presentThread;
static UINT64 g_anatomyT0, g_anatomyCyc0;

UINT64
PerfQpc(void)
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (UINT64)t.QuadPart;
}

static UINT64
ThreadCycles(void)
{
   ULONG64 c = 0;
   QueryThreadCycleTime(GetCurrentThread(), &c);
   return c;
}

static double
Ms(UINT64 ticks)
{
   return g_qpf.QuadPart ? (double)ticks * 1000.0 / (double)g_qpf.QuadPart: 0.0;
}

static UINT64
FtU64(FILETIME f)
{
   return ((UINT64)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

void
PerfInit(void)
{
   static volatile LONG done;
   if (InterlockedExchange(&done, 1))
      return;
   QueryPerformanceFrequency(&g_qpf);
   /* QueryThreadCycleTime's unit against QPC: 2 ms of spinning */
   UINT64 c0 = ThreadCycles(), t0 = PerfQpc(), t1;
   do {
      t1 = PerfQpc();
   } while (Ms(t1 - t0) < 2.0);
   UINT64 c1 = ThreadCycles();
   g_cyclesPerTick = t1 > t0 ? (double)(c1 - c0) / (double)(t1 - t0): 1.0;
   if (g_cyclesPerTick <= 0)
      g_cyclesPerTick = 1.0;
   g_perfOn = UmdTrace() ? 1: 0;
   if (g_perfOn)
      DebugPrintf("perf: on, QPC %lld Hz, thread cycles per QPC tick %.2f\n", (long long)g_qpf.QuadPart,
                  g_cyclesPerTick);
}

void
PerfSiteRegister(PerfSite *s, const char *name, int kind)
{
   if (InterlockedCompareExchange(&s->reg, 1, 0) != 0)
      return;
   s->name = name;
   s->kind = kind;
   while (InterlockedCompareExchange(&g_siteLock, 1, 0) != 0)
      ;
   s->next = g_sites;
   g_sites = s;
   InterlockedExchange(&g_siteLock, 0);
}

void
PerfDdiEnter(PerfSite *s, UINT64 *t0)
{
   (void)s;
   if (t_depth++ != 0)
      return;
   *t0 = PerfQpc();
   if (g_perfAnatomy && GetCurrentThreadId() == g_presentThread && t_lastLeave) {
      UINT64 gap = *t0 - t_lastLeave;
      if (Ms(gap) >= 0.5) {
         UINT64 cyc = ThreadCycles() - t_lastLeaveCyc;
         DebugPrintf("perf: anatomy +%.2f ms: gap %.2f ms outside the UMD (thread CPU %.2f ms) before %s\n",
                     Ms(*t0 - g_anatomyT0), Ms(gap), Ms((UINT64)(cyc / g_cyclesPerTick)), s->name ? s->name: "?");
      }
   }
}

void
PerfDdiLeave(PerfSite *s, UINT64 t0)
{
   if (--t_depth != 0)
      return;
   UINT64 t1 = PerfQpc();
   InterlockedAdd64(&s->ticks, (LONG64)(t1 - t0));
   InterlockedIncrement64(&s->calls);
   InterlockedAdd64(&g_inUmd, (LONG64)(t1 - t0));
   InterlockedIncrement64(&g_ddiCalls);
   if (GetCurrentThreadId() == g_presentThread) {
      if (g_perfAnatomy && Ms(t1 - t0) >= 0.3)
         DebugPrintf("perf: anatomy +%.2f ms: %s %.2f ms\n", Ms(t0 - g_anatomyT0), s->name ? s->name: "?",
                     Ms(t1 - t0));
      t_lastLeave = PerfQpc();
      t_lastLeaveCyc = ThreadCycles();
   }
}

void
PerfCbDone(PerfSite *s, UINT64 t0, UINT64 t1)
{
   InterlockedAdd64(&s->ticks, (LONG64)(t1 - t0));
   InterlockedIncrement64(&s->calls);
   if (g_perfAnatomy && GetCurrentThreadId() == g_presentThread && Ms(t1 - t0) >= 0.2)
      DebugPrintf("perf: anatomy +%.2f ms:   cb %s %.2f ms\n", Ms(t0 - g_anatomyT0), s->name, Ms(t1 - t0));
}

void
PerfAdd(int which, UINT64 ticks)
{
   if (which >= 0 && which < PERF_P_N)
      InterlockedAdd64(&g_presentSplit[which], (LONG64)ticks);
}

static void
Snap(UINT64 *proc, UINT64 *idle, UINT64 *all)
{
   FILETIME c, e, k, u, si, sk, su;
   *proc = 0;
   if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
      *proc = FtU64(k) + FtU64(u);
   *idle = *all = 0;
   if (GetSystemTimes(&si, &sk, &su)) {
      *idle = FtU64(si);
      *all = FtU64(sk) + FtU64(su); /* kernel includes idle */
   }
}

void
PerfFrame(void)
{
   if (!g_perfOn)
      return;
   UINT64 now = PerfQpc();
   g_presentThread = GetCurrentThreadId();
   g_framesTotal++;
   UINT64 ph = g_framesTotal % 1200;
   if (g_perfAnatomy) {
      DebugPrintf("perf: anatomy present +%.2f ms (frame %llu), thread CPU %.2f ms so far\n", Ms(now - g_anatomyT0),
                  (unsigned long long)g_framesTotal,
                  Ms((UINT64)((ThreadCycles() - g_anatomyCyc0) / g_cyclesPerTick)));
      if (ph > 603)
         g_perfAnatomy = 0;
   } else if (ph == 600) {
      g_anatomyT0 = now;
      g_anatomyCyc0 = ThreadCycles();
      DebugPrintf("perf: anatomy begin (frame %llu): 3 frames\n", (unsigned long long)g_framesTotal);
      g_perfAnatomy = 1;
   }
   if (!g_frameT0) {
      g_frameT0 = now;
      g_threadCyc0 = ThreadCycles();
      Snap(&g_procCpu0, &g_sysIdle0, &g_sysAll0);
      return;
   }
   if (++g_frames < 120)
      return;
   double n = (double)g_frames;
   UINT64 cyc = ThreadCycles(), proc, idle, all;
   Snap(&proc, &idle, &all);
   double wall = Ms(now - g_frameT0) / n;
   double sysIdle = all > g_sysAll0 ? 100.0 * (double)(idle - g_sysIdle0) / (double)(all - g_sysAll0): -1;
   DebugPrintf("perf: %llu fr: %.2f ms/fr; in UMD %.2f (%.1f DDIs); Present: flush/wait %.2f, PresentCb %.2f, "
               "scan copy/wait/flip %.2f\n",
               (unsigned long long)g_frames, wall, Ms(InterlockedExchange64(&g_inUmd, 0)) / n,
               (double)InterlockedExchange64(&g_ddiCalls, 0) / n,
               Ms(InterlockedExchange64(&g_presentSplit[PERF_P_FLUSH], 0)) / n,
               Ms(InterlockedExchange64(&g_presentSplit[PERF_P_CB], 0)) / n,
               Ms(InterlockedExchange64(&g_presentSplit[PERF_P_FLIP], 0)) / n);
   DebugPrintf("perf: Flush: copy-out %.2f (%.2f/fr), pipe flush %.2f; Rotate: identities (or copy temp) %.2f, copy-forward (or copies) %.2f\n",
               Ms(InterlockedExchange64(&g_presentSplit[PERF_F_COPYOUT], 0)) / n,
               (double)InterlockedExchange64(&g_presentSplit[PERF_F_COPYOUTS], 0) / n,
               Ms(InterlockedExchange64(&g_presentSplit[PERF_F_PIPE], 0)) / n,
               Ms(InterlockedExchange64(&g_presentSplit[PERF_R_CREATE], 0)) / n,
               Ms(InterlockedExchange64(&g_presentSplit[PERF_R_COPY], 0)) / n);
   DebugPrintf("perf: cpu ms/fr: presenting thread %.2f, process %.2f; machine idle %.0f%%\n",
               Ms((UINT64)((double)(cyc - g_threadCyc0) / g_cyclesPerTick)) / n,
               (double)(proc - g_procCpu0) / 10000.0 / n, sysIdle);
   /* the top DDIs and every callback by time */
   PerfSite *top[10] = {};
   UINT64 topT[10] = {};
   char cb[1024];
   int o = 0;
   cb[0] = 0;
   for (PerfSite *s = g_sites; s; s = s->next) {
      UINT64 t = (UINT64)InterlockedExchange64(&s->ticks, 0);
      UINT64 c = (UINT64)InterlockedExchange64(&s->calls, 0);
      if (!c)
         continue;
      if (s->kind == 1) {
         if (o >= 0 && o < (int)sizeof cb)
            o += snprintf(cb + o, sizeof cb - o, " %s %.2f (%.1f)", s->name + 3, Ms(t) / n, (double)c / n);
         continue;
      }
      s->calls = (LONG64)c; /* the count goes with the name until printed */
      for (int i = 0; i < 10; i++) {
         if (!top[i] || t > topT[i]) {
            for (int j = 9; j > i; j--) {
               top[j] = top[j - 1];
               topT[j] = topT[j - 1];
            }
            top[i] = s;
            topT[i] = t;
            break;
         }
      }
   }
   for (int h = 0; h < 2; h++) {
      char dd[512];
      int p = 0;
      dd[0] = 0;
      for (int i = h * 5; i < h * 5 + 5 && top[i]; i++) {
         if (p >= 0 && p < (int)sizeof dd)
            p += snprintf(dd + p, sizeof dd - p, " %s %.2f (%.1f)", top[i]->name, Ms(topT[i]) / n,
                          (double)top[i]->calls / n);
      }
      if (dd[0])
         DebugPrintf("perf: DDIs %d ms/fr (n):%s\n", h + 1, dd);
   }
   for (PerfSite *s = g_sites; s; s = s->next)
      if (s->kind == 0)
         s->calls = 0;
   DebugPrintf("perf: cbs ms/fr (n):%s\n", cb);
   g_frames = 0;
   g_frameT0 = now;
   g_threadCyc0 = cyc;
   g_procCpu0 = proc;
   g_sysIdle0 = idle;
   g_sysAll0 = all;
}
