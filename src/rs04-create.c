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

/***
 *** Local data package used during encoding
 ***/

typedef struct _ecc_closure ecc_closure;

typedef struct
{  ecc_closure *ec;
   int tid;
   const void **orig;       /* k original shard pointers */
   void **work;             /* encoder work buffers */
   unsigned char *workMem;
   GThread *thread;
} worker;

struct _ecc_closure
{  Method *self;
   RS04Widgets *wl;
   Image *image;
   RS04Set set;
   RS04IO io;
   guint32 *payloadCrc;     /* Crc32 of the Q payload sectors */
   unsigned char *crcSectors; /* the c CRC sectors */
   unsigned char *data;     /* k layers x pw shards of the current pass */
   unsigned char *parity;   /* m layers x pw shards */
   unsigned char *sectors;  /* pw sectors for I/O */
   guint8 *present;
   guint64 W;               /* codewords per pass */
   guint64 i0, pw;          /* first codeword and width of current pass */
   guint64 progressDone, progressTotal;
   int lastPermille;
   int nWorkers;
   worker *workers;
   GTimer *timer;
   double lastElapsed;      /* GUI performance display */
   guint64 lastDone;
   int earlyTermination;
   int zoneWritten;         /* remove partial ecc data on abort */
};

static void ecc_cleanup(gpointer data)
{  ecc_closure *ec = (ecc_closure*)data;
   int i;

   UnregisterCleanup();

   if(ec->earlyTermination)
   {  PrintLog(_("Aborted by unrecoverable error.\n"));
      GuiSetLabelText(ec->wl->encFootline,
		      _("<span %s>Aborted by unrecoverable error.</span>"),
		      Closure->redMarkup);
   }

   GuiAllowActions(TRUE);

   /* The CRC cache does only cover the data portion */

   if(Closure->eccTarget == ECC_IMAGE && Closure->crcBuf)
   {  FreeCrcBuf(Closure->crcBuf);
      Closure->crcBuf = NULL;
   }

   if(ec->image) CloseImage(ec->image);
   if(ec->workers)
   {  for(i=0; i<ec->nWorkers; i++)
      {  g_free(ec->workers[i].orig);
	 g_free(ec->workers[i].work);
	 g_free(ec->workers[i].workMem);
      }
      g_free(ec->workers);
   }
   g_free(ec->payloadCrc);
   g_free(ec->crcSectors);
   g_free(ec->data);
   g_free(ec->parity);
   g_free(ec->sectors);
   g_free(ec->present);
   if(ec->timer) g_timer_destroy(ec->timer);
   g_free(ec);

   GuiExitWorkerThread();
}

static void abort_encoding(ecc_closure *ec)
{  RS04Set *set = &ec->set;

   if(ec->zoneWritten)
   {  if(Closure->eccTarget == ECC_FILE)
      {  LargeClose(ec->image->eccFile);
	 ec->image->eccFile = NULL;
	 g_unlink(Closure->eccName);
      }
      else if(!LargeTruncate(ec->image->file, (gint64)set->dataBytes))
	Stop(_("Could not truncate %s: %s\n"),Closure->imageName,strerror(errno));
      PrintLog(_("Aborted by user request! (partial ecc data removed from image)\n"));
   }
   else PrintLog(_("Aborted by user request!\n"));

   GuiSetLabelText(ec->wl->encFootline,
		   _("<span %s>Aborted by user request!</span>"),
		   Closure->redMarkup);

   ec->earlyTermination = FALSE;
   ecc_cleanup((gpointer)ec);
}

static void progress(ecc_closure *ec, guint64 done)
{  int permille = (int)((1000*done)/ec->progressTotal);

   if(permille == ec->lastPermille)
     return;
   ec->lastPermille = permille;

#ifdef WITH_GUI_YES
   if(Closure->guiMode)
   {  RS04Set *set = &ec->set;
      guint64 pass1 = set->S/8;
      double elapsed = g_timer_elapsed(ec->timer, NULL);

      if(done <= pass1)
      {  GuiSetProgress(ec->wl->encPBar1, (int)((1000*done)/MAX(pass1,1)), 1000);
	 return;
      }

      GuiSetProgress(ec->wl->encPBar2, (int)((1000*(done-pass1))/(ec->progressTotal-pass1)), 1000);

      /* done counts protected sectors; the checksum pass counts 1/8 */

      if(elapsed - ec->lastElapsed > 1.0)
      {  double mbs = ((double)(done - MAX(ec->lastDone, pass1))*set->B)
	              / (1048576.0*(elapsed - ec->lastElapsed));
	 int eta = elapsed * (1000 - permille) / MAX(permille, 1);
	 char *rate = g_strdup_printf(_("%5.2fMiB/s current"), mbs);

	 GuiSetLabelText(ec->wl->encPerformance, "%s (ETA %02d:%02d:%02d)",
			 rate, eta/3600, (eta/60)%60, eta%60);
	 g_free(rate);
	 ec->lastElapsed = elapsed;
	 ec->lastDone = done;
      }
      return;
   }
#endif

   PrintProgressETA(ec->timer, permille, _("Ecc generation: %3d.%1d%%"), permille/10, permille%10);
}

/***
 *** Remove ecc data of a previous run
 ***/

static void remove_old_ecc(ecc_closure *ec)
{  Image *image = ec->image;
   guint64 ignore;

   if(Closure->eccTarget == ECC_FILE)
   {  if(LargeStat(Closure->eccName, &ignore))
      {  if(GuiConfirmEccDeletion(Closure->eccName))
	    g_unlink(Closure->eccName);
	 else
	 {  PrintLog(_("Aborted to keep existing ecc file.\n"));
	    ec->earlyTermination = FALSE;
	    ecc_cleanup((gpointer)ec);
	    return;
	 }
      }
      return;
   }

   if(image->eccHeader)
   {  gint64 data_sectors = uchar_to_gint64(image->eccHeader->sectors);
      int in_last = image->eccHeader->inLast ? image->eccHeader->inLast : 2048;
      guint64 data_bytes = 2048*(data_sectors-1) + in_last;

      if(!ModalWarning(GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL, NULL,
		       _("Image \"%s\" already contains error correction information.\n"
			 "Truncating image to data part (%" PRId64 " sectors).\n"),
		       Closure->imageName, data_sectors))
      {  abort_encoding(ec);
	 return;
      }

      g_free(image->eccHeader);
      image->eccHeader = NULL;
      image->eccMethod = NULL;
      g_free(image->cachedLayout);
      image->cachedLayout = NULL;

      if(!LargeTruncate(image->file, data_bytes))
	Stop(_("Could not truncate %s: %s\n"),Closure->imageName,strerror(errno));
      image->file->size = data_bytes;
      CalcSectors(data_bytes, &image->sectorSize, &image->inLast);

      PrintLog(_("Image size is now"));
      if(image->inLast == 2048)
	   PrintLog(_(": %" PRId64 " medium sectors.\n"), image->sectorSize);
      else PrintLog(_(": %" PRId64 " medium sectors and %d bytes.\n"),
		    image->sectorSize-1, image->inLast);
   }
}

/***
 *** Determine the encoding parameters
 ***/

static double get_ratio(RS04Set *set)
{  char *r = Closure->redundancy;
   int len = r ? strlen(r) : 0;

   if(!len || !strcmp(r, "normal")) return 32.0/223.0;
   if(!strcmp(r, "high")) return 64.0/191.0;

   if(r[len-1] == '%')
   {  double p = atof(r);

      if(p<3.2 || p>200.0)
	Stop(_("Redundancy %4.1f%% out of useful range [3.2%%..200%%]"),p);
      return p/100.0;
   }

   if(r[len-1] == 'r' || g_ascii_isdigit(r[len-1]))  /* roots, as RS03 */
   {  int n = atoi(r);

      if(n < 8 || n > 170)
	Stop(_("Redundancy %d out of useful range [8..170]."),n);
      return (double)n/(255.0-n);
   }

   if(r[len-1] == 'm')   /* largest ecc data within the given MiB */
   {  guint64 S = (set->dataBytes + set->B - 1) / set->B;
      double mib = (double)S * set->B / 1048576.0;
      gint64 size = strtoll(r, NULL, 10);
      double limit = size * 1048576.0 / set->B;   /* in sectors */
      double ratio = limit / S;
      RS04Set tmp = *set;
      int i;

      if(ratio < 8.0/247.0 || ratio > 2.0)
	Stop(_("Ecc file size %" PRId64 "m out of useful range [%" PRId64 " .. %" PRId64 "]"),
	     size, (gint64)ceil(mib*8.0/247.0), (gint64)(mib*2.0));

      /* CRC sectors and tags come on top of the parity */

      for(i=0; i<10 && RS04ChooseParameters(&tmp, ratio, 0, RS04MemoryBudget()) && tmp.A > limit; i++)
	ratio *= (limit - 1) / tmp.A;
      return MIN(ratio, 2.0);
   }

   Stop(_("Invalid redundancy specification '%s'.\n"
	  "Valid formats: normal, high, <number>%%, <number>r, <number>m"), r);
   return 0.0;
}

static void choose_parameters(ecc_closure *ec)
{  RS04Set *set = &ec->set;
   guint64 ram = RS04MemoryBudget();

   if(Closure->eccTarget == ECC_FILE)
   {  if(!RS04ChooseParameters(set, get_ratio(set), 0, ram))
	Stop(_("The data is too small for RS04 with the requested redundancy."));
      return;
   }

   /* Augmented image: given medium size, explicit redundancy or smallest medium */

   if(Closure->mediumSize)
   {  if(!RS04ChooseParameters(set, 0.0, Closure->mediumSize, ram))
	Stop(_("Not enough space on medium left for error correction data.\n"
	       "Data portion of image: %" PRId64 " sect.; maximum possible size: %" PRId64 " sect.\n"
	       "If reducing the image size or using a larger medium is not\n"
	       "an option, please create a separate error correction file."),
	     set->S, Closure->mediumSize);
   }
   else if(Closure->redundancy && strlen(Closure->redundancy))
   {  if(!RS04ChooseParameters(set, get_ratio(set), 0, ram))
	Stop(_("The data is too small for RS04 with the requested redundancy."));
   }
   else
   {  int nodm = Closure->noBdrDefectManagement;
      guint64 sizes[] = { CDR_SIZE, DVD_SL_SIZE, DVD_DL_SIZE,
			  nodm ? BD_SL_SIZE_NODM : BD_SL_SIZE,
			  nodm ? BD_DL_SIZE_NODM : BD_DL_SIZE,
			  nodm ? BDXL_TL_SIZE_NODM : BDXL_TL_SIZE,
			  nodm ? BDXL_QL_SIZE_NODM : BDXL_QL_SIZE, 0 };
      int i;

      for(i=0; sizes[i]; i++)
	if(RS04ChooseParameters(set, 0.0, sizes[i], ram))
	  break;

      if(!sizes[i])
	Stop(_("Not enough space on medium left for error correction data.\n"
	       "Data portion of image: %" PRId64 " sect.; maximum possible size: %" PRId64 " sect.\n"
	       "If reducing the image size or using a larger medium is not\n"
	       "an option, please create a separate error correction file."),
	     set->S, (gint64)sizes[i-1]);
   }
}

/***
 *** Pass 1: data checksums, CRC sectors and the data md5sum
 ***/

static void checksum_data(ecc_closure *ec)
{  RS04Set *set = &ec->set;
   struct MD5Context md5;
   guint64 t;
   int chunk = 1024;
   unsigned char *buf = g_malloc((size_t)chunk*set->B);
   guint8 *present = g_malloc(chunk);

   ec->crcSectors = g_malloc0((size_t)set->c*set->B);
   MD5Init(&md5);

   for(t=0; t<set->S; t+=chunk)
   {  int i, n = MIN(chunk, set->S-t);

      if(Closure->stopActions)
      {  g_free(buf); g_free(present);
	 abort_encoding(ec);
	 return;
      }

      RS04ReadData(&ec->io, t, n, buf, present);
      if(t+n == set->S)
	   MD5Update(&md5, buf, set->dataBytes - t*set->B);
      else MD5Update(&md5, buf, n*set->B);

      for(i=0; i<n; i++)
      {  guint64 s = t+i;
	 guint32 crc = Crc32(buf+(size_t)i*set->B, set->B);
	 unsigned char *p = ec->crcSectors + (s/RS04_CRCS_PER_SECTOR)*set->B + 4*(s%RS04_CRCS_PER_SECTOR);

	 p[0] = crc; p[1] = crc>>8; p[2] = crc>>16; p[3] = crc>>24;
      }

      progress(ec, (t+n)/8);
   }

   MD5Final(set->dataMD5, &md5);
   g_free(buf);
   g_free(present);
}

/***
 *** Pass 2: Reed-Solomon encoding, W codewords at a time
 ***/

static gpointer encoder_thread(gpointer data)
{  worker *wk = (worker*)data;
   ecc_closure *ec = wk->ec;
   RS04Set *set = &ec->set;
   guint64 w;
   unsigned j;
   unsigned wc = RS04EncodeWorkCount(set->k, set->m);

   for(w=wk->tid; w<ec->pw; w+=ec->nWorkers)
   {  int err;

      if(Closure->stopActions)
	break;

      for(j=0; j<set->k; j++)
	wk->orig[j] = ec->data + (j*ec->pw + w)*set->Bs;

      err = RS04Encode(set->Bs, set->k, set->m, wc, wk->orig, wk->work);
      if(err != RS04_SUCCESS)
	Stop("Internal error: RS04Encode() returned %d", err);

      for(j=0; j<set->m; j++)
	memcpy(ec->parity + (j*ec->pw + w)*set->Bs, wk->work[j], set->Bs);

      if(!wk->tid)
	progress(ec, ec->progressDone + (w+1)*set->k);
   }

   return NULL;
}

/* Fill in the original sectors X_t of layer j for the current pass */

static void read_layer(ecc_closure *ec, guint64 j)
{  RS04Set *set = &ec->set;
   guint64 t0 = j*set->L + ec->i0;
   unsigned char *out = ec->data + j*ec->pw*set->Bs;
   guint64 w;

   if(t0 < set->S)
     RS04ReadData(&ec->io, t0, MIN(ec->pw, set->S-t0), ec->sectors, ec->present);

   for(w=0; w<ec->pw; w++)
   {  guint64 t = t0+w;

      if(t < set->S)
	RS04SectorToShard(set->B, ec->sectors+w*set->B, out+w*set->Bs);
      else if(t < set->D)
	RS04SectorToShard(set->B, ec->crcSectors+(t-set->S)*set->B, out+w*set->Bs);
      else memset(out+w*set->Bs, 0, set->Bs);
   }
}

static void write_parity(ecc_closure *ec)
{  RS04Set *set = &ec->set;
   guint64 j,w;

   for(j=0; j<set->m; j++)
   {  guint64 q = set->c + j*set->L + ec->i0;

      for(w=0; w<ec->pw; w++)
      {  unsigned char *sector = ec->sectors+w*set->B;

	 if(!RS04ShardToSector(set->B, ec->parity+(j*ec->pw+w)*set->Bs, sector))
	   Stop("Internal error: RS04 parity has non-zero padding");
	 ec->payloadCrc[q+w] = Crc32(sector, set->B);
      }
      RS04WritePayload(&ec->io, q, ec->pw, ec->sectors);
   }
}

static void encode(ecc_closure *ec)
{  RS04Set *set = &ec->set;
   guint64 per_codeword = (guint64)(set->k + set->m)*set->Bs;
   unsigned wc = RS04EncodeWorkCount(set->k, set->m);
   guint64 j;
   int i;

   RS04ChooseKernel();
   ec->W = MIN(set->L, MAX(1, RS04MemoryBudget()/per_codeword));
   ec->data    = g_malloc(ec->W*set->k*set->Bs);
   ec->parity  = g_malloc(ec->W*set->m*set->Bs);
   ec->sectors = g_malloc(ec->W*set->B);
   ec->present = g_malloc(ec->W);

   ec->nWorkers = MAX(1, MIN(Closure->codecThreads, (int)ec->W));
   ec->workers = g_malloc0(ec->nWorkers*sizeof(worker));
   for(i=0; i<ec->nWorkers; i++)
   {  worker *wk = &ec->workers[i];
      unsigned u;

      wk->ec = ec;
      wk->tid = i;
      wk->orig = g_malloc(set->k*sizeof(void*));
      wk->work = g_malloc(wc*sizeof(void*));
      wk->workMem = g_malloc((size_t)wc*set->Bs);
      for(u=0; u<wc; u++)
	wk->work[u] = wk->workMem + (size_t)u*set->Bs;
   }

   Verbose("RS04 encoding: %" PRIu64 " codewords per pass, %d threads\n", ec->W, ec->nWorkers);

   GuiShowWidget(ec->wl->encLabel2);
   GuiShowWidget(ec->wl->encPBar2);
   GuiShowWidget(ec->wl->encLabel3);
   GuiShowWidget(ec->wl->encInfo);
   GuiShowWidget(ec->wl->encLabel4);
   GuiShowWidget(ec->wl->encPerformance);
   GuiSetProgress(ec->wl->encPBar1, 100, 100);
   GuiSetLabelText(ec->wl->encInfo, _("%d threads, %" PRIu64 " codewords of %u+%u sectors per pass"),
		   ec->nWorkers, ec->W, set->k, set->m);
   GuiSetLabelText(ec->wl->encPerformance, "");

   for(ec->i0=0; ec->i0<set->L; ec->i0+=ec->W)
   {  ec->pw = MIN(ec->W, set->L-ec->i0);

      for(j=0; j<set->k; j++)
	read_layer(ec, j);

      for(i=1; i<ec->nWorkers; i++)
	ec->workers[i].thread = g_thread_new("rs04-encoder", encoder_thread, &ec->workers[i]);
      encoder_thread(&ec->workers[0]);
      for(i=1; i<ec->nWorkers; i++)
	g_thread_join(ec->workers[i].thread);

      if(Closure->stopActions)
      {  abort_encoding(ec);
	 return;
      }

      write_parity(ec);
      ec->progressDone += ec->pw*set->k;
   }
}

static void write_tags(ecc_closure *ec)
{  RS04Set *set = &ec->set;
   unsigned char *buf = g_malloc(set->B);
   guint64 g, ntags = (set->Q + set->T - 2)/(set->T - 1);

   for(g=0; g<ntags; g++)
   {  RS04BuildTag(set, g, ec->payloadCrc, buf);
      RS04WriteZone(&ec->io, g*set->T, 1, buf);
   }
   g_free(buf);
}

/***
 *** Create the error correction data
 ***/

void RS04Create(void)
{  Method *method = FindMethod("RS04");
   ecc_closure *ec = g_malloc0(sizeof(ecc_closure));
   RS04Set *set = &ec->set;
   Image *image;
   guint64 j, ecc_sectors;
   char *msg;

   ec->self = method;
   ec->wl = (RS04Widgets*)method->widgetList;
   ec->earlyTermination = TRUE;
   ec->lastPermille = -1;
   RegisterCleanup(_("Error correction data creation aborted"), ecc_cleanup, ec);

   if(Closure->eccTarget == ECC_IMAGE)
        GuiSetLabelText(ec->wl->encHeadline,
			_("<big>Augmenting the image with error correction data.</big>\n<i>%s</i>"),
			_("- checking image -"));
   else GuiSetLabelText(ec->wl->encHeadline,
			_("<big>Creating the error correction file.</big>\n<i>%s</i>"),
			_("- checking image -"));

   /*** Open the image file */

   PrintLog(_("\nOpening %s"), Closure->imageName);

   if(Closure->eccTarget == ECC_IMAGE)
        image = OpenImageFromFile(Closure->imageName, O_RDWR, IMG_PERMS);
   else image = OpenImageFromFile(Closure->imageName, O_RDONLY, IMG_PERMS);

   if(!image)
   {  PrintLog(": %s.\n", strerror(errno));
      Stop(_("Image file %s: %s."),Closure->imageName, strerror(errno));
   }
   ec->image = image;

   if(image->inLast == 2048)
        PrintLog(_(": %" PRId64 " medium sectors.\n"), image->sectorSize);
   else PrintLog(_(": %" PRId64 " medium sectors and %d bytes.\n"),
		 image->sectorSize-1, image->inLast);

   remove_old_ecc(ec);

   /*** Describe the data */

   set->B = Closure->sectorBytes;
   set->dataBytes = image->file->size;
   set->target = Closure->eccTarget;
   set->creatorVersion = Closure->version;
   set->flags = Closure->regtestMode ? 0 : Closure->releaseFlags & (RS04_FLAG_DEVEL | RS04_FLAG_RC);

   if(!set->dataBytes)
     Stop(_("Image file %s is empty."), Closure->imageName);
   if(set->target == ECC_IMAGE && set->dataBytes % set->B)
     Stop(_("The image size is not a multiple of %d bytes.\n"
	    "Augmented images need complete sectors; please create an error correction file instead."),
	  set->B);

   choose_parameters(ec);
   ecc_sectors = set->A;

   /*** Need to open ecc file too */

   if(Closure->eccTarget == ECC_FILE)
   {  if(!Closure->eccName || !strlen(Closure->eccName))
	Stop(_("No error correction file specified!\n"));

      image->eccFile = LargeOpen(Closure->eccName, O_RDWR | O_CREAT, IMG_PERMS);
      if(!image->eccFile)
	Stop(_("Can't open %s:\n%s"),Closure->eccName,strerror(errno));
   }

   RS04InitIO(&ec->io, image, set);

   /*** Announce what we are going to do */

   msg = g_strdup_printf(_("%" PRId64 " data sectors of %d bytes, %" PRId64 " ecc sectors "
			   "(%u+%u layers of %" PRId64 " sectors; %4.1f%% redundancy)."),
			 set->S, set->B, ecc_sectors, set->k, set->m, set->L,
			 (100.0*set->m*set->L)/set->D);
   if(Closure->eccTarget == ECC_IMAGE)
   {  PrintLog(_("Augmenting the image with Method RS04:\n%s\n"), msg);
      GuiSetLabelText(ec->wl->encHeadline,
		      _("<big>Augmenting the image with error correction data.</big>\n<i>%s</i>"), msg);
   }
   else
   {  PrintLog(_("Creating the error correction file with Method RS04:\n%s\n"), msg);
      GuiSetLabelText(ec->wl->encHeadline,
		      _("<big>Creating the error correction file.</big>\n<i>%s</i>"), msg);
   }
   g_free(msg);

   if(5*set->m*set->L < set->D)
   {  if(!ModalWarning(GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL, NULL,
			"%s", _("Using redundancies below 20% may not give\n"
				"the expected data loss protection.\n")))
      {  abort_encoding(ec);
	 return;
      }
   }

   /*** Pass 1: checksums */

   ec->timer = g_timer_new();
   ec->progressTotal = set->S/8 + set->L*set->k;   /* checksums are fast */
   ec->progressDone = set->S/8;
   checksum_data(ec);

   if(Closure->regtestMode)
     memcpy(set->setID, set->dataMD5, 8);
   else
   {  guint32 r1 = g_random_int(), r2 = g_random_int();
      memcpy(set->setID, &r1, 4);
      memcpy(set->setID+4, &r2, 4);
   }

   ec->zoneWritten = TRUE;
   ec->payloadCrc = g_malloc(set->Q*sizeof(guint32));
   for(j=0; j<set->c; j++)
     ec->payloadCrc[j] = Crc32(ec->crcSectors+j*set->B, set->B);
   RS04WritePayload(&ec->io, 0, set->c, ec->crcSectors);

   /*** Pass 2: parity */

   encode(ec);
   write_tags(ec);

   /*** Summarize */

   PrintProgress(_("Ecc generation: 100.0%%\n"));
   if(Closure->eccTarget == ECC_IMAGE)
   {  PrintLog(_("Image has been augmented with error correction data.\n"
		 "New image size is %" PRId64 " MiB (%" PRId64 " sectors).\n"),
	       ((set->S+set->A)*set->B) >> 20, set->S+set->A);
      GuiSetLabelText(ec->wl->encFootline, _("Image has been augmented with error correction data.\n"
		 "New image size is %" PRId64 " MiB (%" PRId64 " sectors).\n"),
	       ((set->S+set->A)*set->B) >> 20, set->S+set->A);
   }
   else
   {  PrintLog(_("Error correction file \"%s\" created.\n"
		 "Make sure to keep this file on a reliable medium.\n"),
	       Closure->eccName);
      GuiSetLabelText(ec->wl->encFootline, "%s",
		      _("The error correction file has been successfully created.\n"
			"Make sure to keep this file on a reliable medium."));
   }

   GuiSetProgress(ec->wl->encPBar2, 100, 100);
   {  double elapsed = g_timer_elapsed(ec->timer, NULL);
      double mbs = (set->dataBytes/1048576.0)/MAX(elapsed, 0.001);

      if(!Closure->regtestMode)
	PrintLog(_("Avg performance: %5.2fs (%5.2fMiB/s) total\n"), elapsed, mbs);
      GuiSetLabelText(ec->wl->encPerformance, _("%5.2fMiB/s average"), mbs);
   }

   ec->earlyTermination = FALSE;
   ecc_cleanup((gpointer)ec);
}
