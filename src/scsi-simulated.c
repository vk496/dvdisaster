/*  dvdisaster: Additional error correction for optical media.
 *  Copyright (C) 2004-2017 Carsten Gnoerlich.
 *  Copyright (C) 2019-2021 The dvdisaster development team.
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

/***
 *** Add a menu entry for a simulated CD drive.
 ***/

void InitSimulatedCD()
{  
   if(!Closure->simulateCD)
      return;

   g_ptr_array_add(Closure->deviceNodes, g_strdup("sim-cd"));
   g_ptr_array_add(Closure->deviceNames, g_strdup_printf(_("Simulated CD (%s)"), Closure->simulateCD));
}

/***
 *** Simulate the SCSI device
 ***/

/*
 * While we're at it, check the CDB interface for proper usage.
 */

static void assert_cdb_length(unsigned char cdb, int cdb_size, int expected_size)
{
   if(cdb_size != expected_size)
      PrintLog("SendPacket(): Wrong size %d for opcode %0x (expected %d)\n",
	       cdb_size, cdb, expected_size);
}

static void assert_cdb_direction(unsigned char cdb, int expected, int given)
{
   if(expected != given)
      PrintLog("SendPacket(): Wrong data direction %d for opcode %0x (expected %d)\n",
	       given, cdb, expected);
}

static void write_sense(Sense *sense, int sense_key, int asc, int ascq)
{
  sense->sense_key=sense_key;
  sense->asc=asc;
  sense->ascq=ascq;
}

/***
 *** Simulated session/track layout (--sim-toc).
 ***
 * Syntax: tracks are separated by ',', sessions by '/'.
 * A track is "a" (audio), "d" (data, mode 1) or "x" (data, mode 2 XA)
 * followed by its first and last sector, e.g. "d0-999/d1500-2999" (two
 * sessions) or "a0-1199,a1200-2399" (audio CD). ":cdi" makes the disc
 * type CD-i instead of XA. An appended ":noc2" simulates
 * a drive without C2 error pointers, ":offset=n" a drive with a read
 * offset of n samples for audio sectors (it delivers sample i of the
 * disc as sample i+n of the rip), ":cdtext" a disc with CD-TEXT
 * (one pack of it has a CRC error).
 * Sector n is taken from the image at n*2048 bytes, or at n*2352 bytes
 * if the layout contains audio or XA tracks; data sectors are then stored
 * raw (user data at offset 16, or 24 for XA). Reading a Form 2 sector
 * as Form 1 fails. Sectors outside of the tracks are unreadable.
 * A dead sector marker with the simulation hint "c2 error" in an audio
 * sector simulates a sector with C2 errors (interpolated as silence),
 * the hint "unstable" a sector which reads differently each time.
 */

typedef struct
{  gint64 first, last;
   int audio, xa, session;
} sim_track;

static sim_track sim_toc[99];
static int sim_tracks = -1;
static int sim_sessions;
static int sim_sector_size;
static int sim_no_c2;
static int sim_offset;
static int sim_cdi;
static int sim_cdtext;

static void parse_sim_toc(void)
{  char *p = Closure->simulateTOC;

   if(sim_tracks >= 0)
      return;

   sim_tracks = 0;
   sim_sessions = 1;
   sim_sector_size = 2048;

   while(*p)
   {  sim_track *t = &sim_toc[sim_tracks];

      if(*p == '/') { sim_sessions++; p++; continue; }
      if(*p == ',') { p++; continue; }
      if(!strncmp(p, ":noc2", 5)) { sim_no_c2 = TRUE; p += 5; continue; }
      if(!strncmp(p, ":cdi", 4)) { sim_cdi = TRUE; p += 4; continue; }
      if(!strncmp(p, ":cdtext", 7)) { sim_cdtext = TRUE; p += 7; continue; }
      if(!strncmp(p, ":offset=", 8)) { sim_offset = g_ascii_strtoll(p+8, &p, 10); continue; }

      if((*p != 'a' && *p != 'd' && *p != 'x') || sim_tracks >= 99)
	Stop("--sim-toc: syntax error at \"%s\"", p);

      t->audio = (*p == 'a');
      t->xa = (*p++ == 'x');
      t->session = sim_sessions;
      t->first = g_ascii_strtoll(p, &p, 10);
      if(*p++ != '-')
	Stop("--sim-toc: syntax error at \"%s\"", p-1);
      t->last = g_ascii_strtoll(p, &p, 10);
      if(t->last < t->first || (sim_tracks && t->first <= sim_toc[sim_tracks-1].last))
	Stop("--sim-toc: invalid track %d", sim_tracks+1);
      if(t->audio || t->xa)
	sim_sector_size = 2352;
      sim_tracks++;
   }

   if(!sim_tracks)
      Stop("--sim-toc: no tracks given");
}

static sim_track* find_sim_track(gint64 lba)
{  int i;

   for(i=0; i<sim_tracks; i++)
     if(lba >= sim_toc[i].first && lba <= sim_toc[i].last)
       return &sim_toc[i];

   return NULL;
}

static void lba_to_msf(gint64 lba, unsigned char *msf)
{  lba += 150;
   msf[0] = lba / 4500;
   msf[1] = (lba / 75) % 60;
   msf[2] = lba % 75;
}

/* READ TOC formats 0 (TOC) and 2 (full TOC) */

static int simulate_read_toc(unsigned char *cdb, unsigned char *out_buf, int size)
{  unsigned char buf[2048];
   int len = 4;
   int i;

   memset(buf, 0, sizeof(buf));

   if((cdb[2] & 0xf) == 0)
   {  for(i=0; i<=sim_tracks; i++, len+=8)
      {  gint64 lba = i<sim_tracks ? sim_toc[i].first : sim_toc[sim_tracks-1].last+1;

	 buf[len+1] = 0x10 | (i<sim_tracks && !sim_toc[i].audio ? 4 : 0);
	 buf[len+2] = i<sim_tracks ? i+1 : 0xaa;
	 buf[len+4] = (lba>>24) & 0xff;
	 buf[len+5] = (lba>>16) & 0xff;
	 buf[len+6] = (lba>>8) & 0xff;
	 buf[len+7] = lba & 0xff;
      }
      buf[2] = 1;
      buf[3] = sim_tracks;
   }
   else /* full TOC: per session A0, A1, A2 and the track entries */
   {  int session;

      for(session=1; session<=sim_sessions; session++)
      {  int first = -1, last = -1, xa = FALSE;

	 for(i=0; i<sim_tracks; i++)
	   if(sim_toc[i].session == session)
	   {  if(first < 0) first = i;
	      last = i;
	      xa |= sim_toc[i].xa;
	   }

	 buf[len+0] = buf[len+11] = buf[len+22] = session;
	 buf[len+1] = buf[len+12] = buf[len+23] = 0x10 | (sim_toc[first].audio ? 0 : 4);
	 buf[len+3] = 0xa0; buf[len+8] = first+1;
	 buf[len+9] = xa ? (sim_cdi ? 0x10 : 0x20) : 0;   /* disc type */
	 buf[len+14] = 0xa1; buf[len+19] = last+1;
	 buf[len+25] = 0xa2; lba_to_msf(sim_toc[last].last+1, buf+len+30);
	 len += 33;

	 for(i=first; i<=last; i++, len+=11)
	 {  buf[len+0] = session;
	    buf[len+1] = 0x10 | (sim_toc[i].audio ? 0 : 4);
	    buf[len+3] = i+1;
	    lba_to_msf(sim_toc[i].first, buf+len+8);
	 }
      }
      buf[2] = 1;
      buf[3] = sim_sessions;
   }

   buf[0] = ((len-2)>>8) & 0xff;
   buf[1] = (len-2) & 0xff;
   memcpy(out_buf, buf, size < len ? size : len);
   return 0;
}

/* READ TOC format 5 (CD-TEXT): disc and track titles, performers given
   for track 1 and as "same as before" (TAB) for the others, songwriters
   in a pack with a CRC error, and the size information (ISO 8859-1). */

static int simulate_cdtext(unsigned char *out_buf, int size)
{  static unsigned char buf[4+18*1024];
   unsigned char *p = buf+4;
   int type, seq = 0, len;
   guint16 crc;

   for(type=0x80; type<=0x82; type++)
   {  char text[2048];
      int trk[2048], pos[2048];
      int n = 0, t, k;

      for(t=0; t<=sim_tracks; t++)
      {  char *s;

	 switch(type)
	 {  case 0x80: s = t ? g_strdup_printf("Track %d", t) : g_strdup("Caf\xe9 \"Live\""); break;
	    case 0x81: s = g_strdup(t > 1 ? "\t" : "Simulated Artist"); break;
	    default:   s = g_strdup("Simulated Writer"); break;
	 }
	 for(k=0; k<=(int)strlen(s) && n<2048; k++, n++)
	 {  text[n] = s[k];
	    trk[n] = t;
	    pos[n] = k;
	 }
	 g_free(s);
      }

      for(k=0; k<n; k+=12, p+=18)
      {  p[0] = type;
	 p[1] = trk[k];
	 p[2] = seq++;
	 p[3] = MIN(pos[k], 15);
	 memset(p+4, 0, 12);
	 memcpy(p+4, text+k, MIN(12, n-k));
	 crc = CDTextCrc(p);
	 p[16] = crc >> 8;
	 p[17] = (crc & 0xff) ^ (type == 0x82 && !k);  /* CRC error */
      }
   }

   memset(p, 0, 18);  /* size information, character code 0 */
   p[0] = 0x8f;
   p[2] = seq;
   crc = CDTextCrc(p);
   p[16] = crc >> 8;
   p[17] = crc & 0xff;
   p += 18;

   len = p - buf;
   buf[0] = ((len-2)>>8) & 0xff;
   buf[1] = (len-2) & 0xff;
   memcpy(out_buf, buf, size < len ? size : len);
   return 0;
}

/* Audio sector lba as delivered by a drive with a read offset:
   made from parts of two recorded sectors (silence outside of the image).
   Returns -1 if one of them is unreadable, TRUE if one has C2 errors. */

static int read_shifted(DeviceHandle *dh, gint64 lba, unsigned char *out)
{  gint64 pos = 2352*lba - 4*(gint64)sim_offset;
   gint64 first = pos >= 0 ? pos/2352 : (pos-2351)/2352;
   unsigned char rec[2*2352];
   int i, c2 = FALSE;

   memset(rec, 0, sizeof(rec));
   for(i=0; i<2 && (!i || pos > 2352*first); i++)
   {  unsigned char *r = rec+2352*i;

      if(   first+i < 0
	 || !LargeSeek(dh->simImage, 2352*(first+i))
	 || LargeRead(dh->simImage, r, 2352) != 2352)
      {  memset(r, 0, 2352);
	 continue;
      }

      if(CheckForMissingSector(r, first+i, NULL, 0) != SECTOR_PRESENT)
      {  char *sim_hint = GetSimulationHint(r);
	 int is_c2 = sim_hint && !strcmp(sim_hint, "c2 error");

	 g_free(sim_hint);
	 if(!is_c2) return -1;
	 c2 = TRUE;
      }
   }

   if(c2) memset(out, 0, 2352);   /* drive interpolates silence */
   else   memcpy(out, rec + (pos - 2352*first), 2352);

   return c2;
}

/* READ CD for the simulated track layout */

static int simulate_read_cd(DeviceHandle *dh, unsigned char *cdb, unsigned char *out_buf, Sense *sense)
{  gint64 lba = cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5];
   int n = cdb[6] << 16 | cdb[7] << 8 | cdb[8];
   int type = (cdb[1] >> 2) & 7;   /* 0 any, 1 CD-DA, 2 mode 1, 4 mode 2 form 1 */
   int c2 = cdb[9] & 0x06;
   int user_only = (cdb[9] & 0xf8) == 0x10;
   unsigned char rec[2352];
   int i;

   if(c2 && sim_no_c2)
   {  write_sense(sense, 5, 0x24, 0x00);  /* invalid field in CDB */
      return -1;
   }

   for(i=0; i<n; i++, lba++)
   {  sim_track *t = find_sim_track(lba);
      int c2_error = FALSE;
      int bytes;

      if(!t)
      {  write_sense(sense, 5, 0x21, 0x00);  /* Illegal LBA */
	 return -1;
      }
      if((type == 1 && !t->audio) || (type == 2 && (t->audio || t->xa)) || (type == 4 && !t->xa))
      {  write_sense(sense, 5, 0x64, 0x00);  /* Illegal mode for this track */
	 return -1;
      }
      if(!t->audio && !user_only && sim_sector_size == 2048)
      {  write_sense(sense, 5, 0xff, 0xe0);  /* raw data sectors only in 2352 layout */
	 return -1;
      }

      memset(rec, 0, sizeof(rec));
      if(t->audio && sim_offset)
      {  c2_error = read_shifted(dh, lba, rec);
	 if(c2_error < 0)
	 {  write_sense(sense, 3, 0x11, 0x00);  /* unrecovered read error */
	    return -1;
	 }
      }
      else if(   !LargeSeek(dh->simImage, (gint64)sim_sector_size*lba)
	 || LargeRead(dh->simImage, rec, sim_sector_size) != sim_sector_size)
      {  write_sense(sense, 3, 0x11, 0x00);  /* unrecovered read error */
	 return -1;
      }

      /* Dead sector markers are unreadable sectors */

      if(!(t->audio && sim_offset) && CheckForMissingSector(rec, lba, NULL, 0) != SECTOR_PRESENT)
      {  char *sim_hint = GetSimulationHint(rec);

	 if(t->audio && sim_hint && !strcmp(sim_hint, "c2 error"))
	 {  memset(rec, 0, sizeof(rec));   /* drive interpolates silence */
	    c2_error = TRUE;
	    g_free(sim_hint);
	 }
	 else if(t->audio && sim_hint && !strcmp(sim_hint, "unstable"))
	 {  static int reads;

	    memset(rec, ++reads, sizeof(rec));
	    g_free(sim_hint);
	 }
	 else
	 {  g_free(sim_hint);
	    write_sense(sense, 3, 0x11, 0x00);  /* unrecovered read error */
	    return -1;
	 }
      }

      if(type == 4 && (rec[18] & 0x20))
      {  write_sense(sense, 5, 0x64, 0x00);  /* Form 2 sector: illegal mode for this track */
	 return -1;
      }

      if(!t->audio && user_only)
      {  bytes = 2048;
	 memcpy(out_buf, sim_sector_size == 2048 ? rec : rec + (t->xa ? 24 : 16), 2048);
      }
      else
      {  bytes = 2352;
	 memcpy(out_buf, rec, 2352);
      }
      out_buf += bytes;

      if(c2)
      {  memset(out_buf, c2_error ? 0xff : 0, 294);
	 out_buf += 294;
      }
   }

   return 0;
}

int SimulateSendPacket(DeviceHandle *dh, unsigned char *cdb, int cdb_size, unsigned char *out_buf, int size, Sense *sense, int direction)
{  unsigned char buf[2048];
   int real_size;
   int alloc_len;
   char *nodelay;

   switch(cdb[0])
   {  

      case 0x00: assert_cdb_length(cdb[0], cdb_size, 6);   /* TEST UNIT READY */
                 assert_cdb_direction(cdb[0], DATA_NONE, direction);

		 return 0;
	 break;

      case 0x12: assert_cdb_length(cdb[0], cdb_size, 6);   /* INQUIRY */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);

		 real_size = (size < 36 ? size : 36);
		 
		 buf[0] = 0x05;          /* PERIPHERAL DEVICE TYPE */
		 buf[4] = real_size-4;   /* ADDITIONAL LENGTH */

		 memcpy(&buf[8], "Simulate", 8);           /* VENDOR ID */
		 memcpy(&buf[16],"d CD drive       ", 16); /* PRODUCT ID */
		 memcpy(&buf[32],"1.00", 4);               /* VERSION */

		 memcpy(out_buf, buf, real_size);
		 return real_size;
         break;

      case 0x1b: assert_cdb_length(cdb[0], cdb_size, 6);   /* START STOP */
                 assert_cdb_direction(cdb[0], DATA_NONE, direction);
		 return 0;
	 break;

      case 0x1e: assert_cdb_length(cdb[0], cdb_size, 6);   /* PREVENT ALLOW MEDIUM REMOVAL */
                 assert_cdb_direction(cdb[0], DATA_NONE, direction);
		 return 0;
	 break;

#if 0
      case 0x23: assert_cdb_length(cdb[0], cdb_size, 10);  /* READ FORMAT CAPACITIES */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);
	 break;
#endif

      case 0x25: assert_cdb_length(cdb[0], cdb_size, 10);  /* READ CAPACITY */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);
	      {	 guint64 sectors;	 

		 if(Closure->simulateTOC)
		 {  parse_sim_toc();
		    sectors = sim_toc[sim_tracks-1].last;
		 }
		 else sectors = dh->simImage->size/2048 - 1;
		 memset(buf, 0, 16);
		 buf[0] = (sectors >> 24) & 0xff;		 
		 buf[1] = (sectors >> 16) & 0xff;
		 buf[2] = (sectors >> 8 ) & 0xff;
		 buf[3] = sectors & 0xff;

		 buf[6] = (2048>>8)&0xff;
		 memcpy(out_buf, buf, size);
		 return 0;
	      }
	 break;

#if 0
      case 0x28: assert_cdb_length(cdb[0], cdb_size, 10);  /* READ(10) */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);
	 break;
#endif

      case 0x43: assert_cdb_length(cdb[0], cdb_size, 10);  /* READ TOC/PMA/ATIP */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);
	   
		 alloc_len = cdb[7]<<8 | cdb[8];
		 if(Closure->simulateTOC && ((cdb[2] & 0xf) == 0 || (cdb[2] & 0xf) == 2))
		 {  parse_sim_toc();
		    return simulate_read_toc(cdb, out_buf, alloc_len);
		 }
		 if(Closure->simulateTOC && (cdb[2] & 0xf) == 5)
		 {  parse_sim_toc();
		    if(sim_cdtext)
		       return simulate_cdtext(out_buf, alloc_len);
		    write_sense(sense, 5, 0x24, 0x00);  /* invalid field in CDB */
		    return -1;
		 }
		 switch(cdb[2] & 0xf)  /* format field */
		 {  case 0x00:         /* formatted TOC */
		       memset(out_buf, 0, 12);
		       out_buf[0] = 0;
		       out_buf[1] = 10;    /* data length */
		       out_buf[5] = 0x14;  /* CONTROL (data CD) */
		       return 0;

		    case 0x02:         /* full/raw TOC */
		       memset(out_buf, 0, 26);
		       out_buf[0] = 0;
		       out_buf[1] = 24;    /* data length for two track desciptors*/
		       out_buf[3] = 1;     /* 1 session (= last session) */ 
		       out_buf[7] = 0xa0;  /* POINT: first track number */
		       out_buf[13] = 0;    /* disc type: mode 1 */
		       return 0;

		    case 0x04:         /* full ATIP */
		       memset(out_buf, 0, 16);
		       out_buf[0] = 0;
		       out_buf[1] = 16;
		       out_buf[ 8] = 0x61;
		       out_buf[ 9] = 0x1a;
		       out_buf[10] = 0x41;
		       out_buf[11] = 0x00;
		       out_buf[12] = 0x4f;
		       out_buf[13] = 0x3b;
		       out_buf[14] = 0x4a;
		       return 0;

		    default:
		       printf("Simulation of READ TOC failed for format %d\n", cdb[2] & 0xf);
		       return -1;
		 }
		       
	 break;

      case 0x46: assert_cdb_length(cdb[0], cdb_size, 10);  /* GET CONFIGURATION */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);

		 alloc_len = cdb[7]<<8 | cdb[8];

		 memset(out_buf, 0, 8);
		 out_buf[3] = 8;  /* data length */
		 out_buf[7] = 9;  /* CD-R profile */
		 return 0;
	 break;

      case 0x51: assert_cdb_length(cdb[0], cdb_size, 10);  /* READ DISC INFORMATION */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);

		 alloc_len = cdb[7]<<8 | cdb[8];

		 memset(buf, 0, 32);
		 buf[0] = 0;
		 buf[1] = 32;   /* We are a strange CD without OPC tables. */
		 buf[2] = 0x1e; /* finalized and complete */
		 buf[8] = 0;    /* DATA1 format */

		 memcpy(out_buf, buf, alloc_len);
		 return 0;
	 break;

#if 0
      case 0x52: assert_cdb_length(cdb[0], cdb_size, 10);  /* READ TRACK INFORMATION */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);
	 break;
      case 0x55: assert_cdb_length(cdb[0], cdb_size, 10);  /* MODE SELECT */
                 assert_cdb_direction(cdb[0], DATA_WRITE, direction);
	 break;
      case 0x5a: assert_cdb_length(cdb[0], cdb_size, 10);  /* MODE SENSE */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);
	 break;
      case 0xad: assert_cdb_length(cdb[0], cdb_size, 12);  /* READ DVD STRUCTURE */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);
	 break;
#endif

      case 0xbe: assert_cdb_length(cdb[0], cdb_size, 12);  /* READ CD */
                 assert_cdb_direction(cdb[0], DATA_READ, direction);
	   {     guint64 lba,last_pos;
	         useconds_t delay;
		 int fact;
		 int i;

		 if(Closure->simulateTOC)
		 {  parse_sim_toc();
		    return simulate_read_cd(dh, cdb, out_buf, sense);
		 }

		 if(cdb[9] & ~16)  /* We can only deliver user data */
		 {  write_sense(sense, 5, 0xff, 0xe0);
		    return -1;
		 }

		 lba = cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5];
		 alloc_len = cdb[6] << 16 | cdb[7] << 8 | cdb[8];

		 last_pos = 2048*lba + 2048*alloc_len - 1;
		 if(last_pos > dh->simImage->size)
		 {  write_sense(sense, 5, 0x21, 0x00);  /* Illegal LBA */
		    return -1;
		 }

		 if(!LargeSeek(dh->simImage, 2048*lba))
		 {  write_sense(sense, 5, 0x02, 0x00); /* no seek complete */
		    return -1;
		 }

		 if(LargeRead(dh->simImage, out_buf, alloc_len*2048) != alloc_len*2048)
		 {  write_sense(sense, 5, 0x11, 0x00); /* unrecovered read error */
		    return -1;
		 }

		 /* Check for dead sector markers and pass them on
		    as read errors */

		 for(i=0; i<alloc_len; i++)
		 {  int err;

		    err = CheckForMissingSector(out_buf+2048*i, lba+i, NULL, 0);

		    if(err != SECTOR_PRESENT)
		    {  if(err == SECTOR_WITH_SIMULATION_HINT)
		       {  char *sim_hint = GetSimulationHint(out_buf+2048*i);

			  /* simulated hardware error */
			  
			  if(!strcmp(sim_hint, "hardware failure"))
			  {  g_free(sim_hint);
			     memset(out_buf, 0, alloc_len*2048);
			     write_sense(sense, 4, 0x09, 0x02); /* hardware error, focus servo failure  */
			     return -1;
			  }

                          /* pass on dead sector marker from this sector */  

			  if(!strcmp(sim_hint, "pass as dead sector marker"))
			  {  g_free(sim_hint);
			     continue;
			  }

			  /* simulated copy protected sector */

			  if(!strcmp(sim_hint, "scrambled sector"))
			  {  g_free(sim_hint);
			     memset(out_buf, 0, alloc_len*2048);
			     write_sense(sense, 5, 0x6F, 0x03); /* read of scrambled sector without authentication */
			     return -1;
			  }

			  /* sector becomes readable in pass 3 */
			  
			  if(!strcmp(sim_hint, "readable in pass 3"))
			  {  g_free(sim_hint);
			    if(dh->pass == 2)
			    {  memset(out_buf+i*2048, 64, 2048);
			       continue;
			    }
			    else
			    {  memset(out_buf, 0, alloc_len*2048);
			       write_sense(sense, 3, 0x11, 0x00); /* unrecovered read error */
			       return -1;
			    }
			  }

			  /* unknown simulation code becomes standard read error */
			  
			  g_free(sim_hint);
			  memset(out_buf, 0, alloc_len*2048);
			  write_sense(sense, 3, 0x11, 0x00); /* unrecovered read error */
			  return -1;
		       }
		       else /* standard read error */
		       {  memset(out_buf, 0, alloc_len*2048);
			  write_sense(sense, 3, 0x11, 0x00); /* unrecovered read error */
			  return -1;
		       }
		    }
		 }

		 nodelay = getenv("DVDISASTER_SCSI_SIMULATED_NODELAY");
		 if (!nodelay || strcmp(nodelay, "1")) {
		   fact = (int)(200.0*sin(-0.5+(double)lba/6280.0));
		   delay = (500+fact)*alloc_len;
		   usleep(delay);
		 }
	   }
	         return 0;
	 break;

      default:
	 printf("SendPacket(): Unknown opcode %0x\n", cdb[0]);
	 PrintLog("SendPacket(): Unknown opcode %0x\n", cdb[0]);
	 return -1;
   }
}


