#include "argparsing.h"
#include <qthread/qthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SKIP_CONCURRENT_WRITEFF_TEST
#endif
#endif

// Test that a writeFF on a full var performs the write, and leaves the FEB
// state untouched.
static void testBasicWriteFF(void) {
  aligned_t x, val;

  x = 45, val = 55;
  test_check(qthread_feb_status(&x) == 1);

  iprintf("Before x=%d, val=%d, x_full=%d\n", x, val, qthread_feb_status(&x));
  qthread_writeFF(&x, &val);
  iprintf("After  x=%d, val=%d, x_full=%d\n", x, val, qthread_feb_status(&x));

  test_check(qthread_feb_status(&x) == 1);
  test_check(x == 55);
  test_check(val == 55);
}

#define ALL_ONES ~0u
#define ALL_ZEROS 0
#define ITERS_PER_WORKER 1000

// This test deliberately creates a race condition, so
// don't run it when thread sanitizer is enabled.
#ifndef SKIP_CONCURRENT_WRITEFF_TEST
static aligned_t concurrent_t;

static aligned_t alignedWriteFF_iters(void *arg) {
  aligned_t v = (aligned_t)(intptr_t)arg;
  for (int i = 0; i < ITERS_PER_WORKER; i++) {
    qthread_writeFF_const(&concurrent_t, v);
  }
  return 0;
}

// Test that concurrent writeFFs work and that no tearing occurs
static void testConcurrentWriteFF(void) {
  int num_writers = (int)qthread_num_workers() / 2;
  if (num_writers <= 1) num_writers = 2;
  aligned_t rets[num_writers * 2];
  concurrent_t = 0;
  qthread_fill(&concurrent_t);

  for (int i = 0; i < num_writers; i++) {
    qthread_fork(alignedWriteFF_iters, (void *)ALL_ZEROS, &rets[i]);
    qthread_fork(
      alignedWriteFF_iters, (void *)ALL_ONES, &rets[i + num_writers]);
  }

  for (int i = 0; i < num_writers * 2; i++) {
    qthread_readFF(&rets[i], &rets[i]);
  }

  iprintf("concurrent_t=%x\n", concurrent_t);
  test_check((concurrent_t == ALL_ZEROS) || (concurrent_t == ALL_ONES));
  test_check(qthread_feb_status(&concurrent_t) == 1);
}
#endif

int main(int argc, char *argv[]) {
  CHECK_VERBOSE();
  test_check(qthread_initialize() == 0);
  iprintf("%i shepherds...\n", qthread_num_shepherds());
  iprintf("  %i threads total\n", qthread_num_workers());

  testBasicWriteFF();
#ifndef SKIP_CONCURRENT_WRITEFF_TEST
  testConcurrentWriteFF();
#endif

  return 0;
}

/* vim:set expandtab */
