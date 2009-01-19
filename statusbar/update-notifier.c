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

#define _GNU_SOURCE

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <libintl.h>
#include <sys/inotify.h>

#include <gtk/gtk.h>
#include <gconf/gconf-client.h>

#include <hildon/hildon.h>
#include <libosso.h>
#include <clockd/libtime.h>
#include <libalarm.h>

#include <xexp.h>
#include <user_files.h>

#include "update-notifier.h"
#include "update-notifier-conf.h"

#define DEBUG
#ifdef DEBUG
#define LOG(...) my_log (__PRETTY_FUNCTION__, __VA_ARGS__);
#else
#define LOG(...)
#endif

#define APPNAME "hildon_update_notifier"

#define _(x) dgettext ("hildon-application-manager", (x))

#define UPDATE_NOTIFIER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), UPDATE_NOTIFIER_TYPE, UpdateNotifierPrivate))

typedef enum _State State;
enum _State {
  UPNO_STATE_INVISIBLE,
  UPNO_STATE_STATIC,
  UPNO_STATE_BLINKING
};

/* inotify watchers id */
enum {
  HOME,
  VARLIB,
  MAXWATCH
};

typedef struct _UpdateNotifierPrivate UpdateNotifierPrivate;
struct _UpdateNotifierPrivate
{
  /* ui */
  GtkWidget *icon;
  GtkWidget *button;

  /* environment */
  osso_context_t *osso;
  GConfClient *gconf;

  /* state */
  State state;
  cookie_t alarm_cookie;

  /* inotify */
  gint inotify_fd;
  guint io_watch;
  gint wd[2];
};

/* setup prototypes */
static gboolean setup_dbus (UpdateNotifier *self);
static void setup_gconf (UpdateNotifier *self);
static gboolean setup_alarm (UpdateNotifier *upno);
static gboolean setup_alarm_now (gpointer data);
static gboolean setup_inotify (UpdateNotifier *self);
static void close_inotify (UpdateNotifier *self);

/* state handling prototypes */
static void load_state (UpdateNotifier *self);
static void save_state (UpdateNotifier *self);
static void update_state (UpdateNotifier *self);
static void set_state (UpdateNotifier *self, State state);
static State get_state (UpdateNotifier *self);

/* misc prototypes */
static void my_log (const gchar *function, const gchar *fmt, ...);

HD_DEFINE_PLUGIN_MODULE (UpdateNotifier, update_notifier,
                         HD_TYPE_STATUS_MENU_ITEM);

static void
update_notifier_class_finalize (UpdateNotifierClass *klass)
{
  /* noop */
}

static void
update_notifier_finalize (GObject *object)
{
  UpdateNotifier *self;
  UpdateNotifierPrivate *priv;

  self = UPDATE_NOTIFIER (object);
  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  if (priv->osso != NULL)
    osso_deinitialize (priv->osso);

  if (priv->gconf != NULL)
      g_object_unref (priv->gconf);

  close_inotify (self);
}

static void
update_notifier_class_init (UpdateNotifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = update_notifier_finalize;

  g_type_class_add_private (object_class, sizeof (UpdateNotifierPrivate));
}

static void
update_notifier_init (UpdateNotifier *self)
{
  UpdateNotifierPrivate *priv;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  priv->osso = NULL;

  priv->state = UPNO_STATE_INVISIBLE;
  priv->alarm_cookie = 0;

  priv->inotify_fd = -1;
  priv->io_watch = 0;
  priv->wd[HOME] = priv->wd[VARLIB] = -1;

  if (setup_dbus (self))
    {
      LOG ("dbus setup");

      setup_gconf (self);
      /* create_widgets (self); */
      load_state (self);

      if (setup_inotify (self))
        {
          /* update_icon_visibility */
          update_state (self);
          g_timeout_add_seconds (60, setup_alarm_now, self);
        }
      else
        close_inotify (self);
    }
}

static gint
update_notifier_rpc_cb (const gchar* interface, const gchar* method,
                        GArray* arguments, gpointer data,
                        osso_rpc_t* retval)
{
  UpdateNotifier *self;

  g_return_val_if_fail (IS_UPDATE_NOTIFIER (data), OSSO_ERROR);
  g_return_val_if_fail (interface != NULL && method != NULL, OSSO_ERROR);

  LOG ("RPC Message: %s:%s", interface, method);

  self = UPDATE_NOTIFIER (data);

  if (strcmp (interface, UPDATE_NOTIFIER_INTERFACE) != 0)
    return OSSO_ERROR;

  if (strcmp (method, UPDATE_NOTIFIER_OP_CHECK_UPDATES) == 0)
    {
      /* Search for new avalable updates */
      /* check_for_updates (self); */
    }
  else if (strcmp (method, UPDATE_NOTIFIER_OP_CHECK_STATE) == 0)
    {
      /* Update states of the satusbar item */
      update_state (self);
    }
  else
    return OSSO_ERROR;

  return OSSO_OK;
}

static gboolean
setup_dbus (UpdateNotifier *self)
{
  UpdateNotifierPrivate *priv;
  osso_return_t result;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  g_return_val_if_fail (priv->osso == NULL, FALSE);

  priv->osso = osso_initialize (APPNAME, PACKAGE_VERSION, TRUE, NULL);

  if (priv->osso == NULL)
    return FALSE;

  result = osso_rpc_set_cb_f (priv->osso,
                              UPDATE_NOTIFIER_SERVICE,
                              UPDATE_NOTIFIER_OBJECT_PATH,
                              UPDATE_NOTIFIER_INTERFACE,
                              update_notifier_rpc_cb, self);

  return (result == OSSO_OK);
}

static void
update_notifier_interval_changed_cb (GConfClient *client, guint cnxn_id,
                                     GConfEntry *entry, gpointer data)
{
  g_return_if_fail (IS_UPDATE_NOTIFIER (data));

  LOG ("Interval value changed");
  setup_alarm (UPDATE_NOTIFIER (data));
}

static void
setup_gconf (UpdateNotifier *self)
{
  UpdateNotifierPrivate *priv;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  g_return_if_fail (priv->gconf == NULL);

  priv->gconf = gconf_client_get_default ();

  gconf_client_add_dir (priv->gconf,
                        UPNO_GCONF_DIR, GCONF_CLIENT_PRELOAD_ONELEVEL, NULL);

  gconf_client_notify_add (priv->gconf, UPNO_GCONF_CHECK_INTERVAL,
                           update_notifier_interval_changed_cb, self,
                           NULL, NULL);
}

static gboolean
xexp_is_tag_and_not_empty (xexp *x, const char *tag)
{
  return (x != NULL && xexp_is (x, tag) && !xexp_is_empty (x));
}

static gboolean
compare_xexp_text (xexp *x_a, xexp *x_b, const char *tag)
{
  const char *text_a = NULL;
  const char *text_b = NULL;

  if ((x_a == NULL) || (x_b == NULL))
    return ((x_a == NULL) && (x_b == NULL));

  text_a = xexp_aref_text(x_a, tag);
  text_b = xexp_aref_text(x_b, tag);

  if ((text_a == NULL) || (text_b == NULL))
    return ((text_a == NULL) && (text_b == NULL));
  else
    return (strcmp (text_a, text_b) == 0);
}

static void
update_seen_notifications ()
{
  xexp *avail_notifications;
  xexp *seen_notifications;

 avail_notifications = user_file_read_xexp (UFILE_AVAILABLE_NOTIFICATIONS);
 seen_notifications = user_file_read_xexp (UFILE_SEEN_NOTIFICATIONS);

 if (avail_notifications && seen_notifications &&
     xexp_is_tag_and_not_empty (avail_notifications, "info") &&
     (!xexp_is_tag_and_not_empty (seen_notifications, "info") ||
      (xexp_is_tag_and_not_empty (seen_notifications, "info") &&
       !compare_xexp_text (avail_notifications, seen_notifications, "title") &&
       !compare_xexp_text (avail_notifications, seen_notifications, "text") &&
       !compare_xexp_text (avail_notifications, seen_notifications, "uri"))))
   {
     /* as we have new notifications, we no longer need the old seen ones;
      * the writing of UFILE_SEEN_NOTIFICATIONS will trigger an inotify */
     xexp* empty_seen_notifications;

     empty_seen_notifications = xexp_list_new ("info");
     user_file_write_xexp (UFILE_SEEN_NOTIFICATIONS, empty_seen_notifications);
     xexp_free (empty_seen_notifications);
   }

 if (avail_notifications)
   xexp_free (avail_notifications);

 if (seen_notifications)
   xexp_free (seen_notifications);
}

static gboolean
is_file_modified_event (struct inotify_event *event,
                        int watch, const char *name)
{
  return (event->wd == watch
          && (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))
          && event->len > 0
          && strcmp (event->name, name) == 0);
}

#define BUF_LEN 4096

static gboolean
update_notifier_inotify_cb (GIOChannel *source, GIOCondition condition,
                            gpointer data)
{
  UpdateNotifierPrivate *priv;
  gchar buf[BUF_LEN];
  gint i;
  gint len;

  /* Return if the object was already destroyed
     or the if inotify is not still ready */
  g_return_val_if_fail (IS_UPDATE_NOTIFIER (data), FALSE);

  priv = UPDATE_NOTIFIER_GET_PRIVATE (data);
  g_return_val_if_fail (priv->inotify_fd == -1, FALSE);

  while (TRUE)
    {
      while ((len = read (priv->inotify_fd, buf, BUF_LEN)) < 0
             && errno == EINTR);
      if (len > 0)
        break;
      else if (len < 0)
        {
          g_warning ("Error reading inotify event: %s", g_strerror (errno));
          goto error_cancel;
        }

      g_assert (len == 0);
      g_warning ("Buffer size %u too small", BUF_LEN);
      goto error_cancel;
    }

  i = 0;
  while (i < len)
    {
      struct inotify_event *event;

      event = (struct inotify_event *) &buf[i];

      if (is_file_modified_event (event, priv->wd[VARLIB],
                                  AVAILABLE_UPDATES_FILE_NAME) ||
          is_file_modified_event (event, priv->wd[HOME],
                                  UFILE_SEEN_UPDATES) ||
          is_file_modified_event (event, priv->wd[HOME],
                                  UFILE_SEEN_NOTIFICATIONS))
        {
          update_state (UPDATE_NOTIFIER (data));
        }
      else if (is_file_modified_event (event, priv->wd[HOME],
                                       UFILE_AVAILABLE_NOTIFICATIONS))
        {
          update_seen_notifications ();
        }

      i += sizeof (struct inotify_event) + event->len;
    }

  return TRUE;

error_cancel:
  priv->io_watch = 0;
  close_inotify (UPDATE_NOTIFIER (data));
  return FALSE;
}

static gint
add_watch_for_path (UpdateNotifier *self, const gchar *path)
{
  UpdateNotifierPrivate *priv;
  gint watch;

  g_return_val_if_fail (path != NULL, -1);

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  watch = inotify_add_watch (priv->inotify_fd, path,
                             IN_CLOSE_WRITE | IN_MOVED_TO);

  if (watch < 0)
    {
      g_warning ("Failed to create watch for local file %s : %s\n",
                 path, strerror (errno));
      return -1;
    }

  return watch;
}

static gboolean
setup_inotify (UpdateNotifier *self)
{
  UpdateNotifierPrivate *priv;
  gint fd;
  GIOChannel *io_channel;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  g_return_val_if_fail (priv->inotify_fd == -1, FALSE);

  if ((fd = inotify_init ()) < 0)
    {
      g_warning ("Failed to initialize inotify: %s", g_strerror (errno));
      return FALSE;
    }

  priv->inotify_fd = fd;

  io_channel = g_io_channel_unix_new (fd);
  priv->io_watch = g_io_add_watch (io_channel, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                   update_notifier_inotify_cb, self);
  g_io_channel_unref (io_channel);

  {
    gchar *path;

    path = user_file_get_state_dir_path ();
    priv->wd[HOME] = add_watch_for_path (self, path);
    g_free (path);

    path = "/var/lib/hildon-application-manager";
    priv->wd[VARLIB] = add_watch_for_path (self, path);

    if (priv->wd[HOME] == -1 || priv->wd[VARLIB] == -1)
      return FALSE;
  }

  return TRUE;
}

static void
close_inotify (UpdateNotifier *self)
{
  UpdateNotifierPrivate *priv;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  if (priv->io_watch > 0)
    g_source_remove (priv->io_watch);
  priv->io_watch = 0;

  if (priv->inotify_fd > 0)
    {
      gint i;

      for (i = 0; i < MAXWATCH; i++)
        {
          if (priv->wd[i] != -1)
            {
              inotify_rm_watch (priv->inotify_fd, priv->wd[i]);
              priv->wd[i] = -1;
            }
        }
      close (priv->inotify_fd);
    }
  priv->inotify_fd = -1;
}

static void
delete_all_alarms (void)
{
  int i;
  cookie_t *cookies = NULL;

  if ((cookies = alarmd_event_query (0, 0, 0, 0, APPNAME)))
    {
      for (i = 0; cookies[i]; ++i)
        alarmd_event_del (cookies[i]);
      free(cookies);
    }
}

static gint
get_interval (UpdateNotifier* self)
{
  UpdateNotifierPrivate *priv;
  gint interval;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  interval = gconf_client_get_int (priv->gconf,
                                   UPNO_GCONF_CHECK_INTERVAL,
                                   NULL);

  if (interval <= 0)
    {
      /* Use default value and set it from now on */
      interval = UPNO_DEFAULT_CHECK_INTERVAL;
      gconf_client_set_int (priv->gconf,
                            UPNO_GCONF_CHECK_INTERVAL,
                            interval,
                            NULL);
    }

  LOG ("The interval is %d", interval);
  return interval;
}

static gboolean
setup_alarm (UpdateNotifier *self)
{
  UpdateNotifierPrivate *priv;
  alarm_event_t *old_event;
  alarm_event_t *event;
  alarm_action_t *action;
  int interval;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  interval = get_interval (self);

  /* We reset the alarm when we don't have a cookie for the old alarm
     (which probably means there is no old alarm), when we can't find
     the old alarm although we have a cookie (which shouldn't happen
     unless someone manually mucks with the alarm queue), or if the
     interval has changed.

     Otherwise we leave the old alarm in place, but we update its
     parameters without touching the timing.
  */
  LOG ("The stored alarm id is %d", priv->alarm_cookie);

  old_event = NULL;
  if (priv->alarm_cookie > 0)
    old_event = alarmd_event_get (priv->alarm_cookie);

  /* Setup new alarm based on old alarm. */
  event = alarm_event_create ();
  alarm_event_set_alarm_appid (event, APPNAME);

  event->flags = ALARM_EVENT_CONNECTED | ALARM_EVENT_RUN_DELAYED;
  event->recur_count = -1;

  if (old_event == NULL || old_event->recur_secs != interval)
    {
      /* reset timing parameters. */
      event->alarm_time = time_get_time () + interval;
      event->recur_secs = interval;
    }
  else
    {
      /* copy timing parameters */
      event->alarm_time = old_event->alarm_time;
      event->recur_secs = old_event->recur_secs;

      alarm_event_delete (old_event);
    }

  /* create the action */
  action = alarm_event_add_actions (event, 1);
  action->flags = ALARM_ACTION_WHEN_TRIGGERED |
    ALARM_ACTION_TYPE_DBUS |
    ALARM_ACTION_DBUS_USE_ACTIVATION;
  alarm_action_set_dbus_service (action, UPDATE_NOTIFIER_SERVICE);
  alarm_action_set_dbus_path (action, UPDATE_NOTIFIER_OBJECT_PATH);
  alarm_action_set_dbus_interface (action, UPDATE_NOTIFIER_INTERFACE);
  alarm_action_set_dbus_name (action, UPDATE_NOTIFIER_OP_CHECK_UPDATES);

  /* Search for more alarms to delete (if available) */
  delete_all_alarms ();

  priv->alarm_cookie = alarmd_event_add (event);

  LOG ("The new alarm id is %d", priv->alarm_cookie);

  save_state (self);

  return priv->alarm_cookie > 0;
}

static gboolean
setup_alarm_now (gpointer data)
{
  g_return_val_if_fail (IS_UPDATE_NOTIFIER (data), FALSE);

  if (setup_alarm (UPDATE_NOTIFIER (data)))
    return FALSE;

  /* Try again in one minute. */
  return TRUE;
}

static void
load_state (UpdateNotifier *self)
{
  UpdateNotifierPrivate *priv;
  xexp *state = NULL;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  state = user_file_read_xexp (UFILE_UPDATE_NOTIFIER);

  if (state != NULL)
    {
      priv->state = (State) xexp_aref_int (state, "icon-state",
                                           UPNO_STATE_INVISIBLE);
      priv->alarm_cookie = (cookie_t) xexp_aref_int (state, "alarm-cookie", 0);
      xexp_free (state);
    }

  LOG ("loaded state = %d", priv->state);
}

static void
save_state (UpdateNotifier *self)
{
  UpdateNotifierPrivate *priv;
  xexp *x_state = NULL;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  x_state = xexp_list_new ("state");

  xexp_aset_int (x_state, "icon-state", (gint) priv->state);
  xexp_aset_int (x_state, "alarm-cookie", priv->alarm_cookie);

  user_file_write_xexp (UFILE_UPDATE_NOTIFIER, x_state);

  xexp_free (x_state);

  LOG ("saved state = %d", priv->state);
}

static void
set_state (UpdateNotifier* self, State state)
{
  UpdateNotifierPrivate *priv;

  priv = UPDATE_NOTIFIER_GET_PRIVATE (self);

  priv->state = state;
}

static State
get_state (UpdateNotifier *self)
{
}

static void
update_state (UpdateNotifier *self)
{
  LOG ("Don't know yet if this function is needed");
}

static void
my_log (const gchar *function, const gchar *fmt, ...)
{
  va_list args;
  gchar *tmp;

  va_start (args, fmt);
  tmp = g_strdup_vprintf (fmt, args);
  g_printerr ("update-notifier (%s): %s\n", function, tmp);
  g_free (tmp);
  va_end (args);
}
