/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "speech.h"

#include <gio/gio.h>

#include "model-session.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-session.h"
#include "xdp-utils.h"

typedef struct _Speech Speech;
typedef struct _SpeechClass SpeechClass;

struct _Speech
{
  XdpDbusSpeechSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplSpeech *impl;
};

struct _SpeechClass
{
  XdpDbusSpeechSkeletonClass parent_class;
};

GType speech_get_type (void) G_GNUC_CONST;

static void speech_iface_init (XdpDbusSpeechIface *iface);

G_DEFINE_TYPE_WITH_CODE (Speech, speech, XDP_DBUS_TYPE_SPEECH_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SPEECH,
                                                speech_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Speech, g_object_unref)

typedef struct _SpeechSignalForward
{
  Speech *speech;
  XdpDbusImplSpeech *impl;
  char *backend_session_id;
  char *session_handle;
  gulong loading_handler_id;
  gulong handler_id;
} SpeechSignalForward;

static SpeechSignalForward *
speech_signal_forward_new (Speech            *speech,
                           XdpDbusImplSpeech *impl,
                           const char        *backend_session_id,
                           const char        *session_handle)
{
  SpeechSignalForward *forward = g_new0 (SpeechSignalForward, 1);

  forward->speech = g_object_ref (speech);
  forward->impl = g_object_ref (impl);
  forward->backend_session_id = g_strdup (backend_session_id);
  forward->session_handle = g_strdup (session_handle);

  return forward;
}

static void
speech_signal_forward_unref (SpeechSignalForward *forward)
{
  g_clear_object (&forward->speech);
  g_clear_object (&forward->impl);
  g_clear_pointer (&forward->backend_session_id, g_free);
  g_clear_pointer (&forward->session_handle, g_free);
  g_free (forward);
}

static void
speech_signal_forward_free (gpointer  data,
                            GClosure *closure)
{
  speech_signal_forward_unref (data);
}

static void
speech_signal_forward_disconnect (SpeechSignalForward *forward)
{
  XdpDbusImplSpeech *impl = forward->impl;
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
forward_model_loading (XdpDbusImplSpeech *impl,
                       const char        *session_id,
                       const char        *message,
                       gpointer           user_data)
{
  SpeechSignalForward *forward = user_data;

  if (g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  xdp_dbus_speech_emit_model_loading (XDP_DBUS_SPEECH (forward->speech),
                                      forward->session_handle,
                                      message);
}

static void
speech_signal_forward_connect_loading (SpeechSignalForward *forward)
{
  forward->loading_handler_id = g_signal_connect (forward->impl,
                                                  "model-loading",
                                                  G_CALLBACK (forward_model_loading),
                                                  forward);
}

static void
forward_transcription_received (XdpDbusImplSpeech *impl,
                                const char        *session_id,
                                const char        *text,
                                gboolean           done,
                                gpointer           user_data)
{
  SpeechSignalForward *forward = user_data;

  if (g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  xdp_dbus_speech_emit_transcription_received (XDP_DBUS_SPEECH (forward->speech),
                                                forward->session_handle,
                                                text,
                                                done);

  if (done)
    speech_signal_forward_disconnect (forward);
}

static gboolean
handle_speech_get_use_case_availability (XdpDbusSpeech       *object,
                                         GDBusMethodInvocation *invocation,
                                         const char            *arg_use_case,
                                         GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) availability = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_speech_call_get_use_case_availability_sync (speech->impl,
                                                                 xdp_app_info_get_id (app_info),
                                                                 arg_use_case,
                                                                 &availability,
                                                                 NULL,
                                                                 &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_get_use_case_availability (object, invocation, availability);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_speech_create_session (XdpDbusSpeech       *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_use_case,
                              const char            *arg_instructions,
                              GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autofree char *backend_session_id = NULL;
  g_autoptr(XdpSession) session = NULL;
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_speech_call_create_session_sync (speech->impl,
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

  session = XDP_SESSION (model_session_new (speech->context,
                                            connection,
                                            app_info,
                                            G_OBJECT (speech->impl),
                                            MODEL_SESSION_SPEECH,
                                            backend_session_id,
                                            &error));
  if (session == NULL)
    {
      end_backend_session (G_OBJECT (speech->impl),
                           MODEL_SESSION_SPEECH,
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
  xdp_dbus_speech_complete_create_session (object, invocation, session->id);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_speech_prewarm (XdpDbusSpeech       *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_session_handle,
                       GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GError) error = NULL;
  SpeechSignalForward *forward;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_SPEECH);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  forward = speech_signal_forward_new (speech,
                                       speech->impl,
                                       model_session_get_backend_session_id (model_session),
                                       session->id);
  speech_signal_forward_connect_loading (forward);

  if (!xdp_dbus_impl_speech_call_prewarm_sync (speech->impl,
                                                model_session_get_backend_session_id (model_session),
                                                NULL,
                                                &error))
    {
      speech_signal_forward_disconnect (forward);
      speech_signal_forward_unref (forward);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  speech_signal_forward_disconnect (forward);
  speech_signal_forward_unref (forward);
  xdp_dbus_speech_complete_prewarm (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_speech_stream_transcribe (XdpDbusSpeech       *object,
                                 GDBusMethodInvocation *invocation,
                                 const char            *arg_session_handle,
                                 const char            *arg_audio,
                                 const char            *arg_source_language_hint,
                                 GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GError) error = NULL;
  SpeechSignalForward *forward;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_SPEECH);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);

  forward = speech_signal_forward_new (speech,
                                        speech->impl,
                                        model_session_get_backend_session_id (model_session),
                                        session->id);
  speech_signal_forward_connect_loading (forward);
  forward->handler_id = g_signal_connect_data (speech->impl,
                                               "transcription-received",
                                               G_CALLBACK (forward_transcription_received),
                                               forward,
                                               speech_signal_forward_free,
                                               G_CONNECT_DEFAULT);

  if (!xdp_dbus_impl_speech_call_stream_transcribe_sync (speech->impl,
                                                        model_session_get_backend_session_id (model_session),
                                                        arg_audio,
                                                         arg_source_language_hint,
                                                        NULL,
                                                        &error))
    {
      speech_signal_forward_disconnect (forward);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_stream_transcribe (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
speech_iface_init (XdpDbusSpeechIface *iface)
{
  iface->handle_get_use_case_availability = handle_speech_get_use_case_availability;
  iface->handle_create_session = handle_speech_create_session;
  iface->handle_prewarm = handle_speech_prewarm;
  iface->handle_stream_transcribe = handle_speech_stream_transcribe;
}

static void
speech_dispose (GObject *object)
{
  Speech *speech = (Speech *) object;

  g_clear_object (&speech->impl);

  G_OBJECT_CLASS (speech_parent_class)->dispose (object);
}

static void
speech_init (Speech *speech)
{
}

static void
speech_class_init (SpeechClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = speech_dispose;
}

static Speech *
speech_new (XdpContext        *context,
            XdpDbusImplSpeech *impl)
{
  Speech *speech = g_object_new (speech_get_type (), NULL);

  speech->context = context;
  speech->impl = g_object_ref (impl);
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (speech->impl), G_MAXINT);
  xdp_dbus_speech_set_version (XDP_DBUS_SPEECH (speech), 2);

  return speech;
}

void
init_speech (XdpContext *context)
{
  g_autoptr(Speech) speech = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  XdpDbusImplSpeech *impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, SPEECH_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_speech_proxy_new_sync (connection,
                                              G_DBUS_PROXY_FLAGS_NONE,
                                              impl_config->dbus_name,
                                              DESKTOP_DBUS_PATH,
                                              NULL,
                                              &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create speech proxy: %s", error->message);
      return;
    }

  speech = speech_new (context, impl);
  g_clear_object (&impl);
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&speech)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
