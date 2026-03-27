/*
 * Copyright © 2024 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Jules Agent
 */

#include "config.h"

#include <gio/gio.h>
#include <glib/gi18n.h>

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-utils.h"

#include "flatpak.h"

typedef struct _FlatpakPortal FlatpakPortal;
typedef struct _FlatpakPortalClass FlatpakPortalClass;

struct _FlatpakPortal
{
  XdpDbusFlatpakInstallerSkeleton parent_instance;
};

struct _FlatpakPortalClass
{
  XdpDbusFlatpakInstallerSkeletonClass parent_class;
};

typedef struct _InstallMonitor InstallMonitor;
typedef struct _InstallMonitorClass InstallMonitorClass;

struct _InstallMonitor
{
  XdpDbusFlatpakInstallerInstallMonitorSkeleton parent_instance;
  XdpContext *context;
  char *id;
  GCancellable *cancellable;
};

struct _InstallMonitorClass
{
  XdpDbusFlatpakInstallerInstallMonitorSkeletonClass parent_class;
};

GType flatpak_portal_get_type (void) G_GNUC_CONST;
static void flatpak_portal_iface_init (XdpDbusFlatpakInstallerIface *iface);

G_DEFINE_TYPE_WITH_CODE (FlatpakPortal, flatpak_portal,
                         XDP_DBUS_TYPE_FLATPAK_INSTALLER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_FLATPAK_INSTALLER,
                                                flatpak_portal_iface_init));

GType install_monitor_get_type (void) G_GNUC_CONST;
static void install_monitor_iface_init (XdpDbusFlatpakInstallerInstallMonitorIface *iface);

G_DEFINE_TYPE_WITH_CODE (InstallMonitor, install_monitor,
                         XDP_DBUS_TYPE_FLATPAK_INSTALLER_INSTALL_MONITOR_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_FLATPAK_INSTALLER_INSTALL_MONITOR,
                                                install_monitor_iface_init));

static void
install_monitor_init (InstallMonitor *monitor)
{
  monitor->cancellable = g_cancellable_new ();
}

static void
install_monitor_finalize (GObject *object)
{
  InstallMonitor *monitor = (InstallMonitor *)object;

  g_cancellable_cancel (monitor->cancellable);
  g_clear_object (&monitor->cancellable);
  g_free (monitor->id);

  G_OBJECT_CLASS (install_monitor_parent_class)->finalize (object);
}

static void
install_monitor_class_init (InstallMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = install_monitor_finalize;
}

static gboolean
handle_close (XdpDbusFlatpakInstallerInstallMonitor *object,
              GDBusMethodInvocation        *invocation)
{
  InstallMonitor *monitor = (InstallMonitor *)object;

  g_debug ("Closing install monitor %s", monitor->id);

  g_cancellable_cancel (monitor->cancellable);
  xdp_context_unclaim_object_path (monitor->context, monitor->id);
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (monitor));

  xdp_dbus_flatpak_installer_install_monitor_complete_close (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
install_monitor_iface_init (XdpDbusFlatpakInstallerInstallMonitorIface *iface)
{
  iface->handle_close = handle_close;
}

static InstallMonitor *
install_monitor_new (XdpContext *context,
                     const char *id)
{
  InstallMonitor *monitor;

  monitor = g_object_new (install_monitor_get_type (), NULL);
  monitor->context = context;
  monitor->id = g_strdup (id);

  return monitor;
}

static void
flatpak_portal_init (FlatpakPortal *portal)
{
  xdp_dbus_flatpak_installer_set_version (XDP_DBUS_FLATPAK_INSTALLER (portal), 1);
}

static void
flatpak_portal_class_init (FlatpakPortalClass *klass)
{
}

static void
emit_progress (InstallMonitor *monitor,
               guint32         status,
               guint32         progress,
               const char     *error,
               const char     *error_message)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "status", g_variant_new_uint32 (status));
  g_variant_builder_add (&builder, "{sv}", "progress", g_variant_new_uint32 (progress));
  if (error)
    g_variant_builder_add (&builder, "{sv}", "error", g_variant_new_string (error));
  if (error_message)
    g_variant_builder_add (&builder, "{sv}", "error_message", g_variant_new_string (error_message));

  xdp_dbus_flatpak_installer_install_monitor_emit_progress (XDP_DBUS_FLATPAK_INSTALLER_INSTALL_MONITOR (monitor),
                                                  g_variant_builder_end (&builder));
}

static void
install_finished_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr(InstallMonitor) monitor = user_data;
  g_autoptr(GError) error = NULL;

  if (!g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Flatpak install failed: %s", error->message);
      emit_progress (monitor, 2, 0, "org.freedesktop.portal.FlatpakInstaller.Error.Failed", error->message);
    }
  else
    {
      g_debug ("Flatpak install finished successfully");
      emit_progress (monitor, 1, 100, NULL, NULL);
    }
}

static gboolean
handle_install_extensions (XdpDbusFlatpakInstaller        *object,
                           GDBusMethodInvocation *invocation,
                           const char * const    *arg_extensions,
                           GVariant              *arg_options)
{
  XdpContext *context = g_object_get_data (G_OBJECT (object), "xdp-context");
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  const char *app_id = xdp_app_info_get_id (app_info);
  g_autoptr(GError) error = NULL;
  const char *handle_token = NULL;
  g_autofree char *handle = NULL;
  g_autoptr(InstallMonitor) monitor = NULL;
  g_autoptr(GSubprocess) subprocess = NULL;
  g_autoptr(GPtrArray) args = NULL;
  int i;
  size_t app_id_len = strlen (app_id);

  if (xdp_app_info_is_host (app_info))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             _("Only sandboxed applications can install extensions"));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  for (i = 0; arg_extensions[i] != NULL; i++)
    {
      const char *ext = arg_extensions[i];
      if (!g_str_has_prefix (ext, app_id) || ext[app_id_len] != '.')
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                 _("Extension ID %s does not start with app ID %s."),
                                                 ext, app_id);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  g_variant_lookup (arg_options, "handle_token", "&s", &handle_token);
  if (handle_token)
    {
      if (!xdp_is_valid_token (handle_token))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                                 _("Invalid handle token: %s"), handle_token);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }
  else
    {
      handle_token = "t";
    }

  {
    g_autofree char *sender = g_strdup (g_dbus_method_invocation_get_sender (invocation) + 1);
    int j;
    for (j = 0; sender[j] != '\0'; j++)
      if (sender[j] == '.')
        sender[j] = '_';

    handle = g_strdup_printf ("/org/freedesktop/portal/FlatpakInstaller/install_monitor/%s/%s", sender, handle_token);
  }

  if (!xdp_context_claim_object_path (context, handle))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_EXISTS,
                                             _("Handle already in use"));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  monitor = install_monitor_new (context, handle);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (monitor),
                                         xdp_context_get_connection (context),
                                         handle,
                                         &error))
    {
      xdp_context_unclaim_object_path (context, handle);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_debug ("Installing extensions for %s: %s", app_id, handle);

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("install"));
  g_ptr_array_add (args, g_strdup ("--user"));
  g_ptr_array_add (args, g_strdup ("--noninteractive"));
  g_ptr_array_add (args, g_strdup ("-y"));

  for (i = 0; arg_extensions[i] != NULL; i++)
    g_ptr_array_add (args, g_strdup (arg_extensions[i]));

  g_ptr_array_add (args, NULL);

  subprocess = g_subprocess_newv ((const gchar * const *)args->pdata,
                                   G_SUBPROCESS_FLAGS_NONE,
                                   &error);

  if (!subprocess)
    {
      g_warning ("Failed to start flatpak install: %s", error->message);
      emit_progress (monitor, 2, 0, "org.freedesktop.portal.FlatpakInstaller.Error.Failed", error->message);
    }
  else
    {
      emit_progress (monitor, 0, 0, NULL, NULL);
      g_subprocess_wait_async (subprocess,
                               monitor->cancellable,
                               install_finished_cb,
                               g_object_ref (monitor));
    }

  xdp_dbus_flatpak_installer_complete_install_extensions (object, invocation, handle);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
flatpak_portal_iface_init (XdpDbusFlatpakInstallerIface *iface)
{
  iface->handle_install_extensions = handle_install_extensions;
}

void
init_flatpak (XdpContext *context)
{
  g_autoptr(FlatpakPortal) portal = NULL;

  portal = g_object_new (flatpak_portal_get_type (), NULL);
  g_object_set_data (G_OBJECT (portal), "xdp-context", context);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&portal)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
