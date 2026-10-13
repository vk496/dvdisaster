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

/*** src type: some GUI code ***/

#include "dvdisaster.h"

#include "rs04-includes.h"
#include "rs04-ff16.h"

/* Sector states during a pass */

#define ST_OK      0
#define ST_MISSING 'd'   /* unreadable or dead sector marker */
#define ST_CRC     'c'   /* Crc32 mismatch */

/* Codeword results */

enum { CW_NOTHING, CW_REPAIRED, CW_TOO_MANY, CW_FAILED };

typedef struct _fix_closure fix_closure;

typedef struct
{  fix_closure *fc;
   int tid;
   const void **orig, **rec;
   void **dwork, **ework;
   unsigned char *mem;
   GThread *thread;
} worker;

struct _fix_closure
{  RS04Widgets *wl;
   Image *image;
   RS04Info *info;
   RS04Set *set;
   RS04State st;
   guint64 W, i0, pw;       /* codewords per pass; current pass */
   unsigned char *data;     /* k layers x pw shards */
   unsigned char *parity;   /* m layers x pw shards */
   guint8 *dstat, *pstat;   /* sector states */
   guint8 *result;          /* per codeword of the pass */
   guint8 *select;          /* columns to decode (NULL = all) */
   unsigned char *sectors;
   guint8 *present;
   int nWorkers;
   worker *workers;
   GTimer *timer;
   int lastPermille;
   guint64 dataCorr, crcCorr, eccCorr, tagCorr, uncorrected;
   Bitmap *unrepaired;      /* combined sets: image sectors counted in uncorrected */
   guint64 damaged, damagedCw;
   guint32 worst;
   guint64 Ldone, Ltotal;   /* codewords of the previous / all sets (GUI progress) */
};

#define GUI_PERMILLE(fc, col) ((int)((1000*((fc)->Ldone + (gint64)(col) + 1))/(fc)->Ltotal))

static void free_buffers(fix_closure *fc)
{  int i;

   if(fc->workers)
   {  for(i=0; i<fc->nWorkers; i++)
      {  g_free(fc->workers[i].orig);
	 g_free(fc->workers[i].rec);
	 g_free(fc->workers[i].dwork);
	 g_free(fc->workers[i].ework);
	 g_free(fc->workers[i].mem);
      }
      g_free(fc->workers);
      fc->workers = NULL;
   }
   g_free(fc->data);    fc->data = NULL;
   g_free(fc->parity);  fc->parity = NULL;
   g_free(fc->dstat);   fc->dstat = NULL;
   g_free(fc->pstat);   fc->pstat = NULL;
   g_free(fc->result);  fc->result = NULL;
   g_free(fc->select);  fc->select = NULL;
   g_free(fc->sectors); fc->sectors = NULL;
   g_free(fc->present); fc->present = NULL;
   RS04FreeState(&fc->st);
}

static void fix_cleanup(gpointer data)
{  fix_closure *fc = (fix_closure*)data;

   UnregisterCleanup();
   GuiAllowActions(TRUE);

   free_buffers(fc);
   if(fc->unrepaired) FreeBitmap(fc->unrepaired);
   if(fc->timer) g_timer_destroy(fc->timer);
   RS04FreeSets(fc->image, fc->info);
   if(fc->image) CloseImage(fc->image);
   g_free(fc);

   GuiExitWorkerThread();
}

/***
 *** Preparations: read offset of audio rips, image size
 ***/

/* Rewrite the file so that new byte x is old byte x+shift */

static void shift_file(LargeFile *file, gint64 shift, guint64 new_size)
{  gint64 size = file->size;
   gint64 chunk = 4<<20;
   gint64 nchunks = (new_size + chunk - 1)/chunk;
   unsigned char *buf = g_malloc(chunk);
   gint64 i;

   for(i=0; i<nchunks; i++)
   {  gint64 ci = shift > 0 ? i : nchunks-1-i;   /* never overwrite unread data */
      gint64 x = ci*chunk;
      gint64 len = MIN(chunk, (gint64)new_size - x);
      gint64 lo = MAX(x+shift, 0), hi = MIN(x+shift+len, size);

      memset(buf, 0, len);
      if(hi > lo)
      {  if(!LargeSeek(file, lo) || !RS04ReadAll(file, buf+(lo-x-shift), hi-lo))
	   Stop(_("Failed reading from %s: %s"), file->path, strerror(errno));
      }
      RS04WriteAt(file, x, buf, len);
   }

   if(!LargeTruncate(file, new_size))
     Stop(_("Could not truncate %s: %s\n"), file->path, strerror(errno));
   g_free(buf);
}

static void handle_audio_offset(fix_closure *fc)
{  RS04Set *set = fc->set;
   int delta, score, probes;

   if(Closure->audioOffsetGiven)
   {  delta = Closure->audioOffset;
      PrintLog(_("- read offset      : %+d samples (given)\n"), delta);
   }
   else if(RS04DetectAudioOffset(&fc->st, &delta, &score, &probes))
   {  if(!delta) return;
      PrintLog(_("- read offset      : %+d samples (%d of %d probe sectors match)\n"),
	       delta, score, probes);
   }
   else
   {  if(probes)
	Stop(Closure->guiMode
	     ? _("The read offset of the rip could not be determined\n"
		 "(best match: %d of %d probe sectors).\n"
		 "Please specify it in the RS04 preferences under \"Read offset of audio images\".")
	     : _("The read offset of the rip could not be determined\n"
		 "(best match: %d of %d probe sectors).\n"
		 "Please specify it with --audio-offset."), score, probes);
      return;
   }

   if(delta)
   {  shift_file(fc->image->file, 4*(gint64)delta, set->dataBytes);
      PrintLog(_("Image has been shifted by %+d samples.\n"), delta);
   }
}

/* Extend file to total bytes; new sectors of the B grid starting at base
   are marked as missing (only possible for 2048 byte sectors). */

static void expand_file(LargeFile *file, gint64 base, int B, guint64 total)
{  unsigned char *buf;
   gint64 pos = file->size;

   if(pos >= (gint64)total)
     return;

   if(B != 2048 || pos < base)
   {  if(!LargeTruncate(file, total))
	Stop(_("Failed expanding the image: %s\n"), strerror(errno));
      return;
   }

   buf = g_malloc(2048);

   if((pos - base) % 2048)   /* complete a partial sector with zeros */
   {  gint64 fill = 2048 - (pos - base) % 2048;

      memset(buf, 0, fill);
      RS04WriteAt(file, pos, buf, MIN(fill, (gint64)total - pos));
      pos += fill;
   }

   for(; pos < (gint64)total; pos += 2048)
   {  CreateMissingSector(buf, pos/2048, NULL, 0, "RS04 fix placeholder");
      RS04WriteAt(file, pos, buf, MIN(2048, (gint64)total - pos));
   }

   g_free(buf);
}

/* Returns FALSE if the user does not want superfluous bytes to be removed */

static int check_sizes(fix_closure *fc)
{  RS04Set *set = fc->set;
   Image *image = fc->image;

   if(set->target == ECC_FILE)
   {  gint64 diff = (gint64)image->file->size - (gint64)set->dataBytes;

      /* Supplementary sets may protect only a part of the image */

      if(diff > 0 && fc->info->nSets == 1)
      {  char *add_truncate = _("Add the --truncate option to the program call\n"
				"to have the superfluous sectors removed.");
	 char *longer = _("The image file is %lld sectors longer as noted in the\n"
			  "ecc data. This might simply be zero padding, but could\n"
			  "also mean that the image was manipulated after appending\n"
			  "the error correction information.\n\n%s");

	 if(Closure->guiMode && !Closure->truncate)
	 {  int answer;

	    if(diff < set->B)
	         answer = GuiModalDialog(GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, NULL,
					 _("The image file is %d bytes longer than noted\n"
					   "in the ecc file. Shall the superfluous bytes\n"
					   "be removed from the image file?\n"),
					 (int)diff);
	    else answer = GuiModalDialog(GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, NULL,
					 longer, (long long)(diff / set->B),
					 _("Is it okay to remove the superfluous sectors?"));
	    if(!answer)
	      return FALSE;
	 }
	 else if(!Closure->truncate)
	 {  if(diff < set->B)
	      Stop(_("The image file is %d bytes longer than noted\n"
		     "in the ecc file.\n"
		     "Add the --truncate option to the program call\n"
		     "to have the superfluous sectors removed."),
		   (int)diff);
	    else Stop(longer, (long long)(diff / set->B), add_truncate);
	 }
	 if(!LargeTruncate(image->file, set->dataBytes))
	   Stop(_("Could not truncate %s: %s\n"),Closure->imageName,strerror(errno));
	 PrintLog(_("Image has been truncated by %" PRId64 " bytes.\n"), diff);
      }

      expand_file(image->file, 0, set->B, set->dataBytes);
      expand_file(fc->st.io.zoneFile, 0, set->B, set->A*set->B);
   }
   else expand_file(image->file, set->start*set->B, set->B,
		    (set->start + set->S + set->A)*set->B);

   return TRUE;
}

/***
 *** Decoding, W codewords per pass
 ***/

static int payload_status(fix_closure *fc, guint64 q, unsigned char *sector, int present)
{  if(!present) return ST_MISSING;
   if(GetBit(fc->st.payloadKnown, q) && Crc32(sector, fc->set->B) != fc->st.payloadCrc[q])
     return ST_CRC;
   return ST_OK;
}

static void read_pass(fix_closure *fc)
{  RS04Set *set = fc->set;
   unsigned char *sec = fc->sectors;
   guint64 j, w;

   for(j=0; j<set->k; j++)
   {  guint64 t0 = j*set->L + fc->i0;
      unsigned char *out = fc->data + j*fc->pw*set->Bs;
      guint8 *stat = fc->dstat + j*fc->pw;

      memset(out, 0, fc->pw*set->Bs);
      memset(stat, ST_OK, fc->pw);

      if(t0 < set->S)    /* data sectors */
      {  guint64 n = MIN(fc->pw, set->S - t0);

	 RS04ReadData(&fc->st.io, t0, n, sec, fc->present);
	 for(w=0; w<n; w++)
	 {  guint64 t = t0+w;

	    RS04SectorToShard(set->B, sec+w*set->B, out+w*set->Bs);
	    if(!fc->present[w])
	      stat[w] = ST_MISSING;
	    else if(GetBit(fc->st.dataKnown, t) && Crc32(sec+w*set->B, set->B) != fc->st.dataCrc[t])
	      stat[w] = ST_CRC;
	 }
      }

      if(t0 + fc->pw > set->S && t0 < set->D)   /* CRC sectors */
      {  guint64 t1 = MAX(t0, set->S);
	 guint64 n = MIN(t0 + fc->pw, set->D) - t1;

	 RS04ReadPayload(&fc->st.io, t1 - set->S, n, sec, fc->present);
	 for(w=0; w<n; w++)
	 {  guint64 idx = t1 - t0 + w;

	    RS04SectorToShard(set->B, sec+w*set->B, out+idx*set->Bs);
	    stat[idx] = payload_status(fc, t1 - set->S + w, sec+w*set->B, fc->present[w]);
	 }
      }
   }

   for(j=0; j<set->m; j++)
   {  guint64 q0 = set->c + j*set->L + fc->i0;
      unsigned char *out = fc->parity + j*fc->pw*set->Bs;

      RS04ReadPayload(&fc->st.io, q0, fc->pw, sec, fc->present);
      for(w=0; w<fc->pw; w++)
      {  RS04SectorToShard(set->B, sec+w*set->B, out+w*set->Bs);
	 fc->pstat[j*fc->pw+w] = payload_status(fc, q0+w, sec+w*set->B, fc->present[w]);
      }
   }
}

static gpointer decoder_thread(gpointer data)
{  worker *wk = (worker*)data;
   fix_closure *fc = wk->fc;
   RS04Set *set = fc->set;
   unsigned dwc = RS04DecodeWorkCount(set->k, set->m);
   unsigned ewc = RS04EncodeWorkCount(set->k, set->m);
   guint64 w, j;

   for(w=wk->tid; w<fc->pw; w+=fc->nWorkers)
   {  unsigned ed = 0, ep = 0;
      int err;

      fc->result[w] = CW_NOTHING;
      if(Closure->stopActions || (fc->select && !fc->select[fc->i0+w]))
	continue;

      if(!wk->tid && !fc->select)
      {  int permille = (int)((1000*(fc->i0+w))/set->L);

	 if(permille != fc->lastPermille)
	 {  PrintProgressETA(fc->timer, permille, _("Ecc progress: %3d.%1d%%"), permille/10, permille%10);
	    RS04UpdateFixResults(GUI_PERMILLE(fc, fc->i0+w), fc->dataCorr+fc->crcCorr+fc->eccCorr, fc->uncorrected);
	    fc->lastPermille = permille;
	 }
      }

      for(j=0; j<set->k; j++) if(fc->dstat[j*fc->pw+w]) ed++;
      for(j=0; j<set->m; j++) if(fc->pstat[j*fc->pw+w]) ep++;

      if(!ed && !ep) continue;
      if(ed + ep > set->m)
      {  fc->result[w] = CW_TOO_MANY;
	 continue;
      }

      if(ed)
      {  for(j=0; j<set->k; j++)
	   wk->orig[j] = fc->dstat[j*fc->pw+w] ? NULL : fc->data + (j*fc->pw+w)*set->Bs;
	 for(j=0; j<set->m; j++)
	   wk->rec[j] = fc->pstat[j*fc->pw+w] ? NULL : fc->parity + (j*fc->pw+w)*set->Bs;

	 err = RS04Decode(set->Bs, set->k, set->m, dwc, wk->orig, wk->rec, wk->dwork);
	 if(err != RS04_SUCCESS)
	   Stop("Internal error: RS04Decode() returned %d", err);

	 for(j=0; j<set->k; j++)
	   if(fc->dstat[j*fc->pw+w])
	     memcpy(fc->data + (j*fc->pw+w)*set->Bs, wk->dwork[j], set->Bs);
      }

      if(ep)
      {  for(j=0; j<set->k; j++)
	   wk->orig[j] = fc->data + (j*fc->pw+w)*set->Bs;

	 err = RS04Encode(set->Bs, set->k, set->m, ewc, wk->orig, wk->ework);
	 if(err != RS04_SUCCESS)
	   Stop("Internal error: RS04Encode() returned %d", err);

	 for(j=0; j<set->m; j++)
	   if(fc->pstat[j*fc->pw+w])
	     memcpy(fc->parity + (j*fc->pw+w)*set->Bs, wk->ework[j], set->Bs);
      }

      fc->result[w] = CW_REPAIRED;
   }

   return NULL;
}

/* Check the decoded sectors of codeword w against their checksums */

static int check_codeword(fix_closure *fc, guint64 w)
{  RS04Set *set = fc->set;
   unsigned char *sector = fc->sectors;
   guint64 j, col = fc->i0 + w;

   for(j=0; j<set->k; j++)
   {  guint64 t = j*set->L + col;

      if(!fc->dstat[j*fc->pw+w]) continue;
      if(!RS04ShardToSector(set->B, fc->data + (j*fc->pw+w)*set->Bs, sector))
	return FALSE;
      if(t < set->S && GetBit(fc->st.dataKnown, t) && Crc32(sector, set->B) != fc->st.dataCrc[t])
	return FALSE;
      if(t >= set->S && payload_status(fc, t - set->S, sector, TRUE) != ST_OK)
	return FALSE;
   }

   for(j=0; j<set->m; j++)
   {  if(!fc->pstat[j*fc->pw+w]) continue;
      if(!RS04ShardToSector(set->B, fc->parity + (j*fc->pw+w)*set->Bs, sector))
	return FALSE;
      if(payload_status(fc, set->c + j*set->L + col, sector, TRUE) != ST_OK)
	return FALSE;
   }

   return TRUE;
}

/* Count the erased sectors of codeword w as unrepaired. Combined sets
   may share image sectors, which are counted only once. */

static void count_unrepaired(fix_closure *fc, guint64 w)
{  RS04Set *set = fc->set;
   guint64 j, col = fc->i0 + w;

   for(j=0; j<set->k+set->m; j++)
   {  guint64 t = j*set->L + col, q, bit;

      if(j < set->k ? !fc->dstat[j*fc->pw+w] : !fc->pstat[(j-set->k)*fc->pw+w])
	continue;

      if(t < set->S)
	bit = (set->target == ECC_FILE ? 0 : set->start) + t;
      else if(set->target == ECC_FILE)   /* ecc file sectors belong to one set */
      {  fc->uncorrected++;
	 continue;
      }
      else
      {  q = j < set->k ? t - set->S : set->c + (j-set->k)*set->L + col;
	 bit = set->start + set->S + RS04ZoneIndex(set, q);
      }

      if(fc->unrepaired)
      {  if(GetBit(fc->unrepaired, bit)) continue;
	 SetBit(fc->unrepaired, bit);
      }
      fc->uncorrected++;
   }
}

/* Write a repaired payload sector and update the checksums */

static void write_payload(fix_closure *fc, guint64 q, unsigned char *sector)
{  RS04State *st = &fc->st;

   RS04WritePayload(&st->io, q, 1, sector);
   ClearBit(st->payloadBad, q);
   if(!GetBit(st->payloadKnown, q))
   {  st->payloadCrc[q] = Crc32(sector, fc->set->B);
      SetBit(st->payloadKnown, q);
   }
   if(q < fc->set->c)
     RS04SetCrcSector(st, q, sector);
}

static void finish_pass(fix_closure *fc)
{  RS04Set *set = fc->set;
   unsigned char *sector = fc->sectors;
   gint64 dbase = set->target == ECC_FILE ? 0 : set->start;
   gint64 zbase = set->target == ECC_FILE ? 0 : set->start + set->S;
   guint64 w, j;

   for(w=0; w<fc->pw; w++)
   {  guint64 col = fc->i0 + w;
      unsigned count = 0;
      int sep = FALSE;

      if(fc->result[w] == CW_NOTHING)
	continue;

      if(fc->result[w] == CW_REPAIRED && !check_codeword(fc, w))
	fc->result[w] = CW_FAILED;

      /* Failures while recovering CRC sectors may be caused by data errors
	 which are not detectable yet; the main pass tries again. */

      if(fc->select && fc->result[w] != CW_REPAIRED)
	continue;

      for(j=0; j<set->k; j++) if(fc->dstat[j*fc->pw+w]) count++;
      for(j=0; j<set->m; j++) if(fc->pstat[j*fc->pw+w]) count++;

      fc->damaged += count;
      fc->damagedCw++;
      fc->worst = MAX(fc->worst, count);
      RS04AddFixValue(GUI_PERMILLE(fc, col-1)+1, GUI_PERMILLE(fc, col), count);

      if(fc->result[w] != CW_REPAIRED)
      {  if(fc->result[w] == CW_TOO_MANY)
	   PrintCLI(_("* ecc block %" PRId64 ": %d erasures, but only %d can be corrected\n"),
		    col, count, set->m);
	 else PrintCLI(_("* ecc block %" PRId64 ": repaired sectors do not match their checksums\n"), col);
	 count_unrepaired(fc, w);
	 continue;
      }

      /* Write back and report the repaired sectors */

      PrintCLI(_("  %3d repaired sectors: "), count);

      for(j=0; j<set->k; j++)
      {  guint64 t = j*set->L + col;
	 int type = fc->dstat[j*fc->pw+w];

	 if(!type) continue;
	 RS04ShardToSector(set->B, fc->data + (j*fc->pw+w)*set->Bs, sector);

	 if(t < set->S)
	 {  RS04WriteData(&fc->st.io, t, 1, sector);
	    PrintCLI("%" PRId64 "%c ", dbase + t, type);
	    fc->dataCorr++;
	 }
	 else
	 {  guint64 q = t - set->S;

	    if(set->target == ECC_FILE && !sep)
	    {  PrintCLI(_("; ecc file: "));
	       sep = TRUE;
	    }
	    write_payload(fc, q, sector);
	    PrintCLI("%" PRId64 "%c ", zbase + RS04ZoneIndex(set, q), type);
	    fc->crcCorr++;
	 }
      }

      for(j=0; j<set->m; j++)
      {  guint64 q = set->c + j*set->L + col;
	 int type = fc->pstat[j*fc->pw+w];

	 if(!type) continue;
	 RS04ShardToSector(set->B, fc->parity + (j*fc->pw+w)*set->Bs, sector);

	 if(set->target == ECC_FILE && !sep)
	 {  PrintCLI(_("; ecc file: "));
	    sep = TRUE;
	 }
	 write_payload(fc, q, sector);
	 PrintCLI("%" PRId64 "%c ", zbase + RS04ZoneIndex(set, q), type);
	 fc->eccCorr++;
      }

      PrintCLI("\n");
   }

   if(!fc->select)
     RS04UpdateFixResults(GUI_PERMILLE(fc, fc->i0+fc->pw-1),
			  fc->dataCorr+fc->crcCorr+fc->eccCorr, fc->uncorrected);
}

/* Decode all columns, or only those selected */

static int decode(fix_closure *fc)
{  RS04Set *set = fc->set;
   int i;

   RS04ChooseKernel();
   for(fc->i0=0; fc->i0<set->L; fc->i0+=fc->W)
   {  fc->pw = MIN(fc->W, set->L - fc->i0);

      if(fc->select)
      {  guint64 w;

	 for(w=0; w<fc->pw; w++)
	   if(fc->select[fc->i0+w]) break;
	 if(w == fc->pw) continue;
      }

      read_pass(fc);

      for(i=1; i<fc->nWorkers; i++)
	fc->workers[i].thread = g_thread_new("rs04-decoder", decoder_thread, &fc->workers[i]);
      decoder_thread(&fc->workers[0]);
      for(i=1; i<fc->nWorkers; i++)
	g_thread_join(fc->workers[i].thread);

      if(Closure->stopActions)
	return FALSE;

      finish_pass(fc);
   }

   return TRUE;
}

static void alloc_buffers(fix_closure *fc)
{  RS04Set *set = fc->set;
   guint64 per_codeword = (guint64)(set->k + set->m)*set->Bs;
   unsigned dwc = RS04DecodeWorkCount(set->k, set->m);
   unsigned ewc = RS04EncodeWorkCount(set->k, set->m);
   int i;

   fc->W = MIN(set->L, MAX(1, RS04MemoryBudget()/per_codeword));
   fc->data    = g_malloc(fc->W*set->k*set->Bs);
   fc->parity  = g_malloc(fc->W*set->m*set->Bs);
   fc->dstat   = g_malloc(fc->W*set->k);
   fc->pstat   = g_malloc(fc->W*set->m);
   fc->result  = g_malloc(fc->W);
   fc->sectors = g_malloc(fc->W*set->B);
   fc->present = g_malloc(fc->W);

   fc->nWorkers = MAX(1, MIN(Closure->codecThreads, (int)fc->W));
   fc->workers = g_malloc0(fc->nWorkers*sizeof(worker));
   for(i=0; i<fc->nWorkers; i++)
   {  worker *wk = &fc->workers[i];
      unsigned u;

      wk->fc = fc;
      wk->tid = i;
      wk->orig  = g_malloc(set->k*sizeof(void*));
      wk->rec   = g_malloc(set->m*sizeof(void*));
      wk->dwork = g_malloc(dwc*sizeof(void*));
      wk->ework = g_malloc(ewc*sizeof(void*));
      wk->mem   = g_malloc((size_t)(dwc+ewc)*set->Bs);
      for(u=0; u<dwc; u++) wk->dwork[u] = wk->mem + (size_t)u*set->Bs;
      for(u=0; u<ewc; u++) wk->ework[u] = wk->mem + (size_t)(dwc+u)*set->Bs;
   }

   Verbose("RS04 decoding: %" PRIu64 " codewords per pass, %d threads\n", fc->W, fc->nWorkers);
}

/* Rewrite damaged tags whose payload sectors are all good now */

static void repair_tags(fix_closure *fc)
{  RS04Set *set = fc->set;
   RS04State *st = &fc->st;
   unsigned char *buf = g_malloc((size_t)set->T*set->B);
   guint8 present[RS04_MAX_TAG_INTERVAL];
   guint64 g;

   for(g=0; g<st->nTags; g++)
   {  guint64 q0 = g*(set->T-1);
      int u, n = MIN(set->T-1, set->Q - q0), good = TRUE;

      if(!GetBit(st->tagBad, g))
	continue;

      for(u=0; u<n; u++)
	if(GetBit(st->payloadBad, q0+u))
	  good = FALSE;
      if(!good) continue;

      RS04ReadPayload(&st->io, q0, n, buf, present);
      for(u=0; u<n; u++)
      {  if(!present[u]) good = FALSE;
	 st->payloadCrc[q0+u] = Crc32(buf+(size_t)u*set->B, set->B);
      }
      if(!good) continue;

      RS04BuildTag(set, g, st->payloadCrc, buf);
      RS04WriteZone(&st->io, g*set->T, 1, buf);
      fc->tagCorr++;
   }

   g_free(buf);
}

static int data_md5_good(fix_closure *fc)
{  RS04Set *set = fc->set;
   unsigned char *buf = g_malloc((size_t)1024*set->B);
   guint8 present[1024];
   struct MD5Context md5;
   unsigned char sum[16];
   guint64 t;

   MD5Init(&md5);
   for(t=0; t<set->S; t+=1024)
   {  int n = MIN(1024, set->S - t);

      RS04ReadData(&fc->st.io, t, n, buf, present);
      MD5Update(&md5, buf, t+n == set->S ? set->dataBytes - t*set->B : (guint64)n*set->B);
   }
   MD5Final(sum, &md5);
   g_free(buf);

   return !memcmp(sum, set->dataMD5, 16);
}

/***
 *** Repair one set
 ***/

static int fix_set(fix_closure *fc, int idx)
{  RS04Set *set = fc->set;
   RS04State *st = &fc->st;
   guint64 j;

   if(fc->info->nSets > 1 && set->target == ECC_FILE)
     PrintLog(_("\nSet %d of %d (ecc file %s):\n"),
	      idx+1, fc->info->nSets, set->file ? set->file->path : Closure->eccName);
   else if(fc->info->nSets > 1)
     PrintLog(_("\nSet %d of %d (image sectors %" PRId64 " - %" PRId64 "):\n"),
	      idx+1, fc->info->nSets, set->start, set->start + (gint64)(set->S + set->A) - 1);

   if(Closure->guiMode)
   {  char *desc = g_strdup_printf(_("RS04, %u+%u layers of %" PRIu64 " sectors (%d bytes), %4.1f%% redundancy"),
				   set->k, set->m, set->L, set->B, (100.0*set->m*set->L)/set->D);

      if(fc->info->nSets > 1)
      {  char *where = g_strdup_printf(_("Set %d of %d (image sectors %" PRId64 " - %" PRId64 ")"),
				       idx+1, fc->info->nSets, set->start,
				       set->start + (gint64)(set->S + set->A) - 1);
	 char *tmp = g_strdup_printf("%s: %s", where, desc);

	 g_free(where);
	 g_free(desc);
	 desc = tmp;
      }
      GuiSetLabelText(fc->wl->fixHeadline, _("<big>Repairing the image.</big>\n<i>%s</i>"), desc);
      g_free(desc);
   }

   RS04InitState(st, fc->image, set);
   RS04ScanZone(st, NULL);

   if(set->target == ECC_FILE && set->B == 2352)
     handle_audio_offset(fc);

   if(!check_sizes(fc))
     return FALSE;
   alloc_buffers(fc);
   RS04SetFixMaxValues(set->m);
   fc->lastPermille = -1;

   /* Recover lost CRC sectors first (only their codewords) */

   if(st->crcMissing + st->crcErrors)
   {  fc->select = g_malloc0(set->L);
      for(j=0; j<set->c; j++)
	if(GetBit(st->payloadBad, j))
	  fc->select[(set->S + j) % set->L] = 1;

      if(!decode(fc)) return FALSE;
      g_free(fc->select);
      fc->select = NULL;
   }

   if(!decode(fc)) return FALSE;

   repair_tags(fc);
   free_buffers(fc);
   fc->Ldone += set->L;
   return TRUE;
}

/***
 *** Repair the image
 ***/

void RS04Fix(Image *image)
{  fix_closure *fc = g_malloc0(sizeof(fix_closure));
   guint64 corrected;
   char *t = NULL;
   int i, pass, combined;

   fc->wl = (RS04Widgets*)FindMethod("RS04")->widgetList;
   fc->image = image;
   RegisterCleanup(_("Repairing of image aborted"), fix_cleanup, fc);
   GuiSetLabelText(fc->wl->fixHeadline, _("<big>Repairing the image.</big>\n<i>%s</i>"),
		   _("Opening files..."));

   fc->info = RS04GetAllSets(image, O_RDWR);
   if(!fc->info)
     Stop("Internal error: RS04Fix() called without RS04 data");

   PrintLog(_("\nFix mode(%s): Repairable sectors will be fixed in the image.\n"),
	    fc->info->set[0].target == ECC_FILE ? "RS04f" : "RS04i");

   fc->timer = g_timer_new();
   for(i=0; i<fc->info->nSets; i++)
     fc->Ltotal += fc->info->set[i].L;

   /* Sets protecting the same sectors are applied in turn as long as
      one of them repairs sectors which the others could not. */

   combined = RS04Combined(fc->info);
   for(pass=1; ; pass++)
   {  guint64 before = fc->dataCorr + fc->crcCorr + fc->eccCorr;

      if(pass > 1)
	PrintLog(_("\nRepeating the repair with all sets (pass %d).\n"), pass);
      fc->uncorrected = 0;
      fc->Ldone = 0;
      if(combined)
      {  gint64 end = 0;

	 for(i=0; i<fc->info->nSets; i++)
	 {  RS04Set *set = &fc->info->set[i];

	    end = MAX(end, set->target == ECC_FILE ? (gint64)set->S : set->start + (gint64)(set->S + set->A));
	 }
	 if(fc->unrepaired) FreeBitmap(fc->unrepaired);
	 fc->unrepaired = CreateBitmap0(end);
      }

      for(i=0; i<fc->info->nSets; i++)
      {  fc->set = &fc->info->set[i];
	 if(!fix_set(fc, i))
	 {  PrintLog(_("Aborted by user request!\n"));
	    GuiSwitchAndSetFootline(fc->wl->fixNotebook, 1, fc->wl->fixFootline,
				    _("<span %s>Aborted by user request!</span>"), Closure->redMarkup);
	    fix_cleanup((gpointer)fc);
	    return;
	 }
      }

      if(!combined || !fc->uncorrected || fc->dataCorr + fc->crcCorr + fc->eccCorr == before)
	break;
   }

   /*** Print results */

   PrintProgress(_("Ecc progress: 100.0%%\n"));

   corrected = fc->dataCorr + fc->crcCorr + fc->eccCorr;
   RS04UpdateFixResults(1000, corrected, fc->uncorrected);
   if(corrected || fc->tagCorr)
     PrintLog(_("Repaired sectors: %" PRId64 " (%" PRId64 " data, %" PRId64 " crc, %" PRId64 " ecc, %" PRId64 " tags)\n"),
	      corrected + fc->tagCorr, fc->dataCorr, fc->crcCorr, fc->eccCorr, fc->tagCorr);

   if(fc->uncorrected)
   {  PrintLog(_("Unrepaired sectors: %" PRId64 "\n"), fc->uncorrected);
      GuiSwitchAndSetFootline(fc->wl->fixNotebook, 1, fc->wl->fixFootline,
			      _("Image sectors could not be fully restored "
				      "(%" PRId64 " repaired; <span %s>%" PRId64 " unrepaired</span>)"),
		      corrected, Closure->redMarkup, fc->uncorrected);
      SetExitCode(EXIT_CODE_DATA_LOSS);
   }
   else
   {  if(!corrected && !fc->tagCorr)
      {  t = _("Good! All sectors are already present.");
      }
      else
      {  t = _("Good! All sectors are repaired.");
	 SetExitCode(EXIT_CODE_DEFECTS);

	 for(i=0; i<fc->info->nSets; i++)
	 {  fc->set = &fc->info->set[i];
	    RS04InitIO(&fc->st.io, image, fc->set);
	    fc->st.set = fc->set;
	    if(!data_md5_good(fc))
	    {  t = _("* Warning: the md5sum of the repaired data does not match.");
	       SetExitCode(EXIT_CODE_DATA_LOSS);
	    }
	 }
      }
      PrintLog("%s\n", t);
      GuiSwitchAndSetFootline(fc->wl->fixNotebook, 1, fc->wl->fixFootline,
			      "%s %s", _("Repair results:"), t);
   }

   if(fc->damagedCw)
     PrintLog(_("Erasure counts per ecc block:  avg =  %.1f; worst = %d.\n"),
	      (double)fc->damaged/(double)fc->damagedCw, fc->worst);

   fix_cleanup((gpointer)fc);
}
