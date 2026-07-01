/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "model-session.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
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
  char *use_case;
};

struct _ModelSessionClass
{
  XdpSessionClass parent_class;
};

G_DEFINE_TYPE (ModelSession, model_session, xdp_session_get_type ())

static XdpOptionKey generation_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "maximum_response_tokens", G_VARIANT_TYPE_INT64, NULL },
  { "source_language_hint", G_VARIANT_TYPE_STRING, NULL },
  { "target_language_hint", G_VARIANT_TYPE_STRING, NULL },
};

static XdpOptionKey create_session_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
  { "session_handle_token", G_VARIANT_TYPE_STRING, NULL },
};

static XdpOptionKey request_options[] = {
  { "handle_token", G_VARIANT_TYPE_STRING, NULL },
};

static void
model_session_close (XdpSession *session)
{
  /* The shared XdpSession close path calls org.freedesktop.impl.portal.Session.Close. */
}

static void
model_session_finalize (GObject *object)
{
  ModelSession *session = MODEL_SESSION (object);

  g_clear_object (&session->impl);
  g_clear_pointer (&session->use_case, g_free);

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
                   const char       *use_case,
                   GVariant         *options,
                   GError          **error)
{
  g_autofree char *generated_token = NULL;
  GDBusConnection *impl_connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));
  const char *impl_dbus_name = g_dbus_proxy_get_name (G_DBUS_PROXY (impl));
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
                            "impl-connection", impl_connection,
                            "impl-dbus-name", impl_dbus_name,
                            NULL);
  if (session == NULL)
    return NULL;

  model_session = MODEL_SESSION (session);
  model_session->kind = kind;
  model_session->impl = g_object_ref (impl);
  model_session->use_case = g_strdup (use_case);

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

gboolean
model_use_case_is_supported (ModelSessionKind  kind,
                             const char       *use_case)
{
  switch (kind)
    {
    case MODEL_SESSION_LANGUAGE:
      return g_strcmp0 (use_case, "language.summarize") == 0 ||
             g_strcmp0 (use_case, "language.translate") == 0 ||
             g_strcmp0 (use_case, "language.rephrase") == 0 ||
             g_strcmp0 (use_case, "language.complete") == 0 ||
             g_strcmp0 (use_case, "language.classify") == 0 ||
             g_strcmp0 (use_case, "language.extract") == 0 ||
             g_strcmp0 (use_case, "language.analyze") == 0 ||
             g_strcmp0 (use_case, "language.embed") == 0;
    case MODEL_SESSION_SPEECH:
      return g_strcmp0 (use_case, "speech.transcribe") == 0 ||
             g_strcmp0 (use_case, "speech.translate") == 0;
    case MODEL_SESSION_VISION:
      return g_strcmp0 (use_case, "vision.describe") == 0 ||
             g_strcmp0 (use_case, "vision.ocr") == 0 ||
             g_strcmp0 (use_case, "vision.segment") == 0;
    }

  g_assert_not_reached ();
}

GVariant *
model_unsupported_use_case_availability (const char *use_case)
{
  g_autofree char *reason = g_strdup_printf ("unsupported use-case: %s", use_case);

  return g_variant_new ("(bss)",
                        FALSE,
                        "unsupported_use_case",
                        reason);
}

gboolean
model_validate_use_case_for_session (GDBusMethodInvocation *invocation,
                                     ModelSessionKind       kind,
                                     const char            *use_case)
{
  if (model_use_case_is_supported (kind, use_case))
    return TRUE;

  g_dbus_method_invocation_return_error (invocation,
                                         XDG_DESKTOP_PORTAL_ERROR,
                                         XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                         "aileron.Inference.InvalidInput: unsupported use-case: %s",
                                         use_case);
  return FALSE;
}

gboolean
model_session_ensure_language_generation_use_case (GDBusMethodInvocation *invocation,
                                                    ModelSession          *session)
{
  const char *use_case = session->use_case;

  if (g_strcmp0 (use_case, "language.summarize") == 0 ||
      g_strcmp0 (use_case, "language.translate") == 0 ||
      g_strcmp0 (use_case, "language.rephrase") == 0 ||
      g_strcmp0 (use_case, "language.classify") == 0 ||
      g_strcmp0 (use_case, "language.extract") == 0 ||
      g_strcmp0 (use_case, "language.analyze") == 0)
    return TRUE;

  g_dbus_method_invocation_return_error (invocation,
                                         XDG_DESKTOP_PORTAL_ERROR,
                                         XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                         "aileron.Inference.InvalidInput: full text generation requires a language generation use-case, got %s",
                                         use_case);
  return FALSE;
}

gboolean
model_session_ensure_speech_use_case (GDBusMethodInvocation *invocation,
                                      ModelSession          *session,
                                      const char            *method)
{
  const char *use_case = session->use_case;

  if (g_strcmp0 (use_case, "speech.transcribe") == 0 ||
      g_strcmp0 (use_case, "speech.translate") == 0)
    return TRUE;

  g_dbus_method_invocation_return_error (invocation,
                                         XDG_DESKTOP_PORTAL_ERROR,
                                         XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                         "aileron.Inference.InvalidInput: %s requires use-case speech.transcribe or speech.translate, got %s",
                                         method,
                                         use_case);
  return FALSE;
}

gboolean
model_session_ensure_exact_use_case (GDBusMethodInvocation *invocation,
                                     ModelSession          *session,
                                     const char            *expected_use_case,
                                     const char            *method)
{
  if (g_strcmp0 (session->use_case, expected_use_case) == 0)
    return TRUE;

  g_dbus_method_invocation_return_error (invocation,
                                         XDG_DESKTOP_PORTAL_ERROR,
                                         XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                         "aileron.Inference.InvalidInput: %s requires use-case %s, got %s",
                                         method,
                                         expected_use_case,
                                         session->use_case);
  return FALSE;
}

gboolean
model_session_ensure_open (GDBusMethodInvocation *invocation,
                           ModelSession          *session)
{
  XdpSession *xdp_session = XDP_SESSION (session);

  if (!xdp_session->closed)
    return TRUE;

  g_dbus_method_invocation_return_error (invocation,
                                         G_DBUS_ERROR,
                                         G_DBUS_ERROR_ACCESS_DENIED,
                                         "Invalid session");
  return FALSE;
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

gboolean
model_session_options_validate (GVariant  *options,
                                GError   **error)
{
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  const char *token;

  if (!xdp_filter_options (options, &options_builder,
                           create_session_options, G_N_ELEMENTS (create_session_options),
                           NULL, error))
    return FALSE;

  token = lookup_session_token (options);
  if (token != NULL && !xdp_is_valid_token (token))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid token '%s'", token);
      return FALSE;
    }

  return TRUE;
}

gboolean
model_request_options_validate (GVariant  *options,
                                GError   **error)
{
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  return xdp_filter_options (options, &options_builder,
                             request_options, G_N_ELEMENTS (request_options),
                             NULL, error);
}

GVariant *
model_sealed_fd_to_handle (XdpSealedFd  *sealed_fd,
                           GUnixFDList  *fd_list,
                           GError      **error)
{
  int fd_out;

  g_return_val_if_fail (XDP_IS_SEALED_FD (sealed_fd), NULL);
  g_return_val_if_fail (G_IS_UNIX_FD_LIST (fd_list), NULL);

  fd_out = g_unix_fd_list_append (fd_list, xdp_sealed_fd_get_fd (sealed_fd), error);
  if (fd_out == -1)
    return NULL;

  return g_variant_ref_sink (g_variant_new_handle (fd_out));
}

gboolean
model_request_export_with_impl (XdpRequest      *request,
                                GDBusConnection *connection,
                                GDBusProxy      *impl_proxy,
                                GError         **error)
{
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (impl_proxy),
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
    g_dbus_proxy_get_name (impl_proxy),
    xdp_request_get_object_path (request),
    NULL,
    error);
  if (impl_request == NULL)
    return FALSE;

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request, connection);
  return TRUE;
}

static char *
model_error_name_from_message (const char *error_message)
{
  const char *aileron_error;
  const char *candidate;

  if (error_message == NULL)
    return NULL;

  aileron_error = strstr (error_message, "aileron.Inference.");
  if (aileron_error != NULL)
    {
      const char *end = aileron_error;

      while (g_ascii_isalnum (*end) || *end == '_' || *end == '.')
        end++;
      return g_strndup (aileron_error, end - aileron_error);
    }

  for (candidate = error_message; *candidate != '\0'; candidate++)
    {
      const char *end = candidate;
      guint components = 0;

      if (!g_ascii_isalpha (*end) && *end != '_')
        continue;

      while (g_ascii_isalpha (*end) || *end == '_')
        {
          end++;
          while (g_ascii_isalnum (*end) || *end == '_')
            end++;

          components++;
          if (*end != '.')
            break;

          end++;
        }

      if (components >= 3)
        return g_strndup (candidate, end - candidate);
    }

  return NULL;
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

gboolean
model_request_register_session_and_emit_response (XdpRequest *request,
                                                  XdpSession *session)
{
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

  REQUEST_AUTOLOCK (request);

  if (!request->exported)
    return FALSE;

  xdp_session_register (session);
  g_variant_builder_add (&results_builder, "{sv}", "session_handle",
                         g_variant_new_object_path (session->id));
  xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                   0,
                                   g_variant_builder_end (&results_builder));
  xdp_request_unexport (request);
  return TRUE;
}

guint
model_response_from_error (GError *error)
{
  g_autofree char *error_name = NULL;
  g_autofree char *remote_error = NULL;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      g_error_matches (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_CANCELLED))
    return 1;

  error_name = model_error_name_from_message (error->message);
  if (g_strcmp0 (error_name, "aileron.Inference.RequestCancelled") == 0)
    return 1;

  remote_error = g_dbus_error_get_remote_error (error);
  if (g_strcmp0 (remote_error, "org.freedesktop.portal.Error.Cancelled") == 0)
    return 1;

  return 2;
}

gboolean
model_request_is_exported (XdpRequest *request)
{
  REQUEST_AUTOLOCK (request);

  return request->exported;
}
