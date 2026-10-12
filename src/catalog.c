/*  dvdisaster: Additional error correction for optical media.
 *  Copyright (C) 2004-2017 Carsten Gnoerlich.
 *  Copyright (C) 2019-2026 The dvdisaster development team.
 *
 *  Email: support@dvdisaster.org
 *
 *  This file is part of dvdisaster.
 *
 *  dvdisaster is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  dvdisaster is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with dvdisaster. If not, see <http://www.gnu.org/licenses/>.
 */

/*** src type: no GUI code ***/

#include "dvdisaster.h"
#include "md5.h"

/***
 *** Catalog of scan/read results, so that the state of a medium
 *** can be followed over the years.
 ***
 * One line per run, tab separated:
 * date  fingerprint  label  sectors  action  unreadable  crc-errors
 */

#define CATALOG_FIELDS 7

static void sanitize(char *s)
{  for(; *s; s++)
     if(*s == '\t' || *s == '\n' || *s == '\r')
       *s = ' ';
}

void AppendCatalog(Image *image, char *action, char *label,
		   gint64 sectors, gint64 unreadable, gint64 crc_errors)
{  FILE *file = g_fopen(Closure->catalogFile, "ab");
   char fingerprint[33], date[32];
   char *l = g_strdup(label && *label ? label : "-");

   if(!file)
   {  PrintLog(_("Could not append to catalog file %s: %s\n"),
	       Closure->catalogFile, strerror(errno));
      g_free(l);
      return;
   }

   if(Closure->regtestMode)
      strcpy(date, "2000-01-01 00:00:00");
   else
   {  time_t now = time(NULL);
      strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));
   }

   if(image && image->fpState == 2)
        AsciiDigest(fingerprint, image->imageFP);
   else strcpy(fingerprint, "unknown");

   sanitize(l);
   fprintf(file, "%s\t%s\t%s\t%" PRId64 "\t%s\t%" PRId64 "\t%" PRId64 "\n",
	   date, fingerprint, l, sectors, action, unreadable, crc_errors);
   fclose(file);
   g_free(l);
}

/*
 * Print the history of each medium in the catalog
 */

static void free_runs(gpointer data)
{  g_ptr_array_free((GPtrArray*)data, TRUE);
}

void CatalogReport(char *path)
{  GHashTable *media = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_runs);
   GPtrArray *order = g_ptr_array_new();   /* fingerprints in order of appearance */
   char *contents, **lines;
   int i, j;

   if(!g_file_get_contents(path, &contents, NULL, NULL))
     Stop(_("Can't open %s:\n%s"), path, strerror(errno));

   lines = g_strsplit(contents, "\n", -1);
   g_free(contents);

   for(i=0; lines[i]; i++)
   {  char **f = g_strsplit(lines[i], "\t", CATALOG_FIELDS);
      GPtrArray *runs;

      if(g_strv_length(f) != CATALOG_FIELDS)  /* empty or malformed line */
      {  g_strfreev(f);
	 continue;
      }

      runs = g_hash_table_lookup(media, f[1]);
      if(!runs)
      {  char *key = g_strdup(f[1]);  /* owned by the hash table */

	 runs = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	 g_hash_table_insert(media, key, runs);
	 g_ptr_array_add(order, key);
      }
      g_ptr_array_add(runs, f);
   }
   g_strfreev(lines);

   for(i=0; i<order->len; i++)
   {  GPtrArray *runs = g_hash_table_lookup(media, g_ptr_array_index(order, i));
      char **first = g_ptr_array_index(runs, 0);
      gint64 previous = 0;
      int growing = FALSE;

      PrintLog(_("\nMedium \"%s\" (%s sectors, fingerprint %s):\n"), first[2], first[3], first[1]);

      for(j=0; j<runs->len; j++)
      {  char **f = g_ptr_array_index(runs, j);
	 gint64 damaged = g_ascii_strtoll(f[5], NULL, 10) + g_ascii_strtoll(f[6], NULL, 10);

	 PrintLog(_("  %s  %-4s  %s unreadable, %s CRC errors\n"), f[0], f[4], f[5], f[6]);
	 if(j > 0 && damaged > previous)
	    growing = TRUE;
	 previous = damaged;
      }

      if(growing)
	 PrintLog(_("  * The number of damaged sectors is growing: consider copying this medium.\n"));
   }

   g_ptr_array_free(order, TRUE);
   g_hash_table_destroy(media);
}
