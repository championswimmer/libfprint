/*
 * Offline minutiae counter: load P5 PGM files (as produced by the EH577 capture/
 * enroll helpers — already resized, normal polarity), run NBIS minutiae detection
 * on each, and print the count. Lets us measure per-image minutiae without a
 * hardware run.
 *
 * Copyright (C) 2026 workspace contributors
 *
 * LGPL-2.1-or-later
 */

#define FP_COMPONENT "eh577-pgm-minutiae"

#include <stdio.h>
#include <libfprint/fprint.h>
#include "fpi-image.h"

typedef struct
{
  GMainLoop *loop;
  int        i;
  int        n;
  char     **files;
} Ctx;

static void process_next (Ctx *c);

static FpImage *
load_pgm (const char *path)
{
  FILE *f = fopen (path, "rb");
  int w, h, mx;
  char magic[3] = {0};

  if (!f)
    return NULL;
  if (fscanf (f, "%2s %d %d %d", magic, &w, &h, &mx) != 4 || magic[0] != 'P' || magic[1] != '5')
    {
      fclose (f);
      return NULL;
    }
  fgetc (f); /* consume the single whitespace after maxval */

  FpImage *img = fp_image_new (w, h);
  img->width = w;
  img->height = h;
  if (fread (img->data, 1, (size_t) w * h, f) != (size_t) w * h)
    {
      fclose (f);
      g_object_unref (img);
      return NULL;
    }
  fclose (f);
  /* Saved PGM is already normal polarity (dark ridges on white), so no flags. */
  img->flags = 0;
  return img;
}

static void
detect_cb (GObject *src, GAsyncResult *res, gpointer ud)
{
  FpImage *img = FP_IMAGE (src);
  Ctx *c = ud;

  g_autoptr(GError) error = NULL;

  if (fp_image_detect_minutiae_finish (img, res, &error))
    {
      GPtrArray *m = fp_image_get_minutiae (img);
      printf ("%s minutiae=%u\n", c->files[c->i], m ? m->len : 0);
    }
  else
    {
      printf ("%s minutiae=ERR(%s)\n", c->files[c->i], error ? error->message : "?");
    }

  g_object_unref (img);
  c->i++;
  process_next (c);
}

static void
process_next (Ctx *c)
{
  if (c->i >= c->n)
    {
      g_main_loop_quit (c->loop);
      return;
    }

  FpImage *img = load_pgm (c->files[c->i]);
  if (!img)
    {
      printf ("%s LOAD-FAIL\n", c->files[c->i]);
      c->i++;
      process_next (c);
      return;
    }

  fp_image_detect_minutiae (img, NULL, detect_cb, c);
}

int
main (int argc, char **argv)
{
  Ctx c = {0};

  if (argc < 2)
    {
      printf ("usage: %s <file.pgm> [more.pgm ...]\n", argv[0]);
      return 1;
    }

  setbuf (stdout, NULL);
  c.loop = g_main_loop_new (NULL, FALSE);
  c.files = argv + 1;
  c.n = argc - 1;
  c.i = 0;

  process_next (&c);
  g_main_loop_run (c.loop);
  g_main_loop_unref (c.loop);
  return 0;
}
