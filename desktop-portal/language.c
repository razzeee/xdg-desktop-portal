/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "language.h"

#include <gio/gio.h>

#include "model-session.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-portal-config.h"
#include "xdp-session.h"
#include "xdp-utils.h"

typedef struct _Language Language;
typedef struct _LanguageClass LanguageClass;

struct _Language
{
  XdpDbusLanguageSkeleton parent_instance;

  XdpContext *context;
  XdpDbusImplLanguage *impl;
};

struct _LanguageClass
{
  XdpDbusLanguageSkeletonClass parent_class;
};

GType language_get_type (void) G_GNUC_CONST;

static void language_iface_init (XdpDbusLanguageIface *iface);

G_DEFINE_TYPE_WITH_CODE (Language, language, XDP_DBUS_TYPE_LANGUAGE_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_LANGUAGE,
                                                language_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Language, g_object_unref)

typedef struct _LanguageSignalForward
{
  Language *language;
  XdpDbusImplLanguage *impl;
  char *backend_session_id;
  char *session_handle;
  gulong handler_id;
  gulong sibling_handler_id;
} LanguageSignalForward;

static LanguageSignalForward *
language_signal_forward_new (Language           *language,
                             XdpDbusImplLanguage *impl,
                             const char         *backend_session_id,
                             const char         *session_handle)
{
  LanguageSignalForward *forward = g_new0 (LanguageSignalForward, 1);

  forward->language = g_object_ref (language);
  forward->impl = g_object_ref (impl);
  forward->backend_session_id = g_strdup (backend_session_id);
  forward->session_handle = g_strdup (session_handle);

  return forward;
}

static void
language_signal_forward_free (gpointer  data,
                              GClosure *closure)
{
  LanguageSignalForward *forward = data;

  g_clear_object (&forward->language);
  g_clear_object (&forward->impl);
  g_clear_pointer (&forward->backend_session_id, g_free);
  g_clear_pointer (&forward->session_handle, g_free);
  g_free (forward);
}

static void
language_signal_forward_disconnect (LanguageSignalForward *forward)
{
  XdpDbusImplLanguage *impl = forward->impl;
  gulong handler_id = forward->handler_id;
  gulong sibling_handler_id = forward->sibling_handler_id;

  forward->handler_id = 0;
  forward->sibling_handler_id = 0;

  if (sibling_handler_id != 0)
    g_signal_handler_disconnect (impl, sibling_handler_id);
  if (handler_id != 0)
    g_signal_handler_disconnect (impl, handler_id);
}

static void
forward_token_received (XdpDbusImplLanguage *impl,
                        const char          *session_id,
                        const char          *token,
                        gboolean             done,
                        gpointer             user_data)
{
  LanguageSignalForward *forward = user_data;

  if (g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  xdp_dbus_language_emit_token_received (XDP_DBUS_LANGUAGE (forward->language),
                                          forward->session_handle,
                                          token,
                                          done);

  if (done)
    language_signal_forward_disconnect (forward);
}

static void
forward_prediction_received (XdpDbusImplLanguage *impl,
                             const char          *session_id,
                             const char          *completion,
                             gboolean             done,
                             gpointer             user_data)
{
  LanguageSignalForward *forward = user_data;

  if (g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  xdp_dbus_language_emit_prediction_received (XDP_DBUS_LANGUAGE (forward->language),
                                              forward->session_handle,
                                              completion,
                                              done);

  if (done)
    language_signal_forward_disconnect (forward);
}

static void
forward_guided_snapshot_received (XdpDbusImplLanguage *impl,
                                  const char          *session_id,
                                  const char          *snapshot_json,
                                  gboolean             done,
                                  gpointer             user_data)
{
  LanguageSignalForward *forward = user_data;

  if (g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  xdp_dbus_language_emit_guided_snapshot_received (XDP_DBUS_LANGUAGE (forward->language),
                                                    forward->session_handle,
                                                    snapshot_json,
                                                    done);

  if (done)
    language_signal_forward_disconnect (forward);
}

static void
forward_guided_tool_calls_received (XdpDbusImplLanguage *impl,
                                    const char          *session_id,
                                    GVariant            *tool_calls,
                                    gboolean             done,
                                    gpointer             user_data)
{
  LanguageSignalForward *forward = user_data;

  if (g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  xdp_dbus_language_emit_guided_tool_calls_received (XDP_DBUS_LANGUAGE (forward->language),
                                                      forward->session_handle,
                                                      tool_calls,
                                                      done);

  if (done)
    language_signal_forward_disconnect (forward);
}

static void
forward_embedding_received (XdpDbusImplLanguage *impl,
                            const char          *session_id,
                            GVariant            *embedding,
                            gboolean             done,
                            gpointer             user_data)
{
  LanguageSignalForward *forward = user_data;

  if (g_strcmp0 (session_id, forward->backend_session_id) != 0)
    return;

  xdp_dbus_language_emit_embedding_received (XDP_DBUS_LANGUAGE (forward->language),
                                             forward->session_handle,
                                             embedding,
                                             done);

  if (done)
    language_signal_forward_disconnect (forward);
}

static gboolean
handle_language_get_use_case_availability (XdpDbusLanguage      *object,
                                           GDBusMethodInvocation *invocation,
                                           const char            *arg_use_case,
                                           GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(GVariant) availability = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_language_call_get_use_case_availability_sync (language->impl,
                                                                   xdp_app_info_get_id (app_info),
                                                                   arg_use_case,
                                                                   &availability,
                                                                   NULL,
                                                                   &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_language_complete_get_use_case_availability (object, invocation, availability);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_create_session (XdpDbusLanguage      *object,
                                GDBusMethodInvocation *invocation,
                                const char            *arg_use_case,
                                const char            *arg_instructions,
                                GVariant              *arg_options)
{
  Language *language = (Language *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autofree char *backend_session_id = NULL;
  g_autoptr(XdpSession) session = NULL;
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_language_call_create_session_sync (language->impl,
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

  session = XDP_SESSION (model_session_new (language->context,
                                            connection,
                                            app_info,
                                            G_OBJECT (language->impl),
                                            MODEL_SESSION_LANGUAGE,
                                            backend_session_id,
                                            &error));
  if (session == NULL)
    {
      end_backend_session (G_OBJECT (language->impl),
                           MODEL_SESSION_LANGUAGE,
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
  xdp_dbus_language_complete_create_session (object, invocation, session->id);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_prewarm (XdpDbusLanguage      *object,
                         GDBusMethodInvocation *invocation,
                         const char            *arg_session_handle,
                         GVariant              *arg_options)
{
  Language *language = (Language *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GError) error = NULL;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_LANGUAGE);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  xdp_dbus_language_emit_model_loading (object, session->id, "starting model");

  if (!xdp_dbus_impl_language_call_prewarm_sync (language->impl,
                                                 model_session_get_backend_session_id (model_session),
                                                 NULL,
                                                 &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_language_complete_prewarm (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_predict_next (XdpDbusLanguage      *object,
                                     GDBusMethodInvocation *invocation,
                                     const char            *arg_session_handle,
                                     const char            *arg_prefix,
                                     GVariant              *arg_options)
{
  Language *language = (Language *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  LanguageSignalForward *forward;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_LANGUAGE);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = generation_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  xdp_dbus_language_emit_model_loading (object, session->id, "starting model");

  forward = language_signal_forward_new (language,
                                         language->impl,
                                         model_session_get_backend_session_id (model_session),
                                         session->id);
  forward->handler_id = g_signal_connect_data (language->impl,
                                               "prediction-received",
                                               G_CALLBACK (forward_prediction_received),
                                               forward,
                                               language_signal_forward_free,
                                               G_CONNECT_DEFAULT);

  if (!xdp_dbus_impl_language_call_stream_predict_next_sync (language->impl,
                                                             model_session_get_backend_session_id (model_session),
                                                             arg_prefix,
                                                             options,
                                                             NULL,
                                                             &error))
    {
      language_signal_forward_disconnect (forward);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_language_complete_stream_predict_next (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_response (XdpDbusLanguage      *object,
                                 GDBusMethodInvocation *invocation,
                                 const char            *arg_session_handle,
                                 const char            *arg_prompt,
                                 GVariant              *arg_options)
{
  Language *language = (Language *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  LanguageSignalForward *forward;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_LANGUAGE);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = generation_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  xdp_dbus_language_emit_model_loading (object, session->id, "starting model");

  forward = language_signal_forward_new (language,
                                         language->impl,
                                         model_session_get_backend_session_id (model_session),
                                         session->id);
  forward->handler_id = g_signal_connect_data (language->impl,
                                               "token-received",
                                               G_CALLBACK (forward_token_received),
                                               forward,
                                               language_signal_forward_free,
                                               G_CONNECT_DEFAULT);

  if (!xdp_dbus_impl_language_call_stream_response_sync (language->impl,
                                                        model_session_get_backend_session_id (model_session),
                                                        arg_prompt,
                                                        options,
                                                        NULL,
                                                        &error))
    {
      language_signal_forward_disconnect (forward);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_language_complete_stream_response (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_respond_guided (XdpDbusLanguage      *object,
                                       GDBusMethodInvocation *invocation,
                                       const char            *arg_session_handle,
                                       const char            *arg_prompt,
                                       GVariant              *arg_fields,
                                       GVariant              *arg_tools,
                                       GVariant              *arg_options)
{
  Language *language = (Language *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  LanguageSignalForward *forward;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_LANGUAGE);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = generation_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  xdp_dbus_language_emit_model_loading (object, session->id, "starting model");

  forward = language_signal_forward_new (language,
                                         language->impl,
                                         model_session_get_backend_session_id (model_session),
                                         session->id);
  forward->handler_id = g_signal_connect_data (language->impl,
                                               "guided-snapshot-received",
                                               G_CALLBACK (forward_guided_snapshot_received),
                                               forward,
                                               language_signal_forward_free,
                                               G_CONNECT_DEFAULT);
  forward->sibling_handler_id = g_signal_connect (language->impl,
                                                 "guided-tool-calls-received",
                                                 G_CALLBACK (forward_guided_tool_calls_received),
                                                 forward);

  if (!xdp_dbus_impl_language_call_stream_respond_guided_sync (language->impl,
                                                              model_session_get_backend_session_id (model_session),
                                                              arg_prompt,
                                                              arg_fields,
                                                              arg_tools,
                                                              options,
                                                              NULL,
                                                              &error))
    {
      language_signal_forward_disconnect (forward);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_language_complete_stream_respond_guided (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_submit_tool_results_guided (XdpDbusLanguage      *object,
                                                   GDBusMethodInvocation *invocation,
                                                   const char            *arg_session_handle,
                                                   const char            *arg_prompt,
                                                   GVariant              *arg_results,
                                                   GVariant              *arg_fields,
                                                   GVariant              *arg_tools,
                                                   GVariant              *arg_options)
{
  Language *language = (Language *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  LanguageSignalForward *forward;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_LANGUAGE);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  options = generation_options_from_vardict (arg_options, &error);
  if (options == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);
  xdp_dbus_language_emit_model_loading (object, session->id, "starting model");

  forward = language_signal_forward_new (language,
                                         language->impl,
                                         model_session_get_backend_session_id (model_session),
                                         session->id);
  forward->handler_id = g_signal_connect_data (language->impl,
                                               "guided-snapshot-received",
                                               G_CALLBACK (forward_guided_snapshot_received),
                                               forward,
                                               language_signal_forward_free,
                                               G_CONNECT_DEFAULT);
  forward->sibling_handler_id = g_signal_connect (language->impl,
                                                 "guided-tool-calls-received",
                                                 G_CALLBACK (forward_guided_tool_calls_received),
                                                 forward);

  if (!xdp_dbus_impl_language_call_stream_submit_tool_results_guided_sync (language->impl,
                                                                           model_session_get_backend_session_id (model_session),
                                                                           arg_prompt,
                                                                           arg_results,
                                                                           arg_fields,
                                                                           arg_tools,
                                                                           options,
                                                                           NULL,
                                                                           &error))
    {
      language_signal_forward_disconnect (forward);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_language_complete_stream_submit_tool_results_guided (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_language_stream_embed (XdpDbusLanguage      *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_session_handle,
                              const char            *arg_text,
                              GVariant              *arg_options)
{
  Language *language = (Language *) object;
  g_autoptr(XdpSession) session = NULL;
  ModelSession *model_session;
  g_autoptr(GError) error = NULL;
  LanguageSignalForward *forward;

  session = lookup_model_session (invocation, arg_session_handle, MODEL_SESSION_LANGUAGE);
  if (session == NULL)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  SESSION_AUTOLOCK (session);
  model_session = MODEL_SESSION (session);

  forward = language_signal_forward_new (language,
                                         language->impl,
                                         model_session_get_backend_session_id (model_session),
                                         session->id);
  forward->handler_id = g_signal_connect_data (language->impl,
                                               "embedding-received",
                                               G_CALLBACK (forward_embedding_received),
                                               forward,
                                               language_signal_forward_free,
                                               G_CONNECT_DEFAULT);

  if (!xdp_dbus_impl_language_call_stream_embed_sync (language->impl,
                                                      model_session_get_backend_session_id (model_session),
                                                      arg_text,
                                                      NULL,
                                                      &error))
    {
      language_signal_forward_disconnect (forward);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_language_complete_stream_embed (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
language_iface_init (XdpDbusLanguageIface *iface)
{
  iface->handle_get_use_case_availability = handle_language_get_use_case_availability;
  iface->handle_create_session = handle_language_create_session;
  iface->handle_prewarm = handle_language_prewarm;
  iface->handle_stream_response = handle_language_stream_response;
  iface->handle_stream_predict_next = handle_language_stream_predict_next;
  iface->handle_stream_respond_guided = handle_language_stream_respond_guided;
  iface->handle_stream_submit_tool_results_guided = handle_language_stream_submit_tool_results_guided;
  iface->handle_stream_embed = handle_language_stream_embed;
}

static void
language_dispose (GObject *object)
{
  Language *language = (Language *) object;

  g_clear_object (&language->impl);

  G_OBJECT_CLASS (language_parent_class)->dispose (object);
}

static void
language_init (Language *language)
{
}

static void
language_class_init (LanguageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = language_dispose;
}

static Language *
language_new (XdpContext          *context,
              XdpDbusImplLanguage *impl)
{
  Language *language = g_object_new (language_get_type (), NULL);

  language->context = context;
  language->impl = g_object_ref (impl);
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (language->impl), G_MAXINT);
  xdp_dbus_language_set_version (XDP_DBUS_LANGUAGE (language), 2);

  return language;
}

void
init_language (XdpContext *context)
{
  g_autoptr(Language) language = NULL;
  GDBusConnection *connection = xdp_context_get_connection (context);
  XdpPortalConfig *config = xdp_context_get_config (context);
  XdpImplConfig *impl_config;
  XdpDbusImplLanguage *impl = NULL;
  g_autoptr(GError) error = NULL;

  impl_config = xdp_portal_config_find (config, LANGUAGE_DBUS_IMPL_IFACE);
  if (impl_config == NULL)
    return;

  impl = xdp_dbus_impl_language_proxy_new_sync (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                impl_config->dbus_name,
                                                DESKTOP_DBUS_PATH,
                                                NULL,
                                                &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create language proxy: %s", error->message);
      return;
    }

  language = language_new (context, impl);
  g_clear_object (&impl);
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&language)),
                                      XDP_CONTEXT_EXPORT_FLAGS_NONE);
}
