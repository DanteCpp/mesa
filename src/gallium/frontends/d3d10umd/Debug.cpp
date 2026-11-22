#include "Debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>


#if MESA_DEBUG

unsigned st_debug = 0;

static const
struct debug_named_value st_debug_flags[] = {
   {"oldtexops", ST_DEBUG_OLD_TEX_OPS, "oldtexops"},
   {"tgsi", ST_DEBUG_TGSI, "tgsi"},
   DEBUG_NAMED_VALUE_END
};
void
st_debug_parse(void)
{
   st_debug = debug_get_flags_option("ST_DEBUG", st_debug_flags, st_debug);
}

#endif


/*
 * Trace log: G18UMD_LOG=<file>, or C:\\g6u\\umdlog.on present -> C:\\g6u\\out\\umd-<pid>.log.
 * Every entry point is logged while the log is open.
 */
static FILE *umd_log;
static volatile LONG umd_log_init;

static void
UmdLogInit(void)
{
   if (InterlockedCompareExchange(&umd_log_init, 1, 0) != 0)
      return;
   char path[MAX_PATH];
   DWORD n = GetEnvironmentVariableA("G18UMD_LOG", path, sizeof path);
   if (n == 0 || n >= sizeof path) {
      if (GetFileAttributesA("C:\\g6u\\umdlog.on") == INVALID_FILE_ATTRIBUTES)
         return;
      char exe[MAX_PATH] = "";
      GetModuleFileNameA(NULL, exe, sizeof exe);
      const char *base = strrchr(exe, '\\');
      snprintf(path, sizeof path, "C:\\g6u\\out\\umd-%s-%lu.log", base ? base + 1 : exe,
               GetCurrentProcessId());
   }
   umd_log = fopen(path, "a");
}

int
UmdTrace(void)
{
   UmdLogInit();
   return umd_log != NULL;
}

int
UmdTraceAll(void)
{
   static int all = -1;
   if (all < 0)
      all = UmdTrace() && GetFileAttributesA("C:\\g6u\\umdlog.all") != INVALID_FILE_ATTRIBUTES;
   return all;
}

void
DebugPrintf(const char *format, ...)
{
    char buf[4096];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);

    OutputDebugStringA(buf);
    UmdLogInit();
    if (umd_log) {
       fprintf(umd_log, "[%lu %lu] %s", GetCurrentThreadId(), GetTickCount(), buf);
       fflush(umd_log);
    }
}


/**
 * Produce a human readable message from HRESULT.
 *
 * @sa http://msdn.microsoft.com/en-us/library/ms679351(VS.85).aspx
 */
void
CheckHResult(HRESULT hr, const char *function, unsigned line)
{
   if (FAILED(hr)) {
      LPSTR lpMessageBuffer = NULL;

      FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                     FORMAT_MESSAGE_FROM_SYSTEM,
                     NULL,
                     hr,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     (LPSTR)&lpMessageBuffer,
                     0,
                     NULL);

      DebugPrintf("%s: %u: 0x%08lX: %s", function, line, hr, lpMessageBuffer);

      LocalFree(lpMessageBuffer);
   }
}


void
AssertFail(const char *expr,
           const char *file,
           unsigned line,
           const char *function)
{
   DebugPrintf("%s:%u:%s: Assertion `%s' failed.\n", file, line, function, expr);
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
   __asm("int3");
#elif defined(__GNUC__) && defined(__aarch64__)
   __builtin_debugtrap();
#elif defined(_MSC_VER)
   __debugbreak();
#else
   DebugBreak();
#endif
}


/* G6 r7 (A): see LOG_ENTRYPOINT */
volatile unsigned long long UmdDdiTicks;
static thread_local unsigned umd_ddi_depth;

UmdDdiScope::UmdDdiScope(const char *fn)
{
   t0 = 0;
   if (UmdTraceAll())
      DebugPrintf("%s\n", fn);
   if (umd_ddi_depth++ == 0 && UmdTrace()) {
      LARGE_INTEGER q;
      QueryPerformanceCounter(&q);
      t0 = (unsigned long long)q.QuadPart;
   }
}

UmdDdiScope::~UmdDdiScope()
{
   if (--umd_ddi_depth == 0 && t0) {
      LARGE_INTEGER q;
      QueryPerformanceCounter(&q);
      UmdDdiTicks += (unsigned long long)q.QuadPart - t0;
   }
}
