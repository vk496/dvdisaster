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

/*** src type: some GUI code ***/

#include "dvdisaster.h"
#include "scsi-layer.h"
#include "udf.h"

#include "rs04-includes.h"

/***
 *** Open image from a given device or file.
 ***
 * Note that OpenImageFromDevice() is located in scsi-layer.c
 * for convenience reasons.
 */

/*
 * Multi-file images: a .cue or .gdi sheet stands for the concatenation
 * of its .bin files (2352 byte sectors), e.g. one file per track as
 * written by redumper. Gaps which are not stored in the files are not
 * part of the image. Only RS04 ecc files can be used with them.
 */

/* Next blank separated or quoted token; terminates it in place */

static char *next_token(char **p)
{  char *start, *end;

   while(g_ascii_isspace(**p)) (*p)++;
   if(!**p) return NULL;

   if(**p == '"')
   {  start = ++(*p);
      end = strchr(start, '"');
      if(!end) end = start + strlen(start);
   }
   else
   {  for(start = end = *p; *end && !g_ascii_isspace(*end); end++)
	 ;
   }

   *p = *end ? end+1 : end;
   *end = 0;
   return start;
}

static LargeFile *open_multi_file(char *name, int flags, int gdi)
{  FILE *sheet = fopen(name, "rb");
   GPtrArray *parts;
   LargeFile *lf;
   char line[4096];
   int dir_len = 0;
   int i;

   if(!sheet)
      return NULL;

   for(i=0; name[i]; i++)   /* file names are relative to the sheet */
     if(name[i] == '/' || name[i] == G_DIR_SEPARATOR)
       dir_len = i+1;

   parts = g_ptr_array_new();
   lf = g_malloc0(sizeof(LargeFile));

   while(fgets(line, sizeof(line), sheet))
   {  char *p = line, *token[6], *file = NULL;

      if(!strncmp(p, "\xef\xbb\xbf", 3))  /* UTF-8 byte order mark */
	 p += 3;
      for(i=0; i<6; i++)
	 token[i] = next_token(&p);

      if(!token[0]) ;
      else if(gdi)  /* track lba type sector_size file offset */
      {  if(token[4])
	 {  if(atoi(token[3]) != 2352)
	      Stop(_("%s: track %s (%s) is not supported; all tracks need 2352 byte sectors."),
		   name, token[0], token[3]);
	    file = token[4];
	 }
      }
      else if(!g_ascii_strcasecmp(token[0], "FILE"))
      {  if(!token[2] || g_ascii_strcasecmp(token[2], "BINARY"))
	   Stop(_("%s: only BINARY files are supported."), name);
	 file = token[1];
      }
      else if(!g_ascii_strcasecmp(token[0], "TRACK"))
      {  if(   !token[2]
	    || (   g_ascii_strcasecmp(token[2], "AUDIO")
		&& g_ascii_strcasecmp(token[2], "MODE1/2352")
		&& g_ascii_strcasecmp(token[2], "MODE2/2352")
		&& g_ascii_strcasecmp(token[2], "CDI/2352")))
	   Stop(_("%s: track %s (%s) is not supported; all tracks need 2352 byte sectors."),
		name, token[1] ? token[1] : "?", token[2] ? token[2] : "?");
      }

      if(file)
      {  char *path = g_path_is_absolute(file) ? g_strdup(file) : g_strdup_printf("%.*s%s", dir_len, name, file);
	 LargeFile *part = LargeOpen(path, flags, IMG_PERMS);

	 if(!part)
	   Stop(_("Can't open %s:\n%s"), path, strerror(errno));
	 Verbose("%s: %s, %" PRId64 " bytes\n", name, path, part->size);
	 g_ptr_array_add(parts, part);
	 lf->size += part->size;
	 g_free(path);
      }
   }
   fclose(sheet);

   if(!parts->len)
     Stop(_("%s: no image files found."), name);

   lf->fileHandle = -1;
   lf->path = g_strdup(name);
   lf->flags = flags;
   lf->parts = parts;
   return lf;
}

/*
 * Only RS04 ecc files can be used with multi-file images
 */

void RefuseMultiFileImage(Image *image)
{  if(image && image->file && image->file->parts)
     Stop(_("%s: .cue and .gdi images can only be protected by RS04 error correction files."),
	  Closure->imageName);
}

Image *OpenImageFromFile(char *filename, int flags, mode_t mode)
{  LargeFile *file;
   size_t len = strlen(filename);
   char *suffix = len > 4 ? filename+len-4 : "";

   if(!g_ascii_strcasecmp(suffix, ".cue") || !g_ascii_strcasecmp(suffix, ".gdi"))
        file = open_multi_file(filename, flags, !g_ascii_strcasecmp(suffix, ".gdi"));
   else file = LargeOpen(filename, flags, mode);

   if(file)
   {  Image *image = g_malloc0(sizeof(Image));

      image->fpSector = -1;
      image->type = IMAGE_FILE;
      image->file = file;

      CalcSectors(file->size, &image->sectorSize, &image->inLast);

      ExamineUDF(image);
      if(!file->parts)   /* no augmented images */
	 ExamineECC(image);

      GetImageFingerprint(image, NULL, FINGERPRINT_SECTOR);

      return image;
   }

   return NULL;
}

/***
 *** Find out whether the image contains ECC information.
 ***/

void ExamineECC(Image *image)
{  int i;

   Verbose("ExamineECC() started\n");

   for(i=0; i<Closure->methodList->len; i++)  
   {  Method *method = g_ptr_array_index(Closure->methodList, i);
      char buf[5];
      strncpy(buf,method->name,4);
      buf[4]=0;

      Verbose("...trying %s\n", buf);

      if(   method->recognizeEccImage
	 && method->recognizeEccImage(image))
      {  Verbose("...augmented image found\n");
	 image->eccMethod = method;
	 image->expectedSectors = method->expectedImageSize(image);
	 return;
      }
   }

   Verbose("...no augmented image detected.\n");
} 

/***
 *** Warn if the image extends beyond its ISO file system.
 ***
 * Mastering software like xorriso appends 300 KiB of padding by default.
 * The padding gets protected along with the data, wastes space on the medium
 * and moves the ecc header away from where it is quickly found behind the
 * ISO file system.
 */

void WarnAboutImagePadding(Image *image)
{
   /* Images whose size is not a multiple of 2048 are not from a mastering
      program; leave them alone. */

   if(image->isoInfo && image->inLast == 2048
      && image->sectorSize > image->isoInfo->volumeSize)
     ModalWarning(GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, NULL,
		  _("The image is %" PRId64 " sectors larger than its ISO file system (%d sectors).\n"
		    "This is usually padding added by the mastering software\n"
		    "(e.g. xorriso appends 300 KiB unless \"-padding 0\" is given).\n"
		    "The padding uses up space which is lost for error correction data,\n"
		    "and the error correction data may not be found when reading the\n"
		    "medium without an exhaustive search. Consider creating the image\n"
		    "without padding.\n"),
		  image->sectorSize - image->isoInfo->volumeSize,
		  image->isoInfo->volumeSize);
}

/***
 *** Search a directory for the .ecc file matching the image
 ***
 * Only *.ecc files directly inside the directory are examined.
 * A file matches if its header fingerprint equals the image fingerprint
 * (RS04: if its CRC sectors match the image).
 * Returns the newly allocated path of the single match;
 * stops if there is no match or more than one.
 * Further matching RS04 files are added as supplementary sets.
 */

static gint compare_names(gconstpointer a, gconstpointer b)
{  return strcmp(*(char**)a, *(char**)b);
}

static char *find_ecc_file(Image *image, char *dir)
{  GError *err = NULL;
   GDir *gdir = g_dir_open(dir, 0, &err);
   GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
   const char *name;
   char *found = NULL;
   int n, found_rs04 = FALSE;

   if(!gdir)
     Stop(_("Can't open %s:\n%s"), dir, err->message);

   while((name = g_dir_read_name(gdir)))
     if(strlen(name) >= 4 && !g_ascii_strcasecmp(name+strlen(name)-4, ".ecc"))
       g_ptr_array_add(names, g_build_path("/", dir, name, NULL));
   g_dir_close(gdir);

   /* sorted for reproducible results */

   g_ptr_array_sort(names, (GCompareFunc)compare_names);

   for(n=0; n<names->len; n++)
   {  char *path = g_ptr_array_index(names, n);
      LargeFile *file = LargeOpen(path, O_RDONLY, IMG_PERMS);
      int i, match = FALSE, rs04 = FALSE;

      if(!file)
	continue;

      for(i=0; i<Closure->methodList->len; i++)
      {  Method *method = g_ptr_array_index(Closure->methodList, i);
	 EccHeader *eh;
	 guint8 fp[16];

	 if(   method->recognizeEccFile
	    && method->recognizeEccFile(file, &eh) == ECCFILE_PRESENT)
	 {  if(!strncmp((char*)eh->method, "RS04", 4))
	         match = rs04 = RS04ConfirmEccFile(image, file, eh);
	    else match =    GetImageFingerprint(image, fp, eh->fpSector)
	                 && !memcmp(fp, eh->mediumFP, 16);
	    g_free(eh);
	    break;
	 }
      }
      LargeClose(file);

      Verbose("find_ecc_file(): %s %s\n", path, match ? "matches" : "does not match");
      if(!match)
	continue;
      if(found && found_rs04 && rs04)
      {  if(!Closure->moreEccNames)
	   Closure->moreEccNames = g_ptr_array_new_with_free_func(g_free);
	 g_ptr_array_add(Closure->moreEccNames, g_strdup(path));
	 continue;
      }
      if(found)
	Stop(_("Both %s and %s match the image; please select one with -e.\n"), found, path);
      found = g_strdup(path);
      found_rs04 = rs04;
   }
   g_ptr_array_free(names, TRUE);

   if(!found)
     Stop(_("No ecc file in %s matches the image.\n"), dir);

   PrintLog(_("Using ecc file %s\n"), found);
   return found;
}

/***
 *** Open a ECC file for an existing image
 ***/

Image* OpenEccFileForImage(Image *image, char *filename, int flags, mode_t mode)
{  int new_image = 0;
   int i;

   /* A directory was given: pick the matching ecc file from it.
      All callers pass Closure->eccName, which is updated so that
      later messages and reopens refer to the selected file. */

   if(image && filename && g_file_test(filename, G_FILE_TEST_IS_DIR))
   {  char *found = find_ecc_file(image, filename);

      g_free(Closure->eccName);
      filename = Closure->eccName = found;
   }

   if(!image)
   {  new_image = 1;
      image = g_malloc0(sizeof(Image));
   }

   image->eccFile = LargeOpen(filename, flags, mode);
   if(errno == EACCES)
        image->eccFileState = ECCFILE_NOPERM;
   else image->eccFileState = ECCFILE_MISSING;
   
   if(!image->eccFile)
   {  if(new_image) 
      {  g_free(image);
	 return NULL;
      }
      return image;
   }

   /* Determine codec for ecc file */
   
   for(i=0; i<Closure->methodList->len; i++)  
   {  Method *method = g_ptr_array_index(Closure->methodList, i);

      if(method->recognizeEccFile)
      {  image->eccFileState = method->recognizeEccFile(image->eccFile, &image->eccFileHeader);
	 if(image->eccFileState == ECCFILE_PRESENT)
	 {  if(strncmp(method->name, "RS04", 4))
	       RefuseMultiFileImage(image);
	    image->eccFileMethod = method; 
	    image->expectedSectors = method->expectedImageSize(image);
	    GetImageFingerprint(image, NULL, image->eccFileHeader->fpSector);
	    return image;
	 }
      }
   }

   /* bail out (unrecognized ecc file) */

   LargeClose(image->eccFile);
   image->eccFile = NULL;
   if(new_image) 
   {  g_free(image);
      return NULL;
   }
   return image;
}

/*
 * Report inconsistencies found in image and ecc files.
 */

int ReportImageEccInconsistencies(Image *image)
{
  /* No image file */

  if(!image || image->type == IMAGE_NONE)
  {  if(image) CloseImage(image);

#ifdef WITH_GUI_YES
     if(Closure->guiMode)
     {     GuiCreateMessage(_("Image file %s not present or permission denied.\n"), GTK_MESSAGE_ERROR, Closure->imageName);
	   return TRUE;
     }
     else
#endif
     {     Stop(_("Image file %s not present or permission denied.\n"), Closure->imageName);
     }
  }

  /* Image file but unknown ecc */

  if(image->eccFile && !image->eccFileMethod)
  {  CloseImage(image);

#ifdef WITH_GUI_YES
     if(Closure->guiMode)
     {   GuiCreateMessage(_("\nError correction file type unknown.\n"), GTK_MESSAGE_ERROR);
	 return TRUE;
     }
     else
#endif       
     {  Stop(_("\nError correction file type unknown.\n"));
     }
  }

  /* Permission denied for ecc file */

  if(!image->eccFile && image->eccFileState == ECCFILE_NOPERM)
  {  CloseImage(image);

#ifdef WITH_GUI_YES
     if(Closure->guiMode)
     {    GuiCreateMessage(_("\nPermission denied on ecc file (perhaps not writeable?).\n"),
			  GTK_MESSAGE_ERROR);
	  return TRUE;
     }
     else
#endif
     {  Stop(_("\nPermission denied on ecc file (perhaps not writeable?).\n"));
     }
  }

  
  /* Augmented image but unknown ecc method */

  if(!image->eccFile && !image->eccMethod)
  {  CloseImage(image);

#ifdef WITH_GUI_YES
     if(Closure->guiMode)
     {    GuiCreateMessage(_("\nNo error correction file present.\n"
			     "No error correction data recognized in image.\n"), GTK_MESSAGE_ERROR);
	  return TRUE;
     }
     else
#endif
     {  Stop(_("\nNo error correction file present.\n"
	       "No error correction data recognized in image.\n"));
     }
  }

  return FALSE;
}

/***
 *** Image access functions
 ***
 * Once the image has been opened, the following functions
 * provide transparent access to it (e.g. it does not matter
 * whether the image resides in a file or on a optical drive.
 * However these functions are not optimized for recovering
 * data from a defective optical medium - go through the
 * DeviceHandle related functions in scsi-layer.c in that case.
 */

// fixme: Demand AlignedBuffer instead of void *buf 
int ImageReadSectors(Image* image, void *buf, guint64 first, int n)
{  guint64 first_defect;

   switch(image->type)
   {  case IMAGE_FILE:
        if(!LargeSeek(image->file, first*2048))
	  Stop("ImageReadSectors(): seek failed");
        if(LargeRead(image->file, buf, n*2048) == n*2048)
	{  if(CheckForMissingSectors(buf, first, NULL, 0, n, &first_defect) == SECTOR_PRESENT)
	        return n;
	   else return 0;
	}
	else return 0;
	break;
      case IMAGE_MEDIUM:
	if(!ReadSectorsFast(image->dh, buf, first, n))
	     return n;
	else return 0;
	break;
   }

   return 0;
}


/*
 * Get the md5sum from the specified sector. 
 * Results are cached in the Image as multiple queries may occur.
 */

int GetImageFingerprint(Image *image, guint8 *fp_out, guint64 sector)
{  AlignedBuffer *ab;
   int status;

   /* Sector already cached? */

   if(image->fpSector == sector)
      switch(image->fpState)
      {  case 0:    /* not read */
	    break;
	 case 1:    /* unreadable */
	    if(fp_out) 
	      memset(fp_out, 0, 16);
	    Verbose("GetImageFingerprint(%" PRId64 "): cached unreadable\n", sector);
	    return FALSE;
	 case 2:    /* already cached */
	    if(fp_out)
	      memcpy(fp_out, image->imageFP, 16);
	    Verbose("GetImageFingerprint(%" PRId64 "): cached\n", sector);
	    return TRUE;
      }

     ab = CreateAlignedBuffer(CD_RAW_SECTOR_SIZE);  /* CD-DA media deliver 2352 bytes */
     status = ImageReadSectors(image, ab->buf, sector, 1);
   
     image->fpSector = sector;
     if(status != 1)  /* read error */
     {  image->fpState = 1;
        Verbose("GetImageFingerprint(%" PRId64 "): not readable\n", sector);
     }
     else
     {  struct MD5Context md5ctxt;
   
	image->fpState = 2;

	MD5Init(&md5ctxt);
	MD5Update(&md5ctxt, ab->buf, image->type == IMAGE_MEDIUM ? image->dh->sectorBytes : 2048);
	MD5Final(image->imageFP, &md5ctxt);
	if(fp_out)
	  memcpy(fp_out, image->imageFP, 16);
	Verbose("GetImageFingerprint(%" PRId64 "): read & cached\n", sector);
     }

     FreeAlignedBuffer(ab);

     return image->fpState == 2;
}

/*
 * Truncate the image, update internal structures
 */

int TruncateImage(Image *image, guint64 new_size)
{  int result;
  
   if(image->type != IMAGE_FILE)
     Stop("TruncateImage: called for non-file type image!\n");

   result = LargeTruncate(image->file, (gint64)new_size);

   CalcSectors(image->file->size, &image->sectorSize, &image->inLast);

   return result;
}

/*
 * Close the image
 */

void CloseImage(Image *image)
{
  switch(image->type)
  {  case IMAGE_FILE:
       LargeClose(image->file);
       break;

     case IMAGE_MEDIUM:
       CloseDevice(image->dh);
       break;
  }

  if(image->eccHeader)
     g_free(image->eccHeader);
  if(image->isoInfo)
     FreeIsoInfo(image->isoInfo);
  if(image->eccFile)
    LargeClose(image->eccFile);
  if(image->eccFileHeader)
    g_free(image->eccFileHeader);
  if(image->cachedLayout)
    g_free(image->cachedLayout);

  g_free(image);
}

/*
 * Write a GNU ddrescue compatible mapfile describing which sectors
 * of the image are present (+) or missing (-).
 */

static void write_map_run(FILE *map, guint64 pos, guint64 size, int present)
{  fprintf(map, "0x%08" PRIx64 "  0x%08" PRIx64 "  %c\n", pos, size, present ? '+' : '-');
}

void WriteMapfile(char *image_name, char *map_name)
{  LargeFile *image;
   FILE *map;
   unsigned char *buf;
   guint64 size, sectors, s, run_start = 0;
   int run_present = -1;

   if(!LargeStat(image_name, &size))
     Stop(_("Image file %s not present."), image_name);

   image = LargeOpen(image_name, O_RDONLY, IMG_PERMS);
   if(!image)
     Stop(_("Can't open %s:\n%s"), image_name, strerror(errno));

   map = g_fopen(map_name, "wb");
   if(!map)
   {  LargeClose(image);
      Stop(_("Can't open %s:\n%s"), map_name, strerror(errno));
   }

   fprintf(map, "# Mapfile. Created by dvdisaster\n"
	        "# current_pos  current_status  current_pass\n"
	        "0x00000000     +               1\n"
	        "#      pos        size  status\n");

   buf = g_malloc(64*2048);
   sectors = (size+2047)/2048;

   for(s=0; s<sectors; s+=64)
   {  int i, n = MIN(64, sectors-s);
      int bytes = LargeRead(image, buf, n*2048);

      if(bytes < 0) bytes = 0;
      if(bytes < n*2048)   /* incomplete last sector */
	 memset(buf+bytes, 0, n*2048-bytes);

      for(i=0; i<n; i++)
      {  int present = CheckForMissingSector(buf+2048*i, s+i, NULL, 0) == SECTOR_PRESENT;

	 if(present != run_present)
	 {  if(run_present >= 0)
	       write_map_run(map, run_start*2048, (s+i-run_start)*2048, run_present);
	    run_present = present;
	    run_start = s+i;
	 }
      }
   }

   if(run_present >= 0)
      write_map_run(map, run_start*2048, size-run_start*2048, run_present);

   g_free(buf);
   fclose(map);
   LargeClose(image);

   PrintLog(_("Mapfile written to %s.\n"), map_name);
}

