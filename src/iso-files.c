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

/***
 *** List the files of an ISO 9660 image which contain missing sectors
 ***/

typedef struct
{  guint64 first, last;   /* sector range of this extent, inclusive */
   char *path;
} file_extent;

typedef struct
{  LargeFile *file;
   guint64 sectors;
   GPtrArray *extents;
   GHashTable *visited;   /* directory sectors already parsed */
   int joliet;
} iso_walk;

static int read_sector(iso_walk *w, guint64 s, unsigned char *buf)
{  if(s >= w->sectors || !LargeSeek(w->file, 2048*s))
      return FALSE;
   return LargeRead(w->file, buf, 2048) == 2048;
}

static guint32 le32(unsigned char *p)
{  return p[0] | p[1]<<8 | p[2]<<16 | (guint32)p[3]<<24;
}

/* Convert a directory record file name into UTF-8 */

static char* record_name(iso_walk *w, unsigned char *name, int len)
{  char *result;

   if(w->joliet)  /* UCS-2 big endian */
   {  gunichar2 *utf16 = g_malloc0(len+2);
      int i;

      for(i=0; i<len/2; i++)
	 utf16[i] = name[2*i]<<8 | name[2*i+1];
      result = g_utf16_to_utf8(utf16, len/2, NULL, NULL, NULL);
      g_free(utf16);
      if(!result) result = g_strdup("?");
   }
   else result = g_strndup((char*)name, len);

   /* Strip the version number (e.g. ";1") */

   if(result[0])
   {  char *semicolon = strrchr(result, ';');

      if(semicolon) *semicolon = 0;
   }

   return result;
}

static void walk_directory(iso_walk *w, guint32 lba, guint32 length, char *path, int depth)
{  unsigned char buf[2048];
   guint32 n_sectors = (length+2047)/2048;
   guint32 i;

   if(depth > 64 || g_hash_table_contains(w->visited, GUINT_TO_POINTER(lba)))
      return;  /* corrupted file system */
   g_hash_table_add(w->visited, GUINT_TO_POINTER(lba));

   for(i=0; i<n_sectors; i++)
   {  int pos = 0;

      if(!read_sector(w, lba+i, buf)
	 || CheckForMissingSector(buf, lba+i, NULL, 0) != SECTOR_PRESENT)
	 continue;  /* unreadable directory sector; its entries are lost */

      while(pos < 2048-33)
      {  unsigned char *rec = buf+pos;
	 int rec_len = rec[0];
	 int name_len = rec[32];
	 guint32 extent = le32(rec+2);
	 guint32 size = le32(rec+10);
	 int is_dir = rec[25] & 2;
	 char *name, *child;

	 if(rec_len == 0)  /* rest of this sector is unused */
	    break;
	 if(rec_len < 34 || pos+rec_len > 2048 || 33+name_len > rec_len)
	    break;         /* corrupted record */
	 pos += rec_len;

	 if(name_len == 1 && (rec[33] == 0 || rec[33] == 1))
	    continue;      /* "." and ".." */

	 name  = record_name(w, rec+33, name_len);
	 child = g_strdup_printf("%s/%s", path, name);
	 g_free(name);

	 if(is_dir)
	 {  walk_directory(w, extent, size, child, depth+1);
	    g_free(child);
	 }
	 else if(size > 0)
	 {  file_extent *fe = g_malloc(sizeof(file_extent));

	    fe->first = extent;
	    fe->last  = extent + (size+2047)/2048 - 1;
	    fe->path  = child;  /* multi-extent files simply get several extents */
	    g_ptr_array_add(w->extents, fe);
	 }
	 else g_free(child);
      }
   }
}

static gint compare_extents(gconstpointer a, gconstpointer b)
{  file_extent *x = *(file_extent**)a;
   file_extent *y = *(file_extent**)b;

   if(x->first < y->first) return -1;
   if(x->first > y->first) return 1;
   return 0;
}

/* Find the root directory; prefer Joliet names if available */

static int find_root(iso_walk *w, guint32 *lba, guint32 *length)
{  unsigned char buf[2048];
   int found = FALSE;
   guint64 s;

   for(s=16; s<16+64; s++)
   {  if(!read_sector(w, s, buf) || memcmp(buf+1, "CD001", 5))
	 break;
      if(buf[0] == 255)  /* volume descriptor set terminator */
	 break;

      if(buf[0] == 1 && !found)  /* primary volume descriptor */
      {  *lba = le32(buf+156+2);
	 *length = le32(buf+156+10);
	 found = TRUE;
      }

      if(buf[0] == 2 && buf[88] == '%' && buf[89] == '/'
	 && (buf[90] == '@' || buf[90] == 'C' || buf[90] == 'E'))  /* Joliet */
      {  *lba = le32(buf+156+2);
	 *length = le32(buf+156+10);
	 w->joliet = found = TRUE;
	 break;
      }
   }

   return found;
}

void ListDamagedFiles(char *image_name)
{  iso_walk w;
   guint64 size, s;
   guint32 root_lba, root_length;
   unsigned char *buf;
   char *damaged;
   guint64 damaged_total = 0, outside = 0;
   int damaged_files = 0;
   guint i;

   memset(&w, 0, sizeof(w));
   if(!LargeStat(image_name, &size))
     Stop(_("Image file %s not present."), image_name);

   w.file = LargeOpen(image_name, O_RDONLY, IMG_PERMS);
   if(!w.file)
     Stop(_("Can't open %s:\n%s"), image_name, strerror(errno));
   w.sectors = size/2048;

   /* Find the missing sectors */

   damaged = g_malloc0(w.sectors+1);
   buf = g_malloc(64*2048);
   for(s=0; s<w.sectors; s+=64)
   {  int i, n = MIN(64, w.sectors-s);

      if(!LargeSeek(w.file, 2048*s) || LargeRead(w.file, buf, n*2048) != n*2048)
	 break;
      for(i=0; i<n; i++)
	 if(CheckForMissingSector(buf+2048*i, s+i, NULL, 0) != SECTOR_PRESENT)
	 {  damaged[s+i] = 1;
	    damaged_total++;
	 }
   }
   g_free(buf);

   PrintLog(_("\nFiles affected by missing sectors:\n"));

   /* Collect the file extents */

   w.extents = g_ptr_array_new();
   w.visited = g_hash_table_new(g_direct_hash, g_direct_equal);

   if(!find_root(&w, &root_lba, &root_length))
   {  PrintLog(_("- no ISO 9660 file system found\n"));
      goto cleanup;
   }

   walk_directory(&w, root_lba, root_length, "", 0);
   g_ptr_array_sort(w.extents, compare_extents);

   /* Report files containing missing sectors */

   for(i=0; i<w.extents->len; i++)
   {  file_extent *fe = g_ptr_array_index(w.extents, i);
      guint64 bad = 0;

      for(s=fe->first; s<=fe->last && s<w.sectors; s++)
	 if(damaged[s])
	 {  bad++;
	    damaged[s] = 2;  /* belongs to a file */
	 }

      if(bad)
      {  PrintLog(_("- %s: %" PRId64 " of %" PRId64 " sectors missing\n"),
		  fe->path, bad, fe->last-fe->first+1);
	 damaged_files++;
      }
   }

   for(s=0; s<w.sectors; s++)
      if(damaged[s] == 1)
	 outside++;

   PrintLog(_("%d damaged files; %" PRId64 " missing sectors in total, "
	      "%" PRId64 " of them outside of files (file system data or unused).\n"),
	    damaged_files, damaged_total, outside);

cleanup:
   for(i=0; i<w.extents->len; i++)
   {  file_extent *fe = g_ptr_array_index(w.extents, i);
      g_free(fe->path);
      g_free(fe);
   }
   g_ptr_array_free(w.extents, TRUE);
   g_hash_table_destroy(w.visited);
   g_free(damaged);
   LargeClose(w.file);
}
