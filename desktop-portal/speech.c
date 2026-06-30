/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "speech.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

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
  XdpRequest *request;
  char *request_handle;
  char *session_handle;
  gulong loading_handler_id;
  gulong handler_id;
  guint call_kind;
} SpeechSignalForward;

typedef enum
{
  SPEECH_CALL_PREWARM,
  SPEECH_CALL_STREAM_TRANSCRIBE,
} SpeechCallKind;

typedef struct _SpeechCreateSession
{
  Speech *speech;
  XdpRequest *request;
  XdpSession *session;
} SpeechCreateSession;

static SpeechCreateSession *
speech_create_session_new (Speech         *speech,
                            XdpRequest     *request,
                           XdpSession     *session)
{
  SpeechCreateSession *create = g_new0 (SpeechCreateSession, 1);

  create->speech = g_object_ref (speech);
  create->request = g_object_ref (request);
  create->session = g_object_ref (session);

  return create;
}

static void
speech_create_session_free (SpeechCreateSession *create)
{
  g_clear_object (&create->speech);
  g_clear_object (&create->request);
  g_clear_object (&create->session);
  g_free (create);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SpeechCreateSession, speech_create_session_free)

static SpeechSignalForward *
speech_signal_forward_new (Speech            *speech,
                           XdpDbusImplSpeech *impl,
                           XdpRequest        *request,
                           const char        *session_handle,
                           SpeechCallKind     call_kind)
{
  SpeechSignalForward *forward = g_new0 (SpeechSignalForward, 1);

  forward->speech = g_object_ref (speech);
  forward->impl = g_object_ref (impl);
  forward->request = g_object_ref (request);
  forward->request_handle = g_strdup (xdp_request_get_object_path (request));
  forward->session_handle = g_strdup (session_handle);
  forward->call_kind = call_kind;

  return forward;
}

static void
speech_signal_forward_unref (SpeechSignalForward *forward)
{
  g_clear_object (&forward->speech);
  g_clear_object (&forward->impl);
  g_clear_object (&forward->request);
  g_clear_pointer (&forward->request_handle, g_free);
  g_clear_pointer (&forward->session_handle, g_free);
  g_free (forward);
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
speech_emit_signal_to_request (SpeechSignalForward *forward,
                               const char          *signal_name,
                               GVariant            *parameters)
{
  REQUEST_AUTOLOCK (forward->request);

  if (!forward->request->exported)
    {
      g_variant_unref (parameters);
      return;
    }

  g_dbus_connection_emit_signal (xdp_context_get_connection (forward->speech->context),
                                 forward->request->sender,
                                 DESKTOP_DBUS_PATH,
                                 SPEECH_DBUS_IFACE,
                                 signal_name,
                                 parameters,
                                 NULL);
}

static void
forward_model_loading (XdpDbusImplSpeech *impl,
                       const char        *request_id,
                       const char        *session_id,
                       const char        *message,
                       gpointer           user_data)
{
  SpeechSignalForward *forward = user_data;

  if (g_strcmp0 (request_id, forward->request_handle) != 0 ||
      g_strcmp0 (session_id, forward->session_handle) != 0)
    return;

  speech_emit_signal_to_request (forward,
                                 "ModelLoading",
                                 g_variant_new ("(oos)",
                                                forward->request_handle,
                                                forward->session_handle,
                                                message));
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
finish_speech_call (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  SpeechSignalForward *forward = user_data;
  XdpDbusImplSpeech *impl = XDP_DBUS_IMPL_SPEECH (source);
  g_autoptr(GError) error = NULL;
  gboolean ok = FALSE;

  switch ((SpeechCallKind) forward->call_kind)
    {
    case SPEECH_CALL_PREWARM:
      ok = xdp_dbus_impl_speech_call_prewarm_finish (impl, result, &error);
      break;
    case SPEECH_CALL_STREAM_TRANSCRIBE:
      ok = xdp_dbus_impl_speech_call_stream_transcribe_finish (impl, NULL, result, &error);
      break;
    }

  speech_signal_forward_disconnect (forward);

  if (!ok)
    {
      model_request_emit_response (forward->request,
                                   model_response_from_error (error),
                                   error->message);
      speech_signal_forward_unref (forward);
      return;
    }

  model_request_emit_response (forward->request, 0, NULL);
  speech_signal_forward_unref (forward);
}

static void
forward_transcription_received (XdpDbusImplSpeech *impl,
                                const char        *request_id,
                                const char        *session_id,
                                const char        *text,
                                gboolean           done,
                                gpointer           user_data)
{
  SpeechSignalForward *forward = user_data;

  if (g_strcmp0 (request_id, forward->request_handle) != 0 ||
      g_strcmp0 (session_id, forward->session_handle) != 0)
    return;

  speech_emit_signal_to_request (forward,
                                 "TranscriptionReceived",
                                 g_variant_new ("(oosb)",
                                                forward->request_handle,
                                                forward->session_handle,
                                                text,
                                                done));

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

  if (!model_use_case_is_supported (MODEL_SESSION_SPEECH, arg_use_case))
    {
      availability = model_unsupported_use_case_availability (arg_use_case);
      xdp_dbus_speech_complete_get_use_case_availability (object, invocation, availability);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_dbus_impl_speech_call_get_use_case_availability_sync (speech->impl,
                                                                  model_app_id_from_invocation (invocation, app_info),
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

static void
speech_create_session_done (GObject      *source,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  XdpDbusImplSpeech *impl = XDP_DBUS_IMPL_SPEECH (source);
  g_autoptr(SpeechCreateSession) create = user_data;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_speech_call_create_session_finish (impl, result, &error))
    {
      model_request_emit_response (create->request,
                                   model_response_from_error (error),
                                   error->message);
      xdp_session_close (create->session, FALSE);
      return;
    }

  if (!model_request_is_exported (create->request))
    {
      xdp_session_close (create->session, FALSE);
      return;
    }

  {
    SESSION_AUTOLOCK (create->session);

    if (!xdp_session_export (create->session, &error))
      {
        xdp_session_close (create->session, FALSE);
        model_request_emit_response (create->request, 2, error->message);
        return;
      }

    if (!model_request_register_session_and_emit_response (create->request, create->session))
      xdp_session_close (create->session, FALSE);
  }
}

static gboolean
handle_speech_create_session (XdpDbusSpeech       *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_parent_window,
                              const char            *arg_use_case,
                              const char            *arg_instructions,
                              GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  XdpRequest *request = xdp_request_from_invocation (invocation);
  XdpSession *session;
  SpeechCreateSession *create;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (!model_validate_use_case_for_session (invocation, MODEL_SESSION_SPEECH, arg_use_case))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_request_export_with_impl (request,
                                       connection,
                                       G_DBUS_PROXY (speech->impl),
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
                                            arg_use_case,
                                            arg_options,
                                            &error));
  if (session == NULL)
    {
      xdp_request_unexport (request);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  create = speech_create_session_new (speech,
                                       request,
                                       session);
  xdp_dbus_impl_speech_call_create_session (speech->impl,
                                             xdp_request_get_object_path (request),
                                             session->id,
                                             model_app_id_from_invocation (invocation, app_info),
                                             arg_parent_window,
                                             arg_use_case,
                                             arg_instructions,
                                             NULL,
                                            speech_create_session_done,
                                            create);
  xdp_dbus_speech_complete_create_session (object,
                                           invocation,
                                           xdp_request_get_object_path (request));
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
  XdpRequest *request = xdp_request_from_invocation (invocation);
  SpeechSignalForward *forward;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_SPEECH);
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
                                       G_DBUS_PROXY (speech->impl),
                                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  forward = speech_signal_forward_new (speech,
                                         speech->impl,
                                         request,
                                         session->id,
                                         SPEECH_CALL_PREWARM);
  speech_signal_forward_connect_loading (forward);

  xdp_dbus_impl_speech_call_prewarm (speech->impl,
                                     xdp_request_get_object_path (request),
                                     session->id,
                                      NULL,
                                     finish_speech_call,
                                     forward);
  xdp_dbus_speech_complete_prewarm (object,
                                    invocation,
                                    xdp_request_get_object_path (request));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_speech_stream_transcribe (XdpDbusSpeech       *object,
                                 GDBusMethodInvocation *invocation,
                                 GUnixFDList          *fd_list,
                                 const char            *arg_session_handle,
                                 GVariant              *arg_audio_fd,
                                 const char            *arg_source_language_hint,
                                 GVariant              *arg_options)
{
  Speech *speech = (Speech *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  XdpRequest *request = xdp_request_from_invocation (invocation);
  SpeechSignalForward *forward;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_SPEECH);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_speech_use_case (invocation,
                                             MODEL_SESSION (session),
                                             "StreamTranscribe"))
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
                                       G_DBUS_PROXY (speech->impl),
                                       &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  forward = speech_signal_forward_new (speech,
                                         speech->impl,
                                         request,
                                         session->id,
                                         SPEECH_CALL_STREAM_TRANSCRIBE);
  speech_signal_forward_connect_loading (forward);
  forward->handler_id = g_signal_connect (speech->impl,
                                          "transcription-received",
                                          G_CALLBACK (forward_transcription_received),
                                          forward);

  xdp_dbus_impl_speech_call_stream_transcribe (speech->impl,
                                               xdp_request_get_object_path (request),
                                               session->id,
                                               arg_audio_fd,
                                               arg_source_language_hint,
                                               fd_list,
                                               NULL,
                                               finish_speech_call,
                                               forward);
  xdp_dbus_speech_complete_stream_transcribe (object,
                                              invocation,
                                              NULL,
                                              xdp_request_get_object_path (request));
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
  xdp_dbus_speech_set_version (XDP_DBUS_SPEECH (speech), 4);

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
