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

typedef struct _VisionSignalForward
{
  Vision *vision;
  XdpDbusImplVision *impl;
  XdpRequest *request;
  char *request_handle;
  char *backend_session_id;
  char *session_handle;
  gulong loading_handler_id;
  gulong handler_id;
  guint call_kind;
} VisionSignalForward;

typedef enum
{
  VISION_CALL_PREWARM,
  VISION_CALL_STREAM_DESCRIBE,
  VISION_CALL_STREAM_OCR,
  VISION_CALL_STREAM_SEGMENT,
} VisionCallKind;

typedef struct _VisionCreateSession
{
  Vision *vision;
  XdpRequest *request;
  XdpAppInfo *app_info;
  GDBusConnection *connection;
  char *use_case;
  GVariant *options;
} VisionCreateSession;

static VisionCreateSession *
vision_create_session_new (Vision         *vision,
                           XdpRequest     *request,
                           XdpAppInfo     *app_info,
                           GDBusConnection *connection,
                           const char     *use_case,
                           GVariant       *options)
{
  VisionCreateSession *create = g_new0 (VisionCreateSession, 1);

  create->vision = g_object_ref (vision);
  create->request = g_object_ref (request);
  create->app_info = g_object_ref (app_info);
  create->connection = g_object_ref (connection);
  create->use_case = g_strdup (use_case);
  create->options = g_variant_ref (options);

  return create;
}

static void
vision_create_session_free (VisionCreateSession *create)
{
  g_clear_object (&create->vision);
  g_clear_object (&create->request);
  g_clear_object (&create->app_info);
  g_clear_object (&create->connection);
  g_clear_pointer (&create->use_case, g_free);
  g_clear_pointer (&create->options, g_variant_unref);
  g_free (create);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VisionCreateSession, vision_create_session_free)

static VisionSignalForward *
vision_signal_forward_new (Vision            *vision,
                           XdpDbusImplVision *impl,
                           XdpRequest        *request,
                           const char        *backend_session_id,
                           const char        *session_handle,
                           VisionCallKind     call_kind)
{
  VisionSignalForward *forward = g_new0 (VisionSignalForward, 1);

  forward->vision = g_object_ref (vision);
  forward->impl = g_object_ref (impl);
  forward->request = g_object_ref (request);
  forward->request_handle = g_strdup (xdp_request_get_object_path (request));
  forward->backend_session_id = g_strdup (backend_session_id);
  forward->session_handle = g_strdup (session_handle);
  forward->call_kind = call_kind;

  return forward;
}

static void
vision_signal_forward_unref (VisionSignalForward *forward)
{
  g_clear_object (&forward->vision);
  g_clear_object (&forward->impl);
  g_clear_object (&forward->request);
  g_clear_pointer (&forward->request_handle, g_free);
  g_clear_pointer (&forward->backend_session_id, g_free);
  g_clear_pointer (&forward->session_handle, g_free);
  g_free (forward);
}

static void
vision_signal_forward_disconnect (VisionSignalForward *forward)
{
  XdpDbusImplVision *impl = forward->impl;
  gulong loading_handler_id = forward->loading_handler_id;
  gulong handler_id = forward->handler_id;

  forward->loading_handler_id = 0;
  forward->handler_id = 0;

  if (loading_handler_id != 0)
    g_signal_handler_disconnect (impl, loading_handler_id);
  if (handler_id != 0)
    g_signal_handler_disconnect (impl, handler_id);
}

static void
vision_emit_signal_to_request (VisionSignalForward *forward,
                               const char          *signal_name,
                               GVariant            *parameters)
{
  REQUEST_AUTOLOCK (forward->request);

  if (!forward->request->exported)
    {
      g_variant_unref (parameters);
      return;
    }

  g_dbus_connection_emit_signal (xdp_context_get_connection (forward->vision->context),
                                 forward->request->sender,
                                 DESKTOP_DBUS_PATH,
                                 VISION_DBUS_IFACE,
                                 signal_name,
                                 parameters,
                                 NULL);
}

static void
forward_model_loading (XdpDbusImplVision *impl,
                       const char        *request_id,
                       const char        *session_id,
                       const char        *message,
                       gpointer           user_data)
{
  VisionSignalForward *forward = user_data;

  if (g_strcmp0 (request_id, forward->request_handle) != 0 ||
      g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  vision_emit_signal_to_request (forward,
                                 "ModelLoading",
                                 g_variant_new ("(oos)",
                                                forward->request_handle,
                                                forward->session_handle,
                                                message));
}

static void
vision_signal_forward_connect_loading (VisionSignalForward *forward)
{
  forward->loading_handler_id = g_signal_connect (forward->impl,
                                                  "model-loading",
                                                  G_CALLBACK (forward_model_loading),
                                                  forward);
}

static void
finish_vision_call (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  VisionSignalForward *forward = user_data;
  XdpDbusImplVision *impl = XDP_DBUS_IMPL_VISION (source);
  g_autoptr(GError) error = NULL;
  gboolean ok = FALSE;

  switch ((VisionCallKind) forward->call_kind)
    {
    case VISION_CALL_PREWARM:
      ok = xdp_dbus_impl_vision_call_prewarm_finish (impl, result, &error);
      break;
    case VISION_CALL_STREAM_DESCRIBE:
      ok = xdp_dbus_impl_vision_call_stream_describe_finish (impl, result, &error);
      break;
    case VISION_CALL_STREAM_OCR:
      ok = xdp_dbus_impl_vision_call_stream_ocr_finish (impl, result, &error);
      break;
    case VISION_CALL_STREAM_SEGMENT:
      ok = xdp_dbus_impl_vision_call_stream_segment_finish (impl, result, &error);
      break;
    }

  vision_signal_forward_disconnect (forward);

  if (!ok)
    {
      model_request_emit_response (forward->request,
                                   model_response_from_error (error),
                                   error->message);
      vision_signal_forward_unref (forward);
      return;
    }

  model_request_emit_response (forward->request, 0, NULL);
  vision_signal_forward_unref (forward);
}

static void
forward_vision_text_received (XdpDbusImplVision *impl,
                              const char        *request_id,
                              const char        *session_id,
                              const char        *text,
                              gboolean           done,
                              gpointer           user_data)
{
  VisionSignalForward *forward = user_data;

  if (g_strcmp0 (request_id, forward->request_handle) != 0 ||
      g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  vision_emit_signal_to_request (forward,
                                 "VisionTextReceived",
                                 g_variant_new ("(oosb)",
                                                forward->request_handle,
                                                forward->session_handle,
                                                text,
                                                done));

  if (done)
    vision_signal_forward_disconnect (forward);
}

static void
forward_vision_segments_received (XdpDbusImplVision *impl,
                                  const char        *request_id,
                                  const char        *session_id,
                                  GVariant          *segments,
                                  gboolean           done,
                                  gpointer           user_data)
{
  VisionSignalForward *forward = user_data;

  if (g_strcmp0 (request_id, forward->request_handle) != 0 ||
      g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  vision_emit_signal_to_request (forward,
                                 "VisionSegmentsReceived",
                                 g_variant_new ("(oo@a(sddddd)b)",
                                                forward->request_handle,
                                                forward->session_handle,
                                                g_variant_ref (segments),
                                                done));

  if (done)
    vision_signal_forward_disconnect (forward);
}

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
                                                                 model_app_id_from_invocation (invocation, app_info),
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

static void
vision_create_session_done (GObject      *source,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  XdpDbusImplVision *impl = XDP_DBUS_IMPL_VISION (source);
  g_autoptr(VisionCreateSession) create = user_data;
  g_autofree char *backend_session_id = NULL;
  XdpSession *session = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_vision_call_create_session_finish (impl,
                                                        &backend_session_id,
                                                        result,
                                                        &error))
    {
      model_request_emit_response (create->request,
                                   model_response_from_error (error),
                                   error->message);
      return;
    }

  if (!model_request_is_exported (create->request))
    {
      end_backend_session (G_OBJECT (create->vision->impl),
                           MODEL_SESSION_VISION,
                           backend_session_id);
      return;
    }

  session = XDP_SESSION (model_session_new (create->vision->context,
                                            create->connection,
                                            create->app_info,
                                            G_OBJECT (create->vision->impl),
                                            MODEL_SESSION_VISION,
                                            create->use_case,
                                            create->options,
                                            backend_session_id,
                                            &error));
  if (session == NULL)
    {
      end_backend_session (G_OBJECT (create->vision->impl),
                           MODEL_SESSION_VISION,
                           backend_session_id);
      model_request_emit_response (create->request, 2, error->message);
      return;
    }

  {
    SESSION_AUTOLOCK (session);

    if (!xdp_session_export (session, &error))
      {
        xdp_session_close (session, FALSE);
        model_request_emit_response (create->request, 2, error->message);
        return;
      }

    if (!model_request_register_session_and_emit_response (create->request, session))
      xdp_session_close (session, FALSE);
  }
}

static gboolean
handle_vision_create_session (XdpDbusVision       *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_parent_window,
                              const char            *arg_use_case,
                              const char            *arg_instructions,
                              GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  XdpRequest *request = xdp_request_from_invocation (invocation);
  VisionCreateSession *create;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (!model_session_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_request_export_with_impl (request,
                                       connection,
                                       G_DBUS_PROXY (vision->impl),
                                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  create = vision_create_session_new (vision,
                                       request,
                                       app_info,
                                       connection,
                                       arg_use_case,
                                       arg_options);
  xdp_dbus_impl_vision_call_create_session (vision->impl,
                                             xdp_request_get_object_path (request),
                                             model_app_id_from_invocation (invocation, app_info),
                                             arg_parent_window,
                                             arg_use_case,
                                             arg_instructions,
                                      NULL,
                                            vision_create_session_done,
                                            create);
  xdp_dbus_vision_complete_create_session (object,
                                           invocation,
                                           xdp_request_get_object_path (request));
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
  XdpRequest *request = xdp_request_from_invocation (invocation);
  VisionSignalForward *forward;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_VISION);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_request_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);
  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  if (!model_session_ensure_open (invocation, model_session))
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  if (!model_request_export_with_impl (request,
                                       g_dbus_method_invocation_get_connection (invocation),
                                       G_DBUS_PROXY (vision->impl),
                                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  forward = vision_signal_forward_new (vision,
                                       vision->impl,
                                       request,
                                       model_session_get_backend_session_id (model_session),
                                       session->id,
                                       VISION_CALL_PREWARM);
  vision_signal_forward_connect_loading (forward);

  xdp_dbus_impl_vision_call_prewarm (vision->impl,
                                     xdp_request_get_object_path (request),
                                     model_session_get_backend_session_id (model_session),
                                             NULL,
                                     finish_vision_call,
                                     forward);
  xdp_dbus_vision_complete_prewarm (object,
                                    invocation,
                                    xdp_request_get_object_path (request));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_stream_describe (XdpDbusVision       *object,
                               GDBusMethodInvocation *invocation,
                               const char            *arg_session_handle,
                               const char            *arg_image,
                               GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  VisionSignalForward *forward;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_VISION);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_exact_use_case (invocation,
                                            MODEL_SESSION (session),
                                            "vision.describe",
                                            "StreamDescribe"))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_request_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);
  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  if (!model_session_ensure_open (invocation, model_session))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_request_export_with_impl (request,
                                       g_dbus_method_invocation_get_connection (invocation),
                                       G_DBUS_PROXY (vision->impl),
                                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  forward = vision_signal_forward_new (vision,
                                        vision->impl,
                                        request,
                                        model_session_get_backend_session_id (model_session),
                                        session->id,
                                        VISION_CALL_STREAM_DESCRIBE);
  vision_signal_forward_connect_loading (forward);
  forward->handler_id = g_signal_connect (vision->impl,
                                          "vision-text-received",
                                          G_CALLBACK (forward_vision_text_received),
                                          forward);

  xdp_dbus_impl_vision_call_stream_describe (vision->impl,
                                             xdp_request_get_object_path (request),
                                             model_session_get_backend_session_id (model_session),
                                             arg_image,
                                             NULL,
                                             finish_vision_call,
                                             forward);
  xdp_dbus_vision_complete_stream_describe (object,
                                           invocation,
                                           xdp_request_get_object_path (request));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_stream_ocr (XdpDbusVision       *object,
                          GDBusMethodInvocation *invocation,
                          const char            *arg_session_handle,
                          const char            *arg_image,
                          GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  VisionSignalForward *forward;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_VISION);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_exact_use_case (invocation,
                                            MODEL_SESSION (session),
                                            "vision.ocr",
                                            "StreamOcr"))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_request_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);
  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  if (!model_session_ensure_open (invocation, model_session))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_request_export_with_impl (request,
                                       g_dbus_method_invocation_get_connection (invocation),
                                       G_DBUS_PROXY (vision->impl),
                                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  forward = vision_signal_forward_new (vision,
                                        vision->impl,
                                        request,
                                        model_session_get_backend_session_id (model_session),
                                        session->id,
                                        VISION_CALL_STREAM_OCR);
  vision_signal_forward_connect_loading (forward);
  forward->handler_id = g_signal_connect (vision->impl,
                                          "vision-text-received",
                                          G_CALLBACK (forward_vision_text_received),
                                          forward);

  xdp_dbus_impl_vision_call_stream_ocr (vision->impl,
                                        xdp_request_get_object_path (request),
                                        model_session_get_backend_session_id (model_session),
                                        arg_image,
                                        NULL,
                                        finish_vision_call,
                                        forward);
  xdp_dbus_vision_complete_stream_ocr (object,
                                      invocation,
                                      xdp_request_get_object_path (request));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_vision_stream_segment (XdpDbusVision       *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_session_handle,
                              const char            *arg_image,
                              GVariant              *arg_options)
{
  Vision *vision = (Vision *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  VisionSignalForward *forward;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_VISION);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_exact_use_case (invocation,
                                            MODEL_SESSION (session),
                                            "vision.segment",
                                            "StreamSegment"))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_request_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);
  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  if (!model_session_ensure_open (invocation, model_session))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_request_export_with_impl (request,
                                       g_dbus_method_invocation_get_connection (invocation),
                                       G_DBUS_PROXY (vision->impl),
                                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  forward = vision_signal_forward_new (vision,
                                        vision->impl,
                                        request,
                                        model_session_get_backend_session_id (model_session),
                                        session->id,
                                        VISION_CALL_STREAM_SEGMENT);
  vision_signal_forward_connect_loading (forward);
  forward->handler_id = g_signal_connect (vision->impl,
                                          "vision-segments-received",
                                          G_CALLBACK (forward_vision_segments_received),
                                          forward);

  xdp_dbus_impl_vision_call_stream_segment (vision->impl,
                                            xdp_request_get_object_path (request),
                                            model_session_get_backend_session_id (model_session),
                                            arg_image,
                                            NULL,
                                            finish_vision_call,
                                            forward);
  xdp_dbus_vision_complete_stream_segment (object,
                                          invocation,
                                          xdp_request_get_object_path (request));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
vision_iface_init (XdpDbusVisionIface *iface)
{
  iface->handle_get_use_case_availability = handle_vision_get_use_case_availability;
  iface->handle_create_session = handle_vision_create_session;
  iface->handle_prewarm = handle_vision_prewarm;
  iface->handle_stream_describe = handle_vision_stream_describe;
  iface->handle_stream_ocr = handle_vision_stream_ocr;
  iface->handle_stream_segment = handle_vision_stream_segment;
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
  xdp_dbus_vision_set_version (XDP_DBUS_VISION (vision), 4);

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
