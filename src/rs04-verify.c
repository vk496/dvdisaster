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

#include "rs04-includes.h"

#define EXIT_CODE_SIZE_MISMATCH 1
#define EXIT_CODE_MISSING_SECTOR 11
#define EXIT_CODE_CHECKSUM_ERROR 12

#define CHUNK 1024     /* sectors per read */

/***
 *** Checksum state of a set
 ***/

void RS04InitState(RS04State *st, Image *image, RS04Set *set)
{  memset(st, 0, sizeof(RS04State));
   st->set = set;
   RS04InitIO(&st->io, image, set);
   st->nTags = (set->Q + set->T - 2)/(set->T - 1);
   st->payloadCrc   = g_malloc0(set->Q*sizeof(guint32));
   st->payloadKnown = CreateBitmap0(set->Q);
   st->payloadBad   = CreateBitmap0(set->Q);
   st->dataCrc      = g_malloc0(set->S*sizeof(guint32));
   st->dataKnown    = CreateBitmap0(set->S);
   st->tagBad       = CreateBitmap0(st->nTags);
}

void RS04FreeState(RS04State *st)
{  g_free(st->payloadCrc);
   g_free(st->dataCrc);
   if(st->payloadKnown) FreeBitmap(st->payloadKnown);
   if(st->payloadBad) FreeBitmap(st->payloadBad);
   if(st->dataKnown) FreeBitmap(st->dataKnown);
   if(st->tagBad) FreeBitmap(st->tagBad);
   memset(st, 0, sizeof(RS04State));
}

/* Take the data sector checksums from CRC sector j */

void RS04SetCrcSector(RS04State *st, guint64 j, unsigned char *buf)
{  RS04Set *set = st->set;
   int u;

   for(u=0; u<RS04_CRCS_PER_SECTOR; u++)
   {  guint64 t = j*RS04_CRCS_PER_SECTOR + u;

      if(t >= set->S) break;
      st->dataCrc[t] = buf[4*u] | buf[4*u+1]<<8 | buf[4*u+2]<<16 | (guint32)buf[4*u+3]<<24;
      SetBit(st->dataKnown, t);
   }
}

/***
 *** Reporting of missing sector ranges
 ***/

typedef struct
{  gint64 first, last;
   char *suffix;
   GString *out;
} ranges;

static void range_flush(ranges *r)
{  if(r->first < 0) return;

   if(r->first == r->last)
        g_string_append_printf(r->out, _("* missing sector   : %" PRId64 "%s\n"), r->first, r->suffix);
   else g_string_append_printf(r->out, _("* missing sectors  : %" PRId64 " - %" PRId64 "%s\n"),
			       r->first, r->last, r->suffix);
   r->first = -1;
}

static void range_add(ranges *r, gint64 s)
{  if(r->first >= 0 && s == r->last+1)
   {  r->last = s;
      return;
   }
   range_flush(r);
   r->first = r->last = s;
}

/***
 *** Read the added zone: tags, CRC sectors and parity sectors.
 *** Missing ranges and CRC errors are appended to report (if not NULL).
 *** RS04ScanZonePart() only reads the first zone_sectors sectors.
 ***/

void RS04ScanZone(RS04State *st, GString *report)
{  RS04ScanZonePart(st, report, st->set->A);
}

void RS04ScanZonePart(RS04State *st, GString *report, guint64 zone_sectors)
{  RS04Set *set = st->set;
   unsigned char *buf = g_malloc((size_t)CHUNK*set->B);
   guint8 present[CHUNK];
   guint32 crc[RS04_MAX_TAG_INTERVAL-1];
   ranges r;
   gint64 base = set->target == ECC_FILE ? 0 : set->start + set->S;
   guint64 a0;

   r.first = -1;
   r.suffix = set->target == ECC_FILE ? _(" (in ecc file)") : " ";
   r.out = report ? report : g_string_new(NULL);

   zone_sectors = MIN(zone_sectors, set->A);
   for(a0=0; a0<zone_sectors; a0+=CHUNK)
   {  int i, n = MIN(CHUNK, zone_sectors - a0);

      RS04ReadZone(&st->io, a0, n, buf, present);

      for(i=0; i<n; i++)
      {  guint64 a = a0+i;
	 unsigned char *sector = buf+(size_t)i*set->B;

	 if(!present[i])
	   range_add(&r, base+a);

	 if(!(a % set->T))   /* tag */
	 {  guint64 g = a/set->T, own;
	    RS04Set tag;
	    int u;

	    if(   present[i]
	       && RS04ParseTag(sector, &tag, &own, crc) == RS04_TAG_VALID
	       && own == set->S + a && !memcmp(tag.setID, set->setID, 8)
	       && tag.dataBytes == set->dataBytes && tag.B == set->B && tag.T == set->T
	       && tag.L == set->L && tag.k == set->k && tag.m == set->m)
	    {  for(u=0; u<set->T-1 && g*(set->T-1)+u < set->Q; u++)
	       {  guint64 q = g*(set->T-1)+u;

		  st->payloadCrc[q] = crc[u];
		  SetBit(st->payloadKnown, q);
	       }
	    }
	    else
	    {  SetBit(st->tagBad, g);
	       st->tagsBad++;
	    }
	 }
	 else                /* payload */
	 {  guint64 q = a - a/set->T - 1;
	    int crc_error = present[i] && GetBit(st->payloadKnown, q)
	                    && Crc32(sector, set->B) != st->payloadCrc[q];

	    if(present[i] && !crc_error)
	    {  if(q < set->c)
		 RS04SetCrcSector(st, q, sector);
	       continue;
	    }

	    SetBit(st->payloadBad, q);
	    if(crc_error)
	    {  range_flush(&r);
	       g_string_append_printf(r.out, _("* CRC error, sector: %" PRId64 "%s\n"), base+a, r.suffix);
	    }
	    if(q < set->c)
	    {  if(crc_error) st->crcErrors++;
	       else          st->crcMissing++;
	    }
	    else
	    {  if(crc_error) st->eccErrors++;
	       else          st->eccMissing++;
	    }
	 }
      }
   }

   range_flush(&r);
   if(!report) g_string_free(r.out, TRUE);
   g_free(buf);
}

/***
 *** Read offset of CD-DA rips (rs04.tex, "Audio CDs and read offsets")
 ***
 * Returns TRUE and the offset in samples if it could be determined;
 * score and probes tell how many probe sectors matched.
 */

#define OFFSET_WINDOW 5880

int RS04DetectAudioOffset(RS04State *st, int *delta, int *score, int *probes)
{  RS04Set *set = st->set;
   LargeFile *file = st->io.image->file;
   gint64 size = file->size;
   guint64 *probe = g_malloc(64*sizeof(guint64));
   int *scores = NULL;
   unsigned char *buf = NULL;
   guint64 t, n = 0;
   int i, d, np = 0, best = 0, second = 0, best_d = 0, matched = 0;

   *delta = 0;
   *score = *probes = 0;

   /* Candidates: known Crc32, not digital silence, not near the edges */

   for(t=10; t+10<set->S; t++)
     if(GetBit(st->dataKnown, t) && st->dataCrc[t] != RS04_SILENCE_CRC)
       n++;

   for(t=10, i=0; t+10<set->S && np<64; t++)
     if(GetBit(st->dataKnown, t) && st->dataCrc[t] != RS04_SILENCE_CRC)
     {  if((guint64)i++ * MIN(64,n) / n == (guint64)np)
	  probe[np++] = t;
     }

   if(!np)
   {  g_free(probe);
      return FALSE;
   }
   *probes = np;

   /* Fast path: aligned rip */

   buf = g_malloc(2352 + 8*OFFSET_WINDOW);
   for(i=0; i<np; i++)
   {  gint64 pos = 2352*probe[i];

      if(pos + 2352 <= size && LargeSeek(file, pos) && RS04ReadAll(file, buf, 2352)
	 && Crc32(buf, 2352) == st->dataCrc[probe[i]])
	matched++;
   }

   if(matched == np)
   {  *score = np;
      g_free(buf);
      g_free(probe);
      return TRUE;
   }

   /* Search the window */

   scores = g_malloc0((2*OFFSET_WINDOW+1)*sizeof(int));
   for(i=0; i<np; i++)
   {  gint64 p0 = 2352*(gint64)probe[i] - 4*OFFSET_WINDOW;
      gint64 len = 2352 + 8*OFFSET_WINDOW;
      gint64 lo = MAX(p0, 0), hi = MIN(p0+len, size);

      memset(buf, 0, len);
      if(hi > lo && LargeSeek(file, lo))
	RS04ReadAll(file, buf+(lo-p0), hi-lo);

      for(d=-OFFSET_WINDOW; d<=OFFSET_WINDOW; d++)
      {  gint64 s0 = 2352*(gint64)probe[i] + 4*d;

	 if(s0 < lo || s0 + 2352 > hi)
	   continue;
	 if(Crc32(buf+(s0-p0), 2352) == st->dataCrc[probe[i]])
	   scores[d+OFFSET_WINDOW]++;
      }
   }

   for(d=-OFFSET_WINDOW; d<=OFFSET_WINDOW; d++)
   {  int s = scores[d+OFFSET_WINDOW];

      if(s > best)
      {  second = best;
	 best = s;
	 best_d = d;
      }
      else if(s > second)
	second = s;
   }

   g_free(scores);
   g_free(buf);
   g_free(probe);

   *score = best;
   if(best >= 2 && best >= 2*second)
   {  *delta = best_d;
      return TRUE;
   }

   return FALSE;
}

/***
 *** Verify one set
 ***/

static void print_properties(Image *image, RS04Set *set, int idx, int n)
{  char digest[33];
   int major = set->creatorVersion/10000;
   int minor = (set->creatorVersion%10000)/100;
   int micro = set->creatorVersion%100;
   char *unstable = "";

   PrintLog(_("\nError correction properties:\n"));

   if(set->target == ECC_FILE)
     PrintLog(_("- type             : Error correction file\n"));
   else
   {  PrintLog(_("- type             : Augmented image\n"));
      if(n > 1)
	PrintLog(_("- set              : %d of %d, image sectors %" PRId64 " - %" PRId64 "\n"),
		 idx+1, n, set->start, set->start + (gint64)(set->S + set->A) - 1);
      if(set->matches < 0)
	PrintLog(_("* set              : unconfirmed (no readable CRC and data sectors)\n"));
   }

   PrintLog(_("- method           : RS04, %u+%u layers of %" PRIu64 " sectors, %4.1f%% redundancy.\n"),
	    set->k, set->m, set->L, (100.0*set->m*set->L)/set->D);
   PrintLog(_("- sector size      : %d bytes\n"), set->B);

   if((set->flags & RS04_FLAG_DEVEL) && !Closure->regtestMode)
     unstable = " (unstable)";
   if(micro)
        PrintLog("%s-%d.%d.%d%s\n", _("- created by       : dvdisaster"), major, minor, micro, unstable);
   else PrintLog("%s-%d.%d%s\n", _("- created by       : dvdisaster"), major, minor, unstable);

   AsciiDigest(digest, set->dataMD5);
   PrintLog(_("- data md5sum      : %s\n"), digest);
}

/* Print sizes; returns FALSE if the data part is missing */

static int print_sizes(Image *image, RS04Set *set, int last)
{  PrintLog(_("\nData integrity:\n"));

   if(set->target == ECC_FILE)
   {  guint64 ecc_sectors = image->eccFile->size / set->B;
      guint64 img_bytes = image->file->size;

      if(ecc_sectors != set->A)
      {  char *msg;

	 if(ecc_sectors < set->A)
	      msg = g_strdup_printf(_("Ecc file is %" PRId64 " sectors shorter than expected."), set->A - ecc_sectors);
	 else msg = g_strdup_printf(_("Ecc file is %" PRId64 " sectors longer than expected."), ecc_sectors - set->A);
	 PrintLog(_("* Warning          : %s\n"), msg);
	 g_free(msg);
	 exitCode = EXIT_CODE_SIZE_MISMATCH;
      }

      if(img_bytes == set->dataBytes)
      {  PrintLog(_("- sectors          : %" PRId64 " in image; "), set->S);
	 PrintLog(_("%" PRId64 " in ecc file\n"), set->A);
      }
      else
      {  char *have = g_strdup_printf(_("%" PRId64 " bytes"), img_bytes);
	 char *want = g_strdup_printf(_("%" PRId64 " bytes"), set->dataBytes);

	 PrintLog(_("* sectors          : %s (%s expected); %" PRId64 " sectors in ecc file\n"),
		  have, want, set->A);
	 g_free(have);
	 g_free(want);
	 exitCode = EXIT_CODE_SIZE_MISMATCH;
      }
   }
   else
   {  guint64 sectors = image->file->size / set->B;
      guint64 end = set->start + set->S + set->A;

      if(sectors >= end && (sectors == end || !last))
	PrintLog(_("- medium sectors   : %" PRId64 " total / %" PRId64 " data\n"),
		 set->S + set->A, set->S);
      else
      {  char *have = g_strdup_printf("%" PRId64, sectors);
	 char *want = g_strdup_printf("%" PRId64, end);

	 PrintLog(_("* medium sectors   : %s (%s expected)\n"), have, want);
	 g_free(have);
	 g_free(want);
	 if(sectors < end)
	   exitCode = EXIT_CODE_SIZE_MISMATCH;
      }
   }

   return TRUE;
}

static void verify_set(Image *image, RS04Set *set, int idx, int n)
{  RS04State st;
   GString *zone_report = g_string_new(NULL);
   unsigned char *buf = g_malloc((size_t)CHUNK*set->B);
   guint8 present[CHUNK];
   guint32 *erasures = g_malloc0(set->L*sizeof(guint32));
   struct MD5Context md5;
   unsigned char sum[16];
   char digest[33];
   ranges r;
   guint64 t0, i;
   guint64 data_missing = 0, data_crc_errors = 0;
   guint64 total_missing, total_crc_errors;
   guint64 damaged = 0, damaged_cw = 0, lost = 0;
   guint32 worst = 0;
   gint64 base = set->target == ECC_FILE ? 0 : set->start;
   int last_percent = -1;

   print_properties(image, set, idx, n);

   if(!image->file)
     goto done;

   print_sizes(image, set, idx == n-1);

   if(Closure->quickVerify)
   {  PrintLog(_("* quick mode        : image NOT scanned\n"));
      goto done;
   }

   /*** Added zone */

   RS04InitState(&st, image, set);
   RS04ScanZone(&st, zone_report);

   /*** Read offset of audio rips */

   if(set->target == ECC_FILE && set->B == 2352)
   {  int delta, score, probes;

      if(Closure->audioOffsetGiven)
      {  delta = Closure->audioOffset;
	 PrintLog(_("- read offset      : %+d samples (given)\n"), delta);
      }
      else if(RS04DetectAudioOffset(&st, &delta, &score, &probes))
	PrintLog(_("- read offset      : %+d samples (%d of %d probe sectors match)\n"),
		 delta, score, probes);
      else
      {  if(probes)
	   PrintLog(_("* read offset      : unknown; use --audio-offset (best match: %d of %d probe sectors)\n"),
		    score, probes);
	 else PrintLog(_("- read offset      : not checked (no probe sectors)\n"));
	 delta = 0;
      }
      st.io.dataOffset = 4*delta;
   }

   /*** Data sectors */

   r.first = -1;
   r.suffix = " ";
   r.out = g_string_new(NULL);
   MD5Init(&md5);

   for(t0=0; t0<set->S; t0+=CHUNK)
   {  int k, cnt = MIN(CHUNK, set->S - t0);
      int percent;

      if(Closure->stopActions)
      {  g_string_free(r.out, TRUE);
	 RS04FreeState(&st);
	 goto done;
      }

      RS04ReadData(&st.io, t0, cnt, buf, present);
      if(t0+cnt == set->S)
	   MD5Update(&md5, buf, set->dataBytes - t0*set->B);
      else MD5Update(&md5, buf, (size_t)cnt*set->B);

      for(k=0; k<cnt; k++)
      {  guint64 t = t0+k;

	 if(!present[k])
	 {  range_add(&r, base+t);
	    data_missing++;
	    erasures[t % set->L]++;
	 }
	 else if(GetBit(st.dataKnown, t) && Crc32(buf+(size_t)k*set->B, set->B) != st.dataCrc[t])
	 {  range_flush(&r);
	    g_string_append_printf(r.out, _("* CRC error, sector: %" PRId64 "\n"), base+t);
	    data_crc_errors++;
	    erasures[t % set->L]++;
	 }
      }

      percent = (100*(t0+cnt))/set->S;
      if(percent != last_percent)
      {  PrintProgress(_("- testing sectors  : %3d%%"), percent);
	 last_percent = percent;
      }
   }
   range_flush(&r);
   ClearProgress();
   MD5Final(sum, &md5);
   AsciiDigest(digest, sum);

   PrintCLI("%s%s", r.out->str, zone_report->str);
   g_string_free(r.out, TRUE);

   /*** Erasures in the added zone */

   for(i=0; i<set->Q; i++)
     if(GetBit(st.payloadBad, i))
       erasures[(i < set->c ? set->S + i : i - set->c) % set->L]++;

   /*** Summary */

   total_missing = data_missing + st.crcMissing + st.eccMissing + st.tagsBad;
   total_crc_errors = data_crc_errors + st.crcErrors + st.eccErrors;
   if(total_missing) exitCode = EXIT_CODE_MISSING_SECTOR;
   else if(total_crc_errors) exitCode = EXIT_CODE_CHECKSUM_ERROR;

   if(!total_missing && !total_crc_errors)
   {  PrintLog(_("- good image/file  : all sectors present\n"
		 "- data md5sum      : %s\n"),digest);
      if(memcmp(sum, set->dataMD5, 16))
      {  PrintLog(_("* data md5sum      : BAD, does not match the ecc data\n"));
	 exitCode = EXIT_CODE_CHECKSUM_ERROR;
      }
   }
   else
   {  if(!total_crc_errors)
	PrintLog(_("* BAD image/file   : %" PRId64 " sectors missing\n"), total_missing);
      if(!total_missing)
	PrintLog(_("* suspicious image : all sectors present, but %" PRId64 " CRC errors\n"),
		 total_crc_errors);
      if(total_missing && total_crc_errors)
	PrintLog(_("* BAD image        : %" PRId64 " sectors missing, %" PRId64 " CRC errors\n"),
		 total_missing, total_crc_errors);

      PrintLog(_("  ... data section   : %" PRId64 " sectors missing; %" PRId64 " CRC errors\n"),
	       data_missing, data_crc_errors);
      PrintLog(_("  ... crc section    : %" PRId64 " sectors missing; %" PRId64 " CRC errors\n"),
	       st.crcMissing, st.crcErrors);
      PrintLog(_("  ... ecc section    : %" PRId64 " sectors missing; %" PRId64 " CRC errors\n"),
	       st.eccMissing, st.eccErrors);
      PrintLog(_("  ... tags           : %" PRId64 " of %" PRId64 " missing\n"),
	       st.tagsBad, st.nTags);

      /*** Prognosis */

      for(i=0; i<set->L; i++)
      {  if(erasures[i])
	 {  damaged += erasures[i];
	    damaged_cw++;
	 }
	 if(erasures[i] > set->m)
	   lost += erasures[i];
	 worst = MAX(worst, erasures[i]);
      }

      if(damaged_cw)
	PrintLog(_("- erasure counts   :  avg =  %.1f; worst = %d per ecc block.\n"),
		 (double)damaged/(double)damaged_cw, worst);

      {  guint64 expected = set->S + set->A;
	 guint64 recoverable = expected - lost;
	 int percentage = (int)((1000*recoverable)/expected);

	 PrintLog(_("- prognosis        : %" PRId64 " of %" PRId64 " sectors recoverable (%d.%d%%)\n"),
		  recoverable, expected, percentage/10, percentage%10);
      }
   }

   RS04FreeState(&st);

done:
   g_string_free(zone_report, TRUE);
   g_free(erasures);
   g_free(buf);
}

/***
 *** Verify all sets of the image or the ecc file
 ***/

typedef struct
{  Image *image;
   RS04Info *info;
} verify_closure;

static void cleanup(gpointer data)
{  verify_closure *vc = (verify_closure*)data;

   UnregisterCleanup();
   GuiAllowActions(TRUE);

   if(vc->image) CloseImage(vc->image);
   g_free(vc->info);
   g_free(vc);

   GuiExitWorkerThread();
}

void RS04Verify(Image *image)
{  verify_closure *vc = g_malloc0(sizeof(verify_closure));
   int i;

   RegisterCleanup(_("Check aborted"), cleanup, vc);
   vc->image = image;
   vc->info = RS04GetSets(image);
   if(!vc->info)
     Stop("Internal error: RS04Verify() called without suitable image and ecc file");

   if(image->file)
        PrintLog(_("\n%s present.\n"), Closure->imageName);
   else PrintLog(_("\n%s not present.\n"), Closure->imageName);

   if(vc->info->set[0].target == ECC_FILE)
     PrintLog(_("%s present.\n"), Closure->eccName);

   for(i=0; i<vc->info->nSets; i++)
     verify_set(image, &vc->info->set[i], i, vc->info->nSets);

   GuiSetLabelText(RS04_LABEL(verifyLabel), "%s", exitCode ? _("Damaged image.") : _("Good image."));

   cleanup((gpointer)vc);
}
