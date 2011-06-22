static int convert_time (const char *t, jack_nframes_t srate, jack_nframes_t *ret) {
  if (t == NULL) { *ret = JACK_MAX_FRAMES; return 1; }
  char extra;
  int slen = strlen (t);
  int in_frames = t [slen - 1] == 's';
  if (in_frames) {
    if (sscanf (t, "%u%*c%c", ret, &extra) != 1)
      return 0;
  } else {
    double sec;
    if (sscanf (t, "%lf%c", &sec, &extra) != 1)
      return 0;
    *ret = sec * srate;
  }
  return 1;
}
