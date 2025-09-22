#include "backend_ast/builtin.h"
#include "core/utils.h"

#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/time.h>
#endif

static Value vmBuiltinRealTimeClock(struct VM_s *vm, int arg_count, Value *args) {
  (void)args;
  if (arg_count != 0) {
    runtimeError(vm, "RealTimeClock expects no arguments.");
    return makeDouble(0.0);
  }

#if defined(_WIN32)
  FILETIME ft;
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0602
  GetSystemTimePreciseAsFileTime(&ft);
#else
  GetSystemTimeAsFileTime(&ft);
#endif
  ULARGE_INTEGER ticks;
  ticks.LowPart = ft.dwLowDateTime;
  ticks.HighPart = ft.dwHighDateTime;
  const unsigned long long WINDOWS_TO_UNIX_EPOCH = 11644473600ULL;
  unsigned long long total_100ns = ticks.QuadPart;
  unsigned long long whole_seconds = total_100ns / 10000000ULL;
  unsigned long long remainder_100ns = total_100ns % 10000000ULL;
  long double unix_seconds = (long double)whole_seconds -
                             (long double)WINDOWS_TO_UNIX_EPOCH;
  unix_seconds += (long double)remainder_100ns / 10000000.0L;
  return makeDouble((double)unix_seconds);
#else
#if defined(CLOCK_REALTIME)
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
    long double seconds = (long double)ts.tv_sec +
                          (long double)ts.tv_nsec / 1000000000.0L;
    return makeDouble((double)seconds);
  }
#endif
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    long double seconds = (long double)tv.tv_sec +
                          (long double)tv.tv_usec / 1000000.0L;
    return makeDouble((double)seconds);
  }
  runtimeError(vm, "RealTimeClock is unavailable on this platform.");
  return makeDouble(0.0);
#endif
}

void registerRealTimeClockBuiltin(void) {
  registerVmBuiltin("realtimeclock", vmBuiltinRealTimeClock,
                    BUILTIN_TYPE_FUNCTION, "RealTimeClock");
}
