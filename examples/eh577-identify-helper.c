/*
 * EH577-focused identify helper using public libfprint callbacks
 * Copyright (C) 2026 workspace contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "example-eh577-identify-helper"

#include <stdio.h>
#include <libfprint/fprint.h>
#include <glib-unix.h>

#include "storage.h"
#include "utilities.h"

typedef enum
{
  IDENTIFY_RESULT_NONE,
  IDENTIFY_RESULT_MATCH,
  IDENTIFY_RESULT_NO_MATCH,
  IDENTIFY_RESULT_RETRY,
} IdentifyResult;

typedef struct _IdentifyHelperData
{
  GMainLoop      *loop;
  GCancellable   *cancellable;
  unsigned int    sigint_handler;
  gint            device_index;
  gchar          *save_image_path;
  gint            ret_value;
  IdentifyResult  result;
  gchar          *matched_finger;
} IdentifyHelperData;

static void
identify_helper_data_free (IdentifyHelperData *helper_data)
{
  g_clear_handle_id (&helper_data->sigint_handler, g_source_remove);
  g_clear_object (&helper_data->cancellable);
  g_clear_pointer (&helper_data->save_image_path, g_free);
  g_clear_pointer (&helper_data->matched_finger, g_free);
  g_main_loop_unref (helper_data->loop);
  g_free (helper_data);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (IdentifyHelperData, identify_helper_data_free)

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
  g_print ("EH577_IDENTIFY device-selected index=%d id=%s name=%s driver=%s\n",
           device_index,
           fp_device_get_device_id (dev),
           fp_device_get_name (dev),
           fp_device_get_driver (dev));
  return dev;
}

static void
on_finger_status_changed (FpDevice   *dev,
                          GParamSpec *pspec,
                          gpointer    user_data)
{
  FpFingerStatusFlags status = fp_device_get_finger_status (dev);

  g_print ("EH577_IDENTIFY finger-status status=%s\n",
           finger_status_to_string (status));
}

static void
on_device_closed (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  IdentifyHelperData *helper_data = user_data;
  g_autoptr(GError) error = NULL;

  fp_device_close_finish (dev, res, &error);
  if (error)
    g_warning ("Failed closing device %s", error->message);

  g_main_loop_quit (helper_data->loop);
}

static void
close_device_and_quit (FpDevice           *dev,
                       IdentifyHelperData *helper_data)
{
  if (!fp_device_is_open (dev))
    {
      g_main_loop_quit (helper_data->loop);
      return;
    }

  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_device_closed, helper_data);
}

static FpPrint *
get_stored_print (FpDevice *dev, FpPrint *print)
{
  g_autoptr(GPtrArray) gallery = gallery_data_load (dev);
  guint index;

  if (g_ptr_array_find_with_equal_func (gallery, print,
                                        (GEqualFunc) fp_print_equal,
                                        &index))
    return g_object_ref (g_ptr_array_index (gallery, index));

  return NULL;
}

static void
on_identify_completed (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  IdentifyHelperData *helper_data = user_data;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(FpPrint) match = NULL;
  g_autoptr(GError) error = NULL;

  if (!fp_device_identify_finish (dev, res, &match, &print, &error))
    {
      helper_data->ret_value = EXIT_FAILURE;

      if (error->domain == FP_DEVICE_RETRY)
        g_print ("EH577_IDENTIFY identify-complete result=retry code=%s message=%s\n",
                 retry_to_string (error->code),
                 error->message);
      else
        g_print ("EH577_IDENTIFY identify-complete result=failure domain=%s code=%d message=%s\n",
                 g_quark_to_string (error->domain),
                 error->code,
                 error->message);

      close_device_and_quit (dev, helper_data);
      return;
    }

  if (helper_data->result == IDENTIFY_RESULT_MATCH)
    {
      helper_data->ret_value = EXIT_SUCCESS;
      g_print ("EH577_IDENTIFY identify-complete result=match finger=%s\n",
               helper_data->matched_finger ? helper_data->matched_finger : "unknown");
    }
  else if (helper_data->result == IDENTIFY_RESULT_NO_MATCH)
    {
      helper_data->ret_value = EXIT_SUCCESS;
      g_print ("EH577_IDENTIFY identify-complete result=no-match\n");
    }
  else
    {
      helper_data->ret_value = EXIT_FAILURE;
      g_print ("EH577_IDENTIFY identify-complete result=unknown\n");
    }

  close_device_and_quit (dev, helper_data);
}

static void
on_identify_cb (FpDevice *dev, FpPrint *match, FpPrint *print,
                gpointer user_data, GError *error)
{
  IdentifyHelperData *helper_data = user_data;

  if (error)
    {
      helper_data->result = IDENTIFY_RESULT_RETRY;
      g_print ("EH577_IDENTIFY identify-retry code=%s message=%s\n",
               retry_to_string (error->code),
               error->message);
      return;
    }

  if (print && fp_print_get_image (print) &&
      print_image_save (print, helper_data->save_image_path))
    g_print ("EH577_IDENTIFY image-saved path=%s\n", helper_data->save_image_path);

  if (match)
    {
      g_autoptr(FpPrint) matched_print = g_object_ref (match);

      if (fp_print_get_device_stored (match))
        {
          FpPrint *stored_print = get_stored_print (dev, match);

          if (stored_print)
            matched_print = g_steal_pointer (&stored_print);
        }

      helper_data->result = IDENTIFY_RESULT_MATCH;
      g_free (helper_data->matched_finger);
      helper_data->matched_finger = g_strdup (finger_to_string (fp_print_get_finger (matched_print)));
      g_print ("EH577_IDENTIFY identify-result result=match finger=%s\n",
               helper_data->matched_finger);
    }
  else
    {
      helper_data->result = IDENTIFY_RESULT_NO_MATCH;
      g_print ("EH577_IDENTIFY identify-result result=no-match\n");
    }
}

static gboolean
sigint_cb (void *user_data)
{
  IdentifyHelperData *helper_data = user_data;

  g_cancellable_cancel (helper_data->cancellable);
  return G_SOURCE_CONTINUE;
}

static void
start_identification (FpDevice           *dev,
                      IdentifyHelperData *helper_data)
{
  g_autoptr(GPtrArray) gallery = gallery_data_load (dev);

  if (!gallery || gallery->len == 0)
    {
      helper_data->ret_value = EXIT_FAILURE;
      g_print ("EH577_IDENTIFY identify-complete result=no-gallery\n");
      close_device_and_quit (dev, helper_data);
      return;
    }

  g_print ("EH577_IDENTIFY gallery-loaded count=%u\n", gallery->len);
  fp_device_identify (dev,
                      gallery,
                      helper_data->cancellable,
                      on_identify_cb,
                      helper_data,
                      NULL,
                      (GAsyncReadyCallback) on_identify_completed,
                      helper_data);
}

static void
on_device_opened (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  IdentifyHelperData *helper_data = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      g_print ("EH577_IDENTIFY device-open-failed message=%s\n", error->message);
      g_main_loop_quit (helper_data->loop);
      return;
    }

  g_print ("EH577_IDENTIFY device-opened scan-type=%d\n",
           fp_device_get_scan_type (dev));

  start_identification (dev, helper_data);
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) ctx = NULL;
  g_autoptr(IdentifyHelperData) helper_data = NULL;
  GPtrArray *devices;
  FpDevice *dev;
  gchar *save_image_path = g_strdup ("identify.pgm");
  GOptionContext *context;
  GOptionEntry entries[] = {
    { "device-index", 'd', 0, G_OPTION_ARG_INT, &(gint){0}, "Device index", "INDEX" },
    { "save-image",   'o', 0, G_OPTION_ARG_FILENAME, &save_image_path, "Path to save captured identify image", "PATH" },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
  };
  gint device_index = 0;
  g_autoptr(GError) error = NULL;

  entries[0].arg_data = &device_index;

  setbuf (stdout, NULL);

  context = g_option_context_new ("- EH577 identify helper");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      g_option_context_free (context);
      return EXIT_FAILURE;
    }
  g_option_context_free (context);

  helper_data = g_new0 (IdentifyHelperData, 1);
  helper_data->loop = g_main_loop_new (NULL, FALSE);
  helper_data->cancellable = g_cancellable_new ();
  helper_data->device_index = device_index;
  helper_data->save_image_path = g_steal_pointer (&save_image_path);
  helper_data->ret_value = EXIT_FAILURE;
  helper_data->result = IDENTIFY_RESULT_NONE;
  helper_data->sigint_handler = g_unix_signal_add_full (G_PRIORITY_HIGH,
                                                        SIGINT,
                                                        sigint_cb,
                                                        helper_data,
                                                        NULL);

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);
  dev = select_device (devices, device_index);
  if (!dev)
    {
      g_printerr ("No supported device found at index %d\n", device_index);
      return EXIT_FAILURE;
    }

  g_signal_connect (dev, "notify::finger-status",
                    G_CALLBACK (on_finger_status_changed), helper_data);

  fp_device_open (dev, helper_data->cancellable,
                  (GAsyncReadyCallback) on_device_opened, helper_data);
  g_main_loop_run (helper_data->loop);

  return helper_data->ret_value;
}
