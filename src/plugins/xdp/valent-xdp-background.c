// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Andy Holmes <andrew.g.r.holmes@gmail.com>

#define G_LOG_DOMAIN "valent-xdp-background"

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libportal/portal.h>
#include <valent.h>

#include "valent-xdp-background.h"
#include "valent-xdp-utils.h"


struct _ValentXdpBackground
{
  ValentApplicationPlugin  parent;

  GSettings               *settings;
  unsigned int             autostart : 1;
  unsigned long            active_id;
};

G_DEFINE_FINAL_TYPE (ValentXdpBackground, valent_xdp_background, VALENT_TYPE_APPLICATION_PLUGIN)


static gboolean
valent_xdp_background_is_active (ValentXdpBackground *self)
{
  GListModel *windows = NULL;
  unsigned int n_windows = 0;

  windows = gtk_window_get_toplevels ();
  n_windows = g_list_model_get_n_items (windows);

  for (unsigned int i = 0; i < n_windows; i++)
    {
      g_autoptr (GtkWindow) window = g_list_model_get_item (windows, i);

      if (gtk_window_is_active (window))
        return TRUE;
    }

  return FALSE;
}

static void
xdp_portal_request_background_cb (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  XdpPortal *portal = XDP_PORTAL (object);
  g_autoptr (GError) error = NULL;

  g_assert (XDP_IS_PORTAL (portal));

  if (!xdp_portal_request_background_finish (portal, result, &error))
    g_debug ("ValentXdpPlugin: permission denied");

  if (error != NULL)
    g_warning ("ValentXdpPlugin: %s", error->message);
}

static void
valent_xdp_background_request (ValentXdpBackground *self)
{
  g_autoptr (GPtrArray) commandline = NULL;
  g_autoptr (XdpParent) parent = NULL;
  g_autoptr (GCancellable) destroy = NULL;
  XdpBackgroundFlags flags = XDP_BACKGROUND_FLAG_NONE;

  g_assert (VALENT_IS_XDP_BACKGROUND (self));

  parent = valent_xdp_get_parent (NULL);

  if (self->autostart)
    {
      commandline = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (commandline, g_strdup ("valent"));
      g_ptr_array_add (commandline, g_strdup ("--gapplication-service"));

      flags |= XDP_BACKGROUND_FLAG_AUTOSTART;
    }

  destroy = valent_object_ref_cancellable (VALENT_OBJECT (self));
  xdp_portal_request_background (valent_xdp_get_default (),
                                 parent,
                                 _("Valent wants to run as a service"),
                                 commandline,
                                 flags,
                                 destroy,
                                 xdp_portal_request_background_cb,
                                 NULL);
}

static void
on_window_is_active (GtkWindow           *window,
                     GParamSpec          *pspec,
                     ValentXdpBackground *self)
{
  GListModel *windows = NULL;
  unsigned int n_windows = 0;

  if (!gtk_window_is_active (window))
    return;

  windows = gtk_window_get_toplevels ();
  n_windows = g_list_model_get_n_items (windows);

  for (unsigned int i = 0; i < n_windows; i++)
    {
      g_autoptr (GtkWindow) item = g_list_model_get_item (windows, i);

      g_signal_handlers_disconnect_by_data (item, self);
    }

  g_clear_signal_handler (&self->active_id, windows);
  valent_xdp_background_request (self);
}

static void
on_windows_changed (GListModel          *list,
                    unsigned int         position,
                    unsigned int         removed,
                    unsigned int         added,
                    ValentXdpBackground *self)
{
  for (unsigned int i = 0; i < added; i++)
    {
      g_autoptr (GtkWindow) window = g_list_model_get_item (list, position + i);

      // If the new window is active, we can bail now
      on_window_is_active (window, NULL, self);
      if (self->active_id == 0)
        return;

      g_signal_connect_object (window,
                               "notify::is-active",
                               G_CALLBACK (on_window_is_active),
                               self, 0);
    }
}

static void
on_autostart_changed (GSettings           *settings,
                      const char          *key,
                      ValentXdpBackground *self)
{
  g_assert (VALENT_IS_XDP_BACKGROUND (self));

  self->autostart = g_settings_get_boolean (self->settings, "autostart");

  /* Already waiting for an active window */
  if (self->active_id > 0)
    return;

  /* If there is no window or Valent is not the focused application, defer the
   * request until that changes. */
  if (!valent_xdp_background_is_active (self))
    {
      GListModel *windows = gtk_window_get_toplevels ();

      self->active_id = g_signal_connect_object (windows,
                                                 "items-changed",
                                                 G_CALLBACK (on_windows_changed),
                                                 self, 0);
      on_windows_changed (windows, 0, 0, g_list_model_get_n_items (windows), self);
      return;
    }

  valent_xdp_background_request (self);
}

/*
 * GObject
 */
static void
valent_xdp_background_constructed (GObject *object)
{
  ValentXdpBackground *self = VALENT_XDP_BACKGROUND (object);

  self->settings = g_settings_new ("ca.andyholmes.Valent.Plugin.xdp");
  g_signal_connect (self->settings,
                    "changed::autostart",
                    G_CALLBACK (on_autostart_changed),
                    self);

  on_autostart_changed (self->settings, "autostart", self);

  G_OBJECT_CLASS (valent_xdp_background_parent_class)->constructed (object);
}

static void
valent_xdp_background_dispose (GObject *object)
{
  ValentXdpBackground *self = VALENT_XDP_BACKGROUND (object);

  g_clear_signal_handler (&self->active_id, g_application_get_default ());
  g_clear_object (&self->settings);

  /* If the extension is being disabled during application shutdown, the main
   * window is already closed and this will be skipped. If the user has disabled
   * the extension, then the window must be active and it will succeed */
  if (valent_xdp_background_is_active (self))
    {
      self->autostart = FALSE;
      valent_xdp_background_request (self);
    }

  G_OBJECT_CLASS (valent_xdp_background_parent_class)->dispose (object);
}

static void
valent_xdp_background_class_init (ValentXdpBackgroundClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = valent_xdp_background_constructed;
  object_class->dispose = valent_xdp_background_dispose;
}

static void
valent_xdp_background_init (ValentXdpBackground *self)
{
}

