/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "vision.h"

#include <gio/gio.h>

#include "model-session.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-session.h"
#include "xdp-utils.h"

typedef struct _Vision Vision;
typedef struct _VisionClass VisionClass;

struct _Vision
{
  XdpDbusVisionSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplVision *impl;
};

struct _VisionClass
{
  XdpDbusVisionSkeletonClass parent_class;
};

GType vision_get_type (void) G_GNUC_CONST;

static void vision_iface_init (XdpDbusVisionIface *iface);

G_DEFINE_TYPE_WITH_CODE (Vision, vision, XDP_DBUS_TYPE_VISION_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_VISION,
                                                vision_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Vision, g_object_unref)

static gboolean
handle_vision_get_use_case_availability (XdpDbusVision       *object,
                                         GDBusMethodInvocation *invocation,
                                         const char            *arg_use_case,
                                         GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) availability = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_vision_call_get_use_case_availability_sync (vision->impl,
                                                                 xdp_app_info_get_id (app_info),
                                                                 arg_use_case,
                                                                 &availability,
                                                                 NULL,
                                                                 &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_vision_complete_get_use_case_availability (object, invocation, availability);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_create_session (XdpDbusVision       *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_use_case,
                              const char            *arg_instructions,
                              GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autofree char *backend_session_id = NULL;
  g_autoptr(XdpSession) session = NULL;
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_vision_call_create_session_sync (vision->impl,
                                                      xdp_app_info_get_id (app_info),
                                                      arg_use_case,
                                                      arg_instructions,
                                                      &backend_session_id,
                                                      NULL,
                                                      &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = XDP_SESSION (model_session_new (vision->context,
                                            connection,
                                            app_info,
                                            G_OBJECT (vision->impl),
                                            MODEL_SESSION_VISION,
                                            backend_session_id,
                                            &error));
  if (session == NULL)
    {
      end_backend_session (G_OBJECT (vision->impl),
                           MODEL_SESSION_VISION,
                           backend_session_id);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_session_export (session, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      xdp_session_close (session, FALSE);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_session_register (session);
  xdp_dbus_vision_complete_create_session (object, invocation, session->id);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_prewarm (XdpDbusVision       *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_session_handle,
                       GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_VISION);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  xdp_dbus_vision_emit_model_loading (object, session->id, "starting model");

  if (!xdp_dbus_impl_vision_call_prewarm_sync (vision->impl,
                                               model_session_get_backend_session_id (model_session),
                                               NULL,
                                               &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_vision_complete_prewarm (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_describe (XdpDbusVision       *object,
                        GDBusMethodInvocation *invocation,
                        const char            *arg_session_handle,
                        const char            *arg_image,
                        GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autofree char *text = NULL;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_VISION);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);

  if (!xdp_dbus_impl_vision_call_describe_sync (vision->impl,
                                               model_session_get_backend_session_id (model_session),
                                               arg_image,
                                               &text,
                                               NULL,
                                               &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_vision_complete_describe (object, invocation, text);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_ocr (XdpDbusVision       *object,
                   GDBusMethodInvocation *invocation,
                   const char            *arg_session_handle,
                   const char            *arg_image,
                   GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autofree char *text = NULL;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_VISION);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);

  if (!xdp_dbus_impl_vision_call_ocr_sync (vision->impl,
                                          model_session_get_backend_session_id (model_session),
                                          arg_image,
                                          &text,
                                          NULL,
                                          &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_vision_complete_ocr (object, invocation, text);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_segment (XdpDbusVision       *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_session_handle,
                       const char            *arg_image,
                       GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GVariant) segments = NULL;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_VISION);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);

  if (!xdp_dbus_impl_vision_call_segment_sync (vision->impl,
                                              model_session_get_backend_session_id (model_session),
                                              arg_image,
                                              &segments,
                                              NULL,
                                              &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_vision_complete_segment (object, invocation, segments);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
vision_iface_init (XdpDbusVisionIface *iface)
{
  iface->handle_get_use_case_availability = handle_vision_get_use_case_availability;
  iface->handle_create_session = handle_vision_create_session;
  iface->handle_prewarm = handle_vision_prewarm;
  iface->handle_describe = handle_vision_describe;
  iface->handle_ocr = handle_vision_ocr;
  iface->handle_segment = handle_vision_segment;
}

static void
vision_dispose (GObject *object)
{
  Vision *vision = (Vision *) object;

  g_clear_object (&vision->impl);

  G_OBJECT_CLASS (vision_parent_class)->dispose (object);
}

static void
vision_init (Vision *vision)
{
}

static void
vision_class_init (VisionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = vision_dispose;
}

static Vision *
vision_new (XdpContext        *context,
            XdpDbusImplVision *impl)
{
  Vision *vision = g_object_new (vision_get_type (), NULL);

  vision->context = context;
  vision->impl = g_object_ref (impl);
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (vision->impl), G_MAXINT);
  xdp_dbus_vision_set_version (XDP_DBUS_VISION (vision), 1);

  return vision;
}

void
init_vision (XdpContext *context)
{
  g_autoptr(Vision) vision = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  XdpDbusImplVision *impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, VISION_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_vision_proxy_new_sync (connection,
                                              G_DBUS_PROXY_FLAGS_NONE,
                                              impl_config->dbus_name,
                                              DESKTOP_DBUS_PATH,
                                              NULL,
                                              &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create vision proxy: %s", error->message);
      return;
    }

  vision = vision_new (context, impl);
  g_clear_object (&impl);
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&vision)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
