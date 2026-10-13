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

static RS04Widgets* widgets(void)
{  return (RS04Widgets*)FindMethod("RS04")->widgetList;
}

/* Head line; returns the label which is packed into a horizontal box */

static GtkWidget* add_headline(GtkWidget *parent)
{  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   GtkWidget *label = gtk_label_new(NULL);

   gtk_label_set_xalign(GTK_LABEL(label), 0.0);
   gtk_widget_set_margin_start(label, 5);
   gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
   gtk_box_pack_start(GTK_BOX(parent), hbox, FALSE, FALSE, 3);
   gtk_box_pack_start(GTK_BOX(parent), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
   gtk_box_pack_start(GTK_BOX(parent), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

   return label;
}

static GtkWidget* grid_label(GtkWidget *grid, char *markup, float xalign, int x, int y, int w)
{  GtkWidget *label = gtk_label_new(NULL);

   if(markup) gtk_label_set_markup(GTK_LABEL(label), markup);
   gtk_label_set_xalign(GTK_LABEL(label), xalign);
   gtk_grid_attach(GTK_GRID(grid), label, x, y, w, 1);
   return label;
}

void FreeRS04Widgets(RS04Widgets *wl)
{  int i,j;

   if(!wl) return;

   GuiFreeCurve(wl->fixCurve);
   GuiFreeSpiral(wl->cmpSpiral);
   if(wl->cmpLayout)
     g_object_unref(wl->cmpLayout);
   for(i=0; i<RS04_MAX_SETS; i++)
     for(j=0; j<RS04_V_FIELDS; j++)
       g_free(wl->cmpText[i][j]);
}

/***
 *** Encoding window
 ***/

void ResetRS04EncWindow(Method *method)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;

   GuiSetProgress(wl->encPBar1, 0, 100);
   GuiSetProgress(wl->encPBar2, 0, 100);

   gtk_widget_hide(wl->encLabel2);
   gtk_widget_hide(wl->encPBar2);
   gtk_widget_hide(wl->encLabel3);
   gtk_widget_hide(wl->encInfo);
   gtk_widget_hide(wl->encLabel4);
   gtk_widget_hide(wl->encPerformance);

   gtk_label_set_text(GTK_LABEL(wl->encHeadline), "");
   gtk_label_set_text(GTK_LABEL(wl->encFootline), "");
   gtk_label_set_text(GTK_LABEL(wl->encFootline2), "");
}

static GtkWidget* add_pbar(GtkWidget *grid, int y)
{  GtkWidget *pbar = gtk_progress_bar_new();

   gtk_widget_set_valign(pbar, GTK_ALIGN_CENTER);
   gtk_widget_set_hexpand(pbar, TRUE);
   gtk_grid_attach(GTK_GRID(grid), pbar, 2, y, 1, 1);
   return pbar;
}

static GtkWidget* add_footline(GtkWidget *parent)
{  GtkWidget *label = gtk_label_new(NULL);

   gtk_label_set_xalign(GTK_LABEL(label), 0.0);
   gtk_widget_set_margin_start(label, 20);
   gtk_box_pack_start(GTK_BOX(parent), label, FALSE, FALSE, 3);
   return label;
}

void CreateRS04EncWindow(Method *method, GtkWidget *parent)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;
   GtkWidget *grid, *lab;

   wl->encHeadline = add_headline(parent);

   grid = gtk_grid_new();
   gtk_widget_set_margin_start(grid, 20);
   gtk_widget_set_margin_end(grid, 20);
   gtk_widget_set_margin_top(grid, 5);
   gtk_widget_set_margin_bottom(grid, 5);
   gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
   gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
   gtk_box_pack_start(GTK_BOX(parent), grid, FALSE, FALSE, 30);

   lab = grid_label(grid, _utf("<b>1. Calculating image sector checksums:</b>"), 0.0, 1, 1, 1);
   gtk_widget_set_margin_top(lab, 15);
   gtk_widget_set_margin_bottom(lab, 15);
   wl->encPBar1 = add_pbar(grid, 1);

   lab = wl->encLabel2 = grid_label(grid, _utf("<b>2. Creating error correction data:</b>"), 0.0, 1, 2, 1);
   gtk_widget_set_margin_top(lab, 15);
   gtk_widget_set_margin_bottom(lab, 15);
   wl->encPBar2 = add_pbar(grid, 2);

   wl->encLabel3 = grid_label(grid, _utf("<b>Encoder info:</b>"), 1.0, 1, 3, 1);
   wl->encInfo = grid_label(grid, NULL, 0.0, 2, 3, 1);
   wl->encLabel4 = grid_label(grid, _utf("<b>Performance:</b>"), 1.0, 1, 4, 1);
   wl->encPerformance = grid_label(grid, NULL, 0.0, 2, 4, 1);

   wl->encFootline  = add_footline(parent);
   wl->encFootline2 = add_footline(parent);
}

/***
 *** Fix window
 ***/

static gboolean redraw_idle_func(gpointer data)
{  RS04Widgets *wl = (RS04Widgets*)data;

   gtk_widget_queue_draw(wl->fixCurve->widget);
   return FALSE;
}

/* Correction capacity of the current set; the y axis covers all sets */

void RS04SetFixMaxValues(int ecc_max)
{  RS04Widgets *wl = widgets();
   int max_y = ecc_max - (ecc_max % 5) + 5;

   if(!Closure->guiMode) return;

   wl->eccMax = ecc_max;
   wl->fixCurve->maxX = 100;
   if(max_y > wl->fixCurve->maxY)
     wl->fixCurve->maxY = max_y;

   g_idle_add(redraw_idle_func, wl);
}

/* Erasures of one codeword which covers positions from..to (1..1000) */

void RS04AddFixValue(int from, int to, int erasures)
{  RS04Widgets *wl = widgets();
   int i;

   if(!Closure->guiMode)
     return;

   erasures = MIN(erasures, wl->fixCurve->maxY);
   for(i=MAX(from, 1); i<=MIN(to, 1000); i++)
     if(erasures > wl->fixCurve->ivalue[i])
       wl->fixCurve->ivalue[i] = erasures;
}

static gboolean results_idle_func(gpointer data)
{  RS04Widgets *wl = (RS04Widgets*)data;

   GuiSetLabelText(wl->fixCorrected, _("Repaired: %" PRId64 ""), wl->corrected);
   GuiSetLabelText(wl->fixUncorrected, _("Unrepairable: <span %s>%" PRId64 "</span>"),
		   Closure->redMarkup, wl->uncorrected);
   GuiSetLabelText(wl->fixProgress, _("Progress: %3d.%1d%%"), wl->percent/10, wl->percent%10);
   gtk_widget_queue_draw(wl->fixCurve->widget);

   return FALSE;
}

void RS04UpdateFixResults(int permille, gint64 corrected, gint64 uncorrected)
{  RS04Widgets *wl = widgets();

   if(!Closure->guiMode) return;

   wl->percent = CLAMP(permille, 0, 1000);
   wl->corrected = corrected;
   wl->uncorrected = uncorrected;
   g_idle_add(results_idle_func, wl);
}

static gboolean fix_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{  RS04Widgets *wl = (RS04Widgets*)data;
   Curve *curve = wl->fixCurve;
   char max_label[16];
   int y;

   /* Geometry */

   g_snprintf(max_label, sizeof(max_label), "%d", curve->maxY);
   GuiUpdateCurveGeometry(curve, max_label, 20);
   gtk_box_set_child_packing(GTK_BOX(wl->fixFootlineBox), wl->fixCorrected,
			     TRUE, TRUE, curve->leftX, GTK_PACK_START);
   gtk_box_set_child_packing(GTK_BOX(wl->fixFootlineBox), wl->fixUncorrected,
			     TRUE, TRUE, curve->leftX, GTK_PACK_START);

   /* Curve and the correction capacity */

   GuiRedrawAxes(cr, curve);
   GuiRedrawCurve(cr, curve, wl->percent);

   if(wl->eccMax)
   {  y = GuiCurveY(curve, wl->eccMax);
      gdk_cairo_set_source_rgba(cr, Closure->greenSector);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, curve->leftX-5.5, y+0.5);
      cairo_line_to(cr, curve->rightX+5.5, y+0.5);
      cairo_stroke(cr);
   }

   return TRUE;
}

void ResetRS04FixWindow(Method *method)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;

   gtk_notebook_set_current_page(GTK_NOTEBOOK(wl->fixNotebook), 0);
   gtk_label_set_text(GTK_LABEL(wl->fixHeadline), "");

   GuiZeroCurve(wl->fixCurve);
   wl->fixCurve->maxY = 5;
   wl->eccMax = 0;
   RS04UpdateFixResults(0, 0, 0);
}

void CreateRS04FixWindow(Method *method, GtkWidget *parent)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;
   GtkWidget *d_area, *notebook, *box;

   wl->fixHeadline = add_headline(parent);

   d_area = wl->fixDrawingArea = gtk_drawing_area_new();
   gtk_box_pack_start(GTK_BOX(parent), d_area, TRUE, TRUE, 0);
   g_signal_connect(G_OBJECT(d_area), "draw", G_CALLBACK(fix_draw_cb), (gpointer)wl);

   notebook = wl->fixNotebook = gtk_notebook_new();
   gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
   gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
   gtk_box_pack_end(GTK_BOX(parent), notebook, FALSE, FALSE, 0);

   box = wl->fixFootlineBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_box_set_homogeneous(GTK_BOX(box), TRUE);

   wl->fixCorrected = gtk_label_new(NULL);
   gtk_label_set_xalign(GTK_LABEL(wl->fixCorrected), 0.0);
   gtk_box_pack_start(GTK_BOX(box), wl->fixCorrected, TRUE, TRUE, 0);

   wl->fixProgress = gtk_label_new(NULL);
   gtk_box_pack_start(GTK_BOX(box), wl->fixProgress, TRUE, TRUE, 0);

   wl->fixUncorrected = gtk_label_new(NULL);
   gtk_label_set_xalign(GTK_LABEL(wl->fixUncorrected), 1.0);
   gtk_box_pack_start(GTK_BOX(box), wl->fixUncorrected, TRUE, TRUE, 0);

   gtk_notebook_append_page(GTK_NOTEBOOK(notebook), box, gtk_label_new("progress_tab"));

   wl->fixFootline = gtk_label_new(NULL);
   gtk_label_set_xalign(GTK_LABEL(wl->fixFootline), 0.0);
   gtk_widget_set_margin_start(wl->fixFootline, 5);
   gtk_notebook_append_page(GTK_NOTEBOOK(notebook), wl->fixFootline, gtk_label_new("footer_tab"));

   wl->fixCurve = GuiCreateCurve(d_area, _("Errors/Ecc block"), "%d", 1000, CURVE_PERCENT);
   wl->fixCurve->enable = DRAW_ICURVE;
}

/***
 *** Verify window
 ***/

/*
 * Per-set results. They are kept for all sets so that the user
 * can switch between them; only the main thread touches them.
 */

typedef struct
{  int set, field;
   char *text;
} field_info;

/* The tooltip shows values which are cut off in narrow windows */

static void set_field(GtkWidget *label, char *text)
{  gtk_label_set_markup(GTK_LABEL(label), text ? text : "");
   gtk_widget_set_tooltip_markup(label, text && *text ? text : NULL);
}

static void show_set(RS04Widgets *wl)
{  int i;

   for(i=0; i<RS04_V_FIELDS; i++)
     set_field(wl->cmpField[i], wl->cmpText[wl->cmpShown][i]);
}

static gboolean field_idle_func(gpointer data)
{  field_info *fi = (field_info*)data;
   RS04Widgets *wl = widgets();

   g_free(wl->cmpText[fi->set][fi->field]);
   wl->cmpText[fi->set][fi->field] = fi->text;
   if(fi->set == wl->cmpShown)
     set_field(wl->cmpField[fi->field], fi->text);

   g_free(fi);
   return FALSE;
}

void RS04VerifyField(int set, int field, char *format, ...)
{  field_info *fi;
   va_list argp;
   char *text;

   if(!Closure->guiMode || set < 0 || set >= RS04_MAX_SETS)
     return;

   va_start(argp, format);
   text = g_strdup_vprintf(format, argp);
   va_end(argp);

   fi = g_malloc(sizeof(field_info));
   fi->set = set;
   fi->field = field;
   fi->text = g_locale_to_utf8(text, -1, NULL, NULL, NULL);
   g_free(text);

   g_idle_add(field_idle_func, fi);
}

static void set_combo_cb(GtkWidget *widget, gpointer data)
{  RS04Widgets *wl = (RS04Widgets*)data;
   int active = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));

   if(active < 0 || active >= wl->cmpSets) return;

   wl->cmpShown = active;
   show_set(wl);
}

static gboolean show_set_idle_func(gpointer data)
{  RS04Widgets *wl = widgets();
   int set = GPOINTER_TO_INT(data);

   if(set < wl->cmpSets)
     gtk_combo_box_set_active(GTK_COMBO_BOX(wl->cmpSetCombo), set);

   return FALSE;
}

void RS04VerifyShowSet(int set)
{  if(Closure->guiMode)
     g_idle_add(show_set_idle_func, GINT_TO_POINTER(set));
}

/*
 * Set selection and the image spiral
 */

typedef struct
{  int nSets, imagePresent;
   char *entry[RS04_MAX_SETS];
} init_info;

static gboolean init_idle_func(gpointer data)
{  init_info *ii = (init_info*)data;
   RS04Widgets *wl = widgets();
   int i;

   wl->cmpSets = ii->nSets;
   for(i=0; i<ii->nSets; i++)
   {  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wl->cmpSetCombo), ii->entry[i]);
      g_free(ii->entry[i]);
   }
   gtk_widget_set_visible(wl->cmpSetCombo, ii->nSets > 1);
   gtk_notebook_set_current_page(GTK_NOTEBOOK(wl->cmpImageNotebook), ii->imagePresent ? 1 : 0);

   g_free(ii);
   return FALSE;
}

static char *verify_headline;

void RS04VerifyInit(RS04Info *info, Image *image)
{  RS04Widgets *wl = widgets();
   init_info *ii;
   gint64 sectors = 0;
   int i;

   if(!Closure->guiMode) return;

   if(info->set[0].target == ECC_FILE)
        verify_headline = _("Checking the image and error correction files.");
   else verify_headline = _("Checking the image file.");
   GuiSetLabelText(wl->cmpHeadline, "<big>%s</big>\n<i>%s</i>",
		   verify_headline, _("- Checking image file -"));

   ii = g_malloc0(sizeof(init_info));
   ii->nSets = info->nSets;
   ii->imagePresent = image->file != NULL;

   for(i=0; i<info->nSets; i++)
   {  RS04Set *set = &info->set[i];

      ii->entry[i] = g_strdup_printf(_utf("Set %d of %d (image sectors %" PRId64 " - %" PRId64 ")"),
				     i+1, info->nSets, set->start, set->start + (gint64)(set->S + set->A) - 1);
      sectors = MAX(sectors, set->start + (gint64)(set->target == ECC_FILE ? set->S : set->S + set->A));
   }
   if(image->file && info->set[0].target == ECC_IMAGE)
     sectors = MAX(sectors, image->file->size / info->set[0].B);

   wl->cmpSectors = sectors;
   g_idle_add(init_idle_func, ii);
}

void RS04VerifyResult(int good)
{  RS04Widgets *wl = widgets();
   char *result;

   if(!Closure->guiMode) return;

   if(good)
        result = g_strdup_printf(_("<span %s>Good image.</span>"), Closure->greenMarkup);
   else result = g_strdup_printf(_("<span %s>Damaged image.</span>"), Closure->redMarkup);
   GuiSetLabelText(wl->cmpHeadline, "<big>%s</big>\n<i>%s</i>", verify_headline, result);
   g_free(result);
}

/* Spiral segment states, worst one wins */

enum { SEG_UNSEEN, SEG_GOOD, SEG_CRC_ERROR, SEG_MISSING };

typedef struct
{  int from, to, state;
} spiral_info;

static gboolean spiral_idle_func(gpointer data)
{  spiral_info *si = (spiral_info*)data;
   RS04Widgets *wl = widgets();
   GdkRGBA *colors[] = { NULL, Closure->greenSector, Closure->yellowSector, Closure->redSector };
   int i;

   for(i=si->from; i<=si->to; i++)
   {  if(si->state > wl->cmpSegment[i])
	wl->cmpSegment[i] = si->state;
      GuiSetSpiralSegmentColor(wl->cmpSpiral, colors[wl->cmpSegment[i]], 0, i);
   }

   g_free(si);
   return FALSE;
}

/* Mark image sectors first .. first+count-1;
   state: 1 = good, 2 = CRC errors, 3 = missing sectors */

void RS04VerifySpiral(gint64 first, gint64 count, int state)
{  RS04Widgets *wl = widgets();
   spiral_info *si;

   if(!Closure->guiMode || wl->cmpSectors <= 0 || count <= 0)
     return;

   si = g_malloc(sizeof(spiral_info));
   si->from  = CLAMP((first * RS04_SPIRAL_SEGMENTS) / wl->cmpSectors, 0, RS04_SPIRAL_SEGMENTS-1);
   si->to    = CLAMP(((first+count-1) * RS04_SPIRAL_SEGMENTS) / wl->cmpSectors, 0, RS04_SPIRAL_SEGMENTS-1);
   si->state = state;
   g_idle_add(spiral_idle_func, si);
}

static gboolean spiral_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{  RS04Widgets *wl = (RS04Widgets*)data;
   Spiral *spiral = wl->cmpSpiral;
   GtkAllocation a = {0};
   int w, h, x, size;

   if(!wl->cmpLayout)
   {  GuiSetSpiralWidget(spiral, widget);
      wl->cmpLayout = gtk_widget_create_pango_layout(widget, NULL);
   }

   gtk_widget_get_allocation(widget, &a);
   GuiSetText(wl->cmpLayout, _("Missing sectors"), &w, &h);
   size = spiral->diameter + 20 + 3*(10+h);
   spiral->mx = a.width / 2;
   spiral->my = (spiral->diameter + a.height - size)/2;
   x = spiral->mx - spiral->diameter/2 + 10;

   GuiDrawSpiralLabel(cr, spiral, wl->cmpLayout, _("Good sectors"), Closure->greenSector, x, 1);
   GuiDrawSpiralLabel(cr, spiral, wl->cmpLayout, _("Sectors with CRC errors"), Closure->yellowSector, x, 2);
   GuiDrawSpiralLabel(cr, spiral, wl->cmpLayout, _("Missing sectors"), Closure->redSector, x, 3);
   GuiDrawSpiral(cr, spiral);

   return TRUE;
}

void ResetRS04VerifyWindow(Method *method)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;
   int i,j;

   for(i=0; i<RS04_MAX_SETS; i++)
     for(j=0; j<RS04_V_FIELDS; j++)
     {  g_free(wl->cmpText[i][j]);
	wl->cmpText[i][j] = NULL;
     }
   wl->cmpSets = wl->cmpShown = 0;
   wl->cmpSectors = 0;
   show_set(wl);

   gtk_label_set_text(GTK_LABEL(wl->cmpHeadline), "");
   gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(wl->cmpSetCombo));
   gtk_widget_hide(wl->cmpSetCombo);
   gtk_notebook_set_current_page(GTK_NOTEBOOK(wl->cmpImageNotebook), 1);

   memset(wl->cmpSegment, 0, sizeof(wl->cmpSegment));
   GuiFillSpiral(wl->cmpSpiral, &transparent);
   if(wl->cmpSpiral->widget)
     gtk_widget_queue_draw(wl->cmpSpiral->widget);
}

static GtkWidget* create_frame(GtkWidget *grid, char *title, int y, GtkWidget **inner)
{  GtkWidget *frame = gtk_frame_new(title);
   GtkWidget *grid2 = *inner = gtk_grid_new();

   gtk_widget_set_hexpand(frame, TRUE);
   gtk_widget_set_vexpand(frame, y == 2);
   gtk_grid_attach(GTK_GRID(grid), frame, 1, y, 1, 1);

   gtk_widget_set_margin_start(grid2, 5);
   gtk_widget_set_margin_end(grid2, 5);
   gtk_widget_set_margin_top(grid2, 2);
   gtk_widget_set_margin_bottom(grid2, 2);
   gtk_grid_set_column_spacing(GTK_GRID(grid2), 5);
   gtk_grid_set_row_spacing(GTK_GRID(grid2), 4);
   gtk_container_set_border_width(GTK_CONTAINER(grid2), 5);

   return frame;
}

/* One line of the verify window: a fixed key, or a key from key_field,
   or no key at all (the value spans both columns) */

static void add_field(RS04Widgets *wl, GtkWidget *grid, int y, char *key, int key_field, int field)
{  GtkWidget *label;

   if(key || key_field >= 0)
   {  label = grid_label(grid, key, 0.0, 1, y, 1);
      if(key_field >= 0) wl->cmpField[key_field] = label;
      wl->cmpField[field] = grid_label(grid, NULL, 0.0, 2, y, 1);
   }
   else
   {  wl->cmpField[field] = label = grid_label(grid, NULL, 0.0, 1, y, 2);
      gtk_widget_set_margin_top(label, 2);
   }

   /* long values must not widen the main window */

   gtk_label_set_ellipsize(GTK_LABEL(wl->cmpField[field]), PANGO_ELLIPSIZE_END);
}

void CreateRS04VerifyWindow(Method *method, GtkWidget *parent)
{  RS04Widgets *wl = (RS04Widgets*)method->widgetList;
   GtkWidget *grid, *grid2, *notebook, *frame, *d_area;

   /* Head line with the set selection */

   wl->cmpHeadline = add_headline(parent);

   wl->cmpSetCombo = gtk_combo_box_text_new();
   gtk_widget_set_valign(wl->cmpSetCombo, GTK_ALIGN_CENTER);
   gtk_widget_set_margin_end(wl->cmpSetCombo, 5);
   gtk_widget_set_no_show_all(wl->cmpSetCombo, TRUE);
   g_signal_connect(G_OBJECT(wl->cmpSetCombo), "changed", G_CALLBACK(set_combo_cb), wl);
   gtk_box_pack_end(GTK_BOX(gtk_widget_get_parent(wl->cmpHeadline)), wl->cmpSetCombo, FALSE, FALSE, 0);

   grid = gtk_grid_new();
   gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
   gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
   gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
   gtk_box_pack_start(GTK_BOX(parent), grid, TRUE, TRUE, 0);

   /*** Ecc data info */

   frame = create_frame(grid, _utf("Error correction properties"), 1, &grid2);
   gtk_container_add(GTK_CONTAINER(frame), grid2);

   add_field(wl, grid2, 1, _utf("Type:"), -1, RS04_V_TYPE);
   add_field(wl, grid2, 2, _utf("Method:"), -1, RS04_V_METHOD);
   add_field(wl, grid2, 3, _utf("Created by:"), -1, RS04_V_CREATOR);
   add_field(wl, grid2, 4, _utf("Data checksum:"), -1, RS04_V_ECCMD5);
   add_field(wl, grid2, 5, NULL, -1, RS04_V_ECCRESULT);

   /*** Image spiral */

   frame = gtk_frame_new(_utf("Image state"));
   gtk_widget_set_vexpand(frame, TRUE);
   gtk_grid_attach(GTK_GRID(grid), frame, 2, 1, 1, 2);

   wl->cmpSpiral = GuiCreateSpiral(&transparent, 10, 5, RS04_SPIRAL_SEGMENTS);
   d_area = wl->cmpDrawingArea = gtk_drawing_area_new();
   gtk_widget_set_size_request(d_area, wl->cmpSpiral->diameter+20, -1);
   gtk_container_add(GTK_CONTAINER(frame), d_area);
   g_signal_connect(G_OBJECT(d_area), "draw", G_CALLBACK(spiral_draw_cb), (gpointer)wl);

   /*** Image info */

   frame = create_frame(grid, _utf("Data integrity"), 2, &grid2);

   notebook = wl->cmpImageNotebook = gtk_notebook_new();
   gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
   gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
   gtk_container_add(GTK_CONTAINER(frame), notebook);
   gtk_notebook_append_page(GTK_NOTEBOOK(notebook), gtk_label_new(_utf("No image present.")), NULL);
   gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid2, NULL);

   add_field(wl, grid2, 1, _utf("Medium sectors:"), -1, RS04_V_SECTORS);
   add_field(wl, grid2, 2, _utf("Data checksum:"), -1, RS04_V_IMGMD5);
   add_field(wl, grid2, 3, _utf("Data section:"), -1, RS04_V_DATA);
   add_field(wl, grid2, 4, _utf("Crc section:"), -1, RS04_V_CRC);
   add_field(wl, grid2, 5, _utf("Ecc section:"), -1, RS04_V_ECC);
   add_field(wl, grid2, 6, _utf("Tags:"), -1, RS04_V_TAGS);
   add_field(wl, grid2, 7, NULL, RS04_V_OFFSET_KEY, RS04_V_OFFSET);
   add_field(wl, grid2, 8, NULL, RS04_V_ERASURE_KEY, RS04_V_ERASURE);
   add_field(wl, grid2, 9, NULL, RS04_V_MARGIN_KEY, RS04_V_MARGIN);
   add_field(wl, grid2, 10, NULL, RS04_V_PROGNOSIS_KEY, RS04_V_PROGNOSIS);
   add_field(wl, grid2, 11, NULL, -1, RS04_V_RESULT);
   add_field(wl, grid2, 12, NULL, -1, RS04_V_XORRISO);
}

#endif /* WITH_GUI_YES */
