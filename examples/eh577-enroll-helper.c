/*
 * EH577-focused enrollment helper using public libfprint callbacks
 * Copyright (C) 2026 workspace contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "example-eh577-enroll-helper"

#include <stdio.h>
#include <libfprint/fprint.h>
#include <glib-unix.h>

#include "storage.h"
#include "utilities.h"

typedef struct _HelperData
{
  GMainLoop    *loop;
  GCancellable *cancellable;
  unsigned int  sigint_handler;
  FpFinger      finger;
  gint          device_index;
  gboolean      update_fingerprint;
  gchar        *save_image_path;
  gint          ret_value;
} HelperData;

static void
helper_data_free (HelperData *helper_data)
{
  g_clear_handle_id (&helper_data->sigint_handler, g_source_remove);
  g_clear_object (&helper_data->cancellable);
  g_clear_pointer (&helper_data->save_image_path, g_free);
  g_main_loop_unref (helper_data->loop);
  g_free (helper_data);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (HelperData, helper_data_free)

static const char *
finger_status_to_string (FpFingerStatusFlags status)
{
  if (status & FP_FINGER_STATUS_PRESENT)
    return "present";
  if (status & FP_FINGER_STATUS_NEEDED)
    return "needed";
  return "none";
}

static const char *
retry_to_string (gint retry_code)
{
  switch (retry_code)
    {
    case FP_DEVICE_RETRY_GENERAL:
      return "general";
    case FP_DEVICE_RETRY_TOO_SHORT:
      return "too-short";
    case FP_DEVICE_RETRY_CENTER_FINGER:
      return "center-finger";
    case FP_DEVICE_RETRY_REMOVE_FINGER:
      return "remove-finger";
    case FP_DEVICE_RETRY_TOO_FAST:
      return "too-fast";
    default:
      return "unknown";
    }
}

static FpDevice *
select_device (GPtrArray *devices,
               gint       device_index)
{
  FpDevice *dev;

  if (!devices || devices->len == 0)
    return NULL;

  if (device_index < 0 || device_index >= (gint) devices->len)
    return NULL;

  dev = g_ptr_array_index (devices, device_index);
  g_print ("EH577_HELPER device-selected index=%d id=%s name=%s driver=%s\n",
           device_index,
           fp_device_get_device_id (dev),
           fp_device_get_name (dev),
           fp_device_get_driver (dev));
  return dev;
}

static void
on_finger_status_changed (FpDevice    *dev,
                          GParamSpec  *pspec,
                          gpointer     user_data)
{
  FpFingerStatusFlags status = fp_device_get_finger_status (dev);

  g_print ("EH577_HELPER finger-status status=%s\n",
           finger_status_to_string (status));
}

static void
on_device_closed (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  HelperData *helper_data = user_data;
  g_autoptr(GError) error = NULL;

  fp_device_close_finish (dev, res, &error);
  if (error)
    g_warning ("Failed closing device %s", error->message);

  g_main_loop_quit (helper_data->loop);
}

static void
close_device_and_quit (FpDevice   *dev,
                       HelperData *helper_data)
{
  if (!fp_device_is_open (dev))
    {
      g_main_loop_quit (helper_data->loop);
      return;
    }

  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_device_closed, helper_data);
}

static void
on_enroll_completed (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  HelperData *helper_data = user_data;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;

  print = fp_device_enroll_finish (dev, res, &error);

  if (!error)
    {
      gint save_result;

      helper_data->ret_value = EXIT_SUCCESS;
      save_result = print_data_save (print,
                                     helper_data->finger,
                                     helper_data->update_fingerprint);
      if (save_result < 0)
        {
          helper_data->ret_value = EXIT_FAILURE;
          g_print ("EH577_HELPER enroll-complete result=save-failed\n");
        }
      else
        {
          g_print ("EH577_HELPER enroll-complete result=success\n");
        }
    }
  else
    {
      helper_data->ret_value = EXIT_FAILURE;
      g_print ("EH577_HELPER enroll-complete result=failure domain=%s code=%d message=%s\n",
               g_quark_to_string (error->domain),
               error->code,
               error->message);
    }

  close_device_and_quit (dev, helper_data);
}

static void
on_enroll_progress (FpDevice *device,
                    gint      completed_stages,
                    FpPrint  *print,
                    gpointer  user_data,
                    GError   *error)
{
  HelperData *helper_data = user_data;
  gint total_stages = fp_device_get_nr_enroll_stages (device);

  if (error)
    {
      g_print ("EH577_HELPER enroll-retry completed=%d total=%d code=%s message=%s\n",
               completed_stages,
               total_stages,
               retry_to_string (error->code),
               error->message);
      return;
    }

  if (print && fp_print_get_image (print) &&
      print_image_save (print, helper_data->save_image_path))
    g_print ("EH577_HELPER image-saved path=%s\n", helper_data->save_image_path);

  g_print ("EH577_HELPER enroll-stage completed=%d total=%d\n",
           completed_stages,
           total_stages);
}

static gboolean
sigint_cb (void *user_data)
{
  HelperData *helper_data = user_data;

  g_cancellable_cancel (helper_data->cancellable);
  return G_SOURCE_CONTINUE;
}

static void
on_device_opened (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  HelperData *helper_data = user_data;
  FpPrint *print_template;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      g_print ("EH577_HELPER device-open-failed message=%s\n", error->message);
      g_main_loop_quit (helper_data->loop);
      return;
    }

  g_print ("EH577_HELPER device-opened finger=%s total=%d scan-type=%d\n",
           finger_to_string (helper_data->finger),
           fp_device_get_nr_enroll_stages (dev),
           fp_device_get_scan_type (dev));

  print_template = print_create_template (dev,
                                          helper_data->finger,
                                          helper_data->update_fingerprint);
  fp_device_enroll (dev,
                    print_template,
                    helper_data->cancellable,
                    on_enroll_progress,
                    helper_data,
                    NULL,
                    (GAsyncReadyCallback) on_enroll_completed,
                    helper_data);
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) ctx = NULL;
  g_autoptr(HelperData) helper_data = NULL;
  GPtrArray *devices;
  FpDevice *dev;
  gint finger_index = -1;
  gchar *save_image_path = g_strdup ("enrolled.pgm");
  GOptionContext *context;
  GOptionEntry entries[] = {
    { "finger-index", 'f', 0, G_OPTION_ARG_INT, &finger_index, "Finger index (0-9)", "INDEX" },
    { "device-index", 'd', 0, G_OPTION_ARG_INT, &(gint){0}, "Device index", "INDEX" },
    { "save-image",   'o', 0, G_OPTION_ARG_FILENAME, &save_image_path, "Path to save captured stage image", "PATH" },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
  };
  gint device_index = 0;
  g_autoptr(GError) error = NULL;

  entries[1].arg_data = &device_index;

  setbuf (stdout, NULL);

  context = g_option_context_new ("--finger-index N");
  g_option_context_add_main_entries (context, entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("EH577_HELPER option-error message=%s\n", error->message);
      g_option_context_free (context);
      g_free (save_image_path);
      return EXIT_FAILURE;
    }

  g_option_context_free (context);

  if (finger_index < 0 || finger_index > (FP_FINGER_LAST - FP_FINGER_FIRST))
    {
      g_printerr ("EH577_HELPER option-error message=--finger-index must be between 0 and 9\n");
      g_free (save_image_path);
      return EXIT_FAILURE;
    }

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);
  if (!devices)
    {
      g_printerr ("EH577_HELPER context-error message=unable-to-list-devices\n");
      g_free (save_image_path);
      return EXIT_FAILURE;
    }

  dev = select_device (devices, device_index);
  if (!dev)
    {
      g_printerr ("EH577_HELPER context-error message=invalid-device-index\n");
      g_free (save_image_path);
      return EXIT_FAILURE;
    }

  helper_data = g_new0 (HelperData, 1);
  helper_data->finger = FP_FINGER_FIRST + finger_index;
  helper_data->device_index = device_index;
  helper_data->update_fingerprint = FALSE;
  helper_data->save_image_path = g_steal_pointer (&save_image_path);
  helper_data->ret_value = EXIT_FAILURE;
  helper_data->loop = g_main_loop_new (NULL, FALSE);
  helper_data->cancellable = g_cancellable_new ();
  helper_data->sigint_handler = g_unix_signal_add_full (G_PRIORITY_HIGH,
                                                        SIGINT,
                                                        sigint_cb,
                                                        helper_data,
                                                        NULL);

  g_signal_connect (dev,
                    "notify::finger-status",
                    G_CALLBACK (on_finger_status_changed),
                    helper_data);

  fp_device_open (dev,
                  helper_data->cancellable,
                  (GAsyncReadyCallback) on_device_opened,
                  helper_data);
  g_main_loop_run (helper_data->loop);

  return helper_data->ret_value;
}
