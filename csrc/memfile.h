// Copyright (C) 2010-2011 Dan Muresan
// Part of sintvert (http://danmbox.github.com/sintvert/)

#ifndef __SINTVERT_MEMFILE_H
#define __SINTVERT_MEMFILE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "util.h"

/// An in-memory file.
/// Its methods contain no cancellation points (except possibly in asserts).
typedef struct {
  char *buf1, *buf2, *ptr, *end, *other;
  size_t sz;
  pthread_mutex_t lock, switchlock;
} memfile;
static void memfile_init (memfile *f, size_t sz, char *buf1) {
  f->sz = sz;
  if (buf1 == NULL) {
    f->buf1 = calloc (sz, 1); f->buf2 = calloc (sz, 1);
    assert (f->buf1 != NULL && f->buf2 != NULL);
  } else {
    f->buf1 = buf1; f->buf2 = NULL;
  }
  f->ptr = f->buf1; f->end = f->ptr + f->sz; f->other = f->buf2;
  pthread_mutexattr_t lattr;
  pthread_mutexattr_init (&lattr);
  pthread_mutexattr_settype (&lattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init (&f->lock, &lattr);
  pthread_mutex_init (&f->switchlock, &lattr);
  pthread_mutexattr_destroy (&lattr);
}
static memfile *memfile_alloc (size_t sz) {
  memfile *f = malloc (sizeof (memfile));
  assert (f != NULL);
  memfile_init (f, sz, NULL);
  return f;
}
/// Switches the active and shadow buffers of @p f.
/// Locks @p f's switchlock, then its lock.
/// Marks the 2nd-to-last char in old buffer as NUL unless overflow occurred.
static void memfile_switchbuf (memfile *f) {
  MUTEX_LOCK_WITH_CLEANUP (&f->switchlock);
  MUTEX_LOCK_WITH_CLEANUP (&f->lock);
  assert (f->other != NULL);
  if (f->end - f->ptr >= 2) *(f->end - 2) = '\0';  // help overflow detection
  f->other [0] = '\0';
  f->ptr = f->other; f->end = f->other + f->sz;
  f->other = f->other == f->buf1 ? f->buf2 : f->buf1;
  pthread_cleanup_pop (1);
  pthread_cleanup_pop (1);
}
static int memfile_vprintf (memfile *f, const char *fmt, va_list ap) {
  int avail = f->end - f->ptr;
  int written = vsnprintf (f->ptr, avail, fmt, ap);
  f->ptr += written; if (f->ptr > f->end) f->ptr = f->end;
  return written;
}
static int memfile_printf (memfile *f, const char *fmt, ...) {
  va_list ap; va_start (ap, fmt);
  int written = memfile_vprintf (f, fmt, ap);
  va_end (ap);
  return written;
}


#endif  // __SINTVERT_MEMFILE_H
