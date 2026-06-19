/*
 * EH577 press-timing probe
 *
 * Interactive tool for measuring finger press approach/hold timing and
 * capturing a live PGM snapshot at the moment of full contact.
 *
 * Keys (raw terminal, no Enter needed):
 *   f   finger starting to touch sensor  (start approach timer)
 *   r   finger fully pressed             (snapshot live PGM, start hold timer)
 *   d   finger lifted                    (log timings, re-arm for next press)
 *   x   exit gracefully
 *
 * Environment:
 *   EGIS0577_LIVE_FRAME_PATH  path the driver writes the current processed
 *                             frame to; copied on 'r'. Set by the wrapper
 *                             script; the probe still works without it (no PGM).
 *
 * Copyright (C) 2026 workspace contributors
 * LGPL-2.1+
 */

#define FP_COMPONENT "example-eh577-timing-probe"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <libfprint/fprint.h>
#include <glib-unix.h>

#include "storage.h"
#include "utilities.h"

/* ── terminal restore on any exit path ──────────────────────────────────── */

static struct termios g_term_saved;
static gboolean       g_term_is_raw = FALSE;

static void
restore_term (void)
{
  if (g_term_is_raw)
    {
      tcsetattr (STDIN_FILENO, TCSANOW, &g_term_saved);
      g_term_is_raw = FALSE;
    }
}

static void
restore_term_atexit (void)
{
  restore_term ();
}

static void
set_term_raw (void)
{
  struct termios raw;

  tcgetattr (STDIN_FILENO, &g_term_saved);
  raw = g_term_saved;
  raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr (STDIN_FILENO, TCSANOW, &raw);
  g_term_is_raw = TRUE;
}

/* ── state machine ──────────────────────────────────────────────────────── */

typedef enum {
  PROBE_ARMED,    /* fp_device_capture running; waiting for user 'f' */
  PROBE_TIMING,   /* 'f' pressed; counting approach time             */
  PROBE_PRESSING, /* 'r' pressed; counting hold time                 */
} ProbeState;

typedef struct {
  GMainLoop    *loop;
  GCancellable *cancellable;
  FpDevice     *dev;
  ProbeState    state;
  gboolean      quitting;
  gboolean      cancel_requested; /* exit requested; next cb must quit, not error-exit */
  gboolean      capture_active;
  gint64        t_f;              /* monotonic µs at 'f' */
  gint64        t_r;              /* monotonic µs at 'r' */
  gint          press_count;
  const gchar  *out_dir;
  const gchar  *live_path;
  FILE         *logf;
  int           ret;
} ProbeData;

/* ── output helpers ─────────────────────────────────────────────────────── */

G_GNUC_PRINTF (2, 3)
static void
probe_print (ProbeData *pd, const gchar *fmt, ...)
{
  va_list args;
  gchar *msg;

  va_start (args, fmt);
  msg = g_strdup_vprintf (fmt, args);
  va_end (args);

  /* \r\n for raw-mode terminal */
  int ttyfd = (fcntl (3, F_GETFD) != -1) ? 3 : STDERR_FILENO;
  dprintf (ttyfd, "%s\r\n", msg);

  if (pd->logf)
    fprintf (pd->logf, "%s\n", msg);

  g_free (msg);
}

/* ── live PGM snapshot ──────────────────────────────────────────────────── */

static void
copy_live_frame (ProbeData *pd, const gchar *label)
{
  g_autofree gchar *dest = NULL;
  g_autofree gchar *contents = NULL;
  gsize len = 0;
  g_autoptr(GError) error = NULL;

  if (!pd->live_path || !pd->live_path[0])
    {
      probe_print (pd, "[probe] EGIS0577_LIVE_FRAME_PATH not set — no PGM snapshot");
      return;
    }

  dest = g_strdup_printf ("%s/press-%03d-%s.pgm", pd->out_dir, pd->press_count, label);

  if (!g_file_get_contents (pd->live_path, &contents, &len, &error))
    {
      probe_print (pd, "[probe] live frame read failed: %s", error->message);
      return;
    }

  g_clear_error (&error);
  if (!g_file_set_contents (dest, contents, (gssize) len, &error))
    {
      probe_print (pd, "[probe] live frame write failed: %s", error->message);
      return;
    }

  probe_print (pd, "[probe] PGM snapshot: %s", dest);
}

/* ── device close / quit ────────────────────────────────────────────────── */

static void
on_device_closed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  ProbeData *pd = user_data;
  g_autoptr(GError) error = NULL;

  fp_device_close_finish (dev, res, &error);
  if (error)
    probe_print (pd, "[probe] close error: %s", error->message);

  restore_term ();
  g_main_loop_quit (pd->loop);
}

static void
probe_quit (FpDevice *dev, ProbeData *pd)
{
  if (!fp_device_is_open (dev))
    {
      restore_term ();
      g_main_loop_quit (pd->loop);
      return;
    }
  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_device_closed, pd);
}

/* ── rearm ──────────────────────────────────────────────────────────────── */

static void start_capture (FpDevice *dev, ProbeData *pd);

static void
rearm (FpDevice *dev, ProbeData *pd)
{
  pd->cancel_requested = FALSE;
  pd->state = PROBE_ARMED;
  start_capture (dev, pd);
}

/* ── capture callback ───────────────────────────────────────────────────── */

static void
on_capture_done (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  ProbeData *pd = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;

  pd->capture_active = FALSE;
  image = fp_device_capture_finish (dev, res, &error);

  if (pd->quitting)
    {
      probe_quit (dev, pd);
      return;
    }

  if (image)
    {
      /* Driver completed a natural capture (fast-exit or turn-end accept). */
      g_autofree gchar *dest = g_strdup_printf ("%s/press-%03d-capture.pgm",
                                                 pd->out_dir, pd->press_count);
      if (save_image_to_pgm (image, dest))
        probe_print (pd, "[probe] press-%03d: driver captured → %s", pd->press_count, dest);
      else
        probe_print (pd, "[probe] press-%03d: driver captured (PGM save failed)", pd->press_count);

      if (pd->state == PROBE_TIMING || pd->state == PROBE_PRESSING)
        {
          gint64 now = g_get_monotonic_time ();
          probe_print (pd, "[probe] press-%03d: approach=%lldms (captured before 'd')",
                       pd->press_count, (long long) ((now - pd->t_f) / 1000));
        }

      pd->press_count++;
      rearm (dev, pd);
      return;
    }

  /* No image — retry, cancel, or real error. */
  if (pd->cancel_requested || (error && error->domain == FP_DEVICE_RETRY))
    {
      rearm (dev, pd);
      return;
    }

  probe_print (pd, "[probe] capture error: %s", error ? error->message : "unknown");
  pd->ret = 1;
  probe_quit (dev, pd);
}

static void
start_capture (FpDevice *dev, ProbeData *pd)
{
  g_object_unref (pd->cancellable);
  pd->cancellable = g_cancellable_new ();
  pd->capture_active = TRUE;
  probe_print (pd, "[probe] armed  — press 'f' when starting to touch");
  fp_device_capture (dev, TRUE, pd->cancellable,
                     (GAsyncReadyCallback) on_capture_done, pd);
}

/* ── keyboard handler ───────────────────────────────────────────────────── */

static gboolean
on_key (GIOChannel *src, GIOCondition cond, gpointer user_data)
{
  ProbeData *pd = user_data;
  gchar ch;
  gsize nr;
  gint64 now = g_get_monotonic_time ();

  if (g_io_channel_read_chars (src, &ch, 1, &nr, NULL) != G_IO_STATUS_NORMAL || nr == 0)
    return G_SOURCE_CONTINUE;

  switch (ch)
    {
    case 'f':
      if (pd->state != PROBE_ARMED)
        {
          probe_print (pd, "[probe] 'f' ignored (already timing)");
          break;
        }
      pd->t_f = now;
      pd->state = PROBE_TIMING;
      probe_print (pd, "[probe] press-%03d: f  (approaching)", pd->press_count);
      break;

    case 'r':
      if (pd->state != PROBE_TIMING)
        {
          probe_print (pd, "[probe] 'r' ignored (not in approach phase)");
          break;
        }
      pd->t_r = now;
      pd->state = PROBE_PRESSING;
      probe_print (pd, "[probe] press-%03d: r  approach=%lldms (fully pressed)",
                   pd->press_count, (long long) ((pd->t_r - pd->t_f) / 1000));
      copy_live_frame (pd, "r");
      break;

    case 'd':
      if (pd->state == PROBE_ARMED)
        {
          probe_print (pd, "[probe] 'd' ignored (no active press)");
          break;
        }
      {
        gint64 total_ms = (now - pd->t_f) / 1000;

        if (pd->state == PROBE_PRESSING)
          {
            gint64 hold_ms = (now - pd->t_r) / 1000;
            probe_print (pd, "[probe] press-%03d: d  hold=%lldms total=%lldms",
                         pd->press_count, (long long) hold_ms, (long long) total_ms);
          }
        else
          {
            probe_print (pd, "[probe] press-%03d: d  total=%lldms (no 'r' recorded)",
                         pd->press_count, (long long) total_ms);
          }
      }
      pd->press_count++;
      pd->state = PROBE_ARMED;
      if (!pd->capture_active)
        {
          /* Capture already completed naturally; start the next one now. */
          start_capture (pd->dev, pd);
        }
      break;

    case 'x':
    case '\x03': /* Ctrl-C in raw mode */
      probe_print (pd, "[probe] exit requested");
      pd->quitting = TRUE;
      pd->ret = 0;
      if (pd->capture_active)
        {
          pd->cancel_requested = TRUE;
          g_cancellable_cancel (pd->cancellable);
        }
      else
        {
          probe_quit (pd->dev, pd);
        }
      break;

    default:
      break;
    }

  return G_SOURCE_CONTINUE;
}

/* ── SIGINT handler ─────────────────────────────────────────────────────── */

static gboolean
on_sigint (gpointer user_data)
{
  ProbeData *pd = user_data;

  if (pd->quitting)
    return G_SOURCE_CONTINUE;

  probe_print (pd, "[probe] SIGINT — exiting");
  pd->quitting = TRUE;
  pd->ret = 0;
  if (pd->capture_active)
    {
      pd->cancel_requested = TRUE;
      g_cancellable_cancel (pd->cancellable);
    }
  else
    {
      probe_quit (pd->dev, pd);
    }
  return G_SOURCE_CONTINUE;
}

/* ── device open callback ───────────────────────────────────────────────── */

static void
on_device_opened (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  ProbeData *pd = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      probe_print (pd, "[probe] open failed: %s", error->message);
      pd->ret = 1;
      restore_term ();
      g_main_loop_quit (pd->loop);
      return;
    }

  probe_print (pd, "[probe] device open — starting probe loop");
  probe_print (pd, "[probe] keys:  f=touch-start  r=fully-pressed  d=lifted  x=exit");
  start_capture (dev, pd);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  FpDevice *dev;
  GIOChannel *stdin_ch;

  const gchar *out_dir   = (argc >= 2) ? argv[1] : ".";
  const gchar *live_path = g_getenv ("EGIS0577_LIVE_FRAME_PATH");

  g_autofree gchar *log_path = g_build_filename (out_dir, "timing.log", NULL);

  if (g_mkdir_with_parents (out_dir, 0755) != 0)
    {
      g_printerr ("Cannot create output dir: %s\n", out_dir);
      return 1;
    }

  FILE *logf = fopen (log_path, "a");

  setbuf (stdout, NULL);

  atexit (restore_term_atexit);
  set_term_raw ();

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);
  if (!devices)
    {
      restore_term ();
      g_printerr ("No devices found\n");
      return 1;
    }

  dev = discover_device (devices);
  if (!dev)
    {
      restore_term ();
      g_printerr ("No EH577 device found\n");
      return 1;
    }

  ProbeData pd = {
    .loop             = g_main_loop_new (NULL, FALSE),
    .cancellable      = g_cancellable_new (),
    .dev              = dev,
    .state            = PROBE_ARMED,
    .quitting         = FALSE,
    .cancel_requested = FALSE,
    .capture_active   = FALSE,
    .t_f              = 0,
    .t_r              = 0,
    .press_count      = 0,
    .out_dir          = out_dir,
    .live_path        = live_path,
    .logf             = logf,
    .ret              = 0,
  };

  stdin_ch = g_io_channel_unix_new (STDIN_FILENO);
  g_io_channel_set_encoding (stdin_ch, NULL, NULL);
  g_io_channel_set_buffered (stdin_ch, FALSE);
  g_io_add_watch (stdin_ch, G_IO_IN, on_key, &pd);
  g_io_channel_unref (stdin_ch);

  g_unix_signal_add (SIGINT, on_sigint, &pd);

  fp_device_open (dev, pd.cancellable,
                  (GAsyncReadyCallback) on_device_opened, &pd);

  g_main_loop_run (pd.loop);

  if (logf)
    fclose (logf);
  g_object_unref (pd.cancellable);
  g_main_loop_unref (pd.loop);

  return pd.ret;
}
