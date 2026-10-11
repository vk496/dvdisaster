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

#include "rs04-includes.h"
#include "rs04-ff16.h"

/***
 *** Little-endian access to the on-disc structures
 ***/

static guint32 get16(unsigned char *p) { return p[0] | p[1]<<8; }
static guint32 get32(unsigned char *p) { return get16(p) | (guint32)get16(p+2)<<16; }
static guint64 get64(unsigned char *p) { return get32(p) | (guint64)get32(p+4)<<32; }

static void put16(unsigned char *p, guint32 v) { p[0] = v; p[1] = v>>8; }
static void put32(unsigned char *p, guint32 v) { put16(p, v); put16(p+2, v>>16); }
static void put64(unsigned char *p, guint64 v) { put32(p, v); put32(p+4, v>>32); }

#define OFS(field) offsetof(RS04Tag, field)

/***
 *** Layout arithmetic (rs04.tex, "Terminology" and "The added zone")
 ***/

guint64 RS04NextPow2(guint64 x)
{  guint64 p = 1;

   while(p < x) p <<= 1;
   return p;
}

static guint64 pow2floor(guint64 x)
{  guint64 p = 1;

   while(2*p <= x) p <<= 1;
   return p;
}

void RS04DeriveSet(RS04Set *set)
{  set->Bs = (set->B + 63)/64*64;
   set->S  = (set->dataBytes + set->B - 1) / set->B;
   set->c  = (set->S + RS04_CRCS_PER_SECTOR - 1) / RS04_CRCS_PER_SECTOR;
   set->D  = set->S + set->c;
   set->Q  = set->c + (guint64)set->m * set->L;
   set->A  = set->Q + (set->Q + set->T - 2) / (set->T - 1);
}

/* Position of payload sector q in the added zone */

guint64 RS04ZoneIndex(RS04Set *set, guint64 q)
{  return q + q/(set->T-1) + 1;
}

/***
 *** Tags
 ***/

/* Returns RS04_TAG_VALID and fills in set, own position and the 255 payload
   Crc32 values (crc may be NULL) if buf holds a valid tag. */

int RS04ParseTag(unsigned char *buf, RS04Set *set, guint64 *own_pos, guint32 *crc)
{  unsigned char copy[2048];
   guint64 pos;
   int i;

   if(memcmp(buf, "*dvdisaster*RS04", 16))
     return RS04_TAG_INVALID;

   memcpy(copy, buf, 2048);
   memset(copy+OFS(selfCRC), 0, 4);
   if(Crc32(copy, 2048) != get32(buf+OFS(selfCRC)))
     return RS04_TAG_INVALID;

   if(get16(buf+OFS(version)) != RS04_VERSION || buf[OFS(layoutVariant)] != RS04_LAYOUT_VARIANT)
     return RS04_TAG_UNSUPPORTED;

   memset(set, 0, sizeof(RS04Set));
   set->B              = get16(buf+OFS(sectorBytes));
   set->T              = get16(buf+OFS(tagInterval));
   set->flags          = buf[OFS(flags)];
   set->creatorVersion = get32(buf+OFS(creatorVersion));
   set->dataBytes      = get64(buf+OFS(dataBytes));
   set->L              = get64(buf+OFS(layerSectors));
   set->k              = get32(buf+OFS(dataLayers));
   set->m              = get32(buf+OFS(eccLayers));
   memcpy(set->setID, buf+OFS(setID), 8);
   memcpy(set->dataMD5, buf+OFS(dataMD5), 16);
   pos = get64(buf+OFS(ownPosition));

   if(   (set->B != 2048 && set->B != 2324 && set->B != 2352)
      || set->T < 2 || set->T > RS04_MAX_TAG_INTERVAL || (set->T & (set->T-1))
      || set->dataBytes < 1 || set->L < 1
      || set->m < 1 || set->m > set->k
      || RS04NextPow2(set->m) + set->k > 65536)
     return RS04_TAG_INVALID;

   /* Implementation limit; also keeps the arithmetic below from overflowing */

   if(   set->dataBytes / set->B >= RS04_MAX_COUNT
      || set->L >= RS04_MAX_COUNT/set->k)
     return RS04_TAG_INVALID;

   RS04DeriveSet(set);

   if(   set->k * set->L < set->D || set->A >= RS04_MAX_COUNT
      || pos < set->S || (pos - set->S) % set->T || pos - set->S >= set->A)
     return RS04_TAG_INVALID;

   if(own_pos) *own_pos = pos;
   if(crc)
     for(i=0; i<RS04_MAX_TAG_INTERVAL-1; i++)
       crc[i] = get32(buf+OFS(crc)+4*i);

   return RS04_TAG_VALID;
}

/* Create tag g of the set in out[] (B bytes). payload_crc[] holds the
   Crc32 of all payload sectors; NULL leaves the crc entries zero. */

void RS04BuildTag(RS04Set *set, guint64 g, guint32 *payload_crc, unsigned char *out)
{  int i;

   memset(out, 0, set->B);
   memcpy(out, "*dvdisaster*RS04", 16);
   put16(out+OFS(version), RS04_VERSION);
   out[OFS(layoutVariant)] = RS04_LAYOUT_VARIANT;
   out[OFS(flags)] = set->flags;
   put16(out+OFS(sectorBytes), set->B);
   put16(out+OFS(tagInterval), set->T);
   put32(out+OFS(creatorVersion), set->creatorVersion);
   memcpy(out+OFS(setID), set->setID, 8);
   put64(out+OFS(dataBytes), set->dataBytes);
   put64(out+OFS(layerSectors), set->L);
   put32(out+OFS(dataLayers), set->k);
   put32(out+OFS(eccLayers), set->m);
   put64(out+OFS(ownPosition), set->S + g*set->T);
   memcpy(out+OFS(dataMD5), set->dataMD5, 16);

   if(payload_crc)
     for(i=0; i<set->T-1; i++)
     {  guint64 q = g*(set->T-1) + i;

	if(q >= set->Q) break;
	put32(out+OFS(crc)+4*i, payload_crc[q]);
     }

   put32(out+OFS(selfCRC), Crc32(out, 2048));
}

/***
 *** Choice of the encoding parameters (rs04.tex, "Choosing the encoding parameters")
 ***/

static guint64 tag_interval(guint64 x)
{  if(x >= 4080) return 256;
   return MAX(2, pow2floor(x/16));
}

static int geometry_ok(guint64 D, guint64 E, guint64 L, guint64 N)
{  guint64 m = E/L;
   guint64 k = MAX((D+L-1)/L, m);

   return m >= 1 && RS04NextPow2(m) + k <= N;
}

guint64 RS04MemoryBudget(void)
{  return (guint64)Closure->rs04MemoryMiB << 20;
}

/* --encoding-algorithm 32bit/64bit forces the scalar codec kernels,
   SSE2 the SSSE3 ones (if available); results are identical anyway. */

void RS04ChooseKernel(void)
{  int kernel = RS04_KERNEL_AUTO;

   switch(Closure->encodingAlgorithm)
   {  case ENCODING_ALG_32BIT:
      case ENCODING_ALG_64BIT:
      case ENCODING_ALG_ALTIVEC:
	kernel = RS04_KERNEL_SCALAR;
	break;
      case ENCODING_ALG_SSE2:
	kernel = RS04_KERNEL_SSSE3;
	break;
   }

   if(!RS04SelectKernel(kernel))
     RS04SelectKernel(RS04_KERNEL_SCALAR);
   Verbose("RS04 codec kernels: %s\n", RS04KernelName());
}

/* set->B and set->dataBytes must be given. Either ratio (parity sectors
   per protected sector) or capacity (sectors for the whole set) is used.
   Returns FALSE if no suitable parameters exist. */

int RS04ChooseParameters(RS04Set *set, double ratio, guint64 capacity, guint64 ram)
{  guint64 S, c, D, C, E, N, T0 = 256;
   guint64 lo, hi;

   set->T = 2; set->L = 1; set->k = set->m = 1;
   RS04DeriveSet(set);
   S = set->S; c = set->c; D = set->D;

   if(capacity)
   {  guint64 Z, qmax;

      if(capacity <= S) return FALSE;
      Z = capacity - S;
      T0 = tag_interval(Z);
      qmax = Z - (Z + T0 - 1)/T0;
      if(qmax <= c) return FALSE;
      E = qmax - c;
      C = capacity;
   }
   else
   {  E = (guint64)floor(ratio*(double)D + 0.5);
      C = D + MIN(E, 2*D);
   }

   E = MIN(E, 2*D);
   if(E < 1 || E*247 < 8*D)
     return FALSE;

   N = MIN(65536, pow2floor(C/32));
   N = MIN(N, pow2floor(ram/(128*set->Bs)));
   if(!geometry_ok(D, E, E, N))
   {  N = 65536;
      if(!geometry_ok(D, E, E, N))
	return FALSE;
   }

   /* smallest L >= min(32, D, E) with geometry_ok(); bisection */

   lo = MIN(32, MIN(D, E));
   hi = E;
   while(lo < hi)
   {  guint64 mid = lo + (hi-lo)/2;

      if(geometry_ok(D, E, mid, N)) hi = mid;
      else                          lo = mid+1;
   }

   set->L = lo;
   set->m = E/lo;
   set->k = MAX((D+lo-1)/lo, set->m);
   set->T = tag_interval(c + set->m*lo);
   RS04DeriveSet(set);

   if(capacity && S + set->A > capacity)
   {  set->T = T0;
      RS04DeriveSet(set);
   }

   return set->L < RS04_MAX_COUNT && set->A < RS04_MAX_COUNT;
}

/***
 *** Sector <-> shard mapping (rs04.tex, "Sector to shard mapping")
 ***/

void RS04SectorToShard(int B, unsigned char *sector, unsigned char *shard)
{  int F = B/64*64, r = B-F;

   if(!r)
   {  memcpy(shard, sector, B);
      return;
   }

   memcpy(shard, sector, F);
   memset(shard+F, 0, 64);
   memcpy(shard+F, sector+F, r/2);
   memcpy(shard+F+32, sector+F+r/2, r/2);
}

/* Returns FALSE if the unused shard bytes are not zero */

int RS04ShardToSector(int B, unsigned char *shard, unsigned char *sector)
{  int F = B/64*64, r = B-F;
   int i;

   if(!r)
   {  memcpy(sector, shard, B);
      return TRUE;
   }

   memcpy(sector, shard, F);
   memcpy(sector+F, shard+F, r/2);
   memcpy(sector+F+r/2, shard+F+32, r/2);

   for(i=r/2; i<32; i++)
     if(shard[F+i] || shard[F+32+i])
       return FALSE;

   return TRUE;
}

/***
 *** Glue to the generic EccHeader based interface
 ***
 * RS04 has no EccHeader. The other parts of dvdisaster expect one
 * for recognized ecc data, so a synthetic one is created. It describes
 * set (the last one of an image), with sectors/inLast giving the end of
 * its data in 2048 byte units (used e.g. by --strip), and carries a
 * copy of tag 0 in its padding for re-creating the set later.
 */

EccHeader* RS04CreateEccHeader(RS04Set *set, int nsets)
{  EccHeader *eh = g_malloc0(sizeof(EccHeader));
   guint64 end = set->dataBytes;

   if(set->target == ECC_IMAGE)
     end += set->start * set->B;

   memcpy(eh->cookie, "*dvdisaster*", 12);
   memcpy(eh->method, "RS04", 4);
   eh->methodFlags[0] = set->target == ECC_FILE ? MFLAG_ECC_FILE : 0;
   eh->methodFlags[1] = MIN(nsets, 255);
   eh->methodFlags[3] = set->flags;
   memcpy(eh->mediumSum, set->dataMD5, 16);
   gint64_to_uchar(eh->sectors, (end+2047)/2048);
   eh->inLast = end % 2048 ? end % 2048 : 2048;
   eh->dataBytes = set->k;
   eh->eccBytes = set->m;
   eh->creatorVersion = set->creatorVersion;
   eh->fpSector = FINGERPRINT_SECTOR;
   eh->sectorsPerLayer = set->L;
   eh->sectorsAddedByEcc = set->A;
   RS04BuildTag(set, 0, NULL, (unsigned char*)eh->padding);

   return eh;
}

/* Returns a copy of the sets belonging to the image (ecc file preferred),
   or NULL if it has no RS04 data. The caller must g_free() it. */

RS04Info* RS04GetSets(Image *image)
{  RS04Info *info;
   size_t size;

   if(image->eccFileHeader && !strncmp((char*)image->eccFileHeader->method, "RS04", 4))
   {  info = g_malloc0(sizeof(RS04Info) + sizeof(RS04Set));
      info->eh = image->eccFileHeader;
      info->nSets = 1;
      if(RS04ParseTag((unsigned char*)info->eh->padding, &info->set[0], NULL, NULL) != RS04_TAG_VALID)
	Stop("Internal error: RS04GetSets(): bad header");
      info->set[0].target = ECC_FILE;
      return info;
   }

   if(image->eccHeader && image->cachedLayout
      && !strncmp((char*)image->eccHeader->method, "RS04", 4))
   {  RS04Info *cached = (RS04Info*)image->cachedLayout;

      size = sizeof(RS04Info) + cached->nSets*sizeof(RS04Set);
      info = g_malloc(size);
      memcpy(info, cached, size);
      return info;
   }

   return NULL;
}

/* Expected image size in 2048 byte sectors */

guint64 RS04ExpectedImageSize(Image *image)
{  RS04Info *info = RS04GetSets(image);
   guint64 bytes = 0;
   int i;

   if(!info) return 0;

   for(i=0; i<info->nSets; i++)
   {  RS04Set *set = &info->set[i];
      guint64 end = set->dataBytes;

      if(set->target == ECC_IMAGE)
	end = (set->start + set->S + set->A) * set->B;
      bytes = MAX(bytes, end);
   }

   g_free(info);
   return (bytes+2047)/2048;
}

/***
 *** CRC buffer for checking sectors while reading a medium
 ***
 * Covers the data sectors of all sets with 2048 byte sectors at their
 * image positions, and the CRC sectors of augmented sets (from the tags).
 * Only the beginning of each added zone up to the last CRC sector is read.
 * Sectors without a CRC (tags, parity, gaps between sets) are not covered;
 * their CRC is taken from the image while reading, so that the buffer
 * can be used for creating other ecc data from the image afterwards.
 */

static void set_crc(CrcBuf *cb, guint64 idx, int known, guint32 crc)
{  if(idx >= cb->crcSize) return;

   SetBit(cb->covered, idx);
   if(known)
   {  cb->crcbuf[idx] = crc;
      SetBit(cb->valid, idx);
   }
}

CrcBuf* RS04GetCrcBuf(Image *image)
{  RS04Info *info = RS04GetSets(image);
   CrcBuf *cb = CreateCrcBuf(image);
   int i;

   cb->covered = CreateBitmap0(cb->crcSize);
   cb->coveredSectors = cb->crcSize;

   for(i=0; info && i<info->nSets; i++)
   {  RS04Set *set = &info->set[i];
      gint64 base = set->target == ECC_FILE ? 0 : set->start;
      guint64 t, q, S = set->S;
      RS04State st;

      if(set->B != 2048) continue;
      if(set->dataBytes % 2048) S--;  /* partial last sector of an ecc file set */

      RS04InitState(&st, image, set);
      RS04ScanZonePart(&st, NULL, RS04ZoneIndex(set, set->c-1)+1);

      for(t=0; t<S; t++)
	set_crc(cb, base+t, GetBit(st.dataKnown, t), st.dataCrc[t]);

      if(set->target == ECC_IMAGE)
	for(q=0; q<set->c; q++)
	  set_crc(cb, base+set->S+RS04ZoneIndex(set, q),
		  GetBit(st.payloadKnown, q), st.payloadCrc[q]);

      RS04FreeState(&st);
   }

   g_free(info);
   return cb;
}

/***
 *** Sector I/O
 ***
 * Data sector t of a set is at byte io->dataOffset + t*B of the image,
 * added zone sector a at byte io->zonePos + a*B of io->zoneFile.
 * Sectors which are (partially) outside of the file, unreadable on a
 * medium or which contain a dead sector marker are reported as missing
 * (present[i] = 0). Missing parts of the buffer are zero.
 */

int RS04ReadAll(LargeFile *file, void *buf, guint64 count)
{  unsigned char *b = buf;
   guint64 total = 0;

   while(total < count)
   {  ssize_t n = LargeRead(file, b+total, MIN(count-total, 1<<30));

      if(n <= 0) break;
      total += n;
   }

   return total == count;
}

void RS04WriteAt(LargeFile *file, gint64 pos, void *buf, guint64 count)
{  if(!LargeSeek(file, pos))
     Stop(_("Failed seeking to byte %" PRId64 " in %s: %s"), pos, file->path, strerror(errno));
   if(LargeWrite(file, buf, count) != count)
     Stop(_("Failed writing to %s: %s"), file->path, strerror(errno));

   if(pos + count > file->size)
     file->size = pos + count;
}

void RS04InitIO(RS04IO *io, Image *image, RS04Set *set)
{  io->image = image;
   io->set = set;

   if(set->target == ECC_FILE)
   {  io->dataOffset = 0;
      io->zoneFile = image->eccFile;
      io->zonePos = 0;
   }
   else
   {  io->dataOffset = set->start * set->B;
      io->zoneFile = image->file;
      io->zonePos = (set->start + set->S) * set->B;
   }
}

/* Medium size; also usable before query_size() has been called */

guint64 RS04MediumSectors(Image *image)
{  DeviceHandle *dh = image->dh;

   if(dh->sectors) return dh->sectors;
   return MAX(dh->readCapacity+1, dh->userAreaSize);
}

static int read_medium(Image *image, gint64 pos, int n, unsigned char *buf, guint8 *present)
{  guint64 first = pos/2048;
   guint64 sectors = RS04MediumSectors(image);
   int i, count = 0;

   for(i=0; i<n; i++)
     present[i] = first+i < sectors;

   if(present[n-1] && ImageReadSectors(image, buf, first, n) == n)
     count = n;
   else for(i=0; i<n; i++)
	  if(present[i])
	  {  present[i] = ImageReadSectors(image, buf+2048*i, first+i, 1) == 1;
	     if(!present[i]) memset(buf+2048*i, 0, 2048);
	     else count++;
	  }

   return count;
}

static int read_range(RS04IO *io, LargeFile *file, gint64 pos, gint64 limit,
		      int n, unsigned char *buf, guint8 *present)
{  int B = io->set->B;
   gint64 lo, hi, readable;
   int i, count = 0;

   memset(buf, 0, (size_t)n*B);

   if(!file)
   {  if(io->image->type == IMAGE_MEDIUM && B == 2048 && pos >= 0 && !(pos % 2048))
	return read_medium(io->image, pos, n, buf, present);
      memset(present, 0, n);
      return 0;
   }

   lo = MAX(pos, 0);
   hi = MIN(pos + (gint64)n*B, MIN((gint64)file->size, limit));
   readable = lo;

   if(hi > lo)
   {  if(!LargeSeek(file, lo))
	Stop(_("Failed seeking to byte %" PRId64 " in %s: %s"), lo, file->path, strerror(errno));
      if(RS04ReadAll(file, buf + (lo-pos), hi-lo))
	readable = hi;
   }

   for(i=0; i<n; i++)
   {  gint64 s0 = pos + (gint64)i*B;
      gint64 s1 = MIN(s0 + B, limit);

      present[i] = s0 >= 0 && s1 > s0 && s1 <= readable;
      if(present[i] && s1-s0 == B   /* marker in the first 2048 bytes */
	 && CheckForMissingSector(buf+(size_t)B*i, s0/B, NULL, 0) != SECTOR_PRESENT)
	present[i] = 0;
      count += present[i];
   }

   return count;
}

int RS04ReadData(RS04IO *io, guint64 t, int n, unsigned char *buf, guint8 *present)
{  RS04Set *set = io->set;

   return read_range(io, io->image->file, io->dataOffset + (gint64)t*set->B,
		     io->dataOffset + set->dataBytes, n, buf, present);
}

int RS04ReadZone(RS04IO *io, guint64 a, int n, unsigned char *buf, guint8 *present)
{  return read_range(io, io->zoneFile, io->zonePos + (gint64)a*io->set->B,
		     G_MAXINT64, n, buf, present);
}

/* Read the n payload sectors starting with q; tags in between are skipped. */

int RS04ReadPayload(RS04IO *io, guint64 q, int n, unsigned char *buf, guint8 *present)
{  RS04Set *set = io->set;
   guint64 a0 = RS04ZoneIndex(set, q);
   int na = RS04ZoneIndex(set, q+n-1) - a0 + 1;
   int i, j, count = 0;

   if(na == n)
     return RS04ReadZone(io, a0, n, buf, present);

   {  unsigned char *tmp = g_malloc((size_t)na*set->B);
      guint8 *tmp_present = g_malloc(na);

      RS04ReadZone(io, a0, na, tmp, tmp_present);
      for(i=0; i<n; i++)
      {  j = RS04ZoneIndex(set, q+i) - a0;
	 memcpy(buf+(size_t)i*set->B, tmp+(size_t)j*set->B, set->B);
	 present[i] = tmp_present[j];
	 count += present[i];
      }
      g_free(tmp);
      g_free(tmp_present);
   }

   return count;
}

/* Writing: the last data sector of an ecc file set may be shorter than B */

void RS04WriteData(RS04IO *io, guint64 t, int n, unsigned char *buf)
{  RS04Set *set = io->set;
   guint64 bytes = (guint64)n*set->B;

   if(t*set->B + bytes > set->dataBytes)
     bytes = set->dataBytes - t*set->B;

   RS04WriteAt(io->image->file, io->dataOffset + (gint64)t*set->B, buf, bytes);
}

void RS04WriteZone(RS04IO *io, guint64 a, int n, unsigned char *buf)
{  RS04WriteAt(io->zoneFile, io->zonePos + (gint64)a*io->set->B, buf, (guint64)n*io->set->B);
}

void RS04WritePayload(RS04IO *io, guint64 q, int n, unsigned char *buf)
{  RS04Set *set = io->set;

   while(n > 0)
   {  int run = MIN(n, (set->T-1) - q % (set->T-1));

      RS04WriteZone(io, RS04ZoneIndex(set, q), run, buf);
      buf += (size_t)run*set->B;
      q += run;
      n -= run;
   }
}
