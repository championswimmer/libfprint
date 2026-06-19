/*
 * eh577-reset-helper — open and immediately close the fingerprint device.
 * Used at the start of a session to give the driver a clean initialisation
 * cycle (claim USB interface, run pre-init, then release) before enroll or
 * identify helpers take over.  Emits EH577_RESET events on stdout.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "example-eh577-reset"

#include <stdio.h>
#include <libfprint/fprint.h>

typedef struct
{
  GMainLoop *loop;
} ResetData;

static void
on_device_closed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  ResetData *d = user_data;
  g_autoptr(GError) error = NULL;

  fp_device_close_finish (dev, res, &error);
  if (error)
    g_warning ("close error: %s", error->message);

  g_print ("EH577_RESET device-closed\n");
  g_main_loop_quit (d->loop);
}

static void
on_device_opened (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  ResetData *d = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      g_print ("EH577_RESET open-failed message=%s\n", error->message);
      g_main_loop_quit (d->loop);
      return;
    }

  g_print ("EH577_RESET device-opened\n");
  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_device_closed, d);
}

int
main (void)
{
  g_autoptr(FpContext) ctx = fp_context_new ();
  GPtrArray *devices = fp_context_get_devices (ctx);
  ResetData data;

  setbuf (stdout, NULL);

  if (!devices || devices->len == 0)
    {
      g_print ("EH577_RESET no-device\n");
      return 1;
    }

  data.loop = g_main_loop_new (NULL, FALSE);
  fp_device_open (g_ptr_array_index (devices, 0),
                  NULL,
                  (GAsyncReadyCallback) on_device_opened,
                  &data);
  g_main_loop_run (data.loop);
  g_main_loop_unref (data.loop);
  return 0;
}
