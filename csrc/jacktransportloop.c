// Copyright (C) 2011 Dan Muresan
// Part of jack-file (http://danmbox.github.com/jack-file/)

#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>

#include <jack/jack.h>

#include "trace_utils.h"


const char myrelease [] =
#include "release.h"
  ;

// type aliases & constants
#define MYNAME "jacktransportloop"

// configurable params
int afterlife = 0;  ///< don't quit after Jack kills us
int start_transport = 0;
jack_nframes_t t0_frame = JACK_MAX_FRAMES,
  loop_beg_frame = JACK_MAX_FRAMES, loop_end_frame = JACK_MAX_FRAMES;

// internal vars
sigset_t sigmask;
pthread_t poll_thread_tid = 0;
pthread_t main_tid;
jack_nframes_t srate = 0;  ///< Sampling rate
jack_client_t *jclient = NULL;
sem_t zombified;
int cleaned_up = 0;
const char *loop_beg_str = NULL, *loop_end_str = NULL, *t0_str = NULL;

// --- PROGRAM ---

static void myshutdown (int failure);

static void usage (const char *fmt, ...) {
#define NL "\n"
  if (fmt != NULL) {
    printf ("Error: ");
    va_list ap; va_start (ap, fmt);
    vprintf (fmt, ap);
    va_end (ap);
    printf ("\n\n");
  }
  printf ("%s\n",
MYNAME " loops a segment of time in Jack transport."
NL ""
NL "Usage: " MYNAME " [OPTIONS]"
NL "  or:  " MYNAME " { -h | --help | -? | --version }"
NL ""
NL "Options:"
NL "  --seek T0         seek to T0 first"
NL "  --loop START END  activate looping between START and END"
NL "  --start           start the Jack transport rolling initially"
NL ""
NL "START, END and T0 can be in seconds (e.g. 3.750) or samples (e.g. 480s)."
NL "Transport is initially moved to T0 (if supplied)."
NL ""
NL "Transport state is not changed (unless --start is given). Whenever the"
NL "transport reaches END, " MYNAME " moves it back to START."
NL ""
NL "Report bugs at https://github.com/danmbox/jack-file/issues"
          );
#undef NL

  trace_level = TRACE_NONE;
  exit (fmt == NULL ? EXIT_SUCCESS : EXIT_FAILURE);
}

static void *process_thread (void *arg) {
  (void) arg;

  for (;;) {
    // BEGIN REAL-TIME SECTION
    jack_nframes_t nframes = jack_cycle_wait (jclient);

    if (0 == nframes) {
      jack_cycle_signal (jclient, 0);
      continue;
    }

    jack_position_t pos;
    jack_transport_query (jclient, &pos);
    if (pos.frame >= loop_end_frame && pos.frame - loop_end_frame <= nframes &&
        loop_beg_frame != JACK_MAX_FRAMES)
      jack_transport_locate (jclient, loop_beg_frame);

    jack_cycle_signal (jclient, 0);
    // END REAL-TIME SECTION
  }

  return NULL;
}

/// Performs periodic tasks.
/// Stop it with @c pthread_cancel().
static void *poll_thread (void *arg) {
  (void) arg;

  for (;;) {
    trace_flush ();
    struct timespec sleepreq = { tv_sec: 0, tv_nsec: 200000000L };
    nanosleep (&sleepreq, NULL);
  }

  return NULL;
}

static void on_jack_shutdown (void *arg) {
  (void) arg;

  int z = 0;
  if (0 == sem_getvalue (&zombified, &z) && z == 0) {
    sem_post (&zombified);
  }
}

static jack_nframes_t convert_time (const char *t, jack_nframes_t srate) {
  if (t == NULL) return JACK_MAX_FRAMES;
  jack_nframes_t ret;
  char extra;
  int slen = strlen (t);
  int in_frames = t [slen - 1] == 's';
  if (in_frames) {
    if (sscanf (t, "%u%*c%c", &ret, &extra) != 1)
      usage ("Bad number %s", t);
  }
  else {
    double sec;
    if (sscanf (t, "%lf%c", &sec, &extra) != 1)
      usage ("Bad number %s", t);
    ret = sec * srate;
  }
  return ret;
}

static void setup_audio () {
  TRACE (TRACE_DIAG, "jack setup");
  trace_flush (); fflush(NULL);

  jack_options_t jopt = JackNullOption;  // JackNoStartServer;
  jack_status_t jstat;
  ENSURE_CALL_AND_SAVE (jack_client_open, (MYNAME, jopt, &jstat), jclient, NULL);
  jack_on_shutdown (jclient, on_jack_shutdown, NULL);

  srate = jack_get_sample_rate (jclient);
  loop_beg_frame = convert_time (loop_beg_str, srate);
  loop_end_frame = convert_time (loop_end_str, srate);
  t0_frame = convert_time (t0_str, srate);

  ENSURE_CALL (jack_set_process_thread, (jclient, process_thread, NULL) != 0);

  ENSURE_CALL (jack_activate, (jclient) != 0);

  if (t0_frame != JACK_MAX_FRAMES)
    jack_transport_locate (jclient, t0_frame);
  if (start_transport)
    jack_transport_start (jclient);

  TRACE (TRACE_DIAG, "Activated");
}

static void parse_args (char **argv) {
  if (argv [0] == NULL) usage (NULL);
  for (++argv; *argv != NULL; ++argv) {
    if (*argv [0] == '-') {
      if (0 == strcmp (*argv, "-h") || 0 == strcmp (*argv, "--help") ||
          0 == strcmp (*argv, "-?"))
      {
        usage (NULL);
      } else if (0 == strcmp (*argv, "--version")) {
        printf ("%s version %s\n%s\n", MYNAME, myrelease, "Copyright (C) 2011 Dan Muresan");
        exit (EXIT_SUCCESS);
      } else if (0 == strcmp (*argv, "--start")) {
        start_transport = 1;
      } else if (0 == strcmp (*argv, "--seek")) {
        if (NULL == (t0_str = *++argv))
          usage ("Missing arguments");
      } else if (0 == strcmp (*argv, "--loop")) {
        if (NULL == (loop_beg_str = *++argv) || NULL == (loop_end_str = *++argv))
          usage ("Missing arguments");
      } else if (0 == strcmp (*argv, "--log-level")) {
        int l;
        if (sscanf (*++argv, "%d", &l) != 1)
          usage ("Bad log level %s", *argv);
        TRACE (TRACE_IMPT, "Log level %d -> %d", (int) trace_level, l);
        trace_level = l;
      } else if (0 == strcmp (*argv, "--log-tid")) {
        trace_print_tid = 1;
      } else if (0 == strcmp (*argv, "--log-fun")) {
        trace_print_fn = 1;
      } else if (0 == strcmp (*argv, "--zombify")) {
        afterlife = 1;
      } else {
        usage ("unknown option %s", *argv);
      }
    }
    else break;
  }
}

static void init_trace () {
  trace_buf = memfile_alloc (1 << 12);
  stdtrace = fdopen (dup (fileno (stdout)), "w");
  setvbuf (stdtrace, NULL, _IONBF, 0);
  TRACE (TRACE_INFO, "Logging enabled, level=%d", (int) trace_level);
}

static void cleanup () {
  int z = 0;
  if (0 == sem_getvalue (&zombified, &z) && z > 0)
    TRACE (TRACE_FATAL, "Jack shut us down");
  pthread_cancel_and_join_if_started (poll_thread_tid, main_tid);
  if (jclient != NULL) {
    if (z == 0) sem_post (&zombified);  // main already knows about it, signal others
    jack_client_close (jclient);
    jclient = NULL;
  }

  TRACE (TRACE_INFO, "Cleanup finished");
  trace_flush ();
  cleaned_up = 1;
}

// pitfall: name clash with shutdown(2)
static void myshutdown (int failure) {
  TRACE (TRACE_INFO, "shutdown requested");
  // print_backtrace ();
  pthread_sigmask (SIG_BLOCK, &sigmask, NULL);
  cleanup ();
  pthread_sigmask (SIG_UNBLOCK, &sigmask, NULL);
  if (afterlife) pause ();
  switch (failure) {
  case 0: case 1: exit (failure ? EXIT_FAILURE : EXIT_SUCCESS);
  case 2: abort ();
  }
}

static void sig_handler (int sig) {
  // We should in the main thread -- all other threads block signals.
  // But linked libs could screw up our sigmask and handlers...
  if (! pthread_equal (pthread_self (), main_tid)) {
    TRACE (TRACE_WARN, "Signal %d redirected to main thread", sig);
    pthread_kill (main_tid, sig); for (;;) pause ();
  }
  if (sig == SIGPIPE) return;  // we don't do pipes
  TRACE (TRACE_INFO, "Caught signal %d", sig);
  cleanup ();
  struct sigaction act =
    { .sa_mask = sigmask, .sa_flags = 0, .sa_handler = SIG_DFL };
  sigaction (sig, &act, NULL);
  pthread_sigmask (SIG_UNBLOCK, &sigmask, NULL);
  pthread_kill (pthread_self (), sig);
}

/// Initializes variables and resources that cannot be initialized statically.
/// Postcondition: all globals must be in a defined state.
static void init_globals () {
  main_tid = pthread_self ();
  poll_thread_tid = main_tid;
  ENSURE_SYSCALL (sigemptyset, (&sigmask));
  ENSURE_SYSCALL (sem_init, (&zombified, 0, 0));
}

int main (int argc, char **argv) {
  (void) argc;

  init_trace ();

  init_globals ();

  if (! setup_sigs (sig_handler, &sigmask, 7, SIGTERM, SIGQUIT, SIGABRT, SIGPIPE, SIGFPE, SIGINT, SIGALRM)) {
    TRACE_PERROR (TRACE_FATAL, "signal setup");
    return 1;
  }

  parse_args (argv);

  ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigmask, NULL));
  pthread_create (&poll_thread_tid, NULL, poll_thread, NULL);
  ENSURE_SYSCALL (pthread_sigmask, (SIG_UNBLOCK, &sigmask, NULL));

  ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigmask, NULL));
  setup_audio ();
  ENSURE_SYSCALL (pthread_sigmask, (SIG_UNBLOCK, &sigmask, NULL));

  sem_wait (&zombified);

  myshutdown (0); assert (0);
}

// Local Variables:
// write-file-functions: (lambda () (delete-trailing-whitespace) nil)
// compile-command: "cc -Os -g -std=c99 -pthread -Wall -Wextra -march=native -pipe -ljack -lsndfile -lm jacktransportloop.c -o jacktransportloop"
// End:
