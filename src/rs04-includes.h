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
   rs04-common.c reads and writes them byte-wise; all integers are
   little-endian on disc, independent of the host byte order. */

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

/***
 *** In-memory structures of the RS04 codec.
 *** Everything below requires dvdisaster.h to be included first.
 ***/

#define RS04_SILENCE_CRC 0x416831C0  /* Crc32 of 2352 zero bytes (digital silence) */
#define RS04_MAX_COUNT   G_MAXINT32  /* implementation limit for sector counts (bitmaps) */
#define RS04_MAX_SETS    64          /* sets enumerated in one image */

/* One RS04 set; symbols as in rs04.tex */

typedef struct _RS04Set
{  int B, Bs, T;            /* sector size, shard size, tag interval */
   guint64 dataBytes;       /* stored parameters */
   guint64 L;
   guint32 k, m;
   guint8 setID[8];
   guint8 dataMD5[16];
   guint32 creatorVersion;
   int flags;
   guint64 S, c, D, Q, A;   /* derived by RS04DeriveSet() */
   gint64 start;            /* augmented: first sector of the set in the image */
   int target;              /* ECC_FILE or ECC_IMAGE */
   int tags;                /* valid tags seen during recognition */
   int matches;             /* confirmed data sectors (-1 = not confirmable) */
} RS04Set;

/* All sets of an image. Kept in Image->cachedLayout for augmented images;
   eh must be the first member as other codecs peek at it there. */

typedef struct _RS04Info
{  EccHeader *eh;
   int nSets;
   RS04Set set[];
} RS04Info;

/* Access to the sectors of a set, see rs04-common.c */

typedef struct _RS04IO
{  Image *image;
   RS04Set *set;
   LargeFile *zoneFile;     /* ecc file or image file holding the added zone */
   gint64 dataOffset;       /* byte position of data sector 0 in the image */
   gint64 zonePos;          /* byte position of added zone sector 0 in zoneFile */
} RS04IO;

typedef struct
{  GtkWidget *createLabel, *fixLabel, *verifyLabel;
} RS04Widgets;

#define RS04_LABEL(name) (((RS04Widgets*)FindMethod("RS04")->widgetList)->name)

/* ecc-rs04.c */

void CreateRS04EncWindow(Method*, GtkWidget*);
void CreateRS04FixWindow(Method*, GtkWidget*);
void CreateRS04VerifyWindow(Method*, GtkWidget*);
void ResetRS04EncWindow(Method*);
void ResetRS04FixWindow(Method*);
void ResetRS04VerifyWindow(Method*);

/* rs04-common.c */

enum { RS04_TAG_INVALID, RS04_TAG_VALID, RS04_TAG_UNSUPPORTED };

guint64 RS04NextPow2(guint64);
void RS04DeriveSet(RS04Set*);
int  RS04ParseTag(unsigned char*, RS04Set*, guint64*, guint32*);
void RS04BuildTag(RS04Set*, guint64, guint32*, unsigned char*);
int  RS04ChooseParameters(RS04Set*, double, guint64, guint64);
guint64 RS04MemoryBudget(void);
guint64 RS04ZoneIndex(RS04Set*, guint64);
void RS04SectorToShard(int, unsigned char*, unsigned char*);
int  RS04ShardToSector(int, unsigned char*, unsigned char*);
EccHeader* RS04CreateEccHeader(RS04Set*, int);
RS04Info* RS04GetSets(Image*);
guint64 RS04ExpectedImageSize(Image*);
guint64 RS04MediumSectors(Image*);

void RS04InitIO(RS04IO*, Image*, RS04Set*);
int  RS04ReadData(RS04IO*, guint64, int, unsigned char*, guint8*);
int  RS04ReadZone(RS04IO*, guint64, int, unsigned char*, guint8*);
int  RS04ReadPayload(RS04IO*, guint64, int, unsigned char*, guint8*);
void RS04WriteData(RS04IO*, guint64, int, unsigned char*);
void RS04WriteZone(RS04IO*, guint64, int, unsigned char*);
void RS04WritePayload(RS04IO*, guint64, int, unsigned char*);
int  RS04ReadAll(LargeFile*, void*, guint64);
void RS04WriteAt(LargeFile*, gint64, void*, guint64);

/* rs04-create.c */

void RS04Create(void);

/* rs04-recognize.c */

int RS04RecognizeFile(LargeFile*, EccHeader**);
int RS04RecognizeImage(Image*);

/* rs04-verify.c */

typedef struct _RS04State   /* results of RS04ScanZone() */
{  RS04Set *set;
   RS04IO io;
   guint32 *payloadCrc;     /* Crc32 of the payload sectors from the tags */
   Bitmap *payloadKnown;
   Bitmap *payloadBad;      /* erased payload sectors */
   guint32 *dataCrc;        /* Crc32 of the data sectors from the CRC sectors */
   Bitmap *dataKnown;
   Bitmap *tagBad;          /* missing or invalid tags */
   guint64 nTags, tagsBad;
   guint64 crcMissing, crcErrors, eccMissing, eccErrors;
} RS04State;

void RS04InitState(RS04State*, Image*, RS04Set*);
void RS04FreeState(RS04State*);
void RS04ScanZone(RS04State*, GString*);
void RS04SetCrcSector(RS04State*, guint64, unsigned char*);
int  RS04DetectAudioOffset(RS04State*, int*, int*, int*);
void RS04Verify(Image*);

/* rs04-fix.c */

void RS04Fix(Image*);

#endif /* RS04INCLUDES_H */
