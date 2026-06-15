/*
 * All-pairs bozorth3 score matrix for a set of P5 PGM captures.
 *
 * Loads each PGM, runs NBIS minutiae detection, then prints an NxN matrix of
 * raw bozorth3 scores so we can see which captures match each other.
 *
 * Usage: eh577-pgm-match <a.pgm> [b.pgm ...] [--threshold N]
 *
 * The default threshold printed in the summary is 40 (libfprint default).
 *
 * Copyright (C) 2026 workspace contributors
 * LGPL-2.1-or-later
 */

#define FP_COMPONENT "eh577-pgm-match"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfprint/fprint.h>

/* Internal libfprint headers (available because meson adds ../libfprint to
 * the include path for this executable). */
#include "fpi-image.h"
#include "fpi-minutiae.h"

/* Forward declarations for the NBIS functions and types we need directly.
 * Pulling the NBIS headers in full causes redundant-decl errors when they're
 * used outside the libfprint build (they're only designed to be compiled as
 * part of that internal static library). */

#define MAX_BOZORTH_MINUTIAE 200
#define MAX_FILE_MINUTIAE    1000
#define sround(x) ((int) (((x) < 0) ? (x) - 0.5 : (x) + 0.5))

struct minutiae_struct { int col[4]; };

struct xyt_struct
{
  int nrows;  /* MUST be first — matches the real NBIS xyt_struct layout */
  int xcol[MAX_BOZORTH_MINUTIAE];
  int ycol[MAX_BOZORTH_MINUTIAE];
  int thetacol[MAX_BOZORTH_MINUTIAE];
};

extern void lfs2nist_minutia_XYT (int *, int *, int *,
                                  const struct fp_minutia *,
                                  const int, const int);
extern int  sort_x_y (const void *, const void *);
extern int  bozorth_probe_init (struct xyt_struct *);
extern int  bozorth_to_gallery (int, struct xyt_struct *, struct xyt_struct *);

#define DEFAULT_THRESHOLD 40
#define MAX_IMAGES        64

typedef struct
{
  GMainLoop        *loop;
  int               i;          /* index of image currently being processed */
  int               n;          /* total number of images */
  char            **files;
  struct xyt_struct xyt[MAX_IMAGES];
  int               nmin[MAX_IMAGES]; /* minutiae count for each image */
  int               threshold;
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
  if (fscanf (f, "%2s %d %d %d", magic, &w, &h, &mx) != 4 ||
      magic[0] != 'P' || magic[1] != '5')
    {
      fclose (f);
      return NULL;
    }
  fgetc (f); /* consume the single whitespace after maxval */

  FpImage *img = fp_image_new (w, h);
  img->width  = w;
  img->height = h;
  if (fread (img->data, 1, (size_t) w * h, f) != (size_t) w * h)
    {
      fclose (f);
      g_object_unref (img);
      return NULL;
    }
  fclose (f);
  img->flags = 0; /* already normal polarity: dark ridges on white */
  return img;
}

static void
image_to_xyt (FpImage *img, struct xyt_struct *xyt, int *out_nmin)
{
  GPtrArray *minutiae = fp_image_get_minutiae (img);
  struct minutiae_struct c[MAX_FILE_MINUTIAE];
  int nmin, i;

  *out_nmin = 0;

  if (!minutiae || minutiae->len == 0)
    return;

  nmin = (int) minutiae->len;
  if (nmin > MAX_BOZORTH_MINUTIAE)
    nmin = MAX_BOZORTH_MINUTIAE;

  for (i = 0; i < nmin; i++)
    {
      struct fp_minutia *m = (struct fp_minutia *) minutiae->pdata[i];
      lfs2nist_minutia_XYT (&c[i].col[0], &c[i].col[1], &c[i].col[2],
                            m, (int) img->width, (int) img->height);
      c[i].col[3] = sround (m->reliability * 100.0);
      if (c[i].col[2] > 180)
        c[i].col[2] -= 360;
    }

  qsort ((void *) &c, (size_t) nmin, sizeof (struct minutiae_struct), sort_x_y);

  for (i = 0; i < nmin; i++)
    {
      xyt->xcol[i]     = c[i].col[0];
      xyt->ycol[i]     = c[i].col[1];
      xyt->thetacol[i] = c[i].col[2];
    }
  xyt->nrows  = nmin;
  *out_nmin   = nmin;
}

static void
print_matrix (Ctx *c)
{
  int i, j;

  /* Column header */
  printf ("\nBozorth3 score matrix (%d images, threshold=%d)\n\n", c->n, c->threshold);
  printf ("     ");
  for (i = 0; i < c->n; i++)
    printf (" %4d", i + 1);
  printf ("\n");
  printf ("     ");
  for (i = 0; i < c->n; i++)
    printf (" ----");
  printf ("\n");

  for (i = 0; i < c->n; i++)
    {
      printf ("%3d |", i + 1);
      int probe_len = bozorth_probe_init (&c->xyt[i]);
      for (j = 0; j < c->n; j++)
        {
          int score = bozorth_to_gallery (probe_len, &c->xyt[i], &c->xyt[j]);
          if (score >= c->threshold)
            printf (" [%3d]", score); /* brackets = match */
          else
            printf ("  %3d ", score);
        }
      printf ("   (min=%d)\n", c->nmin[i]);
    }

  printf ("\n[score] = at or above threshold %d\n\n", c->threshold);

  /* Legend */
  printf ("Files:\n");
  for (i = 0; i < c->n; i++)
    printf ("  %2d  %s  (minutiae=%d)\n", i + 1, c->files[i], c->nmin[i]);
  printf ("\n");
}

static void
detect_cb (GObject *src, GAsyncResult *res, gpointer ud)
{
  FpImage *img = FP_IMAGE (src);
  Ctx *c = ud;
  g_autoptr (GError) error = NULL;

  if (fp_image_detect_minutiae_finish (img, res, &error))
    {
      image_to_xyt (img, &c->xyt[c->i], &c->nmin[c->i]);
      printf ("  %s  minutiae=%d\n", c->files[c->i], c->nmin[c->i]);
    }
  else
    {
      printf ("  %s  ERROR(%s)\n", c->files[c->i],
              error ? error->message : "?");
      c->xyt[c->i].nrows = 0;
      c->nmin[c->i]      = 0;
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
      print_matrix (c);
      g_main_loop_quit (c->loop);
      return;
    }

  FpImage *img = load_pgm (c->files[c->i]);
  if (!img)
    {
      printf ("  %s  LOAD-FAIL\n", c->files[c->i]);
      c->xyt[c->i].nrows = 0;
      c->nmin[c->i]      = 0;
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
  int file_argc = 0;
  char *file_argv[MAX_IMAGES];
  int i;

  c.threshold = DEFAULT_THRESHOLD;

  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--threshold") == 0 && i + 1 < argc)
        {
          c.threshold = atoi (argv[++i]);
        }
      else
        {
          if (file_argc >= MAX_IMAGES)
            {
              fprintf (stderr, "Too many files (max %d)\n", MAX_IMAGES);
              return 1;
            }
          file_argv[file_argc++] = argv[i];
        }
    }

  if (file_argc < 2)
    {
      printf ("usage: %s <a.pgm> <b.pgm> [more.pgm ...] [--threshold N]\n", argv[0]);
      printf ("  Prints an NxN bozorth3 score matrix for all pairs.\n");
      printf ("  Default threshold: %d\n", DEFAULT_THRESHOLD);
      return 1;
    }

  setbuf (stdout, NULL);
  c.loop  = g_main_loop_new (NULL, FALSE);
  c.files = file_argv;
  c.n     = file_argc;
  c.i     = 0;

  printf ("Detecting minutiae:\n");
  process_next (&c);
  g_main_loop_run (c.loop);
  g_main_loop_unref (c.loop);
  return 0;
}
