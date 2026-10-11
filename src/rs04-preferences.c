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

static int active(GtkWidget *widget)
{  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

static void set_redundancy(char *value)
{  g_free(Closure->redundancy);
   Closure->redundancy = value;
}

static void update_sensitivity(RS04Widgets *wl)
{  int fill = active(wl->fillRadio);

   gtk_widget_set_sensitive(wl->fillRadio, Closure->eccTarget == ECC_IMAGE);
   gtk_widget_set_sensitive(wl->mediumAuto, fill);
   gtk_widget_set_sensitive(wl->mediumForce, fill);
   gtk_widget_set_sensitive(wl->mediumManual, fill);
   gtk_widget_set_sensitive(wl->mediumCombo, fill && active(wl->mediumForce));
   gtk_widget_set_sensitive(wl->mediumSpin, fill && active(wl->mediumManual));
   gtk_widget_set_sensitive(wl->rootsScale, active(wl->rootsRadio));
   gtk_widget_set_sensitive(wl->sizeSpin, active(wl->sizeRadio));
   gtk_widget_set_sensitive(wl->offsetSpin, !active(wl->offsetAuto));
}

/*
 * Transfer the widget states into the closure
 */

static void read_redundancy(RS04Widgets *wl)
{  if(active(wl->fillRadio))
   {  set_redundancy(NULL);
      if(active(wl->mediumForce))
	Closure->mediumSize = medium_sizes[MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(wl->mediumCombo)))];
      else if(active(wl->mediumManual))
	Closure->mediumSize = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(wl->mediumSpin));
      else Closure->mediumSize = 0;
      return;
   }

   if(active(wl->normalRadio))
     set_redundancy(g_strdup("normal"));
   else if(active(wl->highRadio))
     set_redundancy(g_strdup("high"));
   else if(active(wl->rootsRadio))
     set_redundancy(g_strdup_printf("%dr", (int)gtk_range_get_value(GTK_RANGE(wl->rootsScale))));
   else set_redundancy(g_strdup_printf("%dm", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(wl->sizeSpin))));

   if(Closure->eccTarget == ECC_IMAGE)  /* a medium size would win */
     Closure->mediumSize = 0;
}

static void changed_cb(GtkWidget *widget, gpointer data)
{  RS04Widgets *wl = (RS04Widgets*)data;

   if(wl->prefsUpdating)
     return;

   /* Radio buttons report the deactivation, too */

   if(GTK_IS_RADIO_BUTTON(widget) && !active(widget))
     return;

   if(widget == wl->eccFile || widget == wl->eccImage)
   {  Closure->eccTarget = active(wl->eccFile) ? ECC_FILE : ECC_IMAGE;
      GuiUpdateMethodPreferences();  /* shows what the target means for the redundancy */
      return;
   }

   if(widget == wl->memorySpin)
     Closure->rs04MemoryMiB = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
   else if(widget == wl->sectorCombo)
     Closure->sectorBytes = sector_sizes[MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(widget)))];
   else if(widget == wl->offsetAuto || widget == wl->offsetSpin)
   {  Closure->audioOffsetGiven = !active(wl->offsetAuto);
      Closure->audioOffset = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(wl->offsetSpin));
   }
   else if(widget == wl->threadsSpin)
     Closure->codecThreads = gtk_range_get_value(GTK_RANGE(widget));
   else read_redundancy(wl);

   update_sensitivity(wl);
   GuiUpdateMethodPreferences();
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

void ResetRS04PrefsPage(Method *method)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;
   char *r = Closure->redundancy;
   int len = r ? strlen(r) : 0;
   GtkWidget *radio;
   unsigned int i;

   if(!wl->fillRadio)
     return;

   wl->prefsUpdating = TRUE;

   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(Closure->eccTarget == ECC_FILE ? wl->eccFile : wl->eccImage), TRUE);

   /* Redundancy */

   if(Closure->eccTarget == ECC_IMAGE && (Closure->mediumSize || !len))
     radio = wl->fillRadio;
   else if(!len || !strcmp(r, "normal"))
     radio = wl->normalRadio;
   else if(!strcmp(r, "high"))
     radio = wl->highRadio;
   else if(r[len-1] == 'm')
   {  radio = wl->sizeRadio;
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(wl->sizeSpin), atoi(r));
   }
   else
   {  int roots = atoi(r);

      if(r[len-1] == '%')  /* nearest number of roots */
      {	 double p = atof(r);
	 roots = (int)round((GF_FIELDMAX*p) / (100.0+p));
      }
      radio = wl->rootsRadio;
      gtk_range_set_value(GTK_RANGE(wl->rootsScale), CLAMP(roots, 8, 170));
   }
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);

   /* Medium size for filling */

   radio = wl->mediumAuto;
   for(i=0; i<N_MEDIA && Closure->mediumSize; i++)
     if(medium_sizes[i] == Closure->mediumSize)
     {  gtk_combo_box_set_active(GTK_COMBO_BOX(wl->mediumCombo), i);
	radio = wl->mediumForce;
     }
   if(Closure->mediumSize && radio == wl->mediumAuto)
   {  gtk_spin_button_set_value(GTK_SPIN_BUTTON(wl->mediumSpin), Closure->mediumSize);
      radio = wl->mediumManual;
   }
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);

   /* Other parameters */

   gtk_spin_button_set_value(GTK_SPIN_BUTTON(wl->memorySpin), Closure->rs04MemoryMiB);
   gtk_range_set_value(GTK_RANGE(wl->threadsSpin), Closure->codecThreads);
   for(i=0; i<3; i++)
     if(sector_sizes[i] == Closure->sectorBytes)
       gtk_combo_box_set_active(GTK_COMBO_BOX(wl->sectorCombo), i);
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wl->offsetAuto), !Closure->audioOffsetGiven);
   gtk_spin_button_set_value(GTK_SPIN_BUTTON(wl->offsetSpin), Closure->audioOffset);

   update_sensitivity(wl);
   wl->prefsUpdating = FALSE;
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

static GtkWidget* add_radio(RS04Widgets *wl, GtkWidget *hbox, GtkWidget *group, char *text)
{  GtkWidget *radio = group ? gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(group), text)
                            : gtk_radio_button_new_with_label(NULL, text);

   g_signal_connect(G_OBJECT(radio), "toggled", G_CALLBACK(changed_cb), wl);
   gtk_box_pack_start(GTK_BOX(hbox), radio, FALSE, FALSE, 0);
   return radio;
}

static GtkWidget* add_spin(RS04Widgets *wl, GtkWidget *hbox, int min, int max, int step, int chars)
{  GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);

   gtk_entry_set_width_chars(GTK_ENTRY(spin), chars);
   g_signal_connect(G_OBJECT(spin), "value-changed", G_CALLBACK(changed_cb), wl);
   gtk_box_pack_start(GTK_BOX(hbox), spin, FALSE, FALSE, 0);
   return spin;
}

static GtkWidget* add_scale(RS04Widgets *wl, GtkWidget *hbox, int min, int max, gpointer format)
{  GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1);

   gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
   gtk_range_set_increments(GTK_RANGE(scale), 1, 1);
   g_signal_connect(scale, "format-value", G_CALLBACK(format_cb), format);
   g_signal_connect(scale, "value-changed", G_CALLBACK(changed_cb), wl);
   gtk_box_pack_start(GTK_BOX(hbox), scale, TRUE, TRUE, 0);
   return scale;
}

static GtkWidget* add_combo(RS04Widgets *wl, GtkWidget *hbox)
{  GtkWidget *combo = gtk_combo_box_text_new();

   g_signal_connect(G_OBJECT(combo), "changed", G_CALLBACK(changed_cb), wl);
   gtk_box_pack_start(GTK_BOX(hbox), combo, FALSE, FALSE, 0);
   return combo;
}

void CreateRS04PrefsPage(Method *method, GtkWidget *parent)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;
   GtkWidget *vbox, *hbox;
   unsigned int i;

   wl->prefsUpdating = TRUE;  /* setting up the widgets must not change the closure */

   /*** Target */

   vbox = add_frame(parent, _utf("Error correction data storage"));
   hbox = add_hbox(vbox, 0);
   add_label(hbox, _utf("Store ECC data in: "));
   wl->eccFile  = add_radio(wl, hbox, NULL, _utf("File"));
   wl->eccImage = add_radio(wl, hbox, wl->eccFile, _utf("Image"));

   /*** Redundancy */

   vbox = add_frame(parent, _utf("Redundancy"));

   hbox = add_hbox(vbox, 0);
   wl->fillRadio = add_radio(wl, hbox, NULL, _utf("Fill the medium:"));
   gtk_widget_set_tooltip_text(wl->fillRadio,
      _("Augmented images only: the error correction data fills the rest of the medium."));

   hbox = add_hbox(vbox, 25);
   wl->mediumAuto  = add_radio(wl, hbox, NULL, _utf("Autodetect (recommended)"));
   wl->mediumForce = add_radio(wl, hbox, wl->mediumAuto, _utf("Force medium type:"));
   wl->mediumCombo = add_combo(wl, hbox);
   for(i=0; i<N_MEDIA; i++)
     gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wl->mediumCombo), medium_names[i]);
   gtk_combo_box_set_active(GTK_COMBO_BOX(wl->mediumCombo), 3);

   hbox = add_hbox(vbox, 25);
   wl->mediumManual = add_radio(wl, hbox, wl->mediumAuto, _utf("Manual sector count (read tooltip!):"));
   gtk_widget_set_tooltip_text(wl->mediumManual,
      _("Capacity of the target medium in sectors of 2048 bytes; "
	"use this only if you know the exact capacity."));
   wl->mediumSpin = add_spin(wl, hbox, 1, 70000000, 1000, 10);
   gtk_spin_button_set_value(GTK_SPIN_BUTTON(wl->mediumSpin), BD_SL_SIZE);
   add_label(hbox, _utf("sectors"));

   hbox = add_hbox(vbox, 0);
   wl->normalRadio = add_radio(wl, hbox, wl->fillRadio, _utf("Normal"));
   wl->highRadio   = add_radio(wl, hbox, wl->fillRadio, _utf("High"));

   hbox = add_hbox(vbox, 0);
   wl->rootsRadio = add_radio(wl, hbox, wl->fillRadio, _utf("Other"));
   wl->rootsScale = add_scale(wl, hbox, 8, 170, NULL);
   gtk_range_set_value(GTK_RANGE(wl->rootsScale), 32);

   hbox = add_hbox(vbox, 0);
   wl->sizeRadio = add_radio(wl, hbox, wl->fillRadio, _utf("Use at most"));
   wl->sizeSpin = add_spin(wl, hbox, 1, 1000000, 100, 8);
   gtk_spin_button_set_value(GTK_SPIN_BUTTON(wl->sizeSpin), 100);
   add_label(hbox, _utf("MiB for error correction data"));

   /*** Memory and threads */

   vbox = add_frame(parent, _utf("Memory utilization"));
   hbox = add_hbox(vbox, 0);
   wl->memorySpin = add_spin(wl, hbox, 1, 1024*1024, 256, 8);
   add_label(hbox, _utf("MiB for encoding and decoding"));
   gtk_widget_set_tooltip_text(wl->memorySpin,
      _("More memory allows longer codewords in new error correction data "
	"and fewer passes over the image while repairing it."));

   vbox = add_frame(parent, _utf("Multithreading"));
   hbox = add_hbox(vbox, 0);
   add_label(hbox, _utf("Use"));
   wl->threadsSpin = add_scale(wl, hbox, 1, MAX(32, Closure->codecThreads), GINT_TO_POINTER(1));

   /*** Image format */

   vbox = add_frame(parent, _utf("Image format"));
   hbox = add_hbox(vbox, 0);
   add_label(hbox, _utf("Sector size:"));
   wl->sectorCombo = add_combo(wl, hbox);
   for(i=0; i<3; i++)
   {  char *text = g_strdup_printf(_utf("%" PRId64 " bytes"), (gint64)sector_sizes[i]);

      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wl->sectorCombo), text);
      g_free(text);
   }
   gtk_widget_set_tooltip_text(wl->sectorCombo,
      _("2048 bytes for ISO images; 2352 bytes for audio and raw CD images (.bin)."));

   hbox = add_hbox(vbox, 0);
   add_label(hbox, _utf("Read offset of audio images (samples):"));
   wl->offsetAuto = gtk_check_button_new_with_label(_utf("auto"));
   g_signal_connect(G_OBJECT(wl->offsetAuto), "toggled", G_CALLBACK(changed_cb), wl);
   gtk_box_pack_start(GTK_BOX(hbox), wl->offsetAuto, FALSE, FALSE, 0);
   wl->offsetSpin = add_spin(wl, hbox, -5880, 5880, 1, 6);

   ResetRS04PrefsPage(method);  /* clears prefsUpdating */
}

#endif /* WITH_GUI_YES */
