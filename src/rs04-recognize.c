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
#include "scsi-layer.h"
#include "udf.h"

#include "rs04-includes.h"

/* See rs04.tex, "Recognising RS04 data" */

#define SCAN_CHUNK (4<<20)
#define SIGNATURE "*dvdisaster*RS04"

static void unsupported(char *name)
{  PrintLog(_("* Warning: %s contains RS04 data of a newer format.\n"
	      "*          Please upgrade dvdisaster to process it.\n"), name);
}

/* Calls found(tag, byte position) for every 4-aligned signature at the
   offsets 0..last of buf (len bytes); pos must be a multiple of 4.
   Stops when found() returns TRUE. */

static int scan_buffer(unsigned char *buf, gint64 len, gint64 last, gint64 pos,
		       int (*found)(unsigned char*, gint64, gpointer), gpointer data)
{  gint64 o;

   for(o=0; o<=last && o+2048 <= len; o+=4)
     if(buf[o] == '*' && !memcmp(buf+o, SIGNATURE, 16)
	&& found(buf+o, pos+o, data))
       return TRUE;

   return FALSE;
}

/***
 *** Recognize an RS04 error correction file
 ***/

typedef struct
{  RS04Set set;
   int unsupported;
} file_scan;

static int file_tag_found(unsigned char *buf, gint64 pos, gpointer data)
{  file_scan *fs = (file_scan*)data;
   guint64 own;

   switch(RS04ParseTag(buf, &fs->set, &own, NULL))
   {  case RS04_TAG_VALID:
	 return (guint64)pos == (own - fs->set.S) * fs->set.B;
      case RS04_TAG_UNSUPPORTED:
	 fs->unsupported = TRUE;
	 break;
   }
   return FALSE;
}

int RS04RecognizeFile(LargeFile *ecc_file, EccHeader **eh)
{  unsigned char *buf = g_malloc(SCAN_CHUNK);
   file_scan fs;
   gint64 pos;
   int found = FALSE;

   memset(&fs, 0, sizeof(fs));
   *eh = NULL;

   /* Sector 0 is tag 0 */

   LargeSeek(ecc_file, 0);
   if(!RS04ReadAll(ecc_file, buf, 2048))
   {  g_free(buf);
      return ECCFILE_INVALID;
   }

   /* Another codec's header with a valid checksum: report it like RS03 does.
      Otherwise keep looking for RS04 tags. */

   if(!memcmp(buf, "*dvdisaster*", 12) && memcmp(buf+12, "RS04", 4)
      && RS04ReadAll(ecc_file, buf+2048, 2048))
   {  EccHeader *eh = (EccHeader*)buf;
      guint32 recorded = eh->selfCRC;

#ifdef HAVE_BIG_ENDIAN
      eh->selfCRC = 0x47504c00;
#else
      eh->selfCRC = 0x4c5047;
#endif
      if(Crc32(buf, sizeof(EccHeader)) == recorded)
      {  g_free(buf);
	 return ECCFILE_WRONG_CODEC;
      }
   }

   found = file_tag_found(buf, 0, &fs);

   /* Otherwise look for any tag at its proper place */

   for(pos=0; !found && pos < (gint64)ecc_file->size; pos += SCAN_CHUNK-2048)
   {  gint64 len = MIN(SCAN_CHUNK, (gint64)ecc_file->size - pos);

      if(!LargeSeek(ecc_file, pos) || !RS04ReadAll(ecc_file, buf, len))
	break;
      found = scan_buffer(buf, len, len, pos, file_tag_found, &fs);
      if(len < SCAN_CHUNK) break;
   }
   g_free(buf);

   if(!found)
   {  if(fs.unsupported)
	unsupported(ecc_file->path);
      return ECCFILE_INVALID;
   }

   fs.set.target = ECC_FILE;
   *eh = RS04CreateEccHeader(&fs.set, 1);
   Verbose("RS04RecognizeFile(): set found\n");
   return ECCFILE_PRESENT;
}

/***
 *** Recognize RS04 sets in an image or on a medium
 ***/

typedef struct
{  Image *image;
   GArray *cands;            /* candidate sets */
   int unsupported;
} image_scan;

static gint64 image_bytes(Image *image)
{  if(image->type == IMAGE_FILE)
     return image->file->size;
   return RS04MediumSectors(image) * 2048;
}

static int image_tag_found(unsigned char *buf, gint64 pos, gpointer data)
{  image_scan *is = (image_scan*)data;
   RS04Set set;
   guint64 own, i;

   switch(RS04ParseTag(buf, &set, &own, NULL))
   {  case RS04_TAG_VALID:
	 break;
      case RS04_TAG_UNSUPPORTED:
	 is->unsupported = TRUE;
	 return FALSE;
      default:
	 return FALSE;
   }

   if(pos % set.B || pos/set.B < own)
     return FALSE;

   set.start = pos/set.B - own;
   set.target = ECC_IMAGE;

   for(i=0; i<is->cands->len; i++)
   {  RS04Set *c = &g_array_index(is->cands, RS04Set, i);

      if(c->start == set.start && !memcmp(c->setID, set.setID, 8))
      {  c->tags++;
	 return FALSE;
      }
   }

   set.tags = 1;
   set.matches = -2;   /* not yet confirmed */
   g_array_append_val(is->cands, set);
   Verbose("RS04: tag at byte %" PRId64 ", set start %" PRId64 "\n", pos, set.start);
   return FALSE;
}

/* Read [lo, hi) of the image; unreadable medium sectors are zero */

static void read_bytes(Image *image, gint64 lo, gint64 len, unsigned char *buf)
{  if(image->type == IMAGE_FILE)
   {  memset(buf, 0, len);
      if(LargeSeek(image->file, lo))
	RS04ReadAll(image->file, buf, len);
   }
   else
   {  gint64 s;

      for(s=0; s<len/2048; s++)
	if(ImageReadSectors(image, buf+2048*s, lo/2048+s, 1) != 1)
	  memset(buf+2048*s, 0, 2048);
   }
}

/* Scan [lo, hi) from the end towards the start */

static void scan_region(image_scan *is, gint64 lo, gint64 hi)
{  unsigned char *buf = g_malloc(SCAN_CHUNK);
   gint64 limit;            /* tags at limit and above have been seen */

   lo = lo & ~2047;
   hi = MIN((hi + 2047) & ~2047, image_bytes(is->image));
   limit = hi;

   while(hi > lo)
   {  gint64 start = MAX(lo, hi - SCAN_CHUNK);

      read_bytes(is->image, start, hi-start, buf);
      scan_buffer(buf, hi-start, limit-start-4, start, image_tag_found, is);
      if(start == lo) break;
      limit = start;
      hi = start + 2048;    /* overlap for tags crossing the chunk border */
   }

   g_free(buf);
}

static void check_sector(image_scan *is, gint64 sector)
{  unsigned char buf[2048];

   if(sector < 0 || (sector+1)*2048 > image_bytes(is->image))
     return;

   read_bytes(is->image, sector*2048, 2048, buf);
   image_tag_found(buf, sector*2048, is);
}

/* Compare data sectors with their Crc32 from a CRC sector.
   Returns the number of matches, or -1 if nothing could be compared. */

static int confirm(Image *image, RS04Set *set)
{  RS04IO io;
   unsigned char *buf = g_malloc((size_t)RS04_CRCS_PER_SECTOR*set->B);
   guint8 present[RS04_CRCS_PER_SECTOR];
   guint64 j;
   int i, n, compared = 0, matches = 0;

   RS04InitIO(&io, image, set);

   for(j=0; j<MIN(set->c, 8); j++)
     if(RS04ReadPayload(&io, j, 1, buf, present))
       break;

   if(j < MIN(set->c, 8))
   {  guint32 crc[RS04_CRCS_PER_SECTOR];

      for(i=0; i<RS04_CRCS_PER_SECTOR; i++)
	crc[i] = buf[4*i] | buf[4*i+1]<<8 | buf[4*i+2]<<16 | (guint32)buf[4*i+3]<<24;

      n = MIN(RS04_CRCS_PER_SECTOR, set->S - j*RS04_CRCS_PER_SECTOR);
      RS04ReadData(&io, j*RS04_CRCS_PER_SECTOR, n, buf, present);
      for(i=0; i<n && compared<16; i++)
	if(present[i])
	{  compared++;
	   if(Crc32(buf+(size_t)i*set->B, set->B) == crc[i])
	     matches++;
	}
   }

   g_free(buf);
   return compared ? matches : -1;
}

/* Confirm new candidates and drop the rejected ones */

static void confirm_all(image_scan *is)
{  guint i, n = 0;

   for(i=0; i<is->cands->len; i++)
   {  RS04Set *c = &g_array_index(is->cands, RS04Set, i);

      if(c->matches == -2)
      {  c->matches = confirm(is->image, c);
	 Verbose("RS04: candidate start %" PRId64 ": %d tags, %d matching sectors\n",
		 c->start, c->tags, c->matches);
      }
      if(c->matches)
	g_array_index(is->cands, RS04Set, n++) = *c;
   }
   g_array_set_size(is->cands, n);
}

static gint compare_quality(gconstpointer a, gconstpointer b)
{  const RS04Set *x = a, *y = b;

   if(x->matches != y->matches) return y->matches - x->matches;
   return y->tags - x->tags;
}

static gint compare_start(gconstpointer a, gconstpointer b)
{  const RS04Set *x = a, *y = b;

   return x->start < y->start ? -1 : x->start > y->start;
}

static int overlap(RS04Set *a, RS04Set *b)
{  return a->start < b->start + (gint64)(b->S + b->A)
       && b->start < a->start + (gint64)(a->S + a->A);
}

/* Scan the parts of the image which are not covered by a candidate */

static void scan_gaps(image_scan *is)
{  GArray *sorted = g_array_new(FALSE, FALSE, sizeof(RS04Set));
   gint64 pos = 0, end = image_bytes(is->image);
   guint i;

   g_array_append_vals(sorted, is->cands->data, is->cands->len);
   g_array_sort(sorted, compare_start);

   for(i=0; i<=sorted->len; i++)
   {  gint64 next = end;

      if(i < sorted->len)
      {  RS04Set *s = &g_array_index(sorted, RS04Set, i);

	 next = MIN(end, s->start * s->B);
	 if(next > pos)
	   scan_region(is, pos, next);
	 pos = MAX(pos, (s->start + (gint64)(s->S + s->A)) * s->B);
      }
      else if(end > pos)
	scan_region(is, pos, end);
   }

   g_array_free(sorted, TRUE);
}

int RS04RecognizeImage(Image *image)
{  image_scan is;
   RS04Info *info;
   gint64 size;
   guint i, j, n;

   if(image->type != IMAGE_FILE && image->type != IMAGE_MEDIUM)
     return FALSE;

   memset(&is, 0, sizeof(is));
   is.image = image;
   is.cands = g_array_new(FALSE, FALSE, sizeof(RS04Set));
   size = image_bytes(image);

   /* 1. Behind the ISO file system (and its padding) */

   if(image->isoInfo)
   {  check_sector(&is, image->isoInfo->volumeSize);
      check_sector(&is, image->isoInfo->volumeSize + 150);
   }

   /* 2. The last 256 sectors (of the largest sector size) */

   scan_region(&is, MAX(0, size - 256*2352), size);

   /* 3. Further sets in the uncovered parts, or a full scan on request.
      Stale or embedded sets are rejected first so that they do not
      trigger the scan. */

   confirm_all(&is);
   if(is.cands->len && (image->type == IMAGE_FILE || Closure->examineRS04))
     scan_gaps(&is);
   else if(Closure->examineRS04)
     scan_region(&is, 0, size);
   confirm_all(&is);

   if(is.unsupported && !is.cands->len)
     unsupported(image->type == IMAGE_FILE ? image->file->path : image->dh->device);

   /* Resolve overlaps */

   g_array_sort(is.cands, compare_quality);

   for(i=0, n=0; i<is.cands->len && n<RS04_MAX_SETS; i++)
   {  RS04Set *c = &g_array_index(is.cands, RS04Set, i);
      int ok = TRUE;

      for(j=0; ok && j<n; j++)
	if(overlap(c, &g_array_index(is.cands, RS04Set, j)))
	  ok = FALSE;

      if(ok)
	g_array_index(is.cands, RS04Set, n++) = *c;
   }

   if(!n)
   {  g_array_free(is.cands, TRUE);
      return FALSE;
   }

   g_array_set_size(is.cands, n);
   g_array_sort(is.cands, compare_start);

   info = g_malloc0(sizeof(RS04Info) + n*sizeof(RS04Set));
   info->nSets = n;
   memcpy(info->set, is.cands->data, n*sizeof(RS04Set));
   g_array_free(is.cands, TRUE);

   image->eccHeader = RS04CreateEccHeader(&info->set[n-1], n);
   info->eh = image->eccHeader;
   g_free(image->cachedLayout);
   image->cachedLayout = info;

   Verbose("RS04RecognizeImage(): %d set(s) found\n", n);
   return TRUE;
}
