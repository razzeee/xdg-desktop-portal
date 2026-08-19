/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "spoken-language.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "model-session.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-utils.h"

typedef struct _SpokenLanguage SpokenLanguage;
typedef struct _SpokenLanguageClass SpokenLanguageClass;

struct _SpokenLanguage
{
  XdpDbusSpokenLanguageSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplSpokenLanguage *impl;
  XdpSessionDexStore *sessions;
};

struct _SpokenLanguageClass
{
  XdpDbusSpokenLanguageSkeletonClass parent_class;
};

GType spoken_language_get_type (void);

static void spoken_language_iface_init (XdpDbusSpokenLanguageIface *iface);

G_DEFINE_TYPE_WITH_CODE (SpokenLanguage, spoken_language, XDP_DBUS_TYPE_SPOKEN_LANGUAGE_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SPOKEN_LANGUAGE,
                                                spoken_language_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SpokenLanguage, g_object_unref)

static const char * const spoken_language_use_cases[] = {
  "speech.transcribe",
  "speech.translate",
  "speech.synthesize",
  NULL,
};

static const char * const spoken_language_transcribe_use_cases[] = {
  "speech.transcribe",
  "speech.translate",
  NULL,
};

static const char * const spoken_language_synthesize_use_cases[] = {
  "speech.synthesize",
  NULL,
};

static void
forward_transcription_received (XdpDbusImplSpokenLanguage *impl G_GNUC_UNUSED,
                                 const char                 *request_handle,
                                 const char                 *session_handle,
                                 const char                 *text,
                                 gboolean                    done,
                                 ModelRequest               *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "TranscriptionReceived",
                             g_variant_new ("(oosb)",
                                            request_handle,
                                            session_handle,
                                            text,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static void
forward_audio_received (XdpDbusImplSpokenLanguage *impl G_GNUC_UNUSED,
                        const char                 *request_handle,
                        const char                 *session_handle,
                        GVariant                   *audio,
                        guint                       sample_rate,
                        guint                       channels,
                        const char                 *sample_format,
                        gboolean                    done,
                        ModelRequest               *request)
{
  if (!model_request_matches (request, request_handle, session_handle))
    return;

  model_request_emit_signal (request,
                             "AudioReceived",
                             g_variant_new ("(oo@ayuusb)",
                                            request_handle,
                                            session_handle,
                                            audio,
                                            sample_rate,
                                            channels,
                                            sample_format,
                                            done));

  if (done)
    model_request_mark_terminal (request);
}

static gboolean
handle_spoken_language_get_use_case_availability (XdpDbusSpokenLanguage *object,
                                                  GDBusMethodInvocation *invocation,
                                                  const char            *arg_use_case,
                                                  GVariant              *arg_options)
{
  SpokenLanguage *spoken_language = (SpokenLanguage *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) availability = NULL;
  g_autoptr(GError) error = NULL;

  if (!model_availability_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_use_case_is_supported (arg_use_case, spoken_language_use_cases))
    {
      availability = model_unsupported_use_case_availability (arg_use_case);

      xdp_dbus_spoken_language_complete_get_use_case_availability (
        object,
        invocation,
        availability);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  availability = model_get_use_case_availability (
    G_DBUS_PROXY (spoken_language->impl),
    SPOKEN_LANGUAGE_DBUS_IMPL_IFACE,
    xdp_app_info_get_id (app_info),
    arg_use_case,
    &error);
  if (availability == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_spoken_language_complete_get_use_case_availability (
    object,
    invocation,
    availability);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_spoken_language_create_session (XdpDbusSpokenLanguage *object,
                                       GDBusMethodInvocation *invocation,
                                       const char            *arg_parent_window,
                                       const char            *arg_use_case,
                                       const char            *arg_instructions,
                                       GVariant              *arg_options)
{
  SpokenLanguage *spoken_language = (SpokenLanguage *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GError) error = NULL;
  XdpSessionDex *session_dex;
  DexFuture *call_future;

  if (!model_validate_use_case_for_session (invocation,
                                            arg_use_case,
                                            spoken_language_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = model_session_create (spoken_language->context,
                                   app_info,
                                   G_DBUS_INTERFACE_SKELETON (spoken_language),
                                   G_DBUS_PROXY (spoken_language->impl),
                                  arg_use_case,
                                  arg_options,
                                  &error);
  if (session == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session_dex = model_session_get_session (session);
  request = model_request_new (spoken_language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (spoken_language),
                               G_DBUS_PROXY (spoken_language->impl),
                               session_dex,
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  call_future = xdp_dbus_impl_spoken_language_call_create_session_future (
    spoken_language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    xdp_app_info_get_id (app_info),
    arg_parent_window,
    arg_use_case,
    arg_instructions);
  xdp_dbus_spoken_language_complete_create_session (
    object,
    invocation,
    model_request_get_handle (request));

  if (!model_request_await_call (request, call_future))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (model_request_emit_session_response (request, session_dex))
    xdp_session_dex_store_take_session (spoken_language->sessions,
                                        g_steal_pointer (&session));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_spoken_language_prewarm (XdpDbusSpokenLanguage *object,
                                GDBusMethodInvocation *invocation,
                                const char            *arg_session_handle,
                                GVariant              *arg_options)
{
  SpokenLanguage *spoken_language = (SpokenLanguage *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (spoken_language->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_prewarm_options_validate (arg_options, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (spoken_language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (spoken_language),
                               G_DBUS_PROXY (spoken_language->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  call_future = xdp_dbus_impl_spoken_language_call_prewarm_future (
    spoken_language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request));
  xdp_dbus_spoken_language_complete_prewarm (
    object,
    invocation,
    model_request_get_handle (request));
  model_request_finish (request, call_future, FALSE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_spoken_language_stream_transcribe (XdpDbusSpokenLanguage *object,
                                          GDBusMethodInvocation *invocation,
                                          GUnixFDList           *fd_list,
                                          const char            *arg_session_handle,
                                          GVariant              *arg_audio_fd,
                                          GVariant              *arg_options)
{
  SpokenLanguage *spoken_language = (SpokenLanguage *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) sealed_audio_fd = NULL;
  g_autoptr(GUnixFDList) sealed_fd_list = NULL;
  g_autoptr(XdpSealedFd) sealed_audio = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (spoken_language->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamTranscribe",
                                       spoken_language_transcribe_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = model_speech_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!model_seal_fd (arg_audio_fd,
                      fd_list,
                      &sealed_audio_fd,
                      &sealed_fd_list,
                      &sealed_audio,
                      &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (spoken_language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (spoken_language),
                               G_DBUS_PROXY (spoken_language->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  model_request_connect_signal (request,
                                "transcription-received",
                                G_CALLBACK (forward_transcription_received));
  call_future = xdp_dbus_impl_spoken_language_call_stream_transcribe_future (
    spoken_language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    sealed_audio_fd,
    options,
    sealed_fd_list);
  model_request_take_sealed_fd (request,
                                g_steal_pointer (&sealed_audio),
                                g_steal_pointer (&sealed_fd_list));
  xdp_dbus_spoken_language_complete_stream_transcribe (
    object,
    invocation,
    NULL,
    model_request_get_handle (request));
  model_request_finish (request, call_future, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_spoken_language_stream_synthesize (XdpDbusSpokenLanguage *object,
                                          GDBusMethodInvocation *invocation,
                                          const char            *arg_session_handle,
                                          const char            *arg_text,
                                          GVariant              *arg_options)
{
  SpokenLanguage *spoken_language = (SpokenLanguage *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(ModelSession) session = NULL;
  g_autoptr(ModelRequest) request = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  DexFuture *call_future;

  session = model_session_lookup (spoken_language->sessions,
                                  invocation,
                                  arg_session_handle);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (!model_session_ensure_use_case (invocation,
                                      session,
                                      "StreamSynthesize",
                                       spoken_language_synthesize_use_cases))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  if (arg_text[0] == '\0')
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Synthesis text must not be empty");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  options = model_synthesis_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = model_request_new (spoken_language->context,
                               app_info,
                               G_DBUS_INTERFACE_SKELETON (spoken_language),
                               G_DBUS_PROXY (spoken_language->impl),
                               model_session_get_session (session),
                               arg_options,
                               &error);
  if (request == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  model_request_connect_loading (request);
  model_request_connect_signal (request,
                                "audio-received",
                                G_CALLBACK (forward_audio_received));
  call_future = xdp_dbus_impl_spoken_language_call_stream_synthesize_future (
    spoken_language->impl,
    model_request_get_handle (request),
    model_request_get_session_handle (request),
    arg_text,
    options);
  xdp_dbus_spoken_language_complete_stream_synthesize (
    object,
    invocation,
    model_request_get_handle (request));
  model_request_finish (request, call_future, TRUE);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
spoken_language_iface_init (XdpDbusSpokenLanguageIface *iface)
{
  iface->handle_get_use_case_availability = handle_spoken_language_get_use_case_availability;
  iface->handle_create_session = handle_spoken_language_create_session;
  iface->handle_prewarm = handle_spoken_language_prewarm;
  iface->handle_stream_transcribe = handle_spoken_language_stream_transcribe;
  iface->handle_stream_synthesize = handle_spoken_language_stream_synthesize;
}

static void
spoken_language_dispose (GObject *object)
{
  SpokenLanguage *spoken_language = (SpokenLanguage *) object;

  g_clear_object (&spoken_language->sessions);
  g_clear_object (&spoken_language->impl);

  G_OBJECT_CLASS (spoken_language_parent_class)->dispose (object);
}

static void
spoken_language_init (SpokenLanguage *spoken_language)
{
}

static void
spoken_language_class_init (SpokenLanguageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = spoken_language_dispose;
}

static SpokenLanguage *
spoken_language_new (XdpContext                 *context,
                     XdpDbusImplSpokenLanguage *impl)
{
  SpokenLanguage *spoken_language;

  spoken_language = g_object_new (spoken_language_get_type (), NULL);
  spoken_language->context = context;
  spoken_language->impl = g_object_ref (impl);
  spoken_language->sessions = model_session_store_new ();

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (spoken_language->impl), G_MAXINT);
  xdp_dbus_spoken_language_set_version (XDP_DBUS_SPOKEN_LANGUAGE (spoken_language), 1);

  return spoken_language;
}

void
init_spoken_language (XdpContext *context)
{
  g_autoptr(SpokenLanguage) spoken_language = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  g_autoptr(XdpDbusImplSpokenLanguage) impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, SPOKEN_LANGUAGE_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_spoken_language_proxy_new_sync (connection,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       impl_config->dbus_name,
                                                       DESKTOP_DBUS_PATH,
                                                       NULL,
                                                       &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create SpokenLanguage proxy: %s", error->message);
      return;
    }

  spoken_language = spoken_language_new (context, impl);
  xdp_context_take_and_export_portal (
    context,
    G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&spoken_language)),
    XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
}
