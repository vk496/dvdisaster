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

/*** src type: only GUI code ***/

#ifdef WITH_GUI_YES
#include "dvdisaster.h"

#include "rs04-includes.h"

/*
 * The settings are shared with the other codecs and the command line:
 * Closure->eccTarget, ->redundancy (NULL, normal, high, <n>r, <n>m),
 * ->mediumSize and ->codecThreads. For augmented images, a medium size
 * or an empty redundancy means "fill the medium" (see rs04-create.c).
 */

static gint64 medium_sizes[] =
{  CDR_SIZE, DVD_SL_SIZE, DVD_DL_SIZE, BD_SL_SIZE, BD_DL_SIZE, BDXL_TL_SIZE, BDXL_QL_SIZE,
   BD_SL_SIZE_NODM, BD_DL_SIZE_NODM, BDXL_TL_SIZE_NODM, BDXL_QL_SIZE_NODM
};
static char *medium_names[] =
{  "CD", "DVD", "DVD9 (dual layer)", "BD (25 GB)", "BD DL (50 GB)", "BDXL (100 GB)", "BDXL (128 GB)",
   "BD no DM (25 GB)", "BD DL no DM (50 GB)", "BDXL no DM (100 GB)", "BDXL no DM (128 GB)"
};
#define N_MEDIA (sizeof(medium_sizes)/sizeof(gint64))

static int sector_sizes[] = { 2048, 2324, 2352 };

static int updating;  /* setting the widgets must not change the closure */

static int active(GtkWidget *widget)
{  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

static void set_redundancy(char *value)
{  g_free(Closure->redundancy);
   Closure->redundancy = value;
}

static void update_sensitivity(RS04Prefs *p)
{  int fill = active(p->fillRadio);

   gtk_widget_set_sensitive(p->fillRadio, Closure->eccTarget == ECC_IMAGE);
   gtk_widget_set_sensitive(p->mediumAuto, fill);
   gtk_widget_set_sensitive(p->mediumForce, fill);
   gtk_widget_set_sensitive(p->mediumManual, fill);
   gtk_widget_set_sensitive(p->mediumCombo, fill && active(p->mediumForce));
   gtk_widget_set_sensitive(p->mediumSpin, fill && active(p->mediumManual));
   gtk_widget_set_sensitive(p->rootsScale, active(p->rootsRadio));
   gtk_widget_set_sensitive(p->sizeSpin, active(p->sizeRadio));
   gtk_widget_set_sensitive(p->offsetSpin, !active(p->offsetAuto));
}

/*
 * Transfer the widget states into the closure
 */

static void read_redundancy(RS04Prefs *p)
{  if(active(p->fillRadio))
   {  set_redundancy(NULL);
      if(active(p->mediumForce))
	Closure->mediumSize = medium_sizes[MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(p->mediumCombo)))];
      else if(active(p->mediumManual))
	Closure->mediumSize = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->mediumSpin));
      else Closure->mediumSize = 0;
      return;
   }

   if(active(p->normalRadio))
     set_redundancy(g_strdup("normal"));
   else if(active(p->highRadio))
     set_redundancy(g_strdup("high"));
   else if(active(p->rootsRadio))
     set_redundancy(g_strdup_printf("%dr", (int)gtk_range_get_value(GTK_RANGE(p->rootsScale))));
   else set_redundancy(g_strdup_printf("%dm", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->sizeSpin))));

   if(Closure->eccTarget == ECC_IMAGE)  /* a medium size would win */
     Closure->mediumSize = 0;
}

static void changed_cb(GtkWidget *widget, gpointer data)
{  RS04Prefs *p = (RS04Prefs*)data;

   if(updating)
     return;

   /* Radio buttons report the deactivation, too */

   if(GTK_IS_RADIO_BUTTON(widget) && !active(widget))
     return;

   if(widget == p->eccFile || widget == p->eccImage)
   {  Closure->eccTarget = active(p->eccFile) ? ECC_FILE : ECC_IMAGE;
      GuiUpdateMethodPreferences();  /* shows what the target means for the redundancy */
      return;
   }

   if(widget == p->memorySpin)
     Closure->rs04MemoryMiB = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
   else if(widget == p->sectorCombo)
     Closure->sectorBytes = sector_sizes[MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(widget)))];
   else if(widget == p->offsetAuto || widget == p->offsetSpin)
   {  Closure->audioOffsetGiven = !active(p->offsetAuto);
      Closure->audioOffset = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->offsetSpin));
   }
   else if(widget == p->threadsSpin)
     Closure->codecThreads = gtk_range_get_value(GTK_RANGE(widget));
   else read_redundancy(p);

   GuiUpdateMethodPreferences();  /* also updates the other copy */
}

static gchar* format_cb(GtkScale *scale, gdouble value, gpointer data)
{  int nroots = value;
   char *label;

   if(data)
        label = g_strdup_printf(_utf("%d threads"), nroots);
   else label = g_strdup_printf(_utf("%4.1f%% redundancy (%d roots)"),
				((double)nroots*100.0)/(double)(GF_FIELDMAX - nroots), nroots);

   FORGET(label);  /* will be g_free()ed by the scale */
   return label;
}

/*
 * Show the closure values; they may have been changed by the other codecs
 */

static void reset_prefs(RS04Prefs *p)
{  char *r = Closure->redundancy;
   int len = r ? strlen(r) : 0;
   GtkWidget *radio;
   unsigned int i;

   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(Closure->eccTarget == ECC_FILE ? p->eccFile : p->eccImage), TRUE);

   /* Redundancy */

   if(Closure->eccTarget == ECC_IMAGE && (Closure->mediumSize || !len))
     radio = p->fillRadio;
   else if(!len || !strcmp(r, "normal"))
     radio = p->normalRadio;
   else if(!strcmp(r, "high"))
     radio = p->highRadio;
   else if(r[len-1] == 'm')
   {  radio = p->sizeRadio;
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->sizeSpin), atoi(r));
   }
   else
   {  int roots = atoi(r);

      if(r[len-1] == '%')  /* nearest number of roots */
      {	 double p = atof(r);
	 roots = (int)round((GF_FIELDMAX*p) / (100.0+p));
      }
      radio = p->rootsRadio;
      gtk_range_set_value(GTK_RANGE(p->rootsScale), CLAMP(roots, 8, 170));
   }
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);

   /* Medium size for filling */

   radio = p->mediumAuto;
   for(i=0; i<N_MEDIA && Closure->mediumSize; i++)
     if(medium_sizes[i] == Closure->mediumSize)
     {  gtk_combo_box_set_active(GTK_COMBO_BOX(p->mediumCombo), i);
	radio = p->mediumForce;
     }
   if(Closure->mediumSize && radio == p->mediumAuto)
   {  gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->mediumSpin), Closure->mediumSize);
      radio = p->mediumManual;
   }
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);

   /* Other parameters */

   gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->memorySpin), Closure->rs04MemoryMiB);
   gtk_range_set_value(GTK_RANGE(p->threadsSpin), Closure->codecThreads);
   for(i=0; i<3; i++)
     if(sector_sizes[i] == Closure->sectorBytes)
       gtk_combo_box_set_active(GTK_COMBO_BOX(p->sectorCombo), i);
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->offsetAuto), !Closure->audioOffsetGiven);
   gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->offsetSpin), Closure->audioOffset);

   update_sensitivity(p);
}

void ResetRS04PrefsPage(Method *method)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;

   if(!wl->prefs[0].fillRadio)
     return;

   updating = TRUE;
   reset_prefs(&wl->prefs[0]);
   reset_prefs(&wl->prefs[1]);
   updating = FALSE;
}

/*
 * Create the preferences page
 */

static GtkWidget* add_frame(GtkWidget *parent, char *title)
{  GtkWidget *frame = gtk_frame_new(title);
   GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

   gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
   gtk_container_add(GTK_CONTAINER(frame), vbox);
   gtk_box_pack_start(GTK_BOX(parent), frame, FALSE, FALSE, 0);
   return vbox;
}

static GtkWidget* add_hbox(GtkWidget *vbox, int indent)
{  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

   gtk_widget_set_margin_start(hbox, indent);
   gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
   return hbox;
}

static void add_label(GtkWidget *hbox, char *text)
{  gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(text), FALSE, FALSE, 0);
}

static GtkWidget* add_radio(RS04Prefs *p, GtkWidget *hbox, GtkWidget *group, char *text)
{  GtkWidget *radio;

   if(text)
        radio = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(group), text);
   else radio = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(group));

   g_signal_connect(G_OBJECT(radio), "toggled", G_CALLBACK(changed_cb), p);
   gtk_box_pack_start(GTK_BOX(hbox), radio, FALSE, FALSE, 0);
   return radio;
}

static GtkWidget* add_spin(RS04Prefs *p, GtkWidget *hbox, int min, int max, int step, int chars)
{  GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);

   gtk_entry_set_width_chars(GTK_ENTRY(spin), chars);
   g_signal_connect(G_OBJECT(spin), "value-changed", G_CALLBACK(changed_cb), p);
   gtk_box_pack_start(GTK_BOX(hbox), spin, FALSE, FALSE, 0);
   return spin;
}

static GtkWidget* add_scale(RS04Prefs *p, GtkWidget *hbox, int min, int max, gpointer format)
{  GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1);

   gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
   gtk_range_set_increments(GTK_RANGE(scale), 1, 1);
   g_signal_connect(scale, "format-value", G_CALLBACK(format_cb), format);
   g_signal_connect(scale, "value-changed", G_CALLBACK(changed_cb), p);
   gtk_box_pack_start(GTK_BOX(hbox), scale, TRUE, TRUE, 0);
   return scale;
}

static GtkWidget* add_combo(RS04Prefs *p, GtkWidget *hbox)
{  GtkWidget *combo = gtk_combo_box_text_new();

   g_signal_connect(G_OBJECT(combo), "changed", G_CALLBACK(changed_cb), p);
   gtk_box_pack_start(GTK_BOX(hbox), combo, FALSE, FALSE, 0);
   return combo;
}

/* Each setting is created twice: i=0 on the preferences page,
   i=1 in its help window */

static LabelWithOnlineHelp* add_help(char *title, char *link)
{  LabelWithOnlineHelp *lwoh = GuiCreateLabelWithOnlineHelp(title, link);

   GuiRegisterPreferencesHelpWindow(lwoh);
   return lwoh;
}

static GtkWidget* help_vbox(LabelWithOnlineHelp *lwoh, GtkWidget *page, int i)
{  GtkWidget *vbox;

   if(!i) return page;

   vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
   GuiAddHelpWidget(lwoh, vbox);
   return vbox;
}

static void add_link(GtkWidget *hbox, LabelWithOnlineHelp *lwoh, int i)
{  gtk_box_pack_start(GTK_BOX(hbox), i ? lwoh->normalLabel : lwoh->linkBox, FALSE, FALSE, 0);
   if(!i) gtk_box_pack_start(GTK_BOX(hbox), lwoh->tooltip, FALSE, FALSE, 0);
}

void CreateRS04PrefsPage(Method *method, GtkWidget *parent)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;
   LabelWithOnlineHelp *lwoh, *lwoh2;
   GtkWidget *page, *vbox, *hbox;
   RS04Prefs *p;
   unsigned int i, j;

   updating = TRUE;  /* cleared by ResetRS04PrefsPage() */

   /*** Target */

   page = add_frame(parent, _utf("Error correction data storage"));
   lwoh = add_help(_("Error correction data storage"), _("Store ECC data in: "));

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      hbox = add_hbox(help_vbox(lwoh, page, i), 0);
      add_link(hbox, lwoh, i);
      p->eccFile  = add_radio(p, hbox, NULL, _utf("File"));
      p->eccImage = add_radio(p, hbox, p->eccFile, _utf("Image"));
   }

   GuiAddHelpParagraph(lwoh,
		       _("<b>Error correction data storage</b>\n\n"
			 "Select between two ways of storing the "
			 "error correction information:\n"));

   GuiAddHelpListItem(lwoh, _("Augmented image (recommended)\n"
      "The error correction data will be stored along with the user data on the "
      "same medium. This requires the creation of an image file prior to writing the "
      "medium. The error correction data will be appended to that image "
      "and fill up the remaining space.\n"
      "Damaged sectors in the error correction "
      "information reduce the data recovery capacity, but do not make recovery "
      "impossible - a second medium for keeping or protecting the error correction "
      "information is not required.\n"));

   GuiAddHelpListItem(lwoh, _("Error correction file\n"
      "Error correction files are the only way of protecting existing media "
      "as they can be stored somewhere else. They are kept on a separate "
      "medium which must also be protected by dvdisaster. This prevents from losing the "
      "error correction files in case of a medium defect.\n"));

   /*** Redundancy: filling the medium */

   page = add_frame(parent, _utf("Redundancy"));
   lwoh = add_help(_("Target medium for augmented image"), _("Fill the medium:"));

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      vbox = help_vbox(lwoh, page, i);

      hbox = add_hbox(vbox, 0);
      p->fillRadio = add_radio(p, hbox, NULL, NULL);
      add_link(hbox, lwoh, i);

      hbox = add_hbox(vbox, 25);
      p->mediumAuto  = add_radio(p, hbox, NULL, _utf("Autodetect (recommended)"));
      p->mediumForce = add_radio(p, hbox, p->mediumAuto, _utf("Force medium type:"));
      p->mediumCombo = add_combo(p, hbox);
      for(j=0; j<N_MEDIA; j++)
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->mediumCombo), medium_names[j]);
      gtk_combo_box_set_active(GTK_COMBO_BOX(p->mediumCombo), 3);

      hbox = add_hbox(vbox, 25);
      p->mediumManual = add_radio(p, hbox, p->mediumAuto, _utf("Manual sector count:"));
      p->mediumSpin = add_spin(p, hbox, 1, 70000000, 1000, 10);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->mediumSpin), BD_SL_SIZE);
      add_label(hbox, _utf("sectors"));
   }

   GuiAddHelpParagraph(lwoh, _("<b>Target medium for augmented image</b>\n\n"
      "Augmented images only: the error correction data fills up the space "
      "left on the medium, so that the image receives as much redundancy as "
      "the medium can hold (up to 200%%).\n\n"
      "<i>Autodetect</i> selects the smallest medium (CD, DVD, DVD9, BD, BD DL or BDXL) "
      "which holds the image together with a minimal amount of error correction data.\n"
      "<i>Force medium type</i> selects the medium yourself, e.g. a BD for a small image "
      "which will be burned to a BD anyway. The \"no DM\" types are for BD-R discs "
      "burned with defect management turned off.\n"
      "<i>Manual sector count</i> specifies the capacity of the medium in sectors of "
      "2048 bytes. Use it only if you know the exact capacity of your medium.\n\n"
      "Unlike RS02 and RS03, RS04 does not need to know the medium size for "
      "verifying or repairing the image later."));

   /*** Redundancy: presets; "Normal" and "High" share a help window */

   lwoh  = add_help(_("Redundancy"), _("Normal"));
   lwoh2 = GuiCloneLabelWithOnlineHelp(lwoh, _("High"));
   GuiRegisterPreferencesHelpWindow(lwoh2);

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      hbox = add_hbox(help_vbox(lwoh, page, i), 0);
      p->normalRadio = add_radio(p, hbox, p->fillRadio, NULL);
      gtk_box_pack_start(GTK_BOX(hbox), i ? lwoh->normalLabel : lwoh->linkBox, FALSE, FALSE, 0);
      p->highRadio = add_radio(p, hbox, p->fillRadio, NULL);
      add_link(hbox, lwoh2, i);  /* shares the tooltip image with lwoh */
   }

   GuiAddHelpParagraph(lwoh, _("<b>Normal and high redundancy</b>\n\n"
      "The preset \"normal\" creates a redundancy of 14.3%%, "
      "the preset \"high\" one of 33.5%%.\n"
      "For augmented images, the error correction data is appended "
      "to the image without filling up the medium."));

   /*** Redundancy: number of roots */

   lwoh = add_help(_("Other redundancy"), _("Other"));

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      hbox = add_hbox(help_vbox(lwoh, page, i), 0);
      p->rootsRadio = add_radio(p, hbox, p->fillRadio, NULL);
      add_link(hbox, lwoh, i);
      p->rootsScale = add_scale(p, hbox, 8, 170, NULL);
      gtk_range_set_value(GTK_RANGE(p->rootsScale), 32);
   }

   GuiAddHelpParagraph(lwoh, _("<b>Other redundancy</b>\n\n"
      "Specifies the redundancy by percent.\n"
      "An error correction file with x%% redundancy "
      "will be approximately x%% of the size of the "
      "corresponding image file."));

   /*** Redundancy: size limit */

   lwoh = add_help(_("Space-delimited redundancy"), _("Use at most"));

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      hbox = add_hbox(help_vbox(lwoh, page, i), 0);
      p->sizeRadio = add_radio(p, hbox, p->fillRadio, NULL);
      add_link(hbox, lwoh, i);
      p->sizeSpin = add_spin(p, hbox, 1, 1000000, 100, 8);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->sizeSpin), 100);
      add_label(hbox, _utf("MiB for error correction data"));
   }

   GuiAddHelpParagraph(lwoh, _("<b>Space-delimited redundancy</b>\n\n"
      "Specifies the maximum size of the error correction file in MiB. "
      "dvdisaster will choose a suitable redundancy setting so that "
      "the overall size of the error correction file does not exceed "
      "the given limit.\n\n"
      "<b>Advance notice:</b> When using the same size setting for "
      "images of vastly different size, smaller images receive more "
      "redundancy than larger ones. This is usually not what you want."));

   /*** Memory */

   page = add_frame(parent, _utf("Memory utilization"));
   lwoh = add_help(_("Memory utilization"), _("MiB for encoding and decoding"));

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      hbox = add_hbox(help_vbox(lwoh, page, i), 0);
      p->memorySpin = add_spin(p, hbox, 1, 1024*1024, 256, 8);
      add_link(hbox, lwoh, i);
   }

   GuiAddHelpParagraph(lwoh, _("<b>Memory utilization</b>\n\n"
      "RS04 works on many long codewords at the same time.\n"
      "When creating error correction data, the memory limits the length of the "
      "codewords: Longer codewords survive more sectors lost at random places "
      "all over the medium, while shorter ones need less memory for repairing "
      "the image. Do not use more memory than your computer has available.\n\n"
      "Any RS04 data can be repaired with less memory than was used for creating it. "
      "Repairing then needs more passes over the image, which is slower, "
      "especially for images stored on a hard disk."));

   /*** Threads */

   page = add_frame(parent, _utf("Multithreading"));
   lwoh = add_help(_("Multithreading"), _("Use"));

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      hbox = add_hbox(help_vbox(lwoh, page, i), 0);
      add_link(hbox, lwoh, i);
      p->threadsSpin = add_scale(p, hbox, 1, MAX(32, Closure->codecThreads), GINT_TO_POINTER(1));
   }

   GuiAddHelpParagraph(lwoh, _("<b>Multithreading</b>\n\n"
      "RS04 can use multiple threads (and therefore CPU cores) "
      "for creating error correction data and for repairing images.\n"
      "Set the number of threads to the number of CPU cores. "
      "Hard disk speed and memory bandwidth may limit the performance "
      "more than the number of cores."));

   /*** Sector size */

   page = add_frame(parent, _utf("Image format"));
   lwoh = add_help(_("Sector size"), _("Sector size:"));

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      hbox = add_hbox(help_vbox(lwoh, page, i), 0);
      add_link(hbox, lwoh, i);
      p->sectorCombo = add_combo(p, hbox);
      for(j=0; j<3; j++)
      {  char *text = g_strdup_printf(_utf("%" PRId64 " bytes"), (gint64)sector_sizes[j]);

	 gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->sectorCombo), text);
	 g_free(text);
      }
   }

   GuiAddHelpParagraph(lwoh, _("<b>Sector size</b>\n\n"
      "Size of the sectors in the image when creating error correction data:\n"));
   GuiAddHelpListItem(lwoh, _("2048 bytes\n"
      "ISO images of data CDs, DVDs and BDs. Use this for all images "
      "except the following ones.\n"));
   GuiAddHelpListItem(lwoh, _("2324 bytes\n"
      "Mode 2 Form 2 tracks, e.g. the video tracks of Video CDs and "
      "Super Video CDs extracted by other programs.\n"));
   GuiAddHelpListItem(lwoh, _("2352 bytes\n"
      "Images of audio CDs (.bin), and of mixed mode discs read with "
      "the \"CD-DA images\" option.\n"));
   GuiAddHelpParagraph(lwoh, _("The sector size is stored in the error correction "
      "data; verifying and repairing do not use this setting."));

   /*** Read offset */

   lwoh = add_help(_("Read offset of audio images"), _("Read offset of audio images (samples):"));

   for(i=0; i<2; i++)
   {  p = &wl->prefs[i];
      hbox = add_hbox(help_vbox(lwoh, page, i), 0);
      add_link(hbox, lwoh, i);
      p->offsetAuto = gtk_check_button_new_with_label(_utf("auto"));
      g_signal_connect(G_OBJECT(p->offsetAuto), "toggled", G_CALLBACK(changed_cb), p);
      gtk_box_pack_start(GTK_BOX(hbox), p->offsetAuto, FALSE, FALSE, 0);
      p->offsetSpin = add_spin(p, hbox, -5880, 5880, 1, 6);
   }

   GuiAddHelpParagraph(lwoh, _("<b>Read offset of audio images</b>\n\n"
      "Drives deliver the data of audio CDs shifted by a few samples, and this "
      "read offset differs between drive models. An image read with another "
      "drive does not match its error correction file sector by sector.\n"
      "With <i>auto</i>, dvdisaster determines the offset of the image against "
      "the image the error correction file was created from, and shifts the "
      "image accordingly when repairing it.\n\n"
      "Only if this fails, e.g. because the image is badly damaged, turn off "
      "<i>auto</i> and enter the offset in samples of 4 bytes. "
      "The setting is only used with error correction files "
      "for images with 2352 bytes per sector."));

   ResetRS04PrefsPage(method);
}

#endif /* WITH_GUI_YES */
