/*
 * This file is part of the hildon-application-manager.
 *
 * Parts of this file are derived from apt.  Apt is copyright 1997,
 * 1998, 1999 Jason Gunthorpe and others.
 *
 * Copyright (C) 2005, 2006, 2007, 2008 Nokia Corporation.  All Rights reserved.
 *
 * Contact: Marius Vollmer <marius.vollmer@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <string.h>
#include <stdio.h>
#include <libintl.h>
#include <locale.h>
#include <string.h>

#include <gtk/gtk.h>
#include <gconf/gconf-client.h>
#include <hildon/hildon.h>

#include "menu.h"
#include "repo.h"
#include "settings.h"
#include "apt-worker-client.h"
#include "util.h"
#include "log.h"
#include "confutils.h"
#include "apt-utils.h"

#define _(x)       gettext (x)

#define SCROLL_TO_ERROR_TIMEOUT 500

static bool
apt_method_is_available (const char* method)
{
  bool ret = false;

  if (method)
    {
      gchar *method_file = g_strdup_printf ("%s%s", APT_METHOD_PATH, method);
      ret = g_file_test (method_file, G_FILE_TEST_EXISTS);
      g_free (method_file);
    }

  return ret;
}

static bool
repository_uri_is_valid (const gchar* uri)
{
  if (!uri)
    return false;

  char *start_str = strchr (uri, ':');
  if (!start_str)
    return false;
  if (strncmp (start_str, "://", 3) != 0)
    return false;

  gchar *method = g_strndup (uri, start_str - uri);
  if (!apt_method_is_available (method))
    {
      g_free (method);
      return false;
    }
  g_free (method);

  start_str = &start_str[3];
  if (!start_str || all_whitespace (start_str))
    return false;

  return true;
}

static GtkWidget *
add_entry (GtkWidget *box, GtkSizeGroup *group,
	   const char *label,
	   const char *text, const char *end,
	   bool autocap, bool readonly, bool mandatory)
{
  GtkWidget *caption, *entry;
  gint pos = 0;

  entry = hildon_entry_new (HILDON_SIZE_FINGER_HEIGHT);
  gtk_editable_set_editable (GTK_EDITABLE(entry), !readonly);

  if (!readonly)
    {
#ifdef MAEMO_CHANGES
      int mode;
      g_object_get (entry, "hildon-input-mode", &mode, NULL);
      if (autocap)
	mode |= int (HILDON_GTK_INPUT_MODE_AUTOCAP);
      else
	mode &= ~int (HILDON_GTK_INPUT_MODE_AUTOCAP);
      g_object_set (entry, "hildon-input-mode", mode, NULL);
#endif /* MAEMO_CHANGES */
    }

  if (text)
    {
      if (end == NULL)
        end = text + strlen (text);
      gtk_editable_insert_text (GTK_EDITABLE (entry), text, end - text, &pos);
    }

  caption = hildon_caption_new (group, label, entry,
				NULL, (mandatory
				       ? HILDON_CAPTION_MANDATORY
				       : HILDON_CAPTION_OPTIONAL));
  hildon_caption_set_separator (HILDON_CAPTION (caption), NULL);
  gtk_box_pack_start_defaults (GTK_BOX (box), caption);

  return entry;
}

static void
pill_response (GtkDialog *dialog, gint response, gpointer unused)
{
  pop_dialog (GTK_WIDGET (dialog));
  gtk_widget_destroy (GTK_WIDGET (dialog));

  if (red_pill_mode != (response == GTK_RESPONSE_YES))
    {
      red_pill_mode = (response == GTK_RESPONSE_YES);
      save_settings ();

      set_settings_menu_visible (red_pill_mode);
      set_install_from_file_menu_visible (red_pill_mode);
      update_backend_options ();
      if (red_pill_show_all || red_pill_show_magic_sys)
        get_package_list ();
    }
}

static void
ask_the_pill_question ()
{
  GtkWidget *dialog;

  dialog =
    hildon_note_new_confirmation_add_buttons (NULL, 
					      "Which pill?",
					      "Red", GTK_RESPONSE_YES,
					      "Blue", GTK_RESPONSE_NO,
					      NULL);
  push_dialog (dialog);
  g_signal_connect (dialog, "response",
		    G_CALLBACK (pill_response), NULL);
  gtk_widget_show_all (dialog);
}

struct get_catalogues_closure {
  void (*cont) (xexp *catalogues, void *data);
  void *data;
};

static void
get_catalogues_callback (int cmd,
			 apt_proto_decoder *dec,
			 void *callback_data)
{
  get_catalogues_closure *c = (get_catalogues_closure *)callback_data;
  xexp *x = NULL;

  if (dec
      && (x = dec->decode_xexp ())
      && xexp_is (x, "catalogues"))
    {
      c->cont (x, c->data);
    }
  else
    {
      if (dec)
	what_the_fock_p ();
      xexp_free (x);
      c->cont (NULL, c->data);
    }
  delete c;
}

void
get_catalogues (void (*cont) (xexp *catalogues, void *data),
		void *data)
{
  get_catalogues_closure *c = new get_catalogues_closure;
  c->cont = cont;
  c->data = data;
  apt_worker_get_catalogues (get_catalogues_callback, c);
}

struct rm_temp_catalogues_closure {
  void (*cont) (void *data);
  void *data;
};

static void rtc_reply (bool keep_going, void *data)
{
  rm_temp_catalogues_closure* rtc_clos = (rm_temp_catalogues_closure*) data;

  rtc_clos->cont (rtc_clos->data);
  
  delete rtc_clos;
}

static void
rm_temp_catalogues_callback (int cmd,
                             apt_proto_decoder *dec,
                             void *data)
{
  refresh_package_cache_without_user (NULL, rtc_reply, data);
}

void
rm_temp_catalogues (void (*cont) (void* data), void *data)
{
  rm_temp_catalogues_closure* rtc_clos = new rm_temp_catalogues_closure;
  rtc_clos->cont = cont;
  rtc_clos->data = data;
  
  apt_worker_rm_temp_catalogues (rm_temp_catalogues_callback, rtc_clos);
}

const char *
catalogue_name (xexp *x)
{
  const char *name = "";
  xexp *n = xexp_aref (x, "name");
  if (n == NULL)
    ;
  else if (xexp_is_text (n))
    name = xexp_text (n);
  else
    {
      char *current_locale = setlocale (LC_MESSAGES, "");
      xexp *t = (current_locale
		 ? xexp_aref (n, current_locale)
		 : NULL);
      if (t == NULL)
        t = xexp_aref (n, "default");
      if (t && xexp_is_text (t))
        name = xexp_text (t);
    }
  return name;
}

void
set_catalogue_name (xexp *x, const char *name)
{
  xexp *n = xexp_aref (x, "name");
  if (n == NULL || xexp_is_text (n))
    xexp_aset_text (x, "name", name);
  else
    {
      char *current_locale = setlocale (LC_MESSAGES, "");
      xexp_aset_text (n, current_locale, name);
    }
}

/* "Application Catalogues" interaction flow.
 */

struct scdf_clos {
  xexp *catalogues;
};

static void scdf_dialog_done (bool changed, void *date);
static void scdf_end (bool keep_going, void *data);

void
show_catalogue_dialog_flow ()
{
  if (start_interaction_flow ())
    {
      scdf_clos *c = new scdf_clos;

      c->catalogues = NULL;
      show_catalogue_dialog (NULL, false,
			     scdf_dialog_done, c);
    }
}

static void
scdf_dialog_done (bool changed, void *data)
{
  scdf_clos *c = (scdf_clos *)data;

  if (changed)
    set_catalogues_and_refresh (c->catalogues,
				NULL, scdf_end, c);
  else
    scdf_end (true, c);
}

static void
scdf_end (bool keep_going, void *data)
{
  scdf_clos *c = (scdf_clos *)data;

  force_show_catalogue_errors ();
  xexp_free (c->catalogues);
  delete c;

  end_interaction_flow ();
}

enum cat_dialog_type {
  cat_readonly,    // catalogues is defined in /etc/apt/sources.list
  cat_package,     // is a package catalogue (enabled or disabled only)
  cat_editable    // is a catalogue added/installed by the user
};

struct catcache {
  catcache *next;
  struct cat_dialog_closure *cat_dialog;
  xexp *catalogue_xexp;
  bool enabled, foreign, refresh_failed;
  cat_dialog_type type;
  const char *name;
  char *detail;
};

struct cat_dialog_closure {
  catcache *caches;
  xexp *catalogues_xexp;
  bool dirty;
  bool show_only_errors;
  bool has_failing_catalogues;

  bool showing_catalogues;

  GtkWidget *dialog;

  catcache *selected_cat;
  GtkTreeIter selected_iter;

  GtkTreeView *tree;
  GtkListStore *store;
  GtkWidget *new_button;

  void (*cont) (bool changed, void *data);
  void *data;
};

struct cat_edit_closure {
  cat_dialog_closure *cat_dialog;
  xexp *catalogue;
  bool isnew;
  cat_dialog_type type ;

  GtkWidget *name_entry;
  GtkWidget *uri_entry;
  GtkWidget *dist_entry;
  GtkWidget *components_entry;
  GtkWidget *disabled_button;
};

static cat_dialog_closure *current_cat_dialog_clos = NULL;

static void reset_cat_list (cat_dialog_closure *c);
static void set_cat_list (cat_dialog_closure *c, GtkTreeIter *iter_to_select);

static gboolean
is_package_catalogue (xexp *catalogue)
{
  const gchar *file = xexp_aref_text (catalogue, "file");
  const gchar *id   = xexp_aref_text (catalogue, "id");

  return (file && id);
}

struct remove_cat_clos {
  cat_dialog_closure *cat_dialog;
  xexp *catalogue;
  GMainLoop *loop;
};

static void
remove_cat_cont (bool res, gpointer data)
{
  remove_cat_clos *c = (remove_cat_clos *)data;
  cat_dialog_closure *d = c->cat_dialog;

  if (res)
    {
      reset_cat_list (d);
      xexp_del (d->catalogues_xexp, c->catalogue);
      set_cat_list (d, NULL);
      d->dirty = true;
    }

  if (c->loop && g_main_loop_is_running (c->loop))
    g_main_loop_quit (c->loop);

  delete c;
}

#define REPO_RESPONSE_REMOVE 3

static void
cat_edit_response (GtkDialog *dialog, gint response, gpointer clos)
{
  bool should_ask_the_pill_question = false;

  cat_edit_closure *c = (cat_edit_closure *)clos;
  cat_dialog_closure *d = c->cat_dialog;

  if (c->type == cat_readonly) // it cames from an .install file
    ;
  else if (response == REPO_RESPONSE_REMOVE)
    {
      catcache *cat = d->selected_cat;
      if (cat == NULL)
	return;

      char *text = g_strdup_printf (_("ai_nc_remove_repository"), cat->name);
      GMainLoop *loop = g_main_loop_new (NULL, TRUE);

      remove_cat_clos *rc = new remove_cat_clos;
      rc->loop = loop;
      rc->cat_dialog = d;
      rc->catalogue = cat->catalogue_xexp;

      ask_yes_no (text, remove_cat_cont, rc);
      g_free (text);

      /* let's wait for the dialogue response */
      g_main_loop_run (loop);
      g_main_loop_unref (loop);
    }
  else if (response == GTK_RESPONSE_OK && c->type == cat_package)
    {
      bool disabled =
        hildon_check_button_get_active (HILDON_CHECK_BUTTON
                                        (c->disabled_button));
      xexp_aset_bool (c->catalogue, "disabled", disabled);
      set_cat_list (d, &d->selected_iter);
      d->dirty = true;
    }
  else if (response == GTK_RESPONSE_OK && c->type == cat_editable)
    {
      const char *name = gtk_entry_get_text (GTK_ENTRY (c->name_entry));

      if (all_whitespace (name))
      {
        irritate_user (_("ai_ib_enter_name"));
        gtk_widget_grab_focus (c->name_entry);
        return;
      }

      char *uri = g_strstrip (g_strdup (gtk_entry_get_text
                                        (GTK_ENTRY (c->uri_entry))));

      /* validate repository location                                         */

      /* TODO we need a more general text, like "Invalid repository location" */
      /* TODO encode URI to scape special characters?                         */
      if (!repository_uri_is_valid (uri))
        {
          irritate_user (_("ai_ib_enter_web_address"));
          gtk_widget_grab_focus (c->uri_entry);
          g_free (uri);
          return;
        }

      char *dist = g_strstrip (g_strdup (gtk_entry_get_text
                                         (GTK_ENTRY (c->dist_entry))));
      char *comps = g_strstrip (g_strdup (gtk_entry_get_text
                                          (GTK_ENTRY (c->components_entry))));
      bool disabled = hildon_check_button_get_active (HILDON_CHECK_BUTTON
                                                      (c->disabled_button));

      if (all_whitespace (comps))
        {
          /* Ensure there's a trailing '/' at the end of dist */
          char *tmp_dist = dist;

          /* Append the '/' character when needed */
          if (all_whitespace (dist))
            dist = g_strdup ("/");
          else if (!g_str_has_suffix (dist, "/"))
            dist = g_strconcat (tmp_dist, "/", NULL);

          /* Free tmp_dist if new memory was allocated for dist */
          if (dist != tmp_dist)
            g_free (tmp_dist);
        }
      else if (!all_whitespace (dist))
        {
          /* Remove the trailing '/' at the end of dist, if present */
          char *suffix = NULL;
          if (g_str_has_suffix (dist, "/") && (suffix = g_strrstr (dist, "/")))
            *suffix = '\0';
        }

      if (all_whitespace (dist))
        dist = NULL;

      reset_cat_list (d);
      if (c->isnew)
        xexp_append_1 (d->catalogues_xexp, c->catalogue);
      set_catalogue_name (c->catalogue, name);
      xexp_aset_bool (c->catalogue, "disabled", disabled);
      xexp_aset_text (c->catalogue, "components", comps);
      xexp_aset_text (c->catalogue, "dist", dist);
      xexp_aset_text (c->catalogue, "uri", uri);
      set_cat_list (d, &d->selected_iter);
      d->dirty = true;

      g_free (uri);
      g_free (dist);
      g_free (comps);
    }
  else if (c->isnew)
    {
      xexp_free (c->catalogue);
      if (!strcmp (gtk_entry_get_text (GTK_ENTRY (c->uri_entry)), "matrix"))
	should_ask_the_pill_question = true;
    }

  delete c;

  pop_dialog (GTK_WIDGET (dialog));
  gtk_widget_destroy (GTK_WIDGET (dialog));

  if (should_ask_the_pill_question)
    ask_the_pill_question ();

  /* Emit response signal if needed */
  if (d && d->dialog && d->show_only_errors && !d->has_failing_catalogues)
    gtk_dialog_response (GTK_DIALOG (d->dialog), GTK_RESPONSE_CLOSE);
}

struct scroll_to_params {
  GtkWidget *tv;
  GtkWidget *pa;
};

static bool
scroll_to_timeout (scroll_to_params *params)
{
  /*
    When this timeout fires, it is assumed that the dialog has finished
    sizing its children and one can safely scroll to the error message
  */

  hildon_pannable_area_scroll_to
    (HILDON_PANNABLE_AREA (params->pa), -1,
     params->tv->allocation.y + MIN (params->tv->allocation.height,
                                     params->pa->allocation.height / 2));
  g_object_set_data (G_OBJECT (params->tv),
                     "scroll-to-timeout-has-passed", GINT_TO_POINTER (TRUE));

  /* Remove timeout-related data from widget */
  g_object_set_data_full (G_OBJECT (params->tv),
                          "scroll-to-timeout", GINT_TO_POINTER (0), NULL);
  g_object_set_data_full (G_OBJECT(params->tv),
                          "scroll-to-timeout-params", NULL,
                          (GDestroyNotify) g_free);
  return false;
}

static void
add_scroll_timeout (GtkWidget *tv, GtkWidget *pa)
{
  /*
    If the timeout has not yet passed, then add it if not yet present,
    otherwise push it forward
  */
  if (!GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tv),
                                           "scroll-to-timeout-has-passed")))
    {
      if (!GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tv),
                                               "scroll-to-timeout")))
        {
          scroll_to_params *params = new scroll_to_params;

          params->tv = tv;
          params->pa = pa;

          g_object_set_data_full (G_OBJECT (tv),
                                  "scroll-to-timeout-params", params,
                                  (GDestroyNotify) g_free);
          g_object_set_data_full
            (G_OBJECT (tv), "scroll-to-timeout",
             GINT_TO_POINTER (g_timeout_add (SCROLL_TO_ERROR_TIMEOUT,
                                             (GSourceFunc) scroll_to_timeout,
                                             params)),
             (GDestroyNotify) g_source_remove);
        }
      else
        {
          g_object_set_data_full
            (G_OBJECT (tv), "scroll-to-timeout",
             GINT_TO_POINTER (g_timeout_add
                              (SCROLL_TO_ERROR_TIMEOUT,
                               (GSourceFunc) scroll_to_timeout,
                               g_object_get_data (G_OBJECT(tv),
                                                  "scroll-to-timeout-params"))),
             (GDestroyNotify)g_source_remove);
        }
    }
}

static void
tv_size_allocate (GtkWidget *tv, GtkAllocation *alloc, GtkWidget *pa)
{
  /* Add/push forward the scroll timeout */
  add_scroll_timeout (tv, pa);
}

static void
show_cat_edit_dialog (cat_dialog_closure *cat_dialog, xexp *catalogue,
		      bool isnew, cat_dialog_type type, const char *detail)
{
  GtkWidget *dialog, *vbox, *caption, *scrolledw;
  GtkSizeGroup *group;

  if (!xexp_is_list (catalogue) || !xexp_is (catalogue, "catalogue"))
    {
      irritate_user (_("ai_ib_unable_edit"));
      return;
    }

  cat_edit_closure *c = new cat_edit_closure;

  c->isnew = isnew;
  c->type = type;
  c->cat_dialog = cat_dialog;
  c->catalogue = catalogue;

  const char *title;
  if (isnew)
    title = _("ai_ti_new_repository");
  else
    title = _("ai_ti_edit_repository");

  dialog = gtk_dialog_new_with_buttons (title, NULL, GTK_DIALOG_MODAL, NULL);
  scrolledw = hildon_pannable_area_new ();
  gtk_container_add (GTK_CONTAINER (GTK_DIALOG(dialog)->vbox), scrolledw);
  if (detail)
    gtk_widget_set_size_request (scrolledw, -1, 350);
  else
    hildon_pannable_area_set_size_request_children
      (HILDON_PANNABLE_AREA (scrolledw));

  vbox = gtk_vbox_new (FALSE, 0);
  hildon_pannable_area_add_with_viewport (HILDON_PANNABLE_AREA (scrolledw),
                                          vbox);

  bool readonly = !(c->type == cat_editable);

  if (!readonly && !c->isnew)
    gtk_dialog_add_button (GTK_DIALOG (dialog),
                           _("ai_bd_repository_delete"),
                           REPO_RESPONSE_REMOVE);

  gtk_dialog_add_button (GTK_DIALOG (dialog),
                         _("ai_bd_new_repository_cancel"),
                         GTK_RESPONSE_CANCEL);

  if (c->type != cat_readonly)
    gtk_dialog_add_button (GTK_DIALOG (dialog),
                           _("ai_bd_new_repository_ok"),
                           GTK_RESPONSE_OK);

  push_dialog (dialog);

  gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);

  group = GTK_SIZE_GROUP (gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL));

  const char *current_name = catalogue_name (catalogue);

  c->name_entry = add_entry (vbox, group,
			     isnew
                             ? _("ai_fi_new_repository_name")
                             : _("ai_ti_catalogue_details_name"),
			     current_name, NULL, true, readonly, true);

  c->uri_entry = add_entry (vbox, group,
                            isnew
			    ? _("ai_fi_new_repository_web_address")
                            : _("ai_ti_catalogue_details_web_address"),
			    xexp_aref_text (catalogue, "uri"),
			    NULL, false, readonly, true);

  c->dist_entry = add_entry (vbox, group,
                             isnew
                             ? _("ai_fi_new_repository_distribution")
                             : _("ai_ti_catalogue_details_distribution"),
			     xexp_aref_text (catalogue, "dist"),
			     NULL, false, readonly, true);

  c->components_entry = add_entry (vbox, group,
                                   isnew
				   ? _("ai_fi_new_repository_component")
                                   : _("ai_ti_catalogue_details_component"),
				   xexp_aref_text (catalogue, "components"),
				   NULL, false, readonly, false);

  if (c->type != cat_readonly)
    {
      c->disabled_button = hildon_check_button_new (HILDON_SIZE_FINGER_HEIGHT);
      gtk_button_set_label (GTK_BUTTON (c->disabled_button),
                            _("ai_fi_new_repository_disabled"));
      hildon_check_button_set_active (HILDON_CHECK_BUTTON (c->disabled_button),
                                      xexp_aref_bool (catalogue, "disabled"));
      caption = hildon_caption_new (group, NULL, c->disabled_button,
                                    NULL, HILDON_CAPTION_OPTIONAL);
      gtk_box_pack_start_defaults (GTK_BOX (vbox), caption);
      gtk_widget_set_sensitive (c->disabled_button, c->type != cat_readonly);
    }

  gtk_widget_set_size_request (GTK_WIDGET (dialog), 650, -1);

  if (detail)
    {
      GtkTextBuffer *buf = gtk_text_buffer_new (NULL);
      GtkWidget *text_view = GTK_WIDGET (g_object_new
                                         (HILDON_TYPE_TEXT_VIEW,
                                          "editable", FALSE,
                                          "wrap-mode", GTK_WRAP_CHAR,
                                          NULL));

      GTK_WIDGET_UNSET_FLAGS (text_view, GTK_CAN_FOCUS | GTK_CAN_DEFAULT);
      gtk_text_buffer_insert_at_cursor (buf, detail, -1);
      hildon_text_view_set_buffer (HILDON_TEXT_VIEW (text_view), buf);
      gtk_widget_set_name (text_view, "hildon-reversed-textview");

      hildon_helper_set_logical_color (text_view, GTK_RC_TEXT,
                                       GTK_STATE_NORMAL, "AttentionColor");
      hildon_helper_set_logical_color (text_view, GTK_RC_TEXT,
                                       GTK_STATE_ACTIVE, "AttentionColor");
      hildon_helper_set_logical_color (text_view, GTK_RC_TEXT,
                                       GTK_STATE_SELECTED, "AttentionColor");
      hildon_helper_set_logical_color (text_view, GTK_RC_TEXT,
                                       GTK_STATE_PRELIGHT, "AttentionColor");
      gtk_container_add (GTK_CONTAINER (vbox), text_view);

      /* When the dialog has finished sizing its widgets, scroll to the error
         message */
      g_signal_connect (G_OBJECT (text_view), "realize",
                        (GCallback) add_scroll_timeout, scrolledw);
      g_signal_connect (G_OBJECT (text_view), "size-allocate",
                        (GCallback) tv_size_allocate, scrolledw);
    }

  g_signal_connect (dialog, "response",
		    G_CALLBACK (cat_edit_response), c);
  gtk_widget_show_all (dialog);
  g_object_unref (group);
}

static void
cat_icon_func (GtkTreeViewColumn *column,
	       GtkCellRenderer *cell,
	       GtkTreeModel *model,
	       GtkTreeIter *iter,
	       gpointer data)
{
  static GdkPixbuf *ok_browser_pixbuf = NULL;
  static GdkPixbuf *fail_browser_pixbuf = NULL;
  GdkPixbuf *browser_pixbuf = NULL;

  cat_dialog_closure *cd = (cat_dialog_closure *)data;
  catcache *c = NULL;

  gtk_tree_model_get (model, iter, 0, &c, -1);

  /* Load icon for successfully refreshed catalogues */
  if (ok_browser_pixbuf == NULL)
    {
      GtkIconTheme *icon_theme = gtk_icon_theme_get_default ();

      ok_browser_pixbuf =
	gtk_icon_theme_load_icon (icon_theme,
				  "general_web",
				  TREE_VIEW_ICON_SIZE,
				  GtkIconLookupFlags (0),
				  NULL);
    }

  /* Load icon for catalogues which failed while refreshing */
  if (fail_browser_pixbuf == NULL)
    {
      GtkIconTheme *icon_theme = gtk_icon_theme_get_default ();

      fail_browser_pixbuf =
	gtk_icon_theme_load_icon (icon_theme,
				  "app_install_error",
				  TREE_VIEW_ICON_SIZE,
				  GtkIconLookupFlags (0),
				  NULL);
    }

  /* Select icon to show when showing the application catalogue dialog */
  if (!cd->show_only_errors && c && c->refresh_failed)
    browser_pixbuf = fail_browser_pixbuf;
  else
    browser_pixbuf = ok_browser_pixbuf;

  g_object_set (cell,
		"pixbuf", (c && c->foreign) ? NULL : browser_pixbuf,
		"sensitive", c && c->enabled,
		NULL);
}

static void
cat_text_func (GtkTreeViewColumn *column,
	       GtkCellRenderer *cell,
	       GtkTreeModel *model,
	       GtkTreeIter *iter,
	       gpointer data)
{
  cat_dialog_closure *cd = (cat_dialog_closure *)data;
  catcache *c = NULL;
  gchar *full_name = NULL;

  gtk_tree_model_get (model, iter, 0, &c, -1);

  if (c == NULL)
    return;

  /* set 'failed catalogue' suffix when needed */
  if (!cd->show_only_errors && c->refresh_failed)
    {
      full_name = g_strdup_printf("%s - %s",
				  c->name,
				  _("ai_ia_failed_catalogue"));
    }
  else
    full_name = g_strdup (c->name);

  g_object_set (cell, 
      	      	"text", c? full_name : NULL,
		"sensitive", c && c->enabled,
		NULL);

  g_free (full_name);
}

static void
cat_row_activated (GtkTreeView *treeview,
		   GtkTreePath *path,
		   GtkTreeViewColumn *column,
		   gpointer data)
{
  GtkTreeModel *model = gtk_tree_view_get_model (treeview);
  GtkTreeIter iter;

  if (gtk_tree_model_get_iter (model, &iter, path))
    {
      catcache *c;
      gtk_tree_model_get (model, &iter, 0, &c, -1);
      if (c == NULL)
        return;

      c->cat_dialog->selected_cat = c;
      show_cat_edit_dialog (c->cat_dialog, c->catalogue_xexp,
			    false, c->type, c->detail);
    }
}

static void
emit_row_changed (GtkTreeModel *model, GtkTreeIter *iter)
{
  GtkTreePath *path;

  path = gtk_tree_model_get_path (model, iter);
  g_signal_emit_by_name (model, "row-changed", path, iter);
  gtk_tree_path_free (path);
}

static void
cat_selection_changed (GtkTreeSelection *selection, gpointer data)
{
  GtkTreeModel *model;
  GtkTreeIter iter;
  cat_dialog_closure *c = (cat_dialog_closure *)data;

  catcache *old_selected = c->selected_cat;
  catcache *new_selected;

  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    gtk_tree_model_get (model, &iter, 0, &new_selected, -1);
  else
    new_selected = NULL;

  c->selected_cat = new_selected;
  if (old_selected)
    emit_row_changed (model, &c->selected_iter);
  c->selected_iter = iter;

  if (new_selected)
    emit_row_changed (model, &iter);
}

static char *
render_catalogue_errors (xexp *cat)
{
  xexp *errors = xexp_aref (cat, "errors");
  if (errors == NULL || xexp_first (errors) == NULL)
    return NULL;

  GString *report = g_string_new ("");

  for (xexp *err = xexp_first (errors); err; err = xexp_rest (err))
    {
      g_string_append_printf (report, "%s\n %s",
			      xexp_aref_text (err, "uri"),
			      xexp_aref_text (err, "msg"));
      if (xexp_rest (err))
	g_string_append (report, "\n");
    }

  char *str = report->str;
  g_string_free (report, 0);
  return str;
}

static catcache *
make_catcache_from_xexp (cat_dialog_closure *c, xexp *x)
{
  catcache *cat = new catcache;
  cat->catalogue_xexp = x;
  cat->cat_dialog = c;
  cat->refresh_failed = false;
  if (xexp_is (x, "catalogue") && xexp_is_list (x))
    {
      xexp *errors = NULL;

      cat->enabled = !xexp_aref_bool (x, "disabled");
      if (is_package_catalogue (x))
        cat->type = cat_package;
      else
        cat->type = cat_editable;
      cat->foreign = false;
      cat->name = catalogue_name (x);
      cat->detail = render_catalogue_errors (x);

      /* Check for errors during the last refresh */
      errors = xexp_aref (x, "errors");
      if (errors != NULL && xexp_first (errors) != NULL)
        cat->refresh_failed = true;
    }
  else if (xexp_is (x, "source") && xexp_is_text (x))
    {
      cat->enabled = true;
      cat->type = cat_readonly;
      cat->foreign = true;
      cat->name = xexp_text (x);
      cat->detail = NULL;
    }
  else
    {
      delete cat;
      cat = NULL;
    }
  return cat;
}

static void
reset_cat_list (cat_dialog_closure *c)
{
  catcache *next;
  for (catcache *cat = c->caches; cat; cat = next)
    {
      next = cat->next;
      g_free (cat->detail);
      delete cat;
    }
  c->caches = NULL;
  c->selected_cat = NULL;
}

static bool
cat_has_errors (xexp *cat)
{
  xexp *errors = (xexp_is (cat, "catalogue")
		  ? xexp_aref (cat, "errors")
		  : NULL);

  return errors != NULL && xexp_first (errors) != NULL;
}

static int
cat_compare (xexp *cat1, xexp *cat2)
{
  int w1 = (xexp_is (cat1, "catalogue")
	    ? xexp_aref_int (cat1, "sort-weight", 0)
	    : -2000);
  int w2 = (xexp_is (cat2, "catalogue")
	    ? xexp_aref_int (cat2, "sort-weight", 0)
	    : -2000);

  if (cat_has_errors (cat1))
    w1 -= 1000;

  if (cat_has_errors (cat2))
    w2 -= 1000;

  if (w1 > w2)
    return -1;
  else if (w1 == w2)
    {
      const char *n1 = catalogue_name (cat1);
      const char *n2 = catalogue_name (cat2);
      
      return strcmp (n1, n2);
    }
  else
    return 1;
}

static void
set_cat_list (cat_dialog_closure *c, GtkTreeIter *iter_to_select)
{
  gint position = 0;
  guint n_failed_cats = 0;
  catcache **catptr = &c->caches;
  GtkTreePath *path_to_select = NULL;

  if (c->catalogues_xexp == NULL)
    return;

  xexp_list_sort (c->catalogues_xexp, cat_compare);

  /* Retrieve path to select if needed (used inside the loop) */
  if (iter_to_select)
    {
      path_to_select = gtk_tree_model_get_path (GTK_TREE_MODEL (c->store),
						iter_to_select);
    }

  /* If it exists, clear previous list store */
  if (c->store)
    {
      /* @FIXME:
        Gotta set entries to NULL first, because some bug in maemo-gtk/hildon
        is causing calls to cat_icon_func/cat_text_func with garbage data
      */
      GtkTreeIter itr;
      if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (c->store), &itr))
        do
          {
            gtk_list_store_set (c->store, &itr, 0, NULL, -1);
          } while (gtk_tree_model_iter_next (GTK_TREE_MODEL (c->store), &itr));
      gtk_list_store_clear (c->store);
    }

  for (xexp *catx = xexp_first (c->catalogues_xexp); catx;
       catx = xexp_rest (catx))
    {
      gboolean has_errors = cat_has_errors (catx);

      if (has_errors)
        n_failed_cats++;

      if (c->show_only_errors && !has_errors)
	continue;

      catcache *cat = make_catcache_from_xexp (c, catx);
      if (cat)
	{
	  *catptr = cat;
	  catptr = &cat->next;
	  GtkTreeIter iter;
	  gtk_list_store_insert_with_values (c->store, &iter,
					     position,
					     0, cat,
					     -1);
	  /* Select first item by default */
 	  if (position == 0)
	    {
	      c->selected_cat = cat;
	      c->selected_iter = iter;
	    }
	  else if (path_to_select)
	    {
	      /* Select specified item if available */

	      GtkTreePath *current_path =
		gtk_tree_model_get_path (GTK_TREE_MODEL (c->store),
					 &iter);

	      /* Compare current iter with iter_to_select */
	      if (current_path &&
		  !gtk_tree_path_compare (current_path, path_to_select))
		{
		  c->selected_cat = cat;
		  c->selected_iter = iter;
		}

	      gtk_tree_path_free (current_path);
	    }

	  position += 1;
	}
    }

  /* Set flag to know there are failing catalogues */
  c->has_failing_catalogues = (n_failed_cats > 0);

  /* Set the focus in the right list element */
  GtkTreeSelection *tree_selection =
    gtk_tree_view_get_selection (GTK_TREE_VIEW (c->tree));
  gtk_tree_selection_select_iter (tree_selection, &c->selected_iter);

  gtk_widget_grab_focus (GTK_WIDGET (c->tree));

  *catptr = NULL;

  gtk_tree_path_free (path_to_select);
}

static void
refresh_cat_list (cat_dialog_closure *c)
{
  reset_cat_list (c);
  set_cat_list (c, NULL);
}

static GtkWidget *
make_cat_list (cat_dialog_closure *c)
{
  GtkTreeViewColumn *column;
  GtkCellRenderer *renderer;
  GtkWidget *scroller;

  c->store = gtk_list_store_new (1, G_TYPE_POINTER);
  c->tree =
    GTK_TREE_VIEW (gtk_tree_view_new_with_model (GTK_TREE_MODEL (c->store)));

  column = gtk_tree_view_column_new ();
  gtk_tree_view_append_column (c->tree, column);

  renderer = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_set_cell_data_func (column,
      	      	      	      	      	   renderer,
					   cat_icon_func,
					   c,
					   NULL);

  renderer = gtk_cell_renderer_text_new ();
  g_object_set (G_OBJECT (renderer),
      	      	"xpad", HILDON_MARGIN_DEFAULT,
		"ellipsize", PANGO_ELLIPSIZE_END,
		"ellipsize-set", TRUE,
		NULL);
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func (column,
      	      	      	      	      	   renderer,
					   cat_text_func,
					   c,
					   NULL);

  g_signal_connect (c->tree, "row-activated",
		    G_CALLBACK (cat_row_activated), c);

  g_signal_connect
    (G_OBJECT (gtk_tree_view_get_selection (GTK_TREE_VIEW (c->tree))),
     "changed",
     G_CALLBACK (cat_selection_changed), c);

  refresh_cat_list (c);

  scroller = hildon_pannable_area_new ();
  gtk_container_add (GTK_CONTAINER (scroller), GTK_WIDGET (c->tree));

  return scroller;
}

#define REPO_RESPONSE_NEW    1

static void
cat_response (GtkDialog *dialog, gint response, gpointer clos)
{
  cat_dialog_closure *c = (cat_dialog_closure *)clos;

  if (response == REPO_RESPONSE_NEW)
    {
      xexp *x = xexp_list_new ("catalogue");
      xexp_aset_text (x, "name", "");
      xexp_aset_text (x, "uri", "http://");
      xexp_aset_text (x, "components", "user");
      show_cat_edit_dialog (c, x, true, cat_editable, NULL);
      return;
    }

  if (response == GTK_RESPONSE_CLOSE ||
      response == GTK_RESPONSE_DELETE_EVENT)
    {
      reset_cat_list (c);
      pop_dialog (GTK_WIDGET (dialog));
      gtk_widget_destroy (GTK_WIDGET (dialog));

      c->cont (c->dirty, c->data);
      current_cat_dialog_clos = NULL;
      hide_updating ();

      g_object_unref (c->dialog);

      delete c;
    }
}

static void
scd_get_catalogues_reply (xexp *catalogues, void *data)
{
  cat_dialog_closure *c = (cat_dialog_closure *)data;
  scdf_clos *f_clos = NULL;

  if ((c == NULL) || (c != current_cat_dialog_clos) || c->showing_catalogues)
    return;

  f_clos = (scdf_clos *)c->data;
  c->catalogues_xexp = catalogues;
  f_clos->catalogues = catalogues;

  c->showing_catalogues = true;
  refresh_cat_list (c);

  if (!c->show_only_errors)
    gtk_widget_set_sensitive (c->new_button, TRUE);

  /* Prevent the 'Updating' banner from being shown */
  prevent_updating ();
}


static void
ecu_reply (bool ignore, void *data)
{
  g_return_if_fail (data != NULL);

  /* Show the 'Updating' banner */
  allow_updating ();
  show_updating ();

  get_catalogues (scd_get_catalogues_reply, data);
}

static void
ensure_cache_updated (cat_dialog_closure *c)
{
  if (!is_package_cache_updated ())
    {
      /* Force a refresh if package cache is not up-to-date */
      refresh_package_cache_without_user (NULL, ecu_reply, c);
    }
  else
    {
      /* Show the 'Updating' banner */
      allow_updating ();
      show_updating ();

      /* Retrieve the catalogues information */
      get_catalogues (scd_get_catalogues_reply, c);
    }
}

void
show_catalogue_dialog (xexp *catalogues,
		       bool show_only_errors,
		       void (*cont) (bool changed, void *data),
		       void *data)
{
  cat_dialog_closure *c = new cat_dialog_closure;
  GtkWidget *dialog = NULL;

  c->caches = NULL;
  c->catalogues_xexp = catalogues;
  c->dirty = false;
  c->show_only_errors = show_only_errors;
  c->has_failing_catalogues = false;
  c->showing_catalogues = false;
  c->selected_cat = NULL;
  c->cont = cont;
  c->data = data;

  c->new_button = NULL;

  current_cat_dialog_clos = c;

  /* Create dialog (and save reference) */
  dialog = gtk_dialog_new ();
  c->dialog = dialog;
  g_object_ref (dialog);

  if (show_only_errors)
    gtk_window_set_title (GTK_WINDOW (dialog), _("ai_ti_failed_repositories"));
  else
    gtk_window_set_title (GTK_WINDOW (dialog), _("ai_ti_repository"));

  push_dialog (dialog);

  gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);

  if (!show_only_errors)
    c->new_button =
      gtk_dialog_add_button (GTK_DIALOG (dialog),
                             _("ai_bd_repository_new"), REPO_RESPONSE_NEW);


  respond_on_escape (GTK_DIALOG (dialog), GTK_RESPONSE_CLOSE);

  if (!show_only_errors)
      gtk_widget_set_sensitive (c->new_button, FALSE);

  gtk_box_pack_start_defaults (GTK_BOX (GTK_DIALOG (dialog)->vbox),
			       make_cat_list (c));

  g_signal_connect (dialog, "response",
		    G_CALLBACK (cat_response), c);

  gtk_widget_set_size_request (dialog, 600, 300);

  gtk_widget_show_all (dialog);

  /* Make sure the cache is up-to-date first */
  ensure_cache_updated (c);
}

/* Adding catalogues 
 */

struct add_catalogues_closure {
  xexp *catalogues;
  xexp *cur;
  xexp *rest;
  bool ask, update;

  bool catalogues_changed;

  void (*cont) (bool res, void *data);
  void *data;
};

static void add_catalogues_cont_1 (xexp *catalogues, void *data);
static void add_catalogues_cont_2 (add_catalogues_closure *c);
static void add_catalogues_cont_3_add (bool res, void *data);
static void add_catalogues_cont_3_enable (bool res, void *data);
static void add_catalogues_cont_3 (bool res, bool enable, void *data);
static void add_catalogues_cont_4 (bool keep_going, void *data);

static void
add_catalogues_cont_3_add (bool res, void *data)
{
  add_catalogues_cont_3 (res, false, data);
}

static void
add_catalogues_cont_3_enable (bool res, void *data)
{
  add_catalogues_cont_3 (res, true, data);
}

static void
add_catalogues_cont_3 (bool res, bool enable, void *data)
{
  add_catalogues_closure *c = (add_catalogues_closure *)data;

  if (res)
    {
      /* Add or enable it.
       */
      if (enable)
	{
	  g_assert (c->cur);
	  xexp_aset_bool (c->cur, "disabled", false);
	}
      else
	{
	  if (c->cur)
	    xexp_del (c->catalogues, c->cur);
	  xexp_append_1 (c->catalogues, xexp_copy (c->rest));
	}

      c->catalogues_changed = true;

      /* Move to next
       */
      c->rest = xexp_rest (c->rest);
      add_catalogues_cont_2 (c);
    }
  else
    {
      /* User cancelled.  If this operation is for updating the
	 catalogues before installing a package, we abort now.
	 Otherwise, the user can still decide whether to add the
	 remaining catalogues.
       */
      if (c->update)
	{
	  xexp_free (c->catalogues);
	  c->cont (false, c->data);
	  delete c;
	}
      else
	{
	  c->rest = xexp_rest (c->rest);
	  add_catalogues_cont_2 (c);
	}
    }
}

static void
add_catalogues_details (void *data)
{
  add_catalogues_closure *c = (add_catalogues_closure *)data;
  show_cat_edit_dialog (NULL, c->rest, true, cat_readonly, NULL);
}

static void
add_catalogues_cont_4 (bool keep_going, void *data)
{
  add_catalogues_closure *c = (add_catalogues_closure *)data;
  xexp_free (c->catalogues);
  c->cont (keep_going, c->data);
  delete c;
}

static void
add_catalogues_cont_2 (add_catalogues_closure *c)
{
  if (c->rest == NULL)
    {
      /* We want to refresh the cache every time for an 'update'
	 operation since we really want it to be uptodate now even if
	 we didn't make any changes to the catalogue configuration.
      */
      if (c->catalogues_changed || c->update)
	set_catalogues_and_refresh (c->catalogues,
				    (c->update
				     ? _("ai_nw_preparing_installation")
				     : NULL),
				    add_catalogues_cont_4, c);
      else
	add_catalogues_cont_4 (true, c);
    }
  else
    {
      void (*cont) (bool res, void *data);

      c->cur = find_catalogue (c->catalogues, c->rest);

      if (c->cur && xexp_aref_bool (c->cur, "disabled"))
	{
	  // Old version should be enabled
	  cont = add_catalogues_cont_3_enable;
	}
      else if (!c->update || c->cur == NULL)
	{
          // Let's remove useless data
          xexp_adel (c->rest, "file");
          xexp_adel (c->rest, "id");

          const char *uri = xexp_aref_text (c->rest, "uri");
          const char *dist = xexp_aref_text (c->rest, "dist");
          const char *comps = xexp_aref_text (c->rest, "components");

          if (uri != NULL)
            {
              // forbid any catalogue description bigger than 1024 chars
              int len = 10 + strlen (uri)
                + strlen (dist ? dist : "unknown") + strlen (comps ? comps : "");
              if (len < 1024)
                {
                  // New version should be added
                  cont = add_catalogues_cont_3_add;
                }
              else
                {
                  add_log ("attempted to install a bad formated catalogue\n");
                  cont = NULL;
                }
            }
          else
            cont = NULL;
	}
      else
	cont = NULL;

      if (cont)
	{
	  /* The catalogue is new or needs to be enabled.  If wanted,
	     ask the user whether to add it.
	   */

	  if (c->ask)
	    {
	      char *str;
	      const char *name = catalogue_name (c->rest);

	      if (cont == add_catalogues_cont_3_enable)
		str = g_strdup_printf ("%s\n%s",
				       _("ai_ia_add_catalogue_enable"),
				       _("ai_ia_add_catalogue_legal"));
	      else if (c->update)
                {
                  char *tmp = g_strdup_printf (_("ai_ia_add_catalogue_add"),
                                               name);
                  str = g_strdup_printf ("%s\n%s\n%s",
                                         _("ai_ia_add_catalogue_install"),
                                         _("ai_ia_add_catalogue_legal"),
                                         tmp);
                  g_free (tmp);
                }
	      else
                {
                  char *tmp = g_strdup_printf (_("ai_ia_add_catalogue_add"),
                                               name);
                  str = g_strdup_printf ("%s\n%s",
                                         _("ai_ia_add_catalogue_legal"),
                                         tmp);
                  g_free (tmp);
                }

	      ask_yes_no_with_arbitrary_details (_("ai_ti_add_catalogue"),
						 str,
						 cont,
						 add_catalogues_details,
						 c);
	      g_free (str);
	    }
	  else
	    cont (true, c);
	}
      else
	{
	  /* Nothing to be done for this catalogue, move to the next.
	   */
	  c->rest = xexp_rest (c->rest);
	  add_catalogues_cont_2 (c);
	}
    }
}

static void
add_catalogues_cont_1 (xexp *catalogues, void *data)
{
  add_catalogues_closure *c = (add_catalogues_closure *)data;

  if (catalogues == NULL)
    {
      c->cont (false, c->data);
      delete c;
    }
  else
    {
      c->catalogues = catalogues;
      add_catalogues_cont_2 (c);
    }
}

void
add_catalogues (xexp *catalogues,
		bool ask, bool update,
		void (*cont) (bool res, void *data),
		void *data)
{
  add_catalogues_closure *c = new add_catalogues_closure;
  c->cur = NULL;
  c->rest = xexp_first (catalogues);
  c->ask = ask;
  c->update = update;
  c->catalogues_changed = false;
  c->cont = cont;
  c->data = data;

  get_catalogues (add_catalogues_cont_1, c);
}

GString *
render_catalogue_report (xexp *catalogue_report)
{
  if (catalogue_report == NULL)
    return g_string_new (_("ai_ni_operation_failed"));

  GString *report = g_string_new ("");

  for (xexp *cat = xexp_first (catalogue_report); cat; cat = xexp_rest (cat))
    {
      xexp *errors = xexp_aref (cat, "errors");
      if (errors == NULL || xexp_first (errors) == NULL)
	continue;

      g_string_append_printf (report, "%s:\n", catalogue_name (cat));
      for (xexp *err = xexp_first (errors); err; err = xexp_rest (err))
	g_string_append_printf (report, "  %s\n   %s\n",
				xexp_aref_text (err, "uri"),
				xexp_aref_text (err, "msg"));
    }

  return report;
}

xexp *
get_failed_catalogues (xexp *catalogue_report)
{
  xexp *failed_cat_list = xexp_list_new ("catalogues");

  if (catalogue_report == NULL)
    return failed_cat_list;

  for (xexp *cat = xexp_first (catalogue_report); cat; cat = xexp_rest (cat))
    {
      xexp *errors = xexp_aref (cat, "errors");
      if (errors == NULL || xexp_first (errors) == NULL)
	continue;

      xexp_append_1 (failed_cat_list, xexp_copy (cat));
    }

  return failed_cat_list;
}
