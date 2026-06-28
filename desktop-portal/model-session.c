/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "model-session.h"

#include <gio/gio.h>
#include <string.h>

#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-impl-dbus.h"
#include "xdp-session.h"
#include "xdp-utils.h"

typedef struct _ModelSessionClass ModelSessionClass;

struct _ModelSession
{
  XdpSession parent;

  ModelSessionKind kind;
  GObject *impl;
  char *backend_session_id;
};

struct _ModelSessionClass
{
  XdpSessionClass parent_class;
};

G_DEFINE_TYPE (ModelSession, model_session, xdp_session_get_type ())

static XdpOptionKey generation_options[] = {
  { "maximum_response_tokens", G_VARIANT_TYPE_INT64, NULL },
  { "temperature", G_VARIANT_TYPE_DOUBLE, NULL },
  { "sampling_mode", G_VARIANT_TYPE_STRING, NULL },
  { "source_language_hint", G_VARIANT_TYPE_STRING, NULL },
  { "target_language_hint", G_VARIANT_TYPE_STRING, NULL },
};

void
end_backend_session (GObject          *impl,
                     ModelSessionKind  kind,
                     const char       *backend_session_id)
{
  g_autoptr(GError) error = NULL;

  if (backend_session_id == NULL || impl == NULL)
    return;

  switch (kind)
    {
    case MODEL_SESSION_LANGUAGE:
      if (!xdp_dbus_impl_language_call_end_session_sync (XDP_DBUS_IMPL_LANGUAGE (impl),
                                                         backend_session_id,
                                                         NULL,
                                                         &error))
        g_warning ("Failed to close language model session: %s", error->message);
      break;

    case MODEL_SESSION_SPEECH:
      if (!xdp_dbus_impl_speech_call_end_session_sync (XDP_DBUS_IMPL_SPEECH (impl),
                                                       backend_session_id,
                                                       NULL,
                                                       &error))
        g_warning ("Failed to close speech model session: %s", error->message);
      break;

    case MODEL_SESSION_VISION:
      if (!xdp_dbus_impl_vision_call_end_session_sync (XDP_DBUS_IMPL_VISION (impl),
                                                       backend_session_id,
                                                       NULL,
                                                       &error))
        g_warning ("Failed to close vision model session: %s", error->message);
      break;
    }
}

static void
model_session_close (XdpSession *session)
{
  ModelSession *model_session = MODEL_SESSION (session);

  end_backend_session (model_session->impl,
                       model_session->kind,
                       model_session->backend_session_id);

  g_clear_pointer (&model_session->backend_session_id, g_free);
}

static void
model_session_finalize (GObject *object)
{
  ModelSession *session = MODEL_SESSION (object);

  g_clear_object (&session->impl);
  g_clear_pointer (&session->backend_session_id, g_free);

  G_OBJECT_CLASS (model_session_parent_class)->finalize (object);
}

static void
model_session_init (ModelSession *session)
{
}

static void
model_session_class_init (ModelSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  XdpSessionClass *session_class = (XdpSessionClass *) klass;

  object_class->finalize = model_session_finalize;
  session_class->close = model_session_close;
}

ModelSession *
model_session_new (XdpContext       *context,
                   GDBusConnection  *connection,
                   XdpAppInfo       *app_info,
                   GObject          *impl,
                   ModelSessionKind  kind,
                   GVariant         *options,
                   const char       *backend_session_id,
                   GError          **error)
{
  g_autofree char *generated_token = NULL;
  const char *token;
  XdpSession *session;
  ModelSession *model_session;

  token = lookup_session_token (options);
  if (token == NULL)
    {
      generated_token = xdp_generate_token ();
      token = generated_token;
    }

  session = g_initable_new (MODEL_TYPE_SESSION, NULL, error,
                            "context", context,
                            "sender", xdp_app_info_get_sender (app_info),
                            "app-id", xdp_app_info_get_id (app_info),
                            "token", token,
                            "connection", connection,
                            NULL);
  if (session == NULL)
    return NULL;

  model_session = MODEL_SESSION (session);
  model_session->kind = kind;
  model_session->impl = g_object_ref (impl);
  model_session->backend_session_id = g_strdup (backend_session_id);

  return model_session;
}

XdpSession *
lookup_model_session (GDBusMethodInvocation *invocation,
                      const char            *session_handle,
                      ModelSessionKind       kind)
{
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr(XdpSession) session = NULL;

  session = xdp_session_from_app_info (session_handle, app_info);
  if (session == NULL || !IS_MODEL_SESSION (session) || MODEL_SESSION (session)->kind != kind)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return NULL;
    }

  return g_steal_pointer (&session);
}

const char *
model_app_id_from_invocation (GDBusMethodInvocation *invocation,
                              XdpAppInfo            *app_info)
{
  const char *app_id = xdp_app_info_get_id (app_info);

  if (app_id == NULL || app_id[0] == '\0')
    return g_dbus_method_invocation_get_sender (invocation);

  return app_id;
}

const char *
model_session_get_backend_session_id (ModelSession *session)
{
  return session->backend_session_id;
}

GVariant *
generation_options_from_vardict (GVariant  *arg_options,
                                 GError   **error)
{
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = NULL;
  gint64 maximum_response_tokens = 512;
  double temperature = 0.7;
  const char *sampling_mode = "default";
  const char *source_language_hint = "";
  const char *target_language_hint = "";

  if (!xdp_filter_options (arg_options, &options_builder,
                           generation_options, G_N_ELEMENTS (generation_options),
                           NULL, error))
    return NULL;

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));
  g_variant_lookup (options, "maximum_response_tokens", "x", &maximum_response_tokens);
  g_variant_lookup (options, "temperature", "d", &temperature);
  g_variant_lookup (options, "sampling_mode", "&s", &sampling_mode);
  g_variant_lookup (options, "source_language_hint", "&s", &source_language_hint);
  g_variant_lookup (options, "target_language_hint", "&s", &target_language_hint);

  return g_variant_ref_sink (g_variant_new ("(xdsss)",
                                            maximum_response_tokens,
                                            temperature,
                                            sampling_mode,
                                             source_language_hint,
                                             target_language_hint));
}

static char *
model_error_name_from_message (const char *error_message)
{
  const char *start;
  const char *end;

  if (error_message == NULL)
    return NULL;

  start = strstr (error_message, "aileron.Inference.");
  if (start == NULL)
    return NULL;

  end = start;
  while (g_ascii_isalnum (*end) || *end == '_' || *end == '.')
    end++;

  return g_strndup (start, end - start);
}

void
model_request_emit_response (XdpRequest  *request,
                             guint        response,
                             const char  *error_message)
{
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  if (!request->exported)
    return;

  if (error_message != NULL)
    {
      g_autofree char *error_name = model_error_name_from_message (error_message);

      if (error_name != NULL)
        g_variant_builder_add (&results_builder, "{sv}", "error_name",
                               g_variant_new_string (error_name));
      g_variant_builder_add (&results_builder, "{sv}", "error",
                             g_variant_new_string (error_message));
    }

  xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                  response,
                                  g_variant_builder_end (&results_builder));
  xdp_request_unexport (request);
}

void
model_request_emit_session_response (XdpRequest  *request,
                                     const char  *session_handle)
{
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  if (!request->exported)
    return;

  g_variant_builder_add (&results_builder, "{sv}", "session_handle",
                         g_variant_new_object_path (session_handle));
  xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                  0,
                                  g_variant_builder_end (&results_builder));
  xdp_request_unexport (request);
}

gboolean
model_request_is_exported (XdpRequest *request)
{
  REQUEST_AUTOLOCK (request);

  return request->exported;
}
