/*
 * Extracts the views of an fprintd template, for offline evaluation
 *
 * Copyright (C) 2026 Benjamin Allègre (https://github.com/Sigfrodr)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* Extracts the views of an fprintd template into .view files, so that the
 * template ACTUALLY in service can be evaluated offline — not a stand-in. */
#include <fprint.h>
#include <glib/gstdio.h>

int main (int argc, char **argv)
{
  g_autofree gchar *raw = NULL; gsize len = 0;
  g_autoptr(GError) err = NULL;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GVariant) data = NULL;
  GVariantIter it; GVariant *child; int i = 0;

  if (argc < 3) { g_print ("usage: %s <template file> <output dir>\n", argv[0]); return 1; }
  if (!g_file_get_contents (argv[1], &raw, &len, &err))
    { g_print ("%s\n", err->message); return 1; }
  print = fp_print_deserialize ((guint8 *) raw, len, &err);
  if (!print) { g_print ("%s\n", err->message); return 1; }
  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("aay")))
    { g_print ("no usable data\n"); return 1; }
  g_mkdir_with_parents (argv[2], 0755);
  g_variant_iter_init (&it, data);
  while ((child = g_variant_iter_next_value (&it)))
    {
      gsize n = 0;
      const guint8 *b = g_variant_get_fixed_array (child, &n, 1);
      g_autofree gchar *p = g_strdup_printf ("%s/v%03d.view", argv[2], i++);
      g_file_set_contents (p, (const gchar *) b, n, NULL);
      g_variant_unref (child);
    }
  g_print ("%d views extracted\n", i);
  return 0;
}
