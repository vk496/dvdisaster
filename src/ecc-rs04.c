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

/***
 *** Method registration
 ***/

static void destroy(Method*);

void register_rs04(void)
{  Method *method = g_malloc0(sizeof(Method));

   /*** Standard infomation and methods */

   memcpy(method->name, "RS04", 4);
   method->menuEntry = g_strdup(_("Long codeword RS codec (RS04, experimental)"));
   method->description = g_strdup(_("Reed-Solomon codec over GF(2^16) for error correction files and augmented images (experimental)"));
   method->create  = RS04Create;
   method->fix     = RS04Fix;
   method->verify  = RS04Verify;

   /*** Linkage to rs04-common.c and rs04-recognize.c */

   method->expectedImageSize = RS04ExpectedImageSize;
   method->recognizeEccFile  = RS04RecognizeFile;
   method->recognizeEccImage = RS04RecognizeImage;
   method->getCrcBuf         = RS04GetCrcBuf;

   method->widgetList = g_malloc0(sizeof(RS04Widgets));

#ifdef WITH_GUI_YES
   /*** Minimal windows; the results go to the log window */

   method->createCreateWindow = CreateRS04EncWindow;
   method->createFixWindow    = CreateRS04FixWindow;
   method->createVerifyWindow = CreateRS04VerifyWindow;
   method->resetCreateWindow  = ResetRS04EncWindow;
   method->resetFixWindow     = ResetRS04FixWindow;
   method->resetVerifyWindow  = ResetRS04VerifyWindow;
#endif

   method->destroy = destroy;

   RegisterMethod(method);
}

static void destroy(Method *method)
{  g_free(method->widgetList);
}

#ifdef WITH_GUI_YES

/***
 *** Minimal GUI windows
 ***/

static GtkWidget* create_label(GtkWidget *parent)
{  GtkWidget *label = gtk_label_new(NULL);

   gtk_box_pack_start(GTK_BOX(parent), label, FALSE, FALSE, 3);
   gtk_widget_show(label);
   return label;
}

void CreateRS04EncWindow(Method *self, GtkWidget *parent)
{  ((RS04Widgets*)self->widgetList)->createLabel = create_label(parent);
}

void CreateRS04FixWindow(Method *self, GtkWidget *parent)
{  ((RS04Widgets*)self->widgetList)->fixLabel = create_label(parent);
}

void CreateRS04VerifyWindow(Method *self, GtkWidget *parent)
{  ((RS04Widgets*)self->widgetList)->verifyLabel = create_label(parent);
}

static void reset_label(GtkWidget *label, char *text)
{  char *markup = g_markup_printf_escaped("<big>%s</big>\n<i>%s</i>",
					  text, _("See the log window for details."));

   gtk_label_set_markup(GTK_LABEL(label), markup);
   g_free(markup);
}

void ResetRS04EncWindow(Method *self)
{  reset_label(((RS04Widgets*)self->widgetList)->createLabel, _("Creating RS04 error correction data."));
}

void ResetRS04FixWindow(Method *self)
{  reset_label(((RS04Widgets*)self->widgetList)->fixLabel, _("Repairing the image."));
}

void ResetRS04VerifyWindow(Method *self)
{  reset_label(((RS04Widgets*)self->widgetList)->verifyLabel, _("Checking the image."));
}

#endif /* WITH_GUI_YES */
