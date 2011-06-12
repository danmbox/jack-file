#include "trace.h"

/// Sends @c SIGABRT to the process, then loops forever.
/// This function is necessary because plain @c abort() unblocks SIGABRT,
/// making it impossible to guarantee signal delivery to a specific thread.
static void myabort () {
  kill (getpid (), SIGABRT);
  for (;;) pause ();
}

#define ASSERT( cond ) TRACE_ASSERT (cond, myabort ());

#define ENSURE_SYSCALL( syscall, args )                           \
  do {                                                            \
    if (-1 == (syscall args)) {                                   \
      TRACE_PERROR (TRACE_FATAL, #syscall); myabort ();           \
    }                                                             \
  } while (0)
#define ENSURE_SYSCALL_AND_SAVE( syscall, args, rc )              \
  do {                                                            \
    if (-1 == (rc = (syscall args))) {                            \
      TRACE_PERROR (TRACE_FATAL, #syscall); myabort ();            \
    }                                                             \
  } while (0)
// E.g. ENSURE_CALL (myfun, (arg1, arg2) != 0)
#define ENSURE_CALL( call, args)                                        \
  do {                                                                  \
    if (call args) {                                                    \
      TRACE (TRACE_FATAL, #call " failed (%s:%d)", __FILE__, __LINE__); \
      myabort ();                                                       \
    }                                                                   \
  } while (0)
#define ENSURE_CALL_AND_SAVE( call, args, rc, bad )                     \
  do {                                                                  \
    if (bad == (rc = (call args))) {                                    \
      TRACE (TRACE_FATAL, #call " failed (%s:%d)", __FILE__, __LINE__); \
      abort ();                                                         \
    }                                                                   \
  } while (0)
