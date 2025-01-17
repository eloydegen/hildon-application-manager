
/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2007, 2008 Nokia Corporation.  All Rights reserved.
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

#include <stdio.h>
#include <assert.h>
#include <iostream>
#include <libintl.h>
#include <errno.h>
#include <string.h>

#include <gtk/gtk.h>
#include <hildon/hildon-note.h>

#include "operations.h"
#include "instr.h"
#include "util.h"
#include "main.h"
#include "apt-worker-client.h"
#include "log.h"
#include "settings.h"
#include "details.h"
#include "dbus.h"
#include "user_files.h"

#define _(x) gettext (x)

#define HAM_BACKUP_RESPONSE 1

/* Common utilities
 */

static char *
result_code_to_message (package_info *pi, 
			apt_proto_result_code result_code)
{
  char *msg = NULL;
  bool upgrading = (pi->installed_version != NULL);

  if (result_code == rescode_download_failed)
    msg = g_strdup_printf (_("ai_nc_error_download_failed"),
			   pi->get_display_name (false));
  else if (result_code == rescode_packages_not_found)
    msg = g_strdup_printf (_("ai_ni_error_download_missing"),
			   pi->get_display_name (false));
  else if (result_code == rescode_package_corrupted)
    {
      if (upgrading)
	msg = g_strdup_printf (_("ai_ni_error_update_corrupted"),
			       pi->get_display_name (true));
      else
	msg = g_strdup_printf (_("ai_ni_error_install_corrupted"),
			       pi->get_display_name (false));
    }
  else if (result_code == rescode_out_of_space)
    msg = g_strdup (_("ai_ni_memory_shortage"));

  return msg;
}

void
installable_status_to_message (package_info *pi,
			       char *&msg, bool &with_details)
{
  if (pi->info.installable_status == status_missing)
    {
      msg = g_strdup_printf ((pi->installed_version
			      ? _("ai_ni_error_update_missing")
			      : _("ai_ni_error_install_missing")),
			     pi->get_display_name (false));
      with_details = true;
    }
  else if (pi->info.installable_status == status_conflicting)
    {
      msg = g_strdup_printf ((pi->installed_version
			      ? _("ai_ni_error_update_conflict")
			      : _("ai_ni_error_install_conflict")),
			     pi->get_display_name (false));
      with_details = true;
    }
  else if (pi->info.installable_status == status_corrupted)
    {
      msg = g_strdup_printf ((pi->installed_version
			      ? _("ai_ni_error_update_corrupted")
			      : _("ai_ni_error_install_corrupted")),
			     pi->get_display_name (false));
      with_details = false;
    }
  else if (pi->info.installable_status == status_incompatible)
    {
      msg = g_strdup_printf ((pi->installed_version
			      ? _("ai_ni_error_update_incompatible")
			      : _("ai_ni_error_install_incompatible")),
			     pi->get_display_name (false));
      with_details = false;
    }
  else if (pi->info.installable_status == status_incompatible_current)
    {
      msg = g_strdup_printf (_("ai_ni_error_n770package_incompatible"),
			     pi->get_display_name (false));
      with_details = false;
    }
  else if (pi->info.installable_status == status_not_found)
    {
      msg = g_strdup_printf (_("ai_ni_error_download_missing"),
			     pi->get_display_name (false));
      with_details = false;
    }
  else
    {
      msg = g_strdup_printf ((pi->installed_version
			      ? _("ai_ni_error_update_failed")
			      : _("ai_ni_error_installation_failed")),
			     pi->get_display_name (false));
      with_details = true;
    }
}

static void
launch_osso_backup ()
{
  /* Launch osso-backup as a single instance */
  osso_context_t *osso_context = NULL;
  if ((osso_context = get_osso_context ()))
    {
      if (osso_application_top (osso_context,
                                "com.nokia.backup",
                                NULL) != OSSO_OK)
        {
          what_the_fock_p ();
          add_log ("Could not launch backup application.\n");
        }
    }
}

/* INSTALL_PACKAGES - Overview

   0. Filter out already installed packages.  When the list is empty
      after this, an appropriate note is shown and the process aborts.

   1. Confirm packages.  When this is a 'restore' or 'card' flow, the
      multi-package selection dialog is used, otherwise all packages
      except the first are ignored and a single package confirmation
      dialog is used.

   2. Check for the 'certified' status of all selected packages.  For
      each of the selected packages, a 'check_install' operation is
      performed and when one of them would install packages from a
      non-certified domain, the Notice dialog is shown.

   The following is repeated for each selected package, as indicated.
   "Aborting this package" means that an error message is shown and
   when there is another package to install, the user is asked whether
   to continue or not.

   The list of packages is sorted so that packages that require a
   reboot (i.e. that have the reboot or flash-and-reboot flag) are
   handled last.  When multiple packages require a reboot, only one of
   them is kept in the list; the rest are ignored.

   1. Check if the package is actually installable, and abort it when
      not.

   2. Check for domain violations, and abort when some are found.

   3. Check if enough storage is available and decide where to
      download the packages to.

   4. If the package requires a reboot, put up the big information
      note explaining this, with the "Create Backup" button.  When the
      user closes this, close all applications.

   5. Download the packages.

   6. Check the free storage again.

   7. Run the 'checkrm' scripts of the upgraded packages and abort
      this package if the scripts asks for it.

   8. Do the actual install, aborting this package if it fails.  The
      downloaded archive files are removed in any case.

   9. If the packages has the 'flash-and-reboot' flag, call
      /usr/bin/flash-and-reboot.  Otherwise, if the package has the
      'reboot' flag, reboot.

   At the end:

   1. Refresh the lists of packages, if needed.
*/

struct ip_clos {
  char *title;
  char *desc;
  int install_type;
  bool automatic;

  GList *all_packages;   // all packages given to install_packages
  GList *packages;       // the ones that are not installed or uptodate
  GList *cur;            // the one currently under consideration

  // per installation iteration
  int flags;
  int64_t free_space;             // the required free storage space in bytes
  const char *alt_download_root;  // Alternative download root filesystem.
  GSList *upgrade_names;          // the packages and versions that we are going
  GSList *upgrade_versions;       // to upgrade to.

  // at the end
  bool entertaining;        // is the progress bar up?
  int n_successful;         // how many have been installed successfully

  void (*cont) (int n_successful, void *);
  void *data;

  bool refresh_needed;      // a package list refresh would be needed

  device_mode mode;         // original device mode before OS upgrade
};

static void ip_install_with_info (void *data);
static void ip_confirm_install_response (bool res, void *data);
static void ip_select_package_response (gboolean res, GList *selected_packages,
					void *data);
static void ip_ensure_network (ip_clos *c);
static void ip_ensure_network_reply (bool res, void *data);
static void ip_check_cert_start (ip_clos *c);
static void ip_check_cert_loop (ip_clos *c);
static void ip_check_cert_reply (int cmd, apt_proto_decoder *dec,
				 void *data);
static void ip_legalese_response (bool res, void *data);

static void ip_install_start (ip_clos *c);
static void ip_install_loop (ip_clos *c);
static void ip_check_domain_reply (int cmd, apt_proto_decoder *dec, void *data);
static void ip_install_anyway (bool res, void *data);
static void ip_get_info_for_install (void *data);
static void ip_third_party_policy_check (package_info *pi, void *data,
                                         bool changed);
static void ip_third_party_policy_check_reply (package_info *pi, void *data);
static void ip_with_new_info (package_info *pi, void *data);
static void ip_warn_about_reboot (ip_clos *c);
static void ip_warn_about_reboot_response (GtkDialog *dialog, gint response,
					   gpointer data);

static void ip_not_enough_memory (void *data, int64_t download_size);
static void ip_not_enough_battery_confirm (void (*cont) (void *data), void *data);
static void ip_not_enough_battery_confirm_response (void *data);

static void ip_install_one (void *data);
static void ip_install_one_with_space_checked (int cmd, apt_proto_decoder *dec, void *data);
static void ip_maybe_continue (bool res, void *data);

static void ip_execute_checkrm_script (const char *name,
				       const char **params,
				       void (*cont) (int status, void *data),
				       void *data);
static void ip_execute_checkrm_script_done (int status, void *data);

static void ip_check_upgrade (void *data);
static void ip_check_upgrade_reply (int cmd, apt_proto_decoder *dec,
				    void *data);
static void ip_check_upgrade_loop (ip_clos *c);
static void ip_check_upgrade_cmd_done (int status, void *data);
static void ip_download_cur (void *data);
static void ip_download_cur_reply (int cmd, apt_proto_decoder *dec, void *data);
static void ip_download_cur_fail (void *data);
static void ip_download_cur_retry_confirm (apt_proto_result_code result_code, void *data);
static void ip_download_cur_retry_confirm_response (bool result, void *data);
static gboolean ip_kill_all_and_install_delayed (gpointer data);
static void ip_install_cur (void *data);
static void ip_install_cur_with_space_checked (int cmd, apt_proto_decoder *dec, void *data);
static void ip_install_cur_reply (int cmd, apt_proto_decoder *dec, void *data);
static void ip_clean_reply (int cmd, apt_proto_decoder *dec, void *data);
static void ip_install_next (void *data);

static void ip_set_device_mode (ip_clos *c, device_mode dmode);
static void ip_maybe_restore_device_mode (ip_clos *c);

static void ip_upgrade_all_confirm (GList *package_list,
				   void (*cont) (bool res, void *data),
				   void *data);
static void ip_upgrade_all_confirm_response (bool res, void *data);

static void ip_show_cur_details (void *data);
static void ip_show_cur_problem_details (void *data);
static void ip_show_details_done (void *data);

static void ip_abort_cur (ip_clos *c, const char *msg, bool with_details);
static void ip_abort_cur_with_status_details (ip_clos *c);
static void ip_abort_response (GtkDialog *dialog, gint response,
			       gpointer data);

static void ip_end (void *data);

static void ip_reboot (void *data);
static void ip_reboot_delayed (void *data);
static gboolean ip_reboot_now (void *data);
static void ip_reboot_reply (int cmd, apt_proto_decoder *dec, void *data);

struct one_ip_clos {
  GList *list;
  void *data;
  void (*cont) (int n_successful, void*);
};

static void
one_install_package_end (int n_successful, void *data)
{
  one_ip_clos *c = (one_ip_clos *) data;

  g_list_free (c->list);
  c->cont (n_successful, c->data);
  delete c;
}

void
install_package (package_info *pi,
		 void (*cont) (int n_successful, void *), void *data)
{
  one_ip_clos *c = new one_ip_clos;

  c->list = g_list_prepend (NULL, pi);
  c->cont = cont;
  c->data = data;

  install_packages (c->list,
		    INSTALL_TYPE_STANDARD, false,
		    NULL, NULL,
                    one_install_package_end, c);
}

void
install_packages (GList *packages,
		  int install_type,
		  bool automatic,
		  const char *title, const char *desc,
		  void (*cont) (int n_successful, void *), void *data)
{
  ip_clos *c = new ip_clos;

  c->install_type = install_type;
  c->title = g_strdup (title);
  c->desc = g_strdup (desc);
  c->automatic = automatic;
  c->all_packages = packages;
  c->cont = cont;
  c->data = data;
  c->alt_download_root = NULL;
  c->upgrade_names = NULL;
  c->upgrade_versions = NULL;
  c->n_successful = 0;
  c->entertaining = false;
  c->refresh_needed = false;
  c->mode = DEVICE_MODE_UNKNOWN; /* Not known yet (SSU only) */

  get_package_infos (packages,
		     true,
		     ip_install_with_info,
		     c);
}

static bool
package_needs_reboot (package_info *pi)
{
  return (pi->info.install_flags
	  & (pkgflag_reboot
	     | pkgflag_flash_and_reboot
	     | pkgflag_close_apps));
}

static void
ip_install_with_info (void *data)
{
  ip_clos *c = (ip_clos *)data;

  // Filter and sort packages, stopping after the first when this is a
  // standard install.

  c->packages = NULL;
  GList *normal_packages = NULL, *rebooting_packages = NULL;

  for (GList *p = c->all_packages; p; p = p->next)
    {
      /* When restoring a backup, we only show packages that are not
	 installed at all.  Otherwise, we show packages that are not
	 installed or have a newer version available.
      */

      package_info *pi = (package_info *)p->data;
      if ((c->install_type == INSTALL_TYPE_BACKUP
	   && pi->installed_version == NULL)
	  || (c->install_type != INSTALL_TYPE_BACKUP
	      && pi->available_version != NULL))
	{
	  if (package_needs_reboot (pi))
	    rebooting_packages = g_list_append (rebooting_packages, pi);
	  else
	    normal_packages = g_list_append (normal_packages, pi);
	  
	  if (c->install_type == INSTALL_TYPE_STANDARD)
	    break;
	}
    }

  if (rebooting_packages)
    {
      /* Pick the first rebooting package and ignore the rest.
       */
      c->packages = g_list_append (normal_packages,
				   rebooting_packages->data);
      g_list_free (rebooting_packages->next);
    }
  else
    c->packages = normal_packages;

  if (c->packages == NULL)
    {
      annoy_user (_("ai_ni_all_installed"), ip_end, c);
      return;
    }

  // Bring up the appropriate confirmation dialog.

  if (c->install_type == INSTALL_TYPE_BACKUP
#ifdef INSTALL_TYPE_MEMORY_CARD
      || c->install_type == INSTALL_TYPE_MEMORY_CARD
#endif
      || c->install_type == INSTALL_TYPE_MULTI)
    {
      const char *title = c->title;
      const char *desc = c->desc;

      if (title == NULL)
	title = _("ai_ti_install_apps");

      if (desc == NULL)
	{
	  switch (c->install_type)
	    {
	    case INSTALL_TYPE_BACKUP:
	      desc = _("ai_ia_restore");
	      break;
#ifdef INSTALL_TYPE_MEMORY_CARD
	    case INSTALL_TYPE_MEMORY_CARD:
	      desc = _("ai_ia_memory"); /*NOLOC*/
	      break;
#endif
	    default:
	      desc = _("ai_li_install");
	      break;
	    }
	}

      select_package_list (c->packages,
                           title, desc,
                           ip_select_package_response, c);
    }
  else if (c->install_type == INSTALL_TYPE_UPGRADE_ALL_PACKAGES)
    {
      ip_upgrade_all_confirm (c->packages,
			      ip_upgrade_all_confirm_response,
			      c);
    }
  else if (c->install_type != INSTALL_TYPE_UPDATE_SYSTEM)
    {
      c->cur = c->packages;
      ip_confirm_install_response (true, c);
    }
  else
    ip_confirm_install_response (true, c);
}

static void
ip_show_cur_details (void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);
  
  show_package_details (pi, install_details, false, ip_show_details_done, c);
}

static void
ip_show_cur_problem_details (void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);
  
  show_package_details (pi, install_details, true, ip_show_details_done, c);
}

static void
ip_show_details_done (void *data)
{
}

static void
ip_select_package_response (gboolean res, GList *selected_packages,
			    void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (!res)
    {
#ifdef INSTALL_TYPE_MEMORY_CARD
      if (c->install_type == INSTALL_TYPE_MEMORY_CARD
	  && c->automatic)
	annoy_user (_("ai_ni_memory_cancelled"), /*NOLOC*/
		    ip_end, c);
      else
#endif
	ip_end (c);
    }
  else if (selected_packages == NULL)
    {
      ip_end (c);
    }
  else
    {
      g_list_free (c->packages);
      c->packages = selected_packages;
      ip_ensure_network (c);
    }
}

static void
ip_confirm_install_response (bool res, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (res)
    ip_ensure_network (c);
  else
    ip_end (c);
}

static void
ip_ensure_network (ip_clos *c)
{
  /* Start entertaining the user here.  We stop in ip_end, at the
     last.
   */

  set_entertainment_cancel (NULL, NULL);
  set_entertainment_main_title (_("ai_nw_preparing_installation"));
  set_entertainment_sub_title ("");
  set_entertainment_fun (NULL, -1, -1, 0);
  start_entertaining_user (TRUE);
  
  c->entertaining = true;

  ensure_network (ip_ensure_network_reply, c);
}

static void
ip_ensure_network_reply (bool res, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (res)
    ip_check_cert_start (c);
  else
    ip_end (c);
}

static void
ip_check_cert_start (ip_clos *c)
{
  c->cur = c->packages;
  ip_check_cert_loop (c);
}

static void
ip_check_cert_loop (ip_clos *c)
{
  if (c->cur)
    {
      package_info *pi = (package_info *)c->cur->data;

      if (pi->have_info 
	  && pi->info.installable_status == status_not_found)
	{
	  /* Skip packages that we know don't exist.
	   */
	  c->cur = c->cur->next;
	  ip_check_cert_loop (c);
	}
      else
	apt_worker_install_check (pi->name, ip_check_cert_reply, c);
    }
  else
    {
      /* All packages passed the check.  How unusual.
       */
      guint l = g_list_length (c->packages);

      if (l == 1)
        {
          c->cur = c->packages;
          package_info *pi = (package_info *) c->cur->data;
          install_confirm (false, pi, false,
                           ip_legalese_response, ip_show_cur_details, c);
        }
      else // we already annoyed the user
        ip_install_start (c);
    }
}

static void
ip_check_cert_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (dec == NULL)
    {
      ip_end (c);
      return;
    }

  bool some_not_certified = false;

  while (!dec->corrupted ())
    {
      apt_proto_pkgtrust trust = apt_proto_pkgtrust (dec->decode_int ());
      if (trust == pkgtrust_end)
	break;

      if (trust == pkgtrust_not_certified
	  || trust == pkgtrust_domains_violated)
	some_not_certified = true;

      dec->decode_string_in_place ();  // name
    }

  if (some_not_certified)
    {
      package_info *pi = (package_info *) c->cur->data;
      install_confirm (true, pi, g_list_length (c->all_packages) > 1,
                       ip_legalese_response, ip_show_cur_details, c);
    }
  else
    {
      c->cur = c->cur->next;
      ip_check_cert_loop (c);
    }

  /* We ignore the rest of the reply, including the success
     indication and everything.
   */
}

static void
ip_legalese_response (bool res, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (res)
    {
      /* User agrees to take the risk.  Let's start the show!
       */
      ip_install_start (c);
    }
  else
    ip_end (c);
}

static void
ip_install_start (ip_clos *c)
{
  c->cur = c->packages;
  ip_install_loop (c);
}

/* FIXME: Please remove this code when no longer needed. */
static void
force_icons_theme_reload (void)
{
  GdkEventClient event;

  /* Send GDK event to force icons theme reload */
  event.type = GDK_CLIENT_EVENT;
  event.window = NULL;
  event.send_event = TRUE;
  event.message_type = gdk_atom_intern_static_string ("_GTK_LOAD_ICONTHEMES");
  event.data_format = 32;
  gdk_event_send_clientmessage_toall ((GdkEvent *) &event);
}

static void
ip_install_loop (ip_clos *c)
{
  /* ensure device mode is restored in case it was modified by the
     previous installation of another package */
  ip_maybe_restore_device_mode (c);

  if (c->cur == NULL)
    {
      /* End of loop, show a success report to the user.

         If there is only one package in the list, talk specifically
         about that package.  Otherwise, just show the number of
         packages that have been successfully handled.
       */

      stop_entertaining_user ();
      c->entertaining = false;

      if (c->n_successful > 0)
	{
	  if (c->all_packages->next == NULL)
	    {
	      package_info *pi = (package_info *)c->all_packages->data;
	      char *str = NULL;
	      if (pi->installed_version != NULL)
		{
		  str = g_strdup_printf (_("ai_ni_software_update_installed"));
		}
	      else
		{
// 		  str = g_strdup_printf (_("ai_ni_install_successful_launch"),
// 					 pi->get_display_name (false));
                  str = g_strdup_printf (_("ai_ni_multiple_install"),
                                         c->n_successful);
		}

              /* Force reloading icons theme */
              /* FIXME: This shouldn't be done here as it's not
               * responsibility of HAM that the icons don't get
               * updated when changed, added or removed, but of other
               * components (most likely, Gtk+).
               * Please remove this code when no longer needed.
               */
              force_icons_theme_reload ();

	      annoy_user (str, ip_end, c);
	      g_free (str);
	    }
	  else
	    {
	      char *str = NULL;
	      if (c->install_type == INSTALL_TYPE_UPGRADE_ALL_PACKAGES)
		str = g_strdup (_("ai_ni_software_update_installed"));
	      else
		str = g_strdup_printf (ngettext ("ai_ni_multiple_install",
						 "ai_ni_multiple_installs",
						 c->n_successful),
				       c->n_successful);

	      annoy_user (str, ip_end, c);
	      g_free (str);
	    }
	}
      else
	ip_end (c);
    }
  else if (red_pill_mode)
    {
      /* Check for domain violations
       * Only in red_pill mode, because if you are in blue-pill mode
       * package updates from wrong domains aren't visible.
       */
      package_info *pi = (package_info *)(c->cur->data);

      apt_worker_install_check (pi->name, ip_check_domain_reply, c);
    }
  else
    ip_get_info_for_install (c);
}

static void
ip_check_domain_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (dec == NULL)
    {
      ip_end (c);
      return;
    }

  bool some_domains_changed = false;

  while (!dec->corrupted ())
    {
      apt_proto_pkgtrust trust = apt_proto_pkgtrust (dec->decode_int ());
      if (trust == pkgtrust_end)
	break;

      if (trust == pkgtrust_domains_violated)
	some_domains_changed = true;

      dec->decode_string_in_place ();  // name
    }

  if (some_domains_changed)
    {
      gchar *msg = NULL;

      msg = g_strdup_printf ("%s\nInstall anyway?", msg);

      ask_custom (msg,
                  dgettext ("hildon-libs", "wdgt_bd_yes"),
                  dgettext ("hildon-libs", "wdgt_bd_no"),
                  ip_install_anyway, c);

      g_free (msg);
    }
  else
    ip_get_info_for_install (c);
}

static void
ip_install_anyway (bool res, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (res)
    ip_get_info_for_install (c);
  else
    ip_install_next (c);
}

static void
ip_get_info_for_install (void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  /* Reget info.  It might have been changed by previous
     installations.
  */
  pi->have_info = false;
  get_package_info (pi, true, ip_third_party_policy_check, c);
}

static void
ip_third_party_policy_check (package_info *pi, void *data, bool unused)
{
  ip_clos *c = (ip_clos *)data;
  gchar *msg = NULL;

  if ((red_pill_mode && red_pill_ignore_thirdparty_policy)
      || (pi->third_party_policy == third_party_compatible))
    {
      /* Just continue with the installation in this case */
      ip_with_new_info (pi, c);
    }
  else if (pi->third_party_policy == third_party_incompatible)
    {
      /* Set the proper status message and abort */
       msg = g_strdup_printf ("!!!%s breaks the 3rd party package policy",
                              pi->get_display_name (false));
      ip_abort_cur (c, msg, true);
      g_free (msg);
    }
  else
    {
      /* Check the third party policy for the first time */
      check_third_party_policy (pi, ip_third_party_policy_check_reply, c);
    }
}

static void ip_third_party_policy_check_reply (package_info *pi, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (pi->third_party_policy == third_party_incompatible)
    {
      /* Set the proper status message and abort */
      gchar *msg = g_strdup_printf ("!!!%s breaks the 3rd party package policy",
                                    pi->get_display_name (false));
      ip_abort_cur (c, msg, true);
      g_free (msg);
    }
  else
    {
      /* Continue with installation if compliant */
      ip_with_new_info (pi, c);
    }
}

static void
ip_with_new_info (package_info *pi, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (pi->info.installable_status == status_able)
    {
      if (package_needs_reboot (pi))
	ip_warn_about_reboot (c);
      else
	ip_install_one (c);
    }
  else
    ip_abort_cur_with_status_details (c);
}

static void
ip_warn_about_reboot (ip_clos *c)
{
  GtkWidget *dialog = NULL;

  dialog = gtk_dialog_new_with_buttons
    (_("ai_ti_operating_system_update"),
     NULL,
     GTK_DIALOG_MODAL,
     _("ai_bd_create_backup"), HAM_BACKUP_RESPONSE,
     _("ai_bd_confirm_ok"), GTK_RESPONSE_OK,
     NULL);
  push_dialog (dialog);

  gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);
  GtkWidget *label = gtk_label_new (_("ai_ia_osupdate_restart"));
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox),
		     label);
  gtk_widget_show_all (dialog);

  g_signal_connect (dialog, "response",
		    G_CALLBACK (ip_warn_about_reboot_response), c);
}

static void
ip_warn_about_reboot_response (GtkDialog *dialog, gint response,
			       gpointer data)
{
  ip_clos *c = (ip_clos *)data;

  if (response == HAM_BACKUP_RESPONSE)
    {
      launch_osso_backup ();
    }
  else
    {
      pop_dialog (GTK_WIDGET (dialog));
      gtk_widget_destroy (GTK_WIDGET (dialog));

      if (response == GTK_RESPONSE_OK)
	{
	  package_info *pi = (package_info *)(c->cur->data);

          close_apps ();
          set_prestarted_apps_enabled (FALSE);

	  /* Convert the entertainment dialog into system modal if the
	     package to be installed requires rebooting the system */
	  if (pi->info.install_flags & pkgflag_reboot)
	    set_entertainment_system_modal ();

	  /* Make sure we are done before continuing */
	  while (gtk_events_pending ())
	    gtk_main_iteration ();

	  ip_install_one (c);
	}
      else
	ip_end (c);
    }
}

static void
ip_not_enough_memory (void *data, int64_t download_size)
{
  ip_clos *c = (ip_clos *)data;

  /** @todo: how to report the needed download size? */
  
  if (red_pill_mode)
    {
      /* Allow continuation
       */
      char *msg = g_strdup_printf ("%s\n%s", _("ai_ni_memory_shortage"),
                                   _("ai_ni_continue_install"));
      ask_yes_no (msg, ip_maybe_continue, c);
      g_free (msg);
    }
  else
    ip_abort_cur (c, _("ai_ni_memory_shortage"), false);
}

struct ipneb_clos {
  void (*callback) (void *data);
  void *data;
};

static void
ip_not_enough_battery_confirm (void (*cont) (void *data),
			       void *data)
{
  ip_clos *c = (ip_clos *)data;
  ipneb_clos *neb_clos = NULL;
  gchar *text = g_strdup (_("ai_ni_error_battery_empty"));

  neb_clos = new ipneb_clos;
  neb_clos->callback = cont;
  neb_clos->data = c;

  annoy_user (text, ip_not_enough_battery_confirm_response, neb_clos);

  g_free (text);
}

static void
ip_not_enough_battery_confirm_response (void *data)
{
  ipneb_clos *c = (ipneb_clos *)data;

  bool res = enough_battery_p ();

  if (res)
    c->callback (c->data);
  else
    ip_end (c->data);

  delete c;
}

static void
ip_set_device_mode (ip_clos *c, device_mode dmode)
{
  /* XXX - We ought ask to the user if we could turn off the
     device mode
  */

  /* Do nothing if there's no valid ip_clos */
  if (c == NULL)
    return;

  if (dmode != DEVICE_MODE_UNKNOWN)
    {
      /* Save current device mode and change it */
      c->mode = get_device_mode ();
      set_device_mode (dmode);
    }
}

static void
ip_maybe_restore_device_mode (ip_clos *c)
{
  /* Do nothing if there's no valid ip_clos or previous saved mode */
  if ((c == NULL) || (c->mode == DEVICE_MODE_UNKNOWN))
    return;

  /* Restore device mode */
  set_device_mode (c->mode);

  /* Force c->mode has to be set before calling this function
     again in the future */
  c->mode = DEVICE_MODE_UNKNOWN;
}

static void
ip_install_one (void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  add_log ("-----\n");
  if (pi->installed_version)
    add_log ("Upgrading %s %s to %s\n", pi->name,
	     pi->installed_version, pi->available_version);
  else
    add_log ("Installing %s %s\n", pi->name, pi->available_version);

  /* Check battery when doing an upgrade of an OS package */
  if ((pi->info.install_flags & pkgflag_system_update)
      && !enough_battery_p ())
    {
      ip_not_enough_battery_confirm (ip_install_one, c);
      return;
    }

  /* Check free space to install */
  apt_worker_get_free_space (ip_install_one_with_space_checked, c);
}

static void
ip_install_one_with_space_checked (int cmd, apt_proto_decoder *dec, void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  if (dec == NULL)
    {
      ip_end (c);
      return;
    }

  int64_t free_space = dec->decode_int64 ();

  if (free_space < 0)
    annoy_user_with_errno (errno, "get_free_space",
			   ip_end, c);

  /* If there's enough space somewhere, proceed with installation */
  if (pi->info.required_free_space < free_space)
    ip_check_upgrade (c);
  else
    {
      /* Not enough free space */
      ip_not_enough_memory (c, pi->info.required_free_space);
    }
}

static void
ip_maybe_continue (bool res, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (res)
    ip_check_upgrade (c);
  else
    ip_install_next (c);
}


struct ipecs_clos {
  char **argv;
  void (*cont) (int status, void *data);
  void *data;
};

static void
ip_execute_checkrm_script_done (int status, void *data)
{
  ipecs_clos *c = (ipecs_clos *) data;

  g_strfreev (c->argv);
  c->cont (status, c->data);
  delete c;
}

static void
ip_execute_checkrm_script (const char *name,
			   const char **params,
			   void (*cont) (int status, void *data),
			   void *data)
{
  int i;
  int argc;
  char **argv = NULL;
  char *cmd = NULL;
  ipecs_clos *clos;
  struct stat buf;
  int stat_result;

  /* Count the number of params */
  argc = 0;
  for (i = 0; params[i] != NULL; i++)
    argc++;

  /* Choose the right checkrm command */
  cmd = g_strdup_printf ("/var/lib/hildon-application-manager/info/%s.checkrm",
			 name);

  /* If not present in the usual place, use the old one */
  stat_result = stat (cmd, &buf);
  if (stat_result == -1)
    {
      g_free (cmd);
      cmd = g_strdup_printf ("/var/lib/osso-application-installer/info/%s.checkrm",
			     name);
    }

  /* Build the argv array */
  argv = g_new(char *, argc+2);
  argv[0] = g_strdup(cmd);
  for (i = 0; params[i] != NULL; i++)
    argv[i+1] = g_strdup(params[i]);
  argv[i+1] = NULL;

  /* Execute command and continue the process */
  clos = new ipecs_clos;
  clos->argv = argv;
  clos->cont = cont;
  clos->data = data;
  run_cmd (argv, true, ip_execute_checkrm_script_done, clos);

  g_free (cmd);
}

static void
ip_check_upgrade (void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  apt_worker_install_check (pi->name, ip_check_upgrade_reply, c);
}

static void
ip_check_upgrade_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  ip_clos *c = (ip_clos *)data;

  c->upgrade_names = NULL;
  c->upgrade_versions = NULL;

  if (dec == NULL)
    {
      ip_end (c);
      return;
    }

  /* Skip the cert information of the reply.
   */
  while (!dec->corrupted ())
    {
      apt_proto_pkgtrust trust = apt_proto_pkgtrust (dec->decode_int ());
      if (trust == pkgtrust_end)
	break;

      dec->decode_string_in_place ();  // name
    }

  while (!dec->corrupted ())
    {
      char *name = dec->decode_string_dup ();
      if (name == NULL)
	break;

      char *version = dec->decode_string_dup ();

      push (c->upgrade_names, name);
      push (c->upgrade_versions, version);
    }

  int success = dec->decode_int ();

  if (success)
    ip_check_upgrade_loop (c);
  else
    annoy_user (_("ai_ni_operation_failed"), ip_end, c);
}

static void
ip_check_upgrade_loop (ip_clos *c)
{
  if (c->upgrade_names)
    {
      char *name = (char *)(c->upgrade_names->data);
      char *version = (char *)(c->upgrade_versions->data);
      const char *params[] = { "upgrade", version, NULL };

      ip_execute_checkrm_script (name, params, ip_check_upgrade_cmd_done, c);
    }
  else
    ip_download_cur (c);
}

static void
clear (GSList *&lst)
{
  while (lst)
    g_free (pop (lst));
}
  
static void
ip_check_upgrade_cmd_done (int status, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (status != -1 && WIFEXITED (status) && WEXITSTATUS (status) == 111)
    {
      /* XXX - find better package name to use in error message.
       */
      char *str =
	g_strdup_printf (_("ai_ni_error_uninstall_applicationrunning"),
			 (char *)(c->upgrade_names->data));

      clear (c->upgrade_names);
      clear (c->upgrade_versions);

      ip_abort_cur (c, str, false);
      g_free (str);
    }
  else
    {
      g_free (pop (c->upgrade_names));
      g_free (pop (c->upgrade_versions));
      ip_check_upgrade_loop (c);
    }
}

static void
ip_download_cur (void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  char *title = NULL;
  if (pi->installed_version != NULL)
    {
      title = g_strdup_printf (_("ai_nw_updating"),
			       pi->get_display_name (false),
			       pi->get_display_version (false));
    }
  else
    {
      title = g_strdup_printf (_("ai_nw_installing"),
			       pi->get_display_name (false));
    }

  reset_entertainment ();
  set_entertainment_fun (NULL, -1, -1, 0);
  set_entertainment_main_title (title);
  g_free (title);

  set_log_start ();
  apt_worker_download_package (pi->name, ip_download_cur_reply, c);
}

struct ipdcr_clos {
  apt_proto_result_code result_code;
  ip_clos *c;
};

static void
ip_download_cur_retry_confirm (apt_proto_result_code result_code, void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);
  ipdcr_clos *clos = new ipdcr_clos;

  result_code = scan_log_for_result_code (result_code);
  char *msg = result_code_to_message (pi, result_code);
  if (msg == NULL)
    msg = g_strdup_printf ((pi->installed_version != NULL
        ? _("ai_ni_error_update_failed")
        : _("ai_ni_error_installation_failed")),
        pi->get_display_name (false));

  clos->result_code = result_code;
  clos->c = c;

  ask_yes_no (msg, ip_download_cur_retry_confirm_response, clos);

  g_free (msg);
}

static void
ip_download_cur_fail (void *data)
{
  ip_download_cur_retry_confirm_response (false, data);
}

static void
ip_download_cur_retry (bool res, void *data)
{
  ip_clos *c = (ip_clos *) data;

  if (res)
    {
      ip_download_cur (c);
    }
  else
    {
      ip_end (c);
    }
}

static void
ip_download_cur_retry_confirm_response (bool result, void *data)
{
  ipdcr_clos *clos = (ipdcr_clos *)data;
  ip_clos *c = (ip_clos *) clos->c;
  package_info *pi = (package_info *)(c->cur->data);

  if (result)
    {
      ensure_network (ip_download_cur_retry, c);
    }
  else
    {
      if (c->cur->next != NULL)
        {
          char *msg = result_code_to_message (pi, clos->result_code);
          if (msg == NULL)
            msg = g_strdup_printf ((pi->installed_version != NULL
                ? _("ai_ni_error_update_failed")
                : _("ai_ni_error_installation_failed")),
                pi->get_display_name (false));

          ip_abort_cur (c, msg, false);
          g_free (msg);
        }
      else
        ip_end (c);
    }
  
  delete clos;
}

static void
ip_download_cur_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  ip_clos *c = (ip_clos *)data;
  static int failure_count = 0;

  if (dec == NULL)
    {
      ip_end (c);
      return;
    }

  apt_proto_result_code result_code =
    apt_proto_result_code (dec->decode_int ());
  int64_t download_size = dec->decode_int64 ();
  c->alt_download_root = dec->decode_string_dup ();

  add_log ("required disk space: %Ld\n", download_size);

  add_log ("result code = %d\n", result_code);

  if (result_code == rescode_success)
    {
      failure_count = 0;
      ip_install_cur (c);
    }
  else if (result_code == rescode_out_of_space)
    {
      /* Not enough free space */
      ip_not_enough_memory (c, download_size);
    }
  else if (result_code == rescode_download_failed)
    {
      if (entertainment_was_cancelled ()
          && !entertainment_was_broke ())
        {
          apt_worker_clean (ip_clean_reply, NULL);
          ip_end (c);
        }
      else
        {
          failure_count++;

          /*
           * Another stupid request: 3 automatic retries before
           * noticing the user about the problem
           */
          if (failure_count < 3)
            ensure_network (ip_download_cur_retry, c);
          else
            ip_download_cur_retry_confirm (rescode_download_failed, c);
        }
    }
  else
    ip_download_cur_retry_confirm (result_code, c);
}

static gboolean
ip_kill_all_and_install_delayed (gpointer data)
{
  ip_clos *c = (ip_clos *)data;

  if ((c != NULL) && (c->cur != NULL))
    {
      package_info *pi = (package_info *)(c->cur->data);

      /* Kill running processes that could interfere for SSU */
      /* Disclaimer: this is a nasty workaround to prevent from a
         bigger problem to happen because of unknown reasons atm.
         Programmer avoids any responsibility on this code,
         implemented under high pressure as requested "from above". */
      kill_processes_for_SSU ();

      /* Continue the process */
      apt_worker_install_package (pi->name,
                                  c->alt_download_root,
                                  ip_install_cur_reply, c);
    }

  /* Remove source */
  return FALSE;
}

static void
ip_install_cur (void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  /* entertain the user while the packages are checked */
  set_entertainment_cancel (NULL, NULL);
  set_entertainment_fun (NULL, -1, -1, 0);

  /* Check battery when doing an upgrade of an OS package */
  if ((pi->info.install_flags & pkgflag_system_update)
      && !enough_battery_p ())
    {
      ip_not_enough_battery_confirm (ip_install_cur, c);
      return;
    }

  /* Check free space before downloading */
  apt_worker_get_free_space (ip_install_cur_with_space_checked, c);
}

static void
ip_install_cur_with_space_checked (int cmd, apt_proto_decoder *dec, void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  if (dec == NULL)
    {
      ip_end (c);
      return;
    }

  int64_t free_space = dec->decode_int64 ();
  if (free_space < 0)
    annoy_user_with_errno (errno, "get_free_space",
			   ip_end, c);

  if (pi->info.required_free_space < free_space)
    {
      if (pi->info.install_flags & pkgflag_system_update)
        {
          /* Set offline mode for SSU packages */
          ip_set_device_mode (c, DEVICE_MODE_OFFLINE);

          /* Before continuing, stop hildon-status-menu to avoid nasty
           * problem happening sometimes when installing an SSU, but
           * wait some time first because of the offline mode change
           * (and 3 seconds should be more than enough) */
          g_timeout_add (3000, ip_kill_all_and_install_delayed, c);
        }
      else
        {
          /* Proceed to install if there's enough free space and no
             SSU package is being installed */
          apt_worker_install_package (pi->name,
                                      c->alt_download_root,
                                      ip_install_cur_reply, c);
        }
    }
  else
    {
      /* Not enough free space */
      ip_not_enough_memory (c, pi->info.required_free_space);
    }
}

static void
ip_install_cur_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  bool needs_reboot = package_needs_reboot (pi);

  if (dec == NULL)
    {
      ip_end (c);
      return;
    }

  apt_proto_result_code result_code =
    apt_proto_result_code (dec->decode_int ());

  if (clean_after_install
      && ((result_code == rescode_success) || !needs_reboot))
    {
      /* Clean only when needed */
      apt_worker_clean (ip_clean_reply, NULL);
    }

  c->refresh_needed = true;

  /* Save the backup data right after installing the package */
  if (result_code == rescode_success)
    save_backup_data ();

  /* Reboot if needed */
  if (needs_reboot)
    ip_reboot (c);
  else if (result_code == rescode_success)
    {
      c->n_successful += 1;
      ip_install_next (c);
    }
  else
    {
      if (entertainment_was_cancelled ())
	ip_end (c);
      else
	{
	  result_code = scan_log_for_result_code (result_code);
	  char *msg =
	    result_code_to_message (pi, result_code);
	  if (msg == NULL)
	    msg = g_strdup_printf ((pi->installed_version != NULL
				    ? _("ai_ni_error_update_failed")
				    : _("ai_ni_error_installation_failed")),
				   pi->get_display_name (false));

	  ip_abort_cur (c, msg, false);
	  g_free (msg);
	}
    }
}

static void
ip_clean_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  /* Failure messages are in the log.  We don't annoy the user here.
     However, if cleaning takes really long, the user might get
     confused since apt-worker is not responding.
   */
}

static void
ip_install_next (void *data)
{
  ip_clos *c = (ip_clos *)data;

  c->cur = c->cur->next;
  ip_install_loop (c);
}

static void
ip_upgrade_all_confirm (GList *package_list,
		       void (*cont) (bool res, void *data),
		       void *data)
{
  int64_t acc_size = 0;
  GList * iter = NULL;
  char packages_size_str[20] = "";
  gchar *title = NULL;
  gchar *desc = NULL;
  gchar *tmp = NULL;

  /* Count total required size */
  for (iter = package_list; iter != NULL; iter = g_list_next (iter))
    {
      package_info *pi = (package_info *) iter->data;
      acc_size += pi->info.download_size;
    }
  size_string_general (packages_size_str, 20, acc_size);

  /* Title */
  title = g_strdup(_("ai_ti_confirm_update"));

  /* Description */
  tmp = g_strdup_printf (_("ai_ia_storage"), packages_size_str);
  desc = g_strdup_printf ("%s\n%s",
			  _("ai_nc_update_all"),
			  tmp);

  /* Show dialog */
  ask_yes_no_with_title (title, desc, cont, data);

  g_free (title);
  g_free (desc);
  g_free (tmp);
}

static void
ip_upgrade_all_confirm_response (bool res, void *data)
{
  ip_clos *c = (ip_clos *)data;

  if (res)
    ip_ensure_network (c);
  else
    ip_end (c);
}

static void
ip_abort_cur_with_status_details (ip_clos *c)
{
  package_info *pi = (package_info *)(c->cur->data);

  char *msg;
  bool with_details;

  installable_status_to_message (pi, msg, with_details);
  ip_abort_cur (c, msg, with_details);
  g_free (msg);
}

static void
ip_abort_cur (ip_clos *c, const char *msg, bool with_details)
{
  bool is_last = (c->cur->next == NULL);

  GtkWidget *dialog;
  gchar *final_msg = NULL;

  stop_entertaining_user ();
  c->entertaining = false;

  /* Build the final string to be shown as the dialog main text */
  if (!is_last)
    {
      final_msg =
	g_strdup_printf ("%s\n%s",
			 msg,
			 _("ai_ni_continue_install"));
    }
  else
    {
      final_msg = g_strdup (msg);
    }

  if (with_details)
    {
      if (is_last)
	{
          annoy_user_with_arbitrary_details (final_msg,
                                             ip_show_cur_problem_details,
                                             ip_end, c);
          goto annoy;
	}
      else
	{
	  dialog = hildon_note_new_confirmation_add_buttons
	    (NULL,
	     final_msg,
             _("ai_ni_bd_details"), 1,
             dgettext ("hildon-libs", "wdgt_bd_no"), GTK_RESPONSE_CANCEL,
             dgettext ("hildon-libs", "wdgt_bd_yes"), GTK_RESPONSE_OK,
	     NULL);
	}
    }
  else
    {
      if (is_last)
	{
          annoy_user (final_msg, ip_end, c);
          goto annoy;
	}
      else
	{
          dialog = hildon_note_new_confirmation (NULL, final_msg);
	}
    }

  push_dialog (dialog);

  g_signal_connect (dialog, "response",
                    G_CALLBACK (ip_abort_response), c);
  gtk_widget_show_all (dialog);

annoy:
  g_free (final_msg);
}

static void
ip_abort_response (GtkDialog *dialog, gint response, gpointer data)
{
  ip_clos *c = (ip_clos *)data;

  if (response == 1)
    ip_show_cur_problem_details (c);
  else
    {
      pop_dialog (GTK_WIDGET (dialog));
      gtk_widget_destroy (GTK_WIDGET (dialog));

      if (response == GTK_RESPONSE_OK)
	{
	  /* We only get an OK response when there is another package
	     to install.  Thus, we start the entertainment here again
	     since we know that some action will happen.
	  */
	  start_entertaining_user (TRUE);
	  c->entertaining = true;

	  ip_install_next (c);
	}
      else
	ip_end (c);
    }
}

static void
ip_end (void *data)
{
  ip_clos *c = (ip_clos *)data;

  /* restore the device mode if needed */
  ip_maybe_restore_device_mode (c);

  /* Make sure prestarted apps are enabled */
  set_prestarted_apps_enabled (TRUE);

  if (c->entertaining)
    stop_entertaining_user ();

  if (c->refresh_needed)
    {
      force_show_catalogue_errors ();
      get_package_list ();
    }

  if (c->packages != NULL)
    g_list_free (c->packages);

  c->cont (c->n_successful, c->data);

  g_free (c->title);
  g_free (c->desc);
  delete c;
}

static void
ip_autoremove_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  /* actually reboot after some time */
  g_timeout_add (2000, ip_reboot_now, data);
}

static void
ip_reboot (void *data)
{
  ip_clos *c = (ip_clos *)data;

  /* We need to get the package list before rebooting so that the
     "seen updates" state is stored correctly.
   */
  get_package_list_with_cont (ip_reboot_delayed, c);
}

static void
ip_reboot_delayed (void *data)
{
  ip_clos *c = (ip_clos *)data;
  package_info *pi = (package_info *)(c->cur->data);

  if (pi && pi->info.install_flags & pkgflag_system_update)
    irritate_user (_("ai_ni_device_restart_long"));
  else
    irritate_user (_("ai_cb_restarting_device"));

  /* restore the device mode if needed */
  ip_maybe_restore_device_mode (c);

  apt_worker_autoremove (ip_autoremove_reply, c);
}

static gboolean
ip_reboot_now (void *data)
{
  ip_clos *c = (ip_clos *)data;

  xexp *boot = xexp_list_new ("system-update");
  user_file_write_xexp (UFILE_BOOT, boot);
  xexp_free (boot);

  // package_info *pi = (package_info *)(c->cur->data);

  apt_worker_reboot (ip_reboot_reply, c);

  return FALSE;
}

static void
ip_reboot_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  ip_clos *c = (ip_clos *)data;

  sleep (3);
  ip_end (c);
}

/* UNINSTALL_PACKAGE - Overview

   0. Get details.

   1. Get confirmation.

   2. Run the checkrm scripts and abort if requested.

   3. Do the removal.
 */

struct up_clos {
  package_info *pi;

  int flags;
  GSList *remove_names;

  void (*cont) (void *);
  void *data;
};

static void up_checkrm_start (bool res, void *data);
static void up_checkrm_reply (int cmd, apt_proto_decoder *dec, void *data);
static void up_checkrm_loop (up_clos *c);
static void up_checkrm_cmd_done (int status, void *data);
static void up_remove (up_clos *c);
static void up_remove_with_info (package_info *pi, void *data, bool changed);
static void up_remove_reply (int cmd, apt_proto_decoder *dec, void *data);
static void up_autoremove_reply (int cmd, apt_proto_decoder *dec, void *data);
static void up_end (void *data);

void
uninstall_package (package_info *pi,
		   void (*cont) (void *data), void *data)
{
  up_clos *c = new up_clos;
  GString *text = g_string_new ("");
  char size_buf[20];

  c->pi = pi;
  c->cont = cont;
  c->data = data;
  
  size_string_general (size_buf, 20, c->pi->installed_size);
  g_string_printf (text, _("ai_nc_uninstall"),
		   c->pi->get_display_name (true),
		   c->pi->get_display_version (true), size_buf);

  ask_yes_no_with_details (_("ai_ti_confirm_uninstall"), text->str,
			   c->pi, remove_details,
			   up_checkrm_start, c);
  g_string_free (text, 1);
}

static void
up_checkrm_start (bool res, void *data)
{
  up_clos *c = (up_clos *)data;

  if (res)
    apt_worker_remove_check (c->pi->name,
			     up_checkrm_reply, c);
  else
    up_end (c);
}

static void
up_checkrm_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  up_clos *c = (up_clos *)data;
  
  if (dec == NULL)
    {
      up_end (c);
      return;
    }

  c->remove_names = NULL;
  while (true)
    {
      char *name = dec->decode_string_dup ();
      if (name == NULL)
	break;
      push (c->remove_names, name);
    }

  up_checkrm_loop (c);
}

static void
up_checkrm_loop (up_clos *c)
{
  if (c->remove_names)
    {
      char *name = (char *)pop (c->remove_names);
      const char *params[] = { "remove", NULL };

      ip_execute_checkrm_script (name, params, up_checkrm_cmd_done, c);
    }
  else
    up_remove (c);
}

static void
up_checkrm_cmd_done (int status, void *data)
{
  up_clos *c = (up_clos *)data;

  if (status != -1 && WIFEXITED (status) && WEXITSTATUS (status) == 111)
    {
      clear (c->remove_names);

      char *str =
	g_strdup_printf (_("ai_ni_error_uninstall_applicationrunning"),
			 c->pi->get_display_name (true));
      annoy_user (str, up_end, c);
      g_free (str);
    }
  else
    up_checkrm_loop (c);
}

static void
up_remove (up_clos *c)
{
  get_package_info (c->pi, false, up_remove_with_info, c);
}

static void
up_remove_with_info (package_info *pi, void *data, bool changed)
{
  up_clos *c = (up_clos *)data;

  if (c->pi->info.removable_status == status_able)
    {
      add_log ("-----\n");
      add_log ("Uninstalling %s %s\n", c->pi->name, c->pi->installed_version);

      char *title = g_strdup_printf (_("ai_nw_uninstalling"),
				     c->pi->get_display_name (true));
      set_entertainment_fun (NULL, -1, -1, 0);
      set_entertainment_cancel (NULL, NULL);
      set_entertainment_main_title (title);
      g_free (title);

      start_entertaining_user (FALSE);

      apt_worker_remove_package (c->pi->name, up_remove_reply, c);
    }
  else
    {
      if (c->pi->info.removable_status == status_needed)
	{
	  char *str = g_strdup_printf (_("ai_ni_error_uninstall_packagesneeded"),
				       c->pi->get_display_name (true));
	  annoy_user_with_details (str, c->pi, remove_details, up_end, c);
	  g_free (str);
	}
      else
	{
	  char *str = g_strdup_printf (_("ai_ni_error_uninstallation_failed"),
				       c->pi->get_display_name (true));
	  annoy_user_with_details (str, c->pi, remove_details, up_end, c);
	  g_free (str);
	}
    }
}

static void
up_remove_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  up_clos *c = (up_clos *)data;

  if (dec == NULL)
    {
      stop_entertaining_user ();
      up_end (c);
      return;
    }

  int success = dec->decode_int ();
  get_package_list ();
  save_backup_data ();

  if (success)
    apt_worker_autoremove (up_autoremove_reply, c);
  else
    {
      char *str = g_strdup_printf (_("ai_ni_error_uninstallation_failed"),
				   c->pi->get_display_name (true));
      annoy_user (str, up_end, c);
      g_free (str);
    }
}

static void
up_autoremove_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  up_clos *c = (up_clos *)data;

  stop_entertaining_user ();

  if (dec == NULL)
    {
      up_end (c);
      return;
    }

  char *str = g_strdup_printf (_("ai_ni_uninstall_successful"),
                               c->pi->get_display_name (true));
  annoy_user (str, up_end, c);
  g_free (str);
}


static void
up_end (void *data)
{
  up_clos *c = (up_clos *)data;

  force_show_catalogue_errors ();
  c->cont (c->data);
  delete c;
}


/* INSTALL_FILE - Overview

   0. Localize file

   1. Dispatch on extenstion to either if_install_local_deb_file or
      open_local_install_instructions.

   IF_INSTALL_LOCAL_DEB_FILE:

   0. Get details of file.

   1. Get confirmation.

   2. Show legal notice.

   3. Install file.

   XXX - Installing from file is much less powerful than installing
         from repositories.  For example, checkrm scripts are not run
         and dependencies are not automatially fulfilled.  In essence,
         installing from file is the unloved step child of the
         Application Manager.  A good solution might be to create a
         apt method that can access isolated .deb files directly.
 */

struct if_clos {
  char *filename;

  bool trusted;

  package_info *pi;

  void (*cont) (bool, void *);
  void *data;
};

static void if_local (char *local_filename, void *data);

static void if_install_local_deb_file (void *data);

static void if_details_reply (int cmd, apt_proto_decoder *dec, void *data);
static void if_install (bool res, void *data);
static void if_install_reply (int cmd, apt_proto_decoder *dec, void *data);
static void if_fail (bool res, void *data);
static void if_end_with_failure (void *data);
static void if_end_with_success (void *data);
static void if_end (bool success, void *data);

void install_file (const char *filename,
		   bool trusted,
		   void (*cont) (bool success, void *data), void *data)
{
  if_clos *c = new if_clos;

  c->filename = NULL;
  c->pi = NULL;
  c->trusted = trusted;
  c->cont = cont;
  c->data = data;

  localize_file_and_keep_it_open (filename, if_local, c);
}

static void
if_local (char *local_filename, void *data)
{
  if_clos *c = (if_clos *)data;

  if (local_filename)
    {
      if (g_str_has_suffix (local_filename, ".install"))
	{
	  /* XXX - if_end_with_success is too optimistic but
	           open_local_install_instructions doesn't report
	           the results yet.
	  */
	  open_local_install_instructions (local_filename,
					   if_end_with_success, c);
	}
      else
	{
	  c->filename = local_filename;
	  if_install_local_deb_file (c);
	}
    }
  else
    if_end (false, c);
}

void
if_install_local_deb_file (void *data)
{
  if_clos *c = (if_clos *)data;

  apt_worker_get_file_details (!(red_pill_mode && red_pill_show_all),
			       c->filename, if_details_reply, c);
}

static char *
first_line_of (const char *text)
{
  const char *end = strchr (text, '\n');
  if (end == NULL)
    return g_strdup (text);
  else
    return g_strndup (text, end-text);
}

static void
if_show_details_done (void *data)
{
}

static void
if_show_details (void *data)
{
  if_clos *c = (if_clos *) data;

  show_package_details (c->pi, install_details, false, if_show_details_done, c);
}

static void
if_details_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  if_clos *c = (if_clos *)data;

  if (dec == NULL)
    {
      if_end (false, c);
      return;
    }

  package_info *pi = new package_info;

  c->pi = pi;

  /* TODO: Should we check flags for debian files ? */
  pi->flags = 0;

  pi->name = dec->decode_string_dup ();
  pi->available_pretty_name = dec->decode_string_dup ();
  pi->broken = false;
  pi->installed_version = dec->decode_string_dup ();
  pi->installed_size = dec->decode_int64 ();;
  pi->available_version = dec->decode_string_dup ();
  pi->maintainer = dec->decode_string_dup ();
  pi->available_section = dec->decode_string_dup ();
  pi->info.installable_status = dec->decode_int ();
  pi->info.install_user_size_delta = dec->decode_int64 ();
  pi->info.removable_status = status_unable; // not used
  pi->info.remove_user_size_delta = 0;
  pi->info.download_size = 0;
  pi->description = dec->decode_string_dup ();
  nicify_description_in_place (pi->description);
  pi->available_short_description = first_line_of (pi->description);
  pi->available_icon = pixbuf_from_base64 (dec->decode_string_in_place ());

  pi->have_info = true;
  pi->have_detail_kind = install_details;

  if (pi->info.installable_status == status_incompatible)
    pi->summary = g_strdup_printf (_("ai_ni_error_install_incompatible"),
				   pi->get_display_name (false));
  else if (pi->info.installable_status == status_incompatible_current)
    pi->summary = g_strdup_printf (_("ai_ni_error_n770package_incompatible"),
				   pi->get_display_name (false));
  else if (pi->info.installable_status == status_corrupted)
    pi->summary = g_strdup_printf (_("ai_ni_error_install_corrupted"),
				   pi->get_display_name (false));
  else
    decode_summary (dec, pi, install_details);

  void (*cont) (bool res, void *);

  if (pi->info.installable_status == status_able)
    cont = if_install;
  else
    cont = if_fail;

  gboolean scare_user;

  if (c->trusted)
    scare_user = false;
  else
    scare_user = true;

  install_confirm (scare_user, pi, false, cont, if_show_details, c);
}

static void
if_install (bool res, void *data)
{
  if_clos *c = (if_clos *)data;

  if (res)
    {
      char *title = NULL;
      if (c->pi->installed_version != NULL)
	{
	  title = g_strdup_printf (_("ai_nw_updating"),
				   c->pi->get_display_name (false),
				   c->pi->get_display_version (false));
	}
      else
	{
	  title = g_strdup_printf (_("ai_nw_installing"),
				   c->pi->get_display_name (false));
	}

      set_entertainment_fun (NULL, -1, -1, 0);
      set_entertainment_cancel (NULL, NULL);
      set_entertainment_main_title (title);
      g_free (title);

      start_entertaining_user (TRUE);

      set_log_start ();
      apt_worker_install_file (c->filename,
			       if_install_reply, c);
    }
  else
    if_end (false, c);
}

static void
if_install_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  if_clos *c = (if_clos *)data;

  stop_entertaining_user ();

  if (dec == NULL)
    {
      if_end (false, c);
      return;
    }

  int success = dec->decode_int ();

  get_package_list ();
  save_backup_data ();

  if (success)
    {
      char *str = NULL;
      if (c->pi->installed_version != NULL)
	{
	  str = g_strdup_printf (_("ai_ni_software_update_installed"));
	}
      else
        {
//           str = g_strdup_printf (_("ai_ni_install_successful_launch"),
//                                  pi->get_display_name (false));
          str = g_strdup_printf (_("ai_ni_multiple_install"), 1);
	}
      annoy_user (str, if_end_with_success, c);
      g_free (str);
    }
  else
    {
      apt_proto_result_code result_code = rescode_failure;
      result_code = scan_log_for_result_code (result_code);

      char *msg = result_code_to_message (c->pi, result_code);
      if (msg == NULL)
	msg = g_strdup_printf (c->pi->installed_version
			       ? _("ai_ni_error_update_failed")
			       : _("ai_ni_error_installation_failed"),
			       c->pi->get_display_name (false));

      annoy_user (msg, if_end_with_failure, c);
      g_free (msg);
    }
}

static void
if_fail (bool res, void *data)
{
  if_clos *c = (if_clos *)data;

  if (res)
    {
      char *msg;
      bool with_details;
      installable_status_to_message (c->pi, msg, with_details);
      if (with_details)
	annoy_user_with_details (msg, c->pi, install_details,
				 if_end_with_failure, c);
      else
	annoy_user (msg, if_end_with_failure, c);
      g_free (msg);
    }
  else
    if_end (false, c);
}

static void
if_end_with_success (void *data)
{
  if_clos *c = (if_clos *)data;
  
  if_end (true, c);
}

static void
if_end_with_failure (void *data)
{
  if_clos *c = (if_clos *)data;
  
  if_end (false, c);
}

static void
if_end (bool success, void *data)
{
  if_clos *c = (if_clos *)data;
  
  cleanup_temp_file ();

  g_free (c->filename);

  if (c->pi)
    c->pi->unref ();

  c->cont (success, c->data);
  delete c;
}
