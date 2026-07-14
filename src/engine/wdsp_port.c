/* wdsp_port.c — the two utility surfaces our WDSP *subset* needs.
 *
 * We vendor only fir.c / resample.c / impulse_cache.c (see vendor/wdsp/
 * VENDOR.md). Upstream, malloc0 lives in utilities.c and the CriticalSection
 * wrappers in linux_port.c — but utilities.c drags in the txa/ch engine
 * globals via its debug printers and linux_port.c drags in the whole WDSP
 * thread runtime (wdspmain, iobuffs, calcc…). Rather than pull half the
 * engine for two helpers, this shim reimplements them 1:1 (bodies match
 * upstream: malloc0 = zeroed malloc — linux_port.h maps _aligned_malloc to
 * plain malloc on Linux; CriticalSection = recursive pthread mutex).
 *
 * Signatures come from WDSP's own headers so a drift breaks the build, not
 * the runtime. Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

void *malloc0(int size) {
  void *p = malloc((size_t)size);
  if (p != NULL) { memset(p, 0, (size_t)size); }
  return p;
}

static void init_crit_section(pthread_mutex_t *mutex) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
  pthread_mutex_init(mutex, &attr);
  pthread_mutexattr_destroy(&attr);
}

void InitializeCriticalSection(pthread_mutex_t *mutex) {
  init_crit_section(mutex);
}

void InitializeCriticalSectionAndSpinCount(pthread_mutex_t *mutex, int count) {
  (void)count;
  init_crit_section(mutex);
}

void EnterCriticalSection(pthread_mutex_t *mutex) {
  pthread_mutex_lock(mutex);
}

void LeaveCriticalSection(pthread_mutex_t *mutex) {
  pthread_mutex_unlock(mutex);
}

void DeleteCriticalSection(pthread_mutex_t *mutex) {
  pthread_mutex_destroy(mutex);
}
