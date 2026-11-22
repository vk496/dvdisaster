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

#include "dvdisaster.h"
#include "scsi-layer.h"

/***
 *** Checksums and CD-TEXT of CD-DA images for the cue sheet.
 ***
 * AccurateRip v1/v2 and CRC32 per track, the AccurateRip/FreeDB disc id
 * and the CUETools database (CTDB) TOC id and CRC are computed as
 * documented by AccurateRip and CUETools, so that a rip can be compared
 * with these databases using other tools. Nothing is looked up online.
 */

#define CHUNK 16    /* sectors read at once */

typedef struct
{  DeviceHandle *dh;
   LargeFile *file;          /* the image, or NULL for reading from the drive */
   int offset;               /* read offset correction in samples */
   unsigned char *buf;       /* CHUNK sectors as stored/read, from bufStart on */
   gint64 bufStart;
   int bad[CHUNK];           /* sector is missing or unreadable */
} source;

typedef struct
{  guint32 v1, v2, crc;
   int bad;
} track_sums;

/* Sector lba as read from the drive; silence outside of the image */

static unsigned char *raw_sector(source *src, gint64 lba, int *bad)
{  static unsigned char silence[CD_RAW_SECTOR_SIZE];
   int i, n;

   if(lba < 0 || lba >= src->dh->sectors)
      return silence;

   if(lba < src->bufStart || lba >= src->bufStart + CHUNK)
   {  n = MIN(CHUNK, src->dh->sectors - lba);
      src->bufStart = lba;
      memset(src->buf, 0, CHUNK*CD_RAW_SECTOR_SIZE);

      if(src->file)
      {  ssize_t got = 0;

	 if(LargeSeek(src->file, (gint64)CD_RAW_SECTOR_SIZE*lba))
	    got = LargeRead(src->file, src->buf, n*CD_RAW_SECTOR_SIZE);
	 for(i=0; i<n; i++)
	    src->bad[i] =    got < (i+1)*CD_RAW_SECTOR_SIZE
	                  || CheckForMissingSector(src->buf+i*CD_RAW_SECTOR_SIZE, lba+i, NULL, 0) != SECTOR_PRESENT;
      }
      else if(ReadSectors(src->dh, src->buf, lba, n))  /* retry one by one */
      {  for(i=0; i<n; i++)
	    if((src->bad[i] = ReadSectors(src->dh, src->buf+i*CD_RAW_SECTOR_SIZE, lba+i, 1)))
	       memset(src->buf+i*CD_RAW_SECTOR_SIZE, 0, CD_RAW_SECTOR_SIZE);
      }
      else memset(src->bad, 0, sizeof(src->bad));
   }

   *bad |= src->bad[lba - src->bufStart];
   return src->buf + (lba - src->bufStart)*CD_RAW_SECTOR_SIZE;
}

/* Sector lba after read offset correction (made from two sectors as read) */

static void get_sector(source *src, gint64 lba, unsigned char *out, int *bad)
{  gint64 pos = CD_RAW_SECTOR_SIZE*lba + 4*(gint64)src->offset;
   gint64 first = pos >= 0 ? pos/CD_RAW_SECTOR_SIZE : (pos-CD_RAW_SECTOR_SIZE+1)/CD_RAW_SECTOR_SIZE;
   int r = pos - first*CD_RAW_SECTOR_SIZE;

   memcpy(out, raw_sector(src, first, bad) + r, CD_RAW_SECTOR_SIZE - r);
   if(r) memcpy(out + CD_RAW_SECTOR_SIZE - r, raw_sector(src, first+1, bad), r);
}

/* AccurateRip skips 5 sectors at the start of the first and at the end of
   the last audio track. The CTDB CRC covers all audio sectors except
   for 10 sectors at the start and 10 sectors plus the remainder of the
   disc length modulo 10 sectors at the end. */

static void checksum_track(source *src, TrackInfo *t, int first, int last,
			   gint64 ctdb_from, gint64 ctdb_to, guint32 *ctdb, track_sums *ts)
{  unsigned char buf[CD_RAW_SECTOR_SIZE];
   guint32 n = (t->end - t->start)*588;
   guint32 from = first ? 5*588 : 1;
   guint32 to = last ? n - 5*588 : n;
   guint32 mult = 1;
   gint64 lba;
   int i;

   memset(ts, 0, sizeof(track_sums));

   for(lba=t->start; lba<t->end; lba++)
   {  gint64 s0 = lba*588;
      gint64 a = MAX(s0, ctdb_from), b = MIN(s0+588, ctdb_to);

      if(!src->file && Closure->stopActions)
	return;

      get_sector(src, lba, buf, &ts->bad);
      ts->crc = Crc32Update(ts->crc, buf, CD_RAW_SECTOR_SIZE);
      if(a < b)
	 *ctdb = Crc32Update(*ctdb, buf + 4*(a-s0), 4*(b-a));

      for(i=0; i<588; i++, mult++)
	if(mult >= from && mult <= to)
	{  guint32 sample = buf[4*i] | buf[4*i+1]<<8 | buf[4*i+2]<<16 | (guint32)buf[4*i+3]<<24;
	   guint64 p = (guint64)sample * mult;

	   ts->v1 += (guint32)p;
	   ts->v2 += (guint32)p + (guint32)(p>>32);
	}
   }
}

static void checksum_tracks(DeviceHandle *dh, LargeFile *file, int offset, guint32 *ctdb, track_sums *ts)
{  AlignedBuffer *ab = CreateAlignedBuffer(CHUNK*CD_RAW_SECTOR_SIZE);
   source src;
   int i, first = -1, last = -1;
   gint64 ctdb_from, ctdb_to;

   for(i=0; i<dh->nTracks; i++)
     if(dh->track[i].audio)
     {  if(first < 0) first = i;
	last = i;
     }

   ctdb_from = dh->track[first].start*588 + 10*588;
   ctdb_to   = dh->track[last].end*588 - 10*588
               - ((dh->track[last].end - dh->track[first].start)*588) % (10*588);
   *ctdb = 0;

   src.dh = dh;
   src.file = file;
   src.offset = offset;
   src.buf = ab->buf;
   src.bufStart = -CHUNK-1;

   for(i=first; i<=last; i++)
     if(dh->track[i].audio)
       checksum_track(&src, &dh->track[i], i == first, i == last, ctdb_from, ctdb_to, ctdb, &ts[i]);

   FreeAlignedBuffer(ab);
}

/* AccurateRip disc id (with the FreeDB id), CTDB TOC id */

static char *accuraterip_id(DeviceHandle *dh)
{  guint32 id1 = 0, id2 = 0, cddb = 0, leadout = dh->track[dh->nTracks-1].end;
   int i, audio = 0;

   for(i=0; i<dh->nTracks; i++)
   {  guint32 s = dh->track[i].start, m = s/75+2;

      if(dh->track[i].audio)
      {  audio++;
	 id1 += s;
	 id2 += MAX(s, 1) * audio;
      }
      for(; m; m/=10)
	cddb += m%10;
   }
   id1 += leadout;
   id2 += leadout * (audio+1);
   cddb = (cddb % 255) << 24 | (leadout/75 - dh->track[0].start/75) << 8 | dh->nTracks;

   return g_strdup_printf("%03d-%08x-%08x-%08x", audio, id1, id2, cddb);
}

static char *ctdb_tocid(DeviceHandle *dh)
{  GChecksum *sha1 = g_checksum_new(G_CHECKSUM_SHA1);
   guint8 digest[20];
   gsize len = sizeof(digest);
   gint64 first = -1, end = 0;
   int i, audio = 0;
   char *id, *c;

   for(i=0; i<dh->nTracks; i++)
     if(dh->track[i].audio)
     {  char hex[9];

	if(first < 0) first = dh->track[i].start;
	else
	{  g_snprintf(hex, 9, "%08X", (guint32)(dh->track[i].start - first));
	   g_checksum_update(sha1, (guchar*)hex, 8);
	}
	end = dh->track[i].end;
	audio++;
     }
   for(i=audio; i<=100; i++)
   {  char hex[9];

      g_snprintf(hex, 9, "%08X", i == audio ? (guint32)(end - first) : 0);
      g_checksum_update(sha1, (guchar*)hex, 8);
   }

   g_checksum_get_digest(sha1, digest, &len);
   g_checksum_free(sha1);
   id = g_base64_encode(digest, len);
   for(c=id; *c; c++)
     switch(*c)
     {  case '+': *c = '.'; break;
        case '/': *c = '_'; break;
        case '=': *c = '-'; break;
     }

   return id;
}

/***
 *** CD-TEXT (READ TOC/PMA/ATIP format 5).
 ***
 * Only block 0 is used and only if it is in ISO 8859-1 or ASCII;
 * the text is written as UTF-8. Packs with a CRC error are skipped.
 */

guint16 CDTextCrc(unsigned char *pack)  /* CRC-16 CCITT of bytes 0-15, inverted */
{  guint16 crc = 0;
   int i,j;

   for(i=0; i<16; i++)
   {  crc ^= pack[i]<<8;
      for(j=0; j<8; j++)
	crc = crc & 0x8000 ? (crc<<1) ^ 0x1021 : crc<<1;
   }

   return ~crc;
}

static char *cue_string(char *s)  /* ISO 8859-1 -> UTF-8 without '"' */
{  GString *u = g_string_new(NULL);

   for(; *s; s++)
   {  unsigned char c = *s == '"' ? '\'' : *s;

      if(c >= 0x20 && (c < 0x7f || c >= 0xa0))
	g_string_append_unichar(u, c);
   }

   return g_string_free(u, FALSE);
}

static void read_cdtext(DeviceHandle *dh, GString **cue)
{  static char *keyword[] = { "TITLE", "PERFORMER", "SONGWRITER",
			      "REM COMPOSER", "REM ARRANGER", "REM MESSAGE" };
   AlignedBuffer *ab = CreateAlignedBuffer(65536);
   unsigned char *buf = ab->buf;
   unsigned char cmd[MAX_CDB_SIZE];
   char *text[15][100];      /* pack type 0x80+n, track (0 = disc) */
   GString *cur = g_string_new(NULL);
   int alloc, length, i, j, bad = 0;

   memset(text, 0, sizeof(text));
   memset(cmd, 0, MAX_CDB_SIZE);
   cmd[0] = 0x43;     /* READ TOC/PMA/ATIP */
   cmd[2] = 5;        /* CD-TEXT */
   cmd[8] = MIN_TRANSFER_LEN;
   if(SendPacket(dh, cmd, 10, buf, MIN_TRANSFER_LEN, &dh->sense, DATA_READ) < 0)
   {  Verbose("# no CD-TEXT\n");
      goto done;
   }

   alloc = MIN((buf[0]<<8 | buf[1]) + 2, 65534);
   cmd[7] = alloc >> 8;
   cmd[8] = alloc & 0xff;
   if(alloc < 4+18 || SendPacket(dh, cmd, 10, buf, alloc, &dh->sense, DATA_READ) < 0)
   {  Verbose("# no CD-TEXT\n");
      goto done;
   }
   length = 4 + (MIN((buf[0]<<8 | buf[1]) + 2, alloc) - 4) / 18 * 18;

   /* The size information of block 0 gives the character code */

   for(i=4; i<length; i+=18)
   {  unsigned char *p = buf+i;

      if(p[0] == 0x8f && p[1] == 0 && !(p[3] & 0x70) && CDTextCrc(p) == (p[16]<<8 | p[17])
	 && p[4] != 0x00 && p[4] != 0x01)
      {  PrintLog(_("CD-TEXT with unsupported character code %d ignored.\n"), p[4]);
	 goto done;
      }
   }

   /* Strings are zero terminated and follow each other from the track
      given in the pack on. The character position tells how many characters
      of the string were in the preceding packs (at most 15). */

   for(j=0; j<15; j++)
   {  int track = 0, skip = TRUE;

      g_string_truncate(cur, 0);
      for(i=4; i<length; i+=18)
      {  unsigned char *p = buf+i;
	 int pos = p[3] & 0x0f, k;

	 if(p[0] != 0x80+j || (p[1] & 0x80) || (p[3] & 0xf0))  /* other type, extension, block or DBCC */
	   continue;
	 if(CDTextCrc(p) != (p[16]<<8 | p[17]))
	 {  bad++;
	    continue;
	 }

	 if(!pos || (int)MIN(cur->len, 15) != pos)  /* string start, or lost packs */
	 {  skip = pos != 0;
	    track = p[1];
	    g_string_truncate(cur, 0);
	 }

	 for(k=4; k<16; k++)
	 {  if(p[k])
	    {  if(!skip) g_string_append_c(cur, p[k]);
	       continue;
	    }
	    if(!skip && cur->len && track < 100 && !text[j][track])
	    {  if(!strcmp(cur->str, "\t") && track > 0 && text[j][track-1])  /* same as before */
		    text[j][track] = g_strdup(text[j][track-1]);
	       else text[j][track] = cue_string(cur->str);
	    }
	    g_string_truncate(cur, 0);
	    skip = FALSE;
	    track++;
	 }
      }
   }

   if(bad)
     PrintLog(_("CD-TEXT: %d packs with CRC errors ignored.\n"), bad);

   /* Text and UPC/ISRC codes for the cue sheet */

   for(i=0; i<=dh->nTracks; i++)
   {  int track = i ? dh->track[i-1].number : 0;
      char *code = text[14][track];

      for(j=0; j<6; j++)
	if(text[j][track] && *text[j][track])
	  g_string_append_printf(cue[i], "%s%s \"%s\"\n", i ? "    " : "", keyword[j], text[j][track]);
      if(code && !i && strlen(code) == 13)
	g_string_append_printf(cue[i], "CATALOG %s\n", code);
      if(code && i && strlen(code) == 12)
	g_string_append_printf(cue[i], "    ISRC %s\n", code);
   }

   for(i=0; i<15; i++)
     for(j=0; j<100; j++)
       g_free(text[i][j]);

done:
   g_string_free(cur, TRUE);
   FreeAlignedBuffer(ab);
}

/***
 *** Additional cue sheet lines for the disc (cue[0]) and the tracks
 *** (cue[1..nTracks]), with the checksums printed to the log.
 ***/

GString **CDDACueLines(DeviceHandle *dh, char *image_name)
{  GString **cue = g_malloc0((dh->nTracks+1) * sizeof(GString*));
   track_sums *copy, *test;
   LargeFile *file;
   guint32 ctdb;
   int offset = Closure->audioOffsetGiven ? Closure->audioOffset : 0;
   int i, audio = 0, differ = 0, incomplete = 0;
   char *id, *cddb;

   for(i=0; i<=dh->nTracks; i++)
   {  cue[i] = g_string_new(NULL);
      if(i < dh->nTracks && dh->track[i].audio)
	audio++;
   }
   if(!audio)
     return cue;

   if(!(file = LargeOpen(image_name, O_RDONLY, IMG_PERMS)))
   {  LogWarning(_("Could not open %s: %s\n"), image_name, strerror(errno));
      return cue;
   }

   copy = g_malloc0(dh->nTracks * sizeof(track_sums));
   test = g_malloc0(dh->nTracks * sizeof(track_sums));
   checksum_tracks(dh, file, offset, &ctdb, copy);
   LargeClose(file);

   /* AccurateRip checksums */

   if(Closure->audioOffsetGiven)
        PrintLog(_("\nAccurateRip checksums (read offset correction %+d samples):\n"), offset);
   else PrintLog(Closure->guiMode
		 ? _("\nAccurateRip checksums (no read offset correction; set it in the preferences):\n")
		 : _("\nAccurateRip checksums (no read offset correction; see --audio-offset):\n"));
   g_string_append_printf(cue[0], "REM READ OFFSET CORRECTION %+d\n", offset);

   for(i=0; i<dh->nTracks; i++)
   {  TrackInfo *t = &dh->track[i];

      if(!t->audio) continue;
      if(copy[i].bad)
      {  PrintLog(_("  Track %2d: unreadable sectors, no checksums\n"), t->number);
	 incomplete++;
	 continue;
      }
      PrintLog(_("  Track %2d: v1 %08x, v2 %08x, CRC32 %08x\n"),
	       t->number, copy[i].v1, copy[i].v2, copy[i].crc);
      g_string_append_printf(cue[i+1], "    REM ACCURATERIPV1 %08x\n    REM ACCURATERIPV2 %08x\n    REM CRC32 %08x\n",
			     copy[i].v1, copy[i].v2, copy[i].crc);
   }

   id = accuraterip_id(dh);
   cddb = g_ascii_strup(id+22, 8);
   PrintLog(_("AccurateRip disc id: %s\n"), id);
   g_string_append_printf(cue[0], "REM DISCID %s\nREM ACCURATERIPID %s\n", cddb, id);
   g_free(cddb);
   g_free(id);

   id = ctdb_tocid(dh);
   g_string_append_printf(cue[0], "REM CTDBTOCID %s\n", id);
   if(incomplete)
     PrintLog(_("CTDB TOC id: %s\n"), id);
   else
   {  PrintLog(_("CTDB TOC id: %s, CRC32 %08x\n"), id, ctdb);
      g_string_append_printf(cue[0], "REM CTDBCRC32 %08x\n", ctdb);
   }
   g_free(id);

   /* Test & Copy: read the audio tracks once more and compare */

   if(Closure->testAndCopy)
   {  PrintLog(_("\nTest & Copy: reading the audio tracks again.\n"));
      checksum_tracks(dh, NULL, offset, &ctdb, test);
      for(i=0; i<dh->nTracks && !Closure->stopActions; i++)
      {  TrackInfo *t = &dh->track[i];
	 int ok = !test[i].bad && !copy[i].bad && test[i].crc == copy[i].crc;

	 if(!t->audio) continue;
	 if(test[i].bad)
	      PrintLog(_("  Track %2d: unreadable sectors in the test pass\n"), t->number);
	 else PrintLog(_("  Track %2d: test CRC32 %08x, copy CRC32 %08x, %s\n"),
		       t->number, test[i].crc, copy[i].crc, ok ? _("ok") : _("DIFFERENT"));
	 if(ok)
	   g_string_append_printf(cue[i+1], "    REM TEST CRC32 %08x\n", test[i].crc);
	 else differ++;
      }

      if(Closure->stopActions)
	PrintLog(_("Test & Copy aborted.\n"));
      else if(differ)
      {  PrintLog(_("Test & Copy: %d tracks differ; the drive did not read them reliably.\n"), differ);
	 exitCode = EXIT_FAILURE;
      }
      else PrintLog(_("Test & Copy: all tracks match.\n"));
   }

   g_free(copy);
   g_free(test);

   read_cdtext(dh, cue);
   return cue;
}
