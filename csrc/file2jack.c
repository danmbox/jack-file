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
#include <jack/ringbuffer.h>
#include <sndfile.h>

#include "trace_utils.h"

const char myrelease [] =
#include "release.h"
  ;

// type aliases & constants
#define MYNAME "file2jack"
typedef jack_default_audio_sample_t sample_t;

// configurable params
int afterlife = 0;  ///< don't quit after Jack kills us
int jbuf_frags = 0;  ///< Fragments / cache
float prefill = 0.25;  ///< Cache prefill fraction
float cache_secs = 0.0;

// internal vars
sigset_t sigmask;
pthread_t poll_thread_tid = 0; int poll_thread_started = 0;
pthread_t main_tid;
jack_nframes_t srate = 0;  ///< Sampling rate
jack_client_t *jclient = NULL;
sem_t zombified;
int cleaned_up = 0;
unsigned nports = 0;  ///< Number of output ports
jack_port_t **jport = NULL;  ///< Jack audio ports
jack_ringbuffer_t *jbuf = NULL;  ///< Outgoing audio buffer
/// Counting semaphore for free space in @c jbuf (in HALF-fragments).
/// We use 1/2s so we can wait for the semaphore (in the disk thread)
/// before storing the actual data; when the transfer completes, we signal
/// the remaining 1/2. This works because the process thread ignores 1/2s.
sem_t *jbuf_free_sem = NULL;
sem_t jbuf_free_sem_store;  // Actual semaphore for above, invalid initially
int jbuf_free_max = -1;  ///< Max (and initial) value for @c jbuf_free_sem
/// Size of @c jbuf in samples (NOT frames).
jack_nframes_t jbuf_len = 0;
jack_nframes_t frag_frames = JACK_MAX_FRAMES;  ///< Frames / fragment
jack_nframes_t jperiodframes = JACK_MAX_FRAMES;  ///< Frames / Jack period
unsigned nfiles = 0;
const char **fname = NULL;  ///< Names of input files
SNDFILE **infile = NULL;

// --- UTILS ---

// Since sndfile doesn't have it...
static sf_count_t my_sf_tell (SNDFILE *sndfile)
{ return sf_seek (sndfile, 0, SEEK_CUR); }

// --- END UTILS ---


// --- PROGRAM ---

static void myshutdown (int failure);


// Variables for process thread
sample_t **jportbuf = NULL;  ///< Jack buffers for audio ports

/// Transport sync callback.
/// Executed in the process thread per Jack docs.
static int on_jack_sync (jack_transport_state_t state, jack_position_t *pos, void *arg) {
  (void) arg;

  return 1;
}

static void *process_thread (void *arg) {
  (void) arg;

  sample_t *data = NULL;
  jack_nframes_t jdatalen = 0;

  for (;;) {
    if (jdatalen == 0) {
      if (data != NULL) {
        jack_ringbuffer_read_advance (jbuf, frag_frames * nports * sizeof (data [0]));
        for (int i = 0; i < 2; i++)
        ENSURE_SYSCALL (sem_post, (jbuf_free_sem));
        data = NULL;
      }
      int semval = 0;
      ENSURE_SYSCALL (sem_getvalue, (jbuf_free_sem, &semval));
      if (semval <= jbuf_free_max - 2) {  // don't count on a half-fragment
        jack_ringbuffer_data_t jdatainfo [2];
        jack_ringbuffer_get_read_vector (jbuf, jdatainfo);
        jdatalen = jdatainfo [0].len / sizeof (sample_t) / nports;
        ASSERT (jdatalen >= frag_frames);
        jdatalen = frag_frames;
        data = (sample_t *) jdatainfo [0].buf;
      }
    }

    // BEGIN REAL-TIME SECTION

    jack_nframes_t nframes = jack_cycle_wait (jclient);
    ASSERT (nframes == jperiodframes);

    for (unsigned i = 0; i < nports; ++i) {
      ASSERT (jport [i] != NULL);
      jportbuf [i] = jack_port_get_buffer (jport [i], nframes);
      ASSERT (jportbuf [i] != NULL);
    }

    if (data == NULL) {
      TRACE (TRACE_WARN, "Underrun");
      for (unsigned i = 0; i < nports; i++)
        memset (jportbuf [i], 0, nframes * sizeof (data [0]));
    } else {
      // un-interleave samples
      for (jack_nframes_t f = 0; f < nframes; ++f)
        for (unsigned i = 0; i < nports; ++i)
          jportbuf [i] [f] = data [f * nports + i];
      data += nframes * nports;
      jdatalen -= nframes;
    }

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

static void disk_thread () {
  for (;;) {
    sem_wait (jbuf_free_sem);  // this frees a half-fragment
    jack_ringbuffer_data_t jdatainfo [2];
    jack_ringbuffer_get_write_vector (jbuf, jdatainfo);
    ASSERT (jdatainfo [0].len / sizeof (sample_t) / nports >= frag_frames);
    sample_t *data = (sample_t *) jdatainfo [0].buf;
    sf_count_t read = sf_read_float (infile [0], data, frag_frames);
    if (read < frag_frames)
      memset (data + read, 0, (frag_frames - read) * nports * sizeof (sample_t));
    jack_ringbuffer_write_advance (jbuf, frag_frames * nports * sizeof (sample_t));
    sem_wait (jbuf_free_sem);  // now mark the other half as free
  }
}

static void setup_input_files () {
  ASSERT (nfiles > 0);
  infile = malloc (nfiles * sizeof (infile [0]));
  for (unsigned i = 0; i < nfiles; ++i) {
    SF_INFO sf_info; memset (&sf_info, 0, sizeof (sf_info));
    infile [i] = sf_open (fname [i], SFM_READ, &sf_info);
    if (NULL == infile [i]) {
      TRACE (TRACE_FATAL, "Could not open file %s", fname [i]);
      myshutdown (1);
    }
    if (srate == 0) srate = sf_info.samplerate;
    else if (srate != (unsigned) sf_info.samplerate) {
      TRACE (TRACE_FATAL, "File %s has sample rate %d != %d for previous files",
             fname [i], sf_info.samplerate, srate);
      myshutdown (1);
    }
    if (0 == nports) nports = sf_info.channels;
    else if (nports != (unsigned) sf_info.channels) {
      TRACE (TRACE_FATAL, "File %s has # of channels %d != %d for previous files",
             fname [i], sf_info.channels, nports);
      myshutdown (1);
    }
  }
}

static void on_jack_shutdown (void *arg) {
  (void) arg;

  int z = 0;
  if (0 == sem_getvalue (&zombified, &z) && z == 0) {
    sem_post (&zombified);
    // TODO: Wake up get_jsample(). It will test zombified and realize there's no
    // extra sample.
  }
}

static void setup_audio () {
  TRACE (TRACE_DIAG, "jack setup");
  trace_flush (); fflush(NULL);

  jack_options_t jopt = JackNullOption;  // JackNoStartServer;
  jack_status_t jstat;
  ENSURE_CALL_AND_SAVE (jack_client_open, (MYNAME, jopt, &jstat), jclient, NULL);
  jack_on_shutdown (jclient, on_jack_shutdown, NULL);

  jack_nframes_t jsrate = jack_get_sample_rate (jclient);
  if (jsrate != srate) {
    TRACE (TRACE_FATAL, "Jack sample rate %d does not match input files (%d)",
           jsrate, srate);
    myshutdown (1);
  }
  jperiodframes = jack_get_buffer_size (jclient);
  if (cache_secs > 0) jbuf_len = nports * srate * cache_secs + 0.5;
  else jbuf_len = (1 << 18) * nports;
  if (jbuf_frags < 4) {
    jbuf_frags = jbuf_len / 0.25 / nports / srate;
    if (jbuf_frags < 32) {  // ensure power of 2
      int i = 4;
      for (; i < jbuf_frags; i <<= 1);
      jbuf_frags = i;
    }
  }
  {  // fragment must contain integer # of periods
    jack_nframes_t denom = jperiodframes * nports * jbuf_frags;
    jbuf_len = ((jbuf_len - 1) / denom + 1) * denom;
  }
  frag_frames = jbuf_len / (jbuf_frags * nports);
  jbuf_free_max = 2 * (jbuf_frags - 1);  // One frag wasted due to sentinel byte...

  jbuf = jack_ringbuffer_create (jbuf_len * sizeof (sample_t)); ASSERT (jbuf != NULL);
  ENSURE_SYSCALL (sem_init, (&jbuf_free_sem_store, 0, jbuf_free_max));
  jbuf_free_sem = &jbuf_free_sem_store;

  TRACE (TRACE_INFO, "Connected to jack, period=%d, rate=%d, cache=%.3f sec, %d fragments",
         (int) jperiodframes, (int) srate, (float) jbuf_len / (srate * nports), jbuf_frags);

  jport = malloc (nports * sizeof (jport [0]));
  for (unsigned i = 0; i < nports; ++i) {
    char name [10];
    snprintf (name, sizeof (name), "out%02u", i);
    ENSURE_CALL_AND_SAVE (jack_port_register, (jclient, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0), jport [i], NULL);
  }
  jportbuf = malloc (nports * sizeof (jportbuf [0]));

  ENSURE_CALL (jack_set_process_thread, (jclient, process_thread, NULL) != 0);

  ENSURE_CALL (jack_set_sync_callback, (jclient, on_jack_sync, NULL) != 0);

  ENSURE_CALL (jack_activate, (jclient) != 0);

  TRACE (TRACE_DIAG, "Activated");
}

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
MYNAME " is a Jack transport-centric audio player. It maps files onto the"
NL "(possibly periodic) transport timeline."
NL ""
NL "Usage: " MYNAME " [OPTIONS] [[--at POSn] -i FILEn] ... [LOOP]"
NL "  or:  " MYNAME " { -h | --help | -? | --version }"
NL ""
NL "Options:"
NL "  --cache SEC     Cache size; default: ~6s for 44.1 kHz stereo"
NL "  --prefill PERC  Percentage of cache to prefill (default: 25%)"
NL ""
NL "Final LOOP option: [--pos LPOS] --loop T0 { T1 | : }"
NL "  Repeats the T0-T1 segment from LPOS to infinity. LPOS defaults to the end"
NL "  of the last file. T1, if given as ':', is also the end of the last file."
NL ""
NL "All positions can be given in seconds or samples (e.g. 480s)."
NL ""
NL "Report bugs at https://github.com/danmbox/jack-file/issues"
          );
#undef NL

  trace_level = TRACE_NONE;
  exit (fmt == NULL ? EXIT_SUCCESS : EXIT_FAILURE);
}

static void parse_args (char **argv) {
  if (argv [0] == NULL) usage (NULL);

  unsigned nfiles_max = 100;
  fname = malloc (nfiles_max * sizeof (fname [0]));

  for (++argv; *argv != NULL; ++argv) {
    if (*argv [0] == '-') {
      if (0 == strcmp (*argv, "-h") || 0 == strcmp (*argv, "--help") ||
          0 == strcmp (*argv, "-?"))
      {
        usage (NULL);
      } else if (0 == strcmp (*argv, "--version")) {
        printf ("%s version %s\n%s\n", MYNAME, myrelease, "Copyright (C) 2011 Dan Muresan");
        exit (EXIT_SUCCESS);
      } else if (0 == strcmp (*argv, "-i")) {
        ASSERT (nfiles < nfiles_max);
        fname [nfiles++] = *++argv;
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
      }
    }
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
  if (poll_thread_started) {
    pthread_cancel (poll_thread_tid);
    pthread_join (poll_thread_tid, NULL);
  }
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
  // We should be in the main thread -- all other threads block signals.
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

  setup_input_files ();

  ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigmask, NULL));
  setup_audio ();
  ENSURE_SYSCALL (pthread_sigmask, (SIG_UNBLOCK, &sigmask, NULL));

  disk_thread ();

  for (;;) pause ();

  myshutdown (0); assert (0);
}

// Local Variables:
// write-file-functions: (lambda () (delete-trailing-whitespace) nil)
// compile-command: "cc -Os -g -std=c99 -pthread -Wall -Wextra -march=native -pipe -ljack -lsndfile -lm file2jack.c -o file2jack"
// End:
