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

#ifndef RS04INCLUDES_H
#define RS04INCLUDES_H

/* RS04 on-disc structures, see documentation/codec-specs/rs04.tex.
   Draft: not used by the build yet. All integers are little-endian
   on disc; big-endian hosts must swap them after reading. */

#include <glib.h>
#include <stddef.h>

#define RS04_VERSION           1    /* tag format version */
#define RS04_LAYOUT_VARIANT    0    /* contiguous layers, no permutation */
#define RS04_CRCS_PER_SECTOR 512    /* Crc32 values in one CRC sector */
#define RS04_MAX_TAG_INTERVAL 256   /* T: one tag every T added-zone sectors */

#define RS04_FLAG_DEVEL  (1<<0)     /* created by a development release */
#define RS04_FLAG_RC     (1<<1)     /* created by a release candidate */

typedef struct _RS04Tag
{  gint8   cookie[12];      /*    0: "*dvdisaster*" */
   gint8   method[4];       /*   12: "RS04" */
   guint16 version;         /*   16: RS04_VERSION; refuse others */
   guint8  layoutVariant;   /*   18: RS04_LAYOUT_VARIANT; refuse others */
   guint8  flags;           /*   19: RS04_FLAG_*; unknown bits are ignored */
   guint16 sectorBytes;     /*   20: B = 2048, 2324 or 2352 */
   guint16 tagInterval;     /*   22: T, power of two, 2..256 */
   guint32 creatorVersion;  /*   24: e.g. 7910 = dvdisaster 0.79.10 */
   guint32 selfCRC;         /*   28: Crc32 of bytes 0..2047, this field = 0 */
   guint8  setID[8];        /*   32: random, identical in all tags of a set */
   guint64 dataBytes;       /*   40: protected bytes; S = ceil(dataBytes/B) */
   guint64 layerSectors;    /*   48: L */
   guint32 dataLayers;      /*   56: k */
   guint32 eccLayers;       /*   60: m */
   guint64 ownPosition;     /*   64: S + a, relative to the set start */
   guint8  dataMD5[16];     /*   72: md5 of the dataBytes data bytes */
   guint32 crc[RS04_MAX_TAG_INTERVAL-1]; /* 88: Crc32 of the following
                                          tagInterval-1 payload sectors */
   guint8  zero[940];       /* 1108: always zero */
} RS04Tag;                  /* 2048 bytes; rest of a B > 2048 sector is zero */

/* CRC sector j: Crc32 of data sectors 512*j .. 512*j+511, unused entries 0 */

typedef struct _RS04CrcSector
{  guint32 crc[RS04_CRCS_PER_SECTOR];
} RS04CrcSector;

G_STATIC_ASSERT(sizeof(RS04Tag) == 2048);
G_STATIC_ASSERT(offsetof(RS04Tag, dataBytes) == 40);
G_STATIC_ASSERT(offsetof(RS04Tag, crc) == 88);
G_STATIC_ASSERT(sizeof(RS04CrcSector) == 2048);

#endif /* RS04INCLUDES_H */
