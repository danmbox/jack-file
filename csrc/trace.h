// Copyright (C) 2010-2011 Dan Muresan
// Part of sintvert (http://danmbox.github.com/sintvert/)

#include <string.h>

#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "memfile.h"

typedef enum {
  TRACE_NONE, TRACE_FATAL, TRACE_ERR, TRACE_WARN, TRACE_IMPT, TRACE_INFO, TRACE_DIAG, TRACE_INT
} trace_pri_t;
static const char *trace_level_symb = "-FEW!ID                                   ";
static trace_pri_t trace_level = TRACE_INFO;
static int trace_print_tid = 0, trace_print_fn = 0;
static memfile *trace_buf = NULL;
static FILE *stdtrace = NULL;

#if __GNUC__ >= 4
__attribute__ ((format (printf, 3, 4)))
#endif
static void trace_msg (trace_pri_t pri, const char *fn, const char *fmt, ...)
{
  if (pri > trace_level) return;
  struct timeval tv;
  char timestr [80];
  size_t timestrsz = sizeof (timestr) - 3;
  if (gettimeofday (&tv, NULL) == 0) {
    struct tm t;
    if (localtime_r (& tv.tv_sec, &t) == NULL)
      strcpy (timestr, "<unknown error>");
    char *timestrp = timestr +
      strftime (timestr, timestrsz, "%Y-%m-%d %H:%M:%S.", &t);
    snprintf (timestrp, 4, "%03d", (int) (tv.tv_usec / 1000));
  } else {
    if (strerror_r (errno, timestr, timestrsz) != 0)
      strcpy (timestr, "<unknown error>");
  }
  MUTEX_LOCK_WITH_CLEANUP (&trace_buf->lock); {
    memfile_printf (trace_buf, "%c %s ", trace_level_symb [pri], timestr);
    va_list ap; va_start (ap, fmt);
    memfile_vprintf (trace_buf, fmt, ap);
    if (trace_print_fn) memfile_printf (trace_buf, " [%s]", fn);
    if (trace_print_tid)
      memfile_printf (trace_buf, " [tid=0x%X]", (unsigned long) pthread_self ());
    memfile_printf (trace_buf, "\n");
    va_end (ap);
  } pthread_cleanup_pop (1);
}
static void trace_flush () {
  MUTEX_LOCK_WITH_CLEANUP (&trace_buf->switchlock); {
    int old_cstate; pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cstate);
    memfile_switchbuf (trace_buf);
    char *overflow_ch = &trace_buf->other [trace_buf->sz - 2];
    if (*overflow_ch != '\0') *overflow_ch = '\n';
    fprintf (stdtrace, "%s", trace_buf->other);  // cancellation point
    int dummy; pthread_setcancelstate(old_cstate, &dummy);
  } pthread_cleanup_pop (1);
  fflush (stdtrace);
}
#define TRACE( pri, ... ) trace_msg (pri, __func__, __VA_ARGS__)
#define TRACE_PERROR( pri, called ) do {                            \
    char buf [1000]; strerror_r (errno, buf, sizeof (buf));         \
    TRACE (pri, "Error: %s (%s:%d): %s", called, __FILE__, __LINE__, buf); \
  } while (0)
#define TRACE_ASSERT( cond_, fail ) do {                                \
    if (! (cond_)) {                                                    \
      TRACE (TRACE_FATAL, "Assertion %s failed (%s:%d)",                \
             #cond_, __FILE__, __LINE__);                               \
      fail;                                                             \
    }                                                                   \
  } while (0)
#if 0  // enable as needed when debugging
#include <execinfo.h>
static void print_backtrace () {
  void *array[30];
  size_t size = backtrace (array, 30);
  char **strings = backtrace_symbols (array, size);

  TRACE (TRACE_DIAG, "Obtained %zd stack frames", size);
  for (size_t i = 0; i < size; ++i)
    TRACE (TRACE_DIAG, "%s", strings [i]);
  free (strings);
}
#endif
