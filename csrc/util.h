// Copyright (C) 2010-2011 Dan Muresan
// Part of sintvert (http://danmbox.github.com/sintvert/)

#ifndef __SINTVERT__UTIL_H
#define __SINTVERT__UTIL_H

#include <stdarg.h>
#include <assert.h>

#include <pthread.h>
#include <signal.h>

#define SQR( x ) ((x) * (x))
#define ABS( x ) ((x) > 0 ? (x) : -(x))
#define MAX( x, y )  ((x) > (y) ? (x) : (y))

static void mutex_cleanup_routine (void *lock_) {
  pthread_mutex_unlock ((pthread_mutex_t *) lock_);
}
/// Locks a mutex and pushes a @c pthread_cleanup routine.
/// Must be matched with a <code>pthread_cleanup_pop (1)</code>
#define MUTEX_LOCK_WITH_CLEANUP( lock )                 \
  pthread_mutex_lock (lock);                            \
  pthread_cleanup_push (mutex_cleanup_routine, (lock))

static int setup_sigs (void (*sig_handler) (int), sigset_t *sigmask, unsigned n, ...) {
  va_list ap; va_start (ap, n);
  for (unsigned i = 0; i < n; ++i)
    if (0 != sigaddset (sigmask, va_arg (ap, int)))
      return 0;
  va_end (ap);
  struct sigaction act =
    { .sa_mask = *sigmask, .sa_flags = 0, .sa_handler = sig_handler };
  va_start (ap, n);
  for (unsigned i = 0; i < n; ++i)
    if (0 != sigaction (va_arg (ap, int), &act, NULL))
      return 0;
  va_end (ap);

  return 1;
}


#endif  // __SINTVERT__UTIL_H
