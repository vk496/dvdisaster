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
{  GArray *cands;           /* distinct sets of the valid tags found */
   int collect;             /* FALSE: stop at the first valid tag */
   int unsupported;
} file_scan;

/* Tags of one set should be identical except for their position and CRCs */

static int same_params(RS04Set *a, RS04Set *b)
{  return a->B == b->B && a->T == b->T && a->dataBytes == b->dataBytes
       && a->L == b->L && a->k == b->k && a->m == b->m && a->flags == b->flags
       && a->creatorVersion == b->creatorVersion && !memcmp(a->dataMD5, b->dataMD5, 16);
}

static int same_set(RS04Set *a, RS04Set *b)
{  return !memcmp(a->setID, b->setID, 8) && same_params(a, b);
}

/* A valid tag at its proper place in the ecc file */

static int parse_file_tag(unsigned char *buf, gint64 pos, RS04Set *set, int *unsupported)
{  guint64 own;

   switch(RS04ParseTag(buf, set, &own, NULL))
   {  case RS04_TAG_VALID:
	 return (guint64)pos == (own - set->S) * set->B;
      case RS04_TAG_UNSUPPORTED:
	 *unsupported = TRUE;
	 break;
   }
   return FALSE;
}

static int file_tag_found(unsigned char *buf, gint64 pos, gpointer data)
{  file_scan *fs = (file_scan*)data;
   RS04Set set;
   guint i;

   if(!parse_file_tag(buf, pos, &set, &fs->unsupported))
     return FALSE;

   for(i=0; i<fs->cands->len; i++)
     if(same_set(&g_array_index(fs->cands, RS04Set, i), &set))
       return !fs->collect;

   g_array_append_val(fs->cands, set);
   return !fs->collect;
}

/* Tags of set c among up to 16 of its tag positions spread over the file */

static int votes(LargeFile *file, RS04Set *c)
{  unsigned char buf[2048];
   guint64 tags = (c->A + c->T - 1)/c->T;
   int probe, probes = MIN(tags, 16), n = 0, dummy;

   for(probe=0; probe<probes; probe++)
   {  guint64 g = probes > 1 ? probe*(tags-1)/(probes-1) : 0;
      gint64 pos = g*c->T*c->B;
      RS04Set set;

      if(   LargeSeek(file, pos) && RS04ReadAll(file, buf, 2048)
	 && parse_file_tag(buf, pos, &set, &dummy) && same_set(&set, c))
	n++;
   }

   return n;
}

int RS04RecognizeFile(LargeFile *ecc_file, EccHeader **eh)
{  unsigned char *buf = g_malloc(SCAN_CHUNK);
   gint64 size = ecc_file->size;
   file_scan fs;
   RS04Set *best;
   gint64 pos;
   guint i;
   int best_votes;

   memset(&fs, 0, sizeof(fs));
   fs.cands = g_array_new(FALSE, FALSE, sizeof(RS04Set));
   *eh = NULL;

   /* Sector 0 is tag 0 */

   LargeSeek(ecc_file, 0);
   if(!RS04ReadAll(ecc_file, buf, 2048))
   {  g_free(buf);
      g_array_free(fs.cands, TRUE);
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
	 g_array_free(fs.cands, TRUE);
	 return ECCFILE_WRONG_CODEC;
      }
   }

   /* Tag 0 and the tags in the last 256 sectors, so that a tag 0 which
      does not belong to the rest of the file is noticed */

   fs.collect = TRUE;
   file_tag_found(buf, 0, &fs);

   pos = MAX(2048, size - 256*2352) & ~3;
   if(pos < size && LargeSeek(ecc_file, pos) && RS04ReadAll(ecc_file, buf, size-pos))
     scan_buffer(buf, size-pos, size-pos, pos, file_tag_found, &fs);

   /* Otherwise look for any tag at its proper place */

   fs.collect = FALSE;
   for(pos=0; !fs.cands->len && pos < size; pos += SCAN_CHUNK-2048)
   {  gint64 len = MIN(SCAN_CHUNK, size - pos);

      if(!LargeSeek(ecc_file, pos) || !RS04ReadAll(ecc_file, buf, len))
	break;
      scan_buffer(buf, len, len, pos, file_tag_found, &fs);
      if(len < SCAN_CHUNK) break;
   }
   g_free(buf);

   if(!fs.cands->len)
   {  if(fs.unsupported)
	unsupported(ecc_file->path);
      g_array_free(fs.cands, TRUE);
      return ECCFILE_INVALID;
   }

   /* Tags which disagree: the set held by the majority of the tags wins
      (rs04.tex, "Recognising RS04 data"); tag 0 wins a tie. */

   best = &g_array_index(fs.cands, RS04Set, 0);
   if(fs.cands->len > 1)
   {  best_votes = votes(ecc_file, best);
      for(i=1; i<fs.cands->len; i++)
      {  RS04Set *c = &g_array_index(fs.cands, RS04Set, i);
	 int v = votes(ecc_file, c);

	 Verbose("RS04RecognizeFile(): sets disagree, %d vs. %d tags\n", v, best_votes);
	 if(v > best_votes)
	 {  best = c;
	    best_votes = v;
	 }
      }
   }

   best->target = ECC_FILE;
   *eh = RS04CreateEccHeader(best, 1);
   g_array_free(fs.cands, TRUE);
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

      if(c->start == set.start && !memcmp(c->setID, set.setID, 8) && same_params(c, &set))
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

/* Compare up to 16 readable data sectors with their Crc32 from readable
   CRC sectors. Up to 16 CRC sectors spread over the set are tried, and
   the first 16 data sectors behind each of them.
   Returns the number of matches, or -1 if nothing could be compared. */

static int confirm(Image *image, RS04Set *set)
{  RS04IO io;
   unsigned char *buf = g_malloc((size_t)16*set->B);
   guint8 present[16];
   guint32 crc[16];
   int probe, probes = MIN(set->c, 16);
   int i, n, compared = 0, matches = 0;

   RS04InitIO(&io, image, set);

   for(probe=0; probe<probes && compared<16; probe++)
   {  guint64 j = probe*set->c/probes;

      if(!RS04ReadPayload(&io, j, 1, buf, present))
	continue;

      for(i=0; i<16; i++)
	crc[i] = buf[4*i] | buf[4*i+1]<<8 | buf[4*i+2]<<16 | (guint32)buf[4*i+3]<<24;

      n = MIN(16, set->S - j*RS04_CRCS_PER_SECTOR);
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

/* Tags of one set which disagree yield several candidates with the same
   set ID and start; the one held by the majority of the tags wins. */

static int outvoted(image_scan *is, guint i)
{  RS04Set *c = &g_array_index(is->cands, RS04Set, i);
   guint j;

   for(j=0; j<is->cands->len; j++)
   {  RS04Set *d = &g_array_index(is->cands, RS04Set, j);

      if(   j != i && d->start == c->start && !memcmp(d->setID, c->setID, 8)
	 && (d->tags > c->tags || (d->tags == c->tags && j < i)))
	return TRUE;
   }

   return FALSE;
}

/* Confirm new candidates and drop the rejected ones */

static void confirm_all(image_scan *is)
{  guint i, n = 0;

   for(i=0; i<is->cands->len; i++)
     if(outvoted(is, i))
     {  Verbose("RS04: candidate start %" PRId64 " outvoted by other tags of its set\n",
		g_array_index(is->cands, RS04Set, i).start);
	g_array_index(is->cands, RS04Set, i).matches = 0;
     }

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

   /* 2. The last 256 sectors (of the largest sector size),
      also of every session of a multi-session medium */

   scan_region(&is, MAX(0, size - 256*2352), size);
   if(image->type == IMAGE_MEDIUM)
     for(i=0; (int)i+1<image->dh->nTracks; i++)
       if(image->dh->track[i].session != image->dh->track[i+1].session)
       {  gint64 end = image->dh->track[i].end*2048;

	  scan_region(&is, MAX(0, end - 256*2352), end);
       }

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

/***
 *** Supplementary sets (rs04.tex, "Supplementary sets")
 ***
 * Fix and verify combine all RS04 sets which protect the same image:
 * the ecc file, further ecc files given with -e, and the augmented
 * sets of the image which overlap the data of the ecc file.
 * Sets with another sector size are not combined.
 */

/* TRUE if the RS04 ecc file belongs to the image (or this can not be checked) */

int RS04ConfirmEccFile(Image *image, LargeFile *file, EccHeader *eh)
{  RS04Set set;

   if(RS04ParseTag((unsigned char*)eh->padding, &set, NULL, NULL) != RS04_TAG_VALID)
     return FALSE;
   set.target = ECC_FILE;
   set.file = file;
   return confirm(image, &set) != 0;
}

static void ignore_ecc_file(char *name, char *reason)
{  PrintLog(_("* ecc file %s ignored: %s\n"), name, reason);
}

RS04Info* RS04GetAllSets(Image *image, int flags)
{  RS04Info *info = RS04GetSets(image);
   RS04Info *aug = NULL;
   GArray *sets;
   RS04Set *first;
   gint64 extent;
   guint i;

   if(!info || info->set[0].target != ECC_FILE)
     return info;

   sets = g_array_new(FALSE, FALSE, sizeof(RS04Set));
   g_array_append_val(sets, info->set[0]);
   first = &info->set[0];
   extent = first->dataBytes;

   /* Augmented sets overlapping the data of the ecc file */

   if(image->eccHeader && image->cachedLayout
      && !strncmp((char*)image->eccHeader->method, "RS04", 4))
   {  aug = (RS04Info*)image->cachedLayout;

      for(i=0; i<aug->nSets; i++)
      {  RS04Set *set = &aug->set[i];

	 if(set->B == first->B && set->start*set->B < (gint64)first->dataBytes)
	   extent = MAX(extent, (set->start + (gint64)(set->S + set->A))*set->B);
      }
   }

   /* Further ecc files; they must not extend the image */

   for(i=0; Closure->moreEccNames && i<Closure->moreEccNames->len; i++)
   {  char *name = g_ptr_array_index(Closure->moreEccNames, i);
      LargeFile *file = LargeOpen(name, flags, IMG_PERMS);
      EccHeader *eh;
      RS04Set set;
      int ok;

      if(!file)
      {  ignore_ecc_file(name, strerror(errno));
	 continue;
      }
      if(RS04RecognizeFile(file, &eh) != ECCFILE_PRESENT)
      {  ignore_ecc_file(name, _("no RS04 data"));
	 LargeClose(file);
	 continue;
      }

      RS04ParseTag((unsigned char*)eh->padding, &set, NULL, NULL);
      set.target = ECC_FILE;
      set.file = file;
      ok =    set.B == first->B && (gint64)set.dataBytes <= extent
	   && RS04ConfirmEccFile(image, file, eh);
      g_free(eh);

      if(!ok)
      {  ignore_ecc_file(name, _("does not belong to the image"));
	 LargeClose(file);
	 continue;
      }
      g_array_append_val(sets, set);
   }

   if(aug)
     for(i=0; i<aug->nSets; i++)
     {  RS04Set *set = &aug->set[i];

	if(set->B == first->B && set->start*set->B < (gint64)first->dataBytes)
	  g_array_append_val(sets, *set);
     }

   if(sets->len > 1)
   {  RS04Info *all = g_malloc0(sizeof(RS04Info) + sets->len*sizeof(RS04Set));

      all->eh = info->eh;
      all->nSets = sets->len;
      memcpy(all->set, sets->data, sets->len*sizeof(RS04Set));
      g_free(info);
      info = all;
   }

   g_array_free(sets, TRUE);
   return info;
}

void RS04FreeSets(Image *image, RS04Info *info)
{  int i;

   if(!info) return;

   for(i=0; i<info->nSets; i++)
     if(info->set[i].file && info->set[i].file != image->eccFile)
       LargeClose(info->set[i].file);
   g_free(info);
}

/* TRUE if some sets protect the same image sectors */

int RS04Combined(RS04Info *info)
{  int i, j;

   for(i=0; i<info->nSets; i++)
     for(j=0; j<i; j++)
     {  RS04Set *a = &info->set[i], *b = &info->set[j];
	gint64 a0 = a->target == ECC_FILE ? 0 : a->start*a->B;
	gint64 a1 = a->target == ECC_FILE ? (gint64)a->dataBytes : (a->start + (gint64)(a->S + a->A))*a->B;
	gint64 b0 = b->target == ECC_FILE ? 0 : b->start*b->B;
	gint64 b1 = b->target == ECC_FILE ? (gint64)b->dataBytes : (b->start + (gint64)(b->S + b->A))*b->B;

	if(a0 < b1 && b0 < a1)
	  return TRUE;
     }

   return FALSE;
}
