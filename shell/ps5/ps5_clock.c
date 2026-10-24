/* Crux2 - the title's monotonic clock from the processor's counter: the runtime of ps5_clock.h.
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0+
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see Crux2's NOTICE.
 * Ported to PSFlyCast (from Crux2 v1.0.14, Crux2 by Press5elect): the file as Crux2 has it, but for
 * this header. It replaces the earlier copy from Crux2 v1.0.1, whose readers shared a
 * compare-and-swap on one line (see below).
 *
 * shell/ps5/ps5-link.sh wraps clock_gettime for the whole title (--wrap): CLOCK_MONOTONIC is
 * answered here, every other clock by the kernel. The clock starts at the first call: the
 * counter's rate is asked once and the kernel's clock read once, between two counter reads.
 * Until then, and for good if the rate is not believable, the kernel answers.
 *
 * The base is published under a sequence count, so a reader never mixes two bases: it reads
 * the count, the base, the counter, and the count again, and reads again if the count was odd
 * or changed. Once a second the first caller to find the base a second old (and win the flag)
 * checks the clock against the kernel's and publishes the next base (ps5_clock_rebase): one
 * system call a second for the whole title, where each read was one.
 *
 * A read shares nothing written: no reading went through a compare-and-swap on a line every
 * thread's reads shared (which the graphics driver's recording thread, reading the clock in
 * its wait for work, kept bouncing between cores and so slowed every other thread's reads:
 * Crux2's profile on the console, 2026-10-21). Time never goes back on a thread anyway: the
 * next base starts from where the old one is at its moment, and a reader that still holds the
 * old base can have read the counter only up to the moment the new one was published, which
 * the publisher keeps within PS5_CLOCK_PUBLISH_BOUND_NS of the new base's start, and the
 * new base starts the few nanoseconds ahead that the old pace could have gained over that
 * stretch (a step forward of under 200 ns once a second, never one back).
 */

#include "ps5_clock.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

uint64_t sceKernelReadTsc(void);
uint64_t sceKernelGetTscFrequency(void);
int __real_clock_gettime(clockid_t clock, struct timespec *now);

/* How far past the new base's start a reader holding the old base can have read the counter:
 * the publisher moves the start up while the stretch from it to the publishing is longer. */
#define PS5_CLOCK_PUBLISH_BOUND_NS UINT64_C(200000)

enum { CLOCK_NOT_YET, CLOCK_STARTING, CLOCK_RUNNING, CLOCK_OFF };

static _Atomic int s_state = CLOCK_NOT_YET;
static uint64_t s_hz, s_reported_mult, s_bound_ticks; /* written before s_state becomes RUNNING */
/* The checking thread's (under s_checking): the pace in use and where the measuring of it began. */
static uint64_t s_nominal_mult, s_start_tsc, s_start_kernel;

static _Atomic uint64_t s_seq;
static _Atomic uint64_t s_tsc0, s_mono0, s_mult;
static atomic_flag s_checking = ATOMIC_FLAG_INIT;

/* What the checks found, for the boot log. */
static _Atomic uint64_t s_checks;
static _Atomic int64_t s_largest_difference;

static uint64_t
kernel_ns(void)
{
   struct timespec now;
   if (__real_clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * PS5_CLOCK_NS_PER_S + (uint64_t)now.tv_nsec;
}

/* The base in force, with the counter read under the same sequence count. */
static struct ps5_clock_base
current_base(uint64_t *tsc)
{
   struct ps5_clock_base base;
   for (;;) {
      const uint64_t before = atomic_load_explicit(&s_seq, memory_order_acquire);
      if (before & 1)
         continue;
      base.tsc0 = atomic_load_explicit(&s_tsc0, memory_order_relaxed);
      base.mono0 = atomic_load_explicit(&s_mono0, memory_order_relaxed);
      base.mult = atomic_load_explicit(&s_mult, memory_order_relaxed);
      *tsc = sceKernelReadTsc();
      atomic_thread_fence(memory_order_acquire);
      if (atomic_load_explicit(&s_seq, memory_order_relaxed) == before)
         return base;
   }
}

/* Publishes `next`, which goes on from `old` at next->tsc0. The readers wait while the count is
 * odd; one that had the old base and the count even read the counter before the count went odd,
 * so no later than the counter reads here, which the start is moved up to while they are more
 * than the bound past it. The new base then starts ahead by what the old pace could have gained
 * over the bound, so no reading of the old base is ahead of the new one. */
static void
publish(struct ps5_clock_base next, const struct ps5_clock_base *old)
{
   const uint64_t seq = atomic_load_explicit(&s_seq, memory_order_relaxed);
   atomic_store_explicit(&s_seq, seq + 1, memory_order_relaxed);
   atomic_thread_fence(memory_order_release);
   if (old) {
      /* (A publisher held up again and again - the thread taken off its core at each try - keeps
       * the gap it had after a few tries and covers that instead.) */
      uint64_t gap = s_bound_ticks;
      for (int tries = 0;; tries++) {
         const uint64_t now = sceKernelReadTsc();
         if (now < next.tsc0 || now - next.tsc0 <= s_bound_ticks)
            break;
         if (tries == 3) {
            gap = (now - next.tsc0) + s_bound_ticks;
            break;
         }
         next.mono0 = ps5_clock_ns(old, now);
         next.tsc0 = now;
      }
      if (old->mult > next.mult)
         next.mono0 += (uint64_t)(((unsigned __int128)gap * (old->mult - next.mult)) >> 32);
   }
   atomic_store_explicit(&s_tsc0, next.tsc0, memory_order_relaxed);
   atomic_store_explicit(&s_mono0, next.mono0, memory_order_relaxed);
   atomic_store_explicit(&s_mult, next.mult, memory_order_relaxed);
   atomic_store_explicit(&s_seq, seq + 2, memory_order_release);
}

static void
start(void)
{
   int expected = CLOCK_NOT_YET;
   if (!atomic_compare_exchange_strong(&s_state, &expected, CLOCK_STARTING))
      return; /* another thread starts it; the kernel answers this call */
   const uint64_t hz = sceKernelGetTscFrequency();
   const uint64_t mult = ps5_clock_mult(hz);
   if (mult == 0) {
      atomic_store_explicit(&s_state, CLOCK_OFF, memory_order_release);
      return;
   }
   const uint64_t before = sceKernelReadTsc();
   const uint64_t kernel = kernel_ns();
   const uint64_t after = sceKernelReadTsc();
   if (kernel == 0 || after < before) {
      atomic_store_explicit(&s_state, CLOCK_OFF, memory_order_release);
      return;
   }
   s_hz = hz;
   s_reported_mult = mult;
   s_nominal_mult = mult;
   s_bound_ticks = (uint64_t)(((unsigned __int128)hz * PS5_CLOCK_PUBLISH_BOUND_NS) / PS5_CLOCK_NS_PER_S);
   s_start_tsc = before + (after - before) / 2;
   s_start_kernel = kernel;
   const struct ps5_clock_base base = {s_start_tsc, kernel, mult};
   publish(base, NULL);
   atomic_store_explicit(&s_state, CLOCK_RUNNING, memory_order_release);
}

static void
check(void)
{
   if (atomic_flag_test_and_set_explicit(&s_checking, memory_order_acquire))
      return; /* another thread is checking */
   uint64_t before;
   const struct ps5_clock_base base = current_base(&before);
   if (before - base.tsc0 >= s_hz && before >= base.tsc0) {
      const uint64_t kernel = kernel_ns();
      const uint64_t after = sceKernelReadTsc();
      if (kernel != 0 && after >= before) {
         const uint64_t at = before + (after - before) / 2;
         s_nominal_mult = ps5_clock_learned_mult(s_reported_mult, s_start_tsc, s_start_kernel, at, kernel);
         int64_t difference = 0;
         const struct ps5_clock_base next = ps5_clock_rebase(&base, s_nominal_mult, at, kernel, &difference);
         publish(next, &base);
         if (difference > (int64_t)PS5_CLOCK_JUMP_NS) {
            /* The kernel's clock ran on without the counter: the pace is measured from here. */
            s_start_tsc = at;
            s_start_kernel = kernel;
         }
         atomic_fetch_add_explicit(&s_checks, 1, memory_order_relaxed);
         const int64_t size = difference < 0 ? -difference : difference;
         int64_t largest = atomic_load_explicit(&s_largest_difference, memory_order_relaxed);
         while (size > largest &&
                !atomic_compare_exchange_weak_explicit(&s_largest_difference, &largest, size, memory_order_relaxed,
                                                      memory_order_relaxed)) {
         }
      }
   }
   atomic_flag_clear_explicit(&s_checking, memory_order_release);
}

/* The clock's reading, false while it does not run. */
static bool
fast_ns(uint64_t *ns)
{
   const int state = atomic_load_explicit(&s_state, memory_order_acquire);
   if (state != CLOCK_RUNNING) {
      if (state == CLOCK_NOT_YET)
         start();
      if (atomic_load_explicit(&s_state, memory_order_acquire) != CLOCK_RUNNING)
         return false;
   }
   uint64_t tsc;
   struct ps5_clock_base base = current_base(&tsc);
   if (tsc - base.tsc0 >= s_hz && tsc >= base.tsc0) {
      check();
      base = current_base(&tsc);
   }
   *ns = ps5_clock_ns(&base, tsc);
   return true;
}

int
__wrap_clock_gettime(clockid_t clock, struct timespec *now)
{
   uint64_t ns;
   if (clock == CLOCK_MONOTONIC && now && fast_ns(&ns)) {
      now->tv_sec = (time_t)(ns / PS5_CLOCK_NS_PER_S);
      now->tv_nsec = (long)(ns % PS5_CLOCK_NS_PER_S);
      return 0;
   }
   return __real_clock_gettime(clock, now);
}

uint64_t
ps5px_monotonic_ns(void)
{
   uint64_t ns;
   return fast_ns(&ns) ? ns : kernel_ns();
}

void
ps5px_clock_line(char *line, unsigned size)
{
   if (!line || size == 0)
      return;
   line[0] = '\0';
   const int state = atomic_load_explicit(&s_state, memory_order_acquire);
   if (state == CLOCK_OFF) {
      snprintf(line, size, "clock: the kernel's (the counter's rate was not believable)");
      return;
   }
   if (state != CLOCK_RUNNING)
      return;
   snprintf(line, size,
            "clock: the processor's counter at %llu Hz; checked against the kernel's %llu times, "
            "%.3f ms apart at most",
            (unsigned long long)s_hz, (unsigned long long)atomic_load_explicit(&s_checks, memory_order_relaxed),
            (double)atomic_load_explicit(&s_largest_difference, memory_order_relaxed) / 1e6);
}
