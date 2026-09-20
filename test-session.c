/*
 * Drives libfprint directly (no fprintd, no polkit) through a full enrolment,
 * printing each accepted stage. Run it as root and press a finger on the
 * sensor (the power button) when prompted.
 *
 *   gcc test-session.c -o test-session $(pkg-config --cflags --libs libfprint-2)
 *   sudo env G_MESSAGES_DEBUG=all ./test-session [seconds]
 *
 * The cancellable is deliberately leaked: a GSource holding it would otherwise
 * outlive it and crash on exit after the main loop has already finished.
 */
#include <fprint.h>
#include <stdio.h>
#include <stdlib.h>

static gboolean
cancel_cb (gpointer data)
{
  g_cancellable_cancel (G_CANCELLABLE (data));
  return G_SOURCE_REMOVE;
}

static void
progress (FpDevice *dev, gint stage, FpPrint *print,
          gpointer user_data, GError *error)
{
  (void) dev; (void) print; (void) user_data;
  if (error)
    printf ("  [stage %2d] REJECTED: %s\n", stage, error->message);
  else
    printf ("  [stage %2d] accepted\n", stage);
  fflush (stdout);
}

int
main (int argc, char **argv)
{
  int seconds = (argc > 1) ? atoi (argv[1]) : 25;

  g_autoptr(FpContext) ctx = fp_context_new ();
  GPtrArray *devs = fp_context_get_devices (ctx);

  if (!devs || devs->len == 0)
    {
      printf ("no device found\n");
      return 1;
    }

  FpDevice *d = g_ptr_array_index (devs, 0);
  g_autoptr(GError) e = NULL;

  if (!fp_device_open_sync (d, NULL, &e))
    {
      printf ("open FAILED: %s\n", e ? e->message : "?");
      return 1;
    }

  printf ("opened: %s\n", fp_device_get_name (d));
  printf ("PRESS YOUR FINGER ON THE SENSOR (power button) -- %d s window\n", seconds);
  fflush (stdout);

  GCancellable *c = g_cancellable_new ();      /* intentionally leaked */
  g_timeout_add_seconds (seconds, cancel_cb, c);

  GError *err = NULL;
  FpPrint *p = fp_device_enroll_sync (d, fp_print_new (d), c,
                                      progress, NULL, &err);

  if (p)
    printf ("RESULT: enrol SUCCEEDED (print produced)\n");
  else
    printf ("RESULT: enrol ended -> %s\n", err ? err->message : "(cancelled)");

  fp_device_close_sync (d, NULL, NULL);
  return 0;
}
