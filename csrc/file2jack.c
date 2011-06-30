// Copyright (C) 2011 Dan Muresan
// Part of jack-file (http://danmbox.github.com/jack-file/)

#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

#include <sndfile.h>

#include "trace_utils.h"
#include "tmutil.h"

const char myrelease [] =
#include "release.h"
  ;

// type aliases & constants
#define MYNAME "file2jack"
typedef jack_default_audio_sample_t sample_t;

// other types

typedef struct {
  int fd;
  sf_count_t sz;
} mysf_vio_data;

// configurable params
int afterlife = 0;  ///< don't quit after Jack kills us
int jbuf_frags = 0;  ///< Fragments / cache
float cache_secs = 0.0;
float soft_vol = 100.0;

// internal vars
sigset_t sigmask, sigusr2_mask;
pthread_t poll_thread_tid;
pthread_t main_tid;
pthread_t disk_thread_tid [1];
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

/// Frame that Jack transport wants us to move to.
jack_nframes_t reloc_frame = JACK_MAX_FRAMES;
/// Jack periods left till target transport frame is reached
int reloc_countdown = 0;
/// Lock for @p reloc_frame
pthread_mutex_t reloc_lock = PTHREAD_MUTEX_INITIALIZER;
int reloc_pending = 0;  ///< 1 if a transport relocation request is pending

int nfiles = 0;
const char **fname = NULL;  ///< Names of input files
const char **ftpos_str = NULL;  ///< Positions specified by user
mysf_vio_data *infile_vio = NULL;
SNDFILE **infile = NULL;
jack_nframes_t *ftpos = NULL;  ///< Position of files on transport timeline
jack_nframes_t loop_start_frame = JACK_MAX_FRAMES;
const char *loop_start_str = NULL;
unsigned loop_start_file = -1;
pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;


// --- PROGRAM ---

static void myshutdown (int failure);
static void sig_handler (int sig);

// Variables for process thread
sample_t **jportbuf = NULL;  ///< Jack buffers for audio ports
sample_t *jdata = NULL;
jack_nframes_t jdatalen = 0;
int player_ready = 0;  ///< Is relocation & cache prefilling complete?

/// Attempts to post a relocation request (if necessary).
/// If sucessful, computes @c reloc_frame based on @c reloc_countdown.
/// @return target frame; JACK_MAX_FRAMES for temporary failure, or when relocation isn't necessary
jack_nframes_t post_relocation (jack_nframes_t frame) {
  jack_nframes_t target_frame = frame + reloc_countdown * jperiodframes;

  if (0 == pthread_mutex_trylock (&reloc_lock)) {
    if (reloc_frame == target_frame) {
      if (reloc_pending) player_ready = 0;
      else {
        int semval2;
        ENSURE_SYSCALL (sem_getvalue, (jbuf_free_sem, &semval2));
        // start when 1 fragment is ready
        player_ready = ((float) ((jbuf_free_max - semval2) / 2)) >= 1;
        if (player_ready) reloc_frame = JACK_MAX_FRAMES;
      }
      ENSURE_SYSCALL (pthread_mutex_unlock, (&reloc_lock));
      TRACE (TRACE_INT, "Repeated relocation, target=%u, ready=%d", target_frame, player_ready);
      return JACK_MAX_FRAMES;
    } else {  // reloc_frame != target_frame
      int signalled = 0;
      player_ready = 0;
      reloc_frame = target_frame;
      if (! reloc_pending) {  // signal disk thread
        ASSERT (! pthread_equal (main_tid, disk_thread_tid [0]));
        ENSURE_SYSCALL (pthread_kill, (disk_thread_tid [0], SIGUSR2));
        signalled = 1;
        reloc_pending = 1;
      }
      ENSURE_SYSCALL (pthread_mutex_unlock, (&reloc_lock));

      jdata = NULL; jdatalen = 0;
      TRACE (TRACE_INT, "Posted relocation to %u%s", target_frame,
             signalled ? " & signalled" : "");
      return target_frame;
    }
  } else {
    TRACE_PERROR (TRACE_INT, "Couldn't obtain lock -- deferred till next period");
    // Couldn't lock @c reloc_lock -- disk thread is using it.
    // We'll have to retry during the next Jack cycle.
    player_ready = 0;
    return JACK_MAX_FRAMES;
  }
}

/// Transport sync callback -- we are a slow-sync client.
/// Executed in the process thread per Jack docs.
static int on_jack_sync (jack_transport_state_t state, jack_position_t *pos, void *arg) {
  (void) arg;

  switch (state) {
  case JackTransportStopped:
    player_ready = 0;
  case JackTransportStarting:
    reloc_countdown = 0;
    post_relocation (pos->frame);
    TRACE (TRACE_INT, "start/stop, frame=%u", pos->frame);
    break;
  case JackTransportRolling:
    if (--reloc_countdown < 0) {
      reloc_countdown = 2;
      jack_nframes_t target = post_relocation (pos->frame);
      if (target == JACK_MAX_FRAMES) reloc_countdown = 0;
      else {
        jack_transport_stop (jclient);
        jack_transport_locate (jclient, target);
        jack_transport_start (jclient);
        TRACE (TRACE_WARN, "Slow-sync failed, forcing a stop/restart to %u", target);
      }
    }
    break;
  default: ASSERT (0);
  }

  if (player_ready) {
    TRACE (TRACE_DIAG, "Transport relocation completed @%u", pos->frame);
  }
  return player_ready;
}

void clear_port_bufs (sample_t **jportbuf, jack_nframes_t nframes) {
  for (unsigned i = 0; i < nports; i++)
    memset (jportbuf [i], 0, nframes * sizeof (sample_t));
}

static void *process_thread (void *arg) {
  (void) arg;

  for (;;) {
    if (jdatalen == 0 && player_ready) {
      if (jdata != NULL) {
        jack_ringbuffer_read_advance (jbuf, frag_frames * nports * sizeof (sample_t));
        for (int i = 0; i < 2; i++)
          ENSURE_SYSCALL (sem_post, (jbuf_free_sem));
        jdata = NULL;
      }
      int semval;
      ENSURE_SYSCALL (sem_getvalue, (jbuf_free_sem, &semval));
      if (semval <= jbuf_free_max - 2) {  // don't count on half-fragments
        jack_ringbuffer_data_t jdatainfo [2];
        jack_ringbuffer_get_read_vector (jbuf, jdatainfo);
        jdatalen = jdatainfo [0].len / sizeof (sample_t) / nports;
        ASSERT (jdatalen >= frag_frames);
        jdatalen = frag_frames;
        jdata = (sample_t *) jdatainfo [0].buf;
      }
    }

    // BEGIN REAL-TIME SECTION

    jack_nframes_t nframes = jack_cycle_wait (jclient);
    ASSERT (nframes == jperiodframes);
    jack_transport_state_t jtstate = jack_transport_query (jclient, NULL);

    for (unsigned i = 0; i < nports; ++i) {
      ASSERT (jport [i] != NULL);
      jportbuf [i] = jack_port_get_buffer (jport [i], nframes);
      ASSERT (jportbuf [i] != NULL);
    }

    if (jtstate != JackTransportRolling || ! player_ready) {
      clear_port_bufs (jportbuf, nframes);
    } else if (jdata == NULL) {
      TRACE (TRACE_WARN, "Underrun");
      clear_port_bufs (jportbuf, nframes);
    } else {
      // un-interleave samples
      for (jack_nframes_t f = 0; f < nframes; ++f)
        for (unsigned i = 0; i < nports; ++i)
          jportbuf [i] [f] = soft_vol / 100.0 * jdata [f * nports + i];
      jdata += nframes * nports;
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
    struct timespec sleepreq = { tv_sec: 0, tv_nsec: 166000000L };
    nanosleep (&sleepreq, NULL);
  }

  return NULL;
}

/// Flag set by virtual I/O functions
volatile sig_atomic_t disk_cancel_flag = 0;

sf_count_t  mysf_vio_get_filelen (void *user_data) {
  return ((mysf_vio_data *) user_data)->sz;
}

sf_count_t  mysf_vio_seek (sf_count_t offset, int whence, void *user_data) {
  off_t rc = (off_t) -1;
  ENSURE_SYSCALL_AND_SAVE (lseek, (((mysf_vio_data *) user_data)->fd, offset, whence), rc);
  TRACE (TRACE_INT, "lseek %lld (whence=%d), ret=%ld", offset, whence, rc);
  return rc;
}

typedef ssize_t (*io_syscall) (int, void *, size_t);

#if 0
static int test_disk_cancel_sem () {
  TRACE (TRACE_INT + 3, "testing IO cancellation");
  for (;;) {  // test for cancelled I/O
    int rc = sem_trywait (&disk_cancel_sem);
    if (rc == -1) {
      if (EINTR == errno) continue;
      ASSERT (EAGAIN == errno);
      return 0;
    } else {
      return (disk_cancel_flag = 1);
    }
  }
}
#endif

static sf_count_t mysf_vio_rw (void *ptr, sf_count_t count, mysf_vio_data *data, io_syscall io) {
  sf_count_t done = 0;
  int rc;

  while (done < count) {
    if (disk_cancel_flag) {
      TRACE (TRACE_INT + 1, "IO cancelled, returning short count");
      return 0;  // cause decoder to exit ASAP
#if 0  // fake a successful read; still causes un-clearable decoder errors
      off_t cur = lseek (data->fd, 0, SEEK_CUR);
      sf_count_t max_count = done + data->sz - cur;
      TRACE (TRACE_INT, "cancelling IO req=%lld, max=%lld", count, max_count);
      if (count > max_count) count = max_count;
      lseek (data->fd, count - done, SEEK_CUR);
      return count;
#endif
    }

    TRACE (TRACE_INT + 3, "start io=%lld/%lld", done, count);
    rc = io (data->fd, ptr, count - done);
    if (rc < 0) {
      ASSERT (EINTR == errno);
    } else if (rc > 0) {
      done += rc;
    } else break;
  }
  TRACE (TRACE_INT + 2, "end IO=%lld/%lld", done, count);
  return done;
}

static sf_count_t  mysf_vio_read (void *ptr, sf_count_t count, void *user_data) {
  return mysf_vio_rw (ptr, count, (mysf_vio_data *) user_data, read);
}

static sf_count_t  mysf_vio_write (const void *ptr, sf_count_t count, void *user_data) {
  return mysf_vio_rw ((void *) ptr, count, (mysf_vio_data *) user_data, (io_syscall) write);
}

static sf_count_t  mysf_vio_tell (void *user_data) {
  sf_count_t rc;
  ENSURE_SYSCALL_AND_SAVE (lseek, (((mysf_vio_data *) user_data)->fd, 0, SEEK_CUR), rc);
  return rc;
}

SF_VIRTUAL_IO mysf_vio = {
  mysf_vio_get_filelen,
  mysf_vio_seek,
  mysf_vio_read,
  mysf_vio_write,
  mysf_vio_tell
};

static SNDFILE *open_sf_fd_read (mysf_vio_data *data, SF_INFO *sf_info) {
  SF_INFO myinfo; if (sf_info == NULL) sf_info = &myinfo;
  memset (sf_info, 0, sizeof (*sf_info));
  return sf_open_virtual (&mysf_vio, SFM_READ, sf_info, data);
}

/// Converts a frame number to a segment + an offset.
/// @return Segment index; offset in segment overwrites *n
static int frame2file (sf_count_t *n) {
  ASSERT (*n != JACK_MAX_FRAMES);

  for (int i = 0; i < nfiles; ++i)
    if (*n < ftpos [i + 1]) {
      if (JACK_MAX_FRAMES == loop_start_frame || *n < ftpos [nfiles - 1]) {
        *n -= ftpos [i];
        return i;
      }
      ASSERT (i == nfiles - 1);
      *n -= ftpos [i];
      *n = *n % (ftpos [i] - loop_start_frame);
      *n += loop_start_frame;
      return frame2file (n);
    }

  TRACE (TRACE_FATAL, "Could not find frame=%llu, nfiles=%d", *n, nfiles);
  ASSERT (0); return -1;
}


/// Checks the cancellation flag, and if necessary re-opens the SNDFILE structre.
/// @return cancellation flag (reset before returning)
static int disk_thread_chk_cancel_and_cleanup (int f_idx) {
  if (! disk_cancel_flag) return 0;
  else {
    disk_cancel_flag = 0;
    TRACE (TRACE_INT, "IO cancelled, restarting");
    sf_close (infile [f_idx]);
    ENSURE_SYSCALL (lseek, (infile_vio [f_idx].fd, 0, SEEK_SET));
    ENSURE_CALL_AND_SAVE (open_sf_fd_read, (&infile_vio [f_idx], NULL), infile [f_idx], NULL);
    return 1;
  }
}


/// Fills @c jbuf with fragments from the various segments mapped to the transport timeline.
static void *disk_thread (void *dtarg) {
  (void) dtarg;

  {
    sigset_t nonusr2_mask; ENSURE_SYSCALL (sigfillset, (&nonusr2_mask));
    sigdelset (&nonusr2_mask, SIGUSR2);
    ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigusr2_mask, NULL));
    while (! disk_cancel_flag)
      sigsuspend (&nonusr2_mask);
    TRACE (TRACE_DIAG, "Disk thread: got initial signal");
    ENSURE_SYSCALL (pthread_sigmask, (SIG_UNBLOCK, &sigusr2_mask, NULL));
 }

restart: (void) 0;
  jack_ringbuffer_reset (jbuf);
  int semval;
  ENSURE_SYSCALL (sem_getvalue, (jbuf_free_sem, &semval));
  for (; semval < jbuf_free_max; ++semval)
    sem_post (jbuf_free_sem);

  sf_count_t offset;

  ENSURE_SYSCALL (pthread_mutex_lock, (&reloc_lock));
  disk_cancel_flag = 0;
  offset = reloc_frame;
  reloc_pending = 0;
  ENSURE_SYSCALL (pthread_mutex_unlock, (&reloc_lock));

  int f_idx;
  int seek_completed = 0;
  f_idx = frame2file (&offset);
  TRACE (TRACE_INT, "disk thread restart, seg=%d, offset=%lld", f_idx, offset);

  for (;;) {
    // wait on semaphore, possibly cancelling on EINTR
    // this frees up a half-fragment -- we free the other @ the end
    int rc;
    while ((rc = sem_wait (jbuf_free_sem)) == -1) {
      ASSERT (EINTR == errno);
      if (disk_cancel_flag) goto restart;
      else continue;
    }
    TRACE (TRACE_INT + 2, "start frag");
    jack_ringbuffer_data_t jdatainfo [2];
    jack_ringbuffer_get_write_vector (jbuf, jdatainfo);
    ASSERT (jdatainfo [0].len / sizeof (sample_t) / nports >= frag_frames);
    sample_t *wptr = (sample_t *) jdatainfo [0].buf;
    sf_count_t read_frames = 0;
    for (sf_count_t toread = frag_frames; toread > 0;
         toread -= read_frames, wptr += read_frames * nports)
    {
      int seg_done = 0;
      sf_count_t avail = JACK_MAX_FRAMES;  // frames left in this segment
      if (JACK_MAX_FRAMES != ftpos [f_idx + 1])
        avail = ftpos [f_idx + 1] - ftpos [f_idx] - offset;
      sf_count_t req_frames = MIN (toread, avail);  // how many frames to request
      if (infile [f_idx] != NULL) {  // this is a file segment
        if (! seek_completed) {
          read_frames = sf_seek (infile [f_idx], offset, SEEK_SET);
          if (disk_thread_chk_cancel_and_cleanup (f_idx)) goto restart;
          TRACE (TRACE_INT, "seek=%lld frames, ret=%lld", offset, read_frames);
          ASSERT (read_frames == offset);
          seek_completed = 1;
        }
        TRACE (TRACE_INT + 2, "toread=%lld frames", toread);
        read_frames = sf_readf_float (infile [f_idx], wptr, req_frames);
        if (disk_thread_chk_cancel_and_cleanup (f_idx)) goto restart;
        ASSERT (read_frames == req_frames);
      } else {  // this is a silence segment
        read_frames = req_frames;
        memset (wptr, 0, read_frames * nports * sizeof (sample_t));
      }
      offset += read_frames;
      if (read_frames == avail)
        seg_done = 1;

      if (seg_done) {
        TRACE (TRACE_INT, "finished seg=%d", f_idx);
        ++f_idx; seek_completed = 0; offset = 0;
        if (f_idx == (nfiles - 1) && JACK_MAX_FRAMES != loop_start_frame) {
          // last segment is configured to start at loop_pos_frame
          offset += ftpos [f_idx];
          f_idx = frame2file (&offset);
          TRACE (TRACE_DIAG, "Looping to seg=%d, offset=%lld", f_idx, offset);
        }
      }
    }
    TRACE (TRACE_INT + 1, "frag read");
    jack_ringbuffer_write_advance (jbuf, frag_frames * nports * sizeof (sample_t));
    ENSURE_SYSCALL (sem_wait, (jbuf_free_sem));  // now mark the other half as free
  }

  return NULL;
}

static void create_disk_thread () {
  sigset_t oldmask;
  ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigmask, &oldmask));
  ENSURE_SYSCALL (pthread_create, (&disk_thread_tid [0], NULL, disk_thread, NULL));
  ENSURE_SYSCALL (pthread_sigmask, (SIG_SETMASK, &oldmask, NULL));
}

static int mysystem (const char *argv [], int stdfd [3]) {
  ASSERT (NULL != argv [0]);
  pid_t pid = fork ();
  if (pid != 0) {  // parent
    ASSERT (pid != (pid_t) -1)
    int stat_val;
    ENSURE_SYSCALL (waitpid, (pid, &stat_val, 0));
    ASSERT (WIFEXITED (stat_val));
    return WEXITSTATUS (stat_val);
  } else {  // child
    for (int i = 0; argv [i] != NULL; ++i)  {
      // for some reason execv*() wants non-const strings -> strdup() all args
      argv [i] = strdup (argv [i]);
    }
    for (int i = 0; i < 3; ++i) {
      if (stdfd [i] == -1) {
        close (i);
      } else if (stdfd [i] != i) {
        int rc = dup2 (stdfd [i], i);
        assert (rc != -1);
        //close (stdfd [i]);
      }
    }
    execvp (argv [0], (char **) argv);
    printf ("Could not exec %s\n", argv [0]);
    exit (EXIT_FAILURE);
  }
}

static int sox_convert (int infd, const char *name) {
  ASSERT (infd >= 0);
  char *tmpf = strdup ("/tmp/file2jack.XXXXXX");
  int outfd; ENSURE_SYSCALL_AND_SAVE (mkstemp, (tmpf), outfd);
  ENSURE_SYSCALL (unlink, (tmpf)); free (tmpf);
  int stdfd [3] = { infd, outfd, 2 };

  char *ext = strrchr (name, '.'); ASSERT (ext != NULL);
  ++ext;
  char *loext = strdup (ext);
  for (char *p = loext; *p != '\0'; ++p) *p = tolower (*p);

  char ratestr [6 + 1];
  snprintf (ratestr, sizeof (ratestr), "%d", srate);
  const char *argv [] = { //"/tmp/printargs",
    "sox", "-q", "--no-clobber",
    "-t", loext, "-", "-t", "flac", "-",
    "rate", ratestr,
    NULL
  };

  TRACE (TRACE_INFO, "Calling sox to convert file %s", name);
  int rc = mysystem (argv, stdfd);
  close (infd); free (loext);
  if (rc != EXIT_SUCCESS) {
    TRACE (TRACE_FATAL, "Could not execute sox (exit=%d)", rc);
    myshutdown (1);
  }

  return outfd;
}

static void setup_input_files () {
  ASSERT (nfiles > 0);
  infile_vio = malloc ((1 + nfiles) * sizeof (infile_vio [0]));
  infile = calloc (1, (1 + nfiles) * sizeof (infile [0]));
  ftpos = malloc ((1 + nfiles) * sizeof (ftpos [0]));
  ftpos [0] = 0;
  for (int i = 0; i < nfiles; ++i) {
    ftpos [i + 1] = 0;

    if (NULL != fname [i]) {
      SF_INFO sf_info;
      ENSURE_SYSCALL_AND_SAVE (open, (fname [i], O_RDONLY), infile_vio [i].fd);
      for (int try = 0; try < 2; ++try) {
        ENSURE_SYSCALL (lseek, (infile_vio [i].fd, 0, SEEK_SET));
        struct stat buf;
        ENSURE_SYSCALL (fstat, (infile_vio [i].fd, &buf));
        infile_vio [i].sz = buf.st_size;
        infile [i] = open_sf_fd_read (&infile_vio [i], &sf_info);
        if (NULL == infile [i]) {
          int err = sf_error (NULL);
          ASSERT (err != SF_ERR_NO_ERROR);
          if (try == 1 || err == SF_ERR_SYSTEM || err == SF_ERR_MALFORMED_FILE) {
            TRACE (TRACE_FATAL, "Could not open file %s: %s (%d)",
                   fname [i], sf_error_number (err), err);
            myshutdown (1);
          }
        } else {  // sf_open() succeeded, check format
          if (srate == (unsigned) sf_info.samplerate) {
            if (0 == nports) nports = sf_info.channels;
            else if (nports != (unsigned) sf_info.channels) {
              TRACE (TRACE_FATAL, "File %s has # of channels %d != %d for previous files",
                     fname [i], sf_info.channels, nports);
              myshutdown (1);
            }
            break;
          } else {
            sf_close (infile [i]); infile [i] = NULL;
            ENSURE_SYSCALL (lseek, (infile_vio [i].fd, 0, SEEK_SET));
          }
        }

        if (try == 0)
          infile_vio [i].fd = sox_convert (infile_vio [i].fd, fname [i]);
      }

      if (NULL == infile [i]) {
        TRACE (TRACE_FATAL, "Conversion failed for file %s", fname [i]);
        myshutdown (1);
      }
      ASSERT (1 == nports || 2 == nports);
      ftpos [i + 1] += sf_info.frames;
    }

    if (0 != strcmp (ftpos_str [i], ":"))
      ENSURE_CALL (convert_time, (ftpos_str [i], srate, &ftpos [i]) == 0);
    ftpos [i + 1] += ftpos [i];
    if (NULL == fname [i - 1] && ftpos [i] < ftpos [i - 1])
      ftpos [i - 1] = ftpos [i];  // move previous implicit silence segment
    TRACE (TRACE_INT, "| %s @%u",
           fname [i] == NULL ? "*" : fname [i], ftpos [i]);
    ASSERT (0 == i || ftpos [i] >= ftpos [i - 1]);
  }

  fname [nfiles] = NULL;
  ftpos [nfiles] = JACK_MAX_FRAMES;
  ENSURE_CALL (convert_time, (loop_start_str, srate, &loop_start_frame) == 0);

  if (loop_start_frame != JACK_MAX_FRAMES)
    TRACE (TRACE_INT, "Loop goes back to frame=%u", loop_start_frame);
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

static void compute_frames () {
  jperiodframes = jack_get_buffer_size (jclient);
  if (cache_secs > 0) jbuf_len = nports * srate * cache_secs + 0.5;
  else jbuf_len = (1 << 18) * nports;
  jbuf = jack_ringbuffer_create (jbuf_len * sizeof (sample_t)); ASSERT (jbuf != NULL);
  jack_ringbuffer_data_t jdatainfo [2];
  jack_ringbuffer_get_write_vector (jbuf, jdatainfo);
  jbuf_len = (jdatainfo [0].len + 1) / sizeof (sample_t);
  if (jbuf_frags < 4) {
    jbuf_frags = jbuf_len / 0.333 / nports / srate;
    // ensure power of 2
    int i = 4;
    for (; i < jbuf_frags; i <<= 1);
    jbuf_frags = i;
  }
  {  // fragment must contain integer # of periods
    jack_nframes_t denom = jperiodframes * nports * jbuf_frags;
    jbuf_len = ((jbuf_len - 1) / denom + 1) * denom;
  }
  frag_frames = jbuf_len / (jbuf_frags * nports);
  jbuf_free_max = 2 * (jbuf_frags - 1);  // One frag wasted due to sentinel byte...
}

static void connect_jack () {
  TRACE (TRACE_DIAG, "connecting to jack");
  trace_flush (); fflush(NULL);

  jack_options_t jopt = JackNullOption;  // JackNoStartServer;
  jack_status_t jstat;
  ENSURE_CALL_AND_SAVE (jack_client_open, (MYNAME, jopt, &jstat), jclient, NULL);
  jack_on_shutdown (jclient, on_jack_shutdown, NULL);

  srate = jack_get_sample_rate (jclient);
}

static void setup_audio () {
  TRACE (TRACE_DIAG, "audio setup");
  trace_flush (); fflush(NULL);

  compute_frames ();

  ENSURE_SYSCALL (sem_init, (&jbuf_free_sem_store, 0, jbuf_free_max));
  jbuf_free_sem = &jbuf_free_sem_store;

  TRACE (TRACE_INFO, "Connected to jack, period=%d, rate=%d, ports=%d, cache=%.3f sec (%d samples), %d fragments of %d frames",
         (int) jperiodframes, (int) srate, nports, (float) jbuf_len / (srate * nports), jbuf_len, jbuf_frags, frag_frames);

  jport = malloc (nports * sizeof (jport [0]));
  for (unsigned i = 0; i < nports; ++i) {
    char name [10];
    snprintf (name, sizeof (name), "out%02u", i);
    ENSURE_CALL_AND_SAVE (jack_port_register, (jclient, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0), jport [i], NULL);
  }
  jportbuf = malloc (nports * sizeof (jportbuf [0]));

  ENSURE_CALL (jack_set_process_thread, (jclient, process_thread, NULL) != 0);

  ENSURE_CALL (jack_set_sync_callback, (jclient, on_jack_sync, NULL) != 0);

  create_disk_thread ();

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
NL "  --cache SEC  Cache size; default: ~6s for 44.1 kHz stereo"
NL "  --vol PERC   Soft volume (can be greater than 100%)"
NL ""
NL "Final LOOP option: [--at LPOS] --loop T0"
NL "  Repeats the T0-END segment from LPOS to infinity (where END is the"
NL "  end of the last file)."
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
  char extra;
  if (argv [0] == NULL) usage (NULL);

  int nfiles_max = 100;
  fname = malloc (2 * nfiles_max * sizeof (fname [0]));
  ftpos_str = malloc (2 * nfiles_max * sizeof (ftpos [0]));
  const char *loop_pos_str = NULL;

  const char *pos_str = ":";
  for (++argv; *argv != NULL; ++argv) {
    if (*argv [0] == '-') {
      if (0 == strcmp (*argv, "-h") || 0 == strcmp (*argv, "--help") ||
          0 == strcmp (*argv, "-?"))
      {
        usage (NULL);
      } else if (0 == strcmp (*argv, "--version")) {
        printf ("%s version %s\n%s\n", MYNAME, myrelease, "Copyright (C) 2011 Dan Muresan");
        exit (EXIT_SUCCESS);
      } else if (0 == strcmp (*argv, "--at")) {
        pos_str = *++argv;
      } else if (0 == strcmp (*argv, "-i")) {
        ASSERT (nfiles + 1 < nfiles_max);
        ftpos_str [nfiles] = ":"; fname [nfiles++] = NULL; // a gap
        ftpos_str [nfiles] = pos_str; fname [nfiles++] = *++argv;
        pos_str = ":";  // back to default
      } else if (0 == strcmp (*argv, "--loop")) {
        loop_start_str = *++argv;
        loop_pos_str = pos_str; pos_str = ":";
      } else if (0 == strcmp (*argv, "--vol")) {
        if (sscanf (*++argv, "%f%c", &soft_vol, &extra) != 1)
          usage ("Bad soft volume %s", *argv);
      } else if (0 == strcmp (*argv, "--cache")) {
        if (sscanf (*++argv, "%f%c", &cache_secs, &extra) != 1)
          usage ("Bad cache size %s", *argv);
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
  ftpos_str [nfiles] = ":"; fname [nfiles++] = NULL;
  if (loop_start_str != NULL) {
    ftpos_str [nfiles] = loop_pos_str; fname [nfiles++] = NULL;
  }
}

static void init_trace () {
  trace_buf = memfile_alloc (1 << 16);
  stdtrace = fdopen (dup (fileno (stdout)), "w");
  setvbuf (stdtrace, NULL, _IONBF, 0);
  TRACE (TRACE_INFO, "Logging enabled, level=%d", (int) trace_level);
}

static void cleanup () {
  int z = 0;
  if (0 == sem_getvalue (&zombified, &z) && z > 0)
    TRACE (TRACE_FATAL, "Jack shut us down");
  pthread_cancel_and_join_if_started (poll_thread_tid, main_tid);
  pthread_cancel_and_join_if_started (disk_thread_tid [0], main_tid);
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
  pthread_t self_tid = pthread_self ();
  if (sig == SIGUSR2) {  // we should be in a disk thread
    ASSERT (! pthread_equal (self_tid, main_tid));
    disk_cancel_flag = 1;
    return;
  }

  // We should be in the main thread -- all other threads block signals.
  // But linked libs could screw up our sigmask and handlers...
  if (! pthread_equal (self_tid, main_tid)) {
    TRACE (TRACE_WARN, "Signal %d redirected to main thread", sig);
    ENSURE_SYSCALL (pthread_kill, (main_tid, sig)); for (;;) pause ();
  }
  if (sig == SIGPIPE) return;  // we don't do pipes
  TRACE (TRACE_INFO, "Caught signal %d", sig);
  cleanup ();
  struct sigaction act =
    { .sa_mask = sigmask, .sa_flags = 0, .sa_handler = SIG_DFL };
  sigaction (sig, &act, NULL);
  pthread_sigmask (SIG_UNBLOCK, &sigmask, NULL);
  ENSURE_SYSCALL (pthread_kill, (pthread_self (), sig));
}

/// Initializes variables and resources that cannot be initialized statically.
/// Postcondition: all globals must be in a defined state.
static void init_globals () {
  main_tid = pthread_self ();
  poll_thread_tid = main_tid;
  disk_thread_tid [0] = main_tid;
  ENSURE_SYSCALL (sigemptyset, (&sigmask));
  ENSURE_SYSCALL (sigemptyset, (&sigusr2_mask));
  ENSURE_SYSCALL (sem_init, (&zombified, 0, 0));
}

int main (int argc, char **argv) {
  (void) argc;

  init_trace ();

  init_globals ();

  if (! setup_sigs (sig_handler, &sigmask, SA_RESTART, 7, SIGTERM, SIGQUIT, SIGABRT, SIGPIPE, SIGFPE, SIGINT, SIGALRM) ||
      ! setup_sigs (sig_handler, &sigusr2_mask, 0, 1, SIGUSR2))
  {
    TRACE_PERROR (TRACE_FATAL, "signal setup");
    return 1;
  }
  ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigusr2_mask, NULL));

  parse_args (argv);

  ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigmask, NULL));
  ENSURE_SYSCALL (pthread_create, (&poll_thread_tid, NULL, poll_thread, NULL));
  ENSURE_SYSCALL (pthread_sigmask, (SIG_UNBLOCK, &sigmask, NULL));

  ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigmask, NULL));
  connect_jack ();
  ENSURE_SYSCALL (pthread_sigmask, (SIG_UNBLOCK, &sigmask, NULL));

  setup_input_files ();

  ENSURE_SYSCALL (pthread_sigmask, (SIG_BLOCK, &sigmask, NULL));
  setup_audio ();
  ENSURE_SYSCALL (pthread_sigmask, (SIG_UNBLOCK, &sigmask, NULL));

  for (;;) pause ();

  myshutdown (0); assert (0);
}

// Local Variables:
// write-file-functions: (lambda () (delete-trailing-whitespace) nil)
// compile-command: "cc -Os -g -std=c99 -pthread -Wall -Wextra -march=native -pipe -ljack -lsndfile -lm file2jack.c -o file2jack"
// End:
