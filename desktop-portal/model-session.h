/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gunixfdlist.h>

#include "xdp-sealed-fd.h"
#include "xdp-session.h"

typedef enum _ModelSessionKind
{
  MODEL_SESSION_LANGUAGE,
  MODEL_SESSION_SPEECH,
  MODEL_SESSION_VISION,
} ModelSessionKind;

typedef struct _ModelSession ModelSession;

GType model_session_get_type (void) G_GNUC_CONST;

#define MODEL_TYPE_SESSION (model_session_get_type ())
#define MODEL_SESSION(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), MODEL_TYPE_SESSION, ModelSession))
#define IS_MODEL_SESSION(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), MODEL_TYPE_SESSION))

ModelSession *model_session_new (XdpContext       *context,
                                 GDBusConnection  *connection,
                                 XdpAppInfo       *app_info,
                                 GObject          *impl,
                                 ModelSessionKind  kind,
                                 const char       *use_case,
                                 GVariant         *options,
                                 GError          **error);

XdpSession *lookup_model_session (GDBusMethodInvocation *invocation,
                                  const char            *session_handle,
                                  ModelSessionKind       kind);

const char *model_app_id_from_invocation (GDBusMethodInvocation *invocation,
                                           XdpAppInfo            *app_info);

gboolean model_use_case_is_supported (ModelSessionKind  kind,
                                      const char       *use_case);

GVariant *model_unsupported_use_case_availability (const char *use_case);

gboolean model_validate_use_case_for_session (GDBusMethodInvocation *invocation,
                                              ModelSessionKind       kind,
                                              const char            *use_case);

gboolean model_session_ensure_language_generation_use_case (GDBusMethodInvocation *invocation,
                                                             ModelSession          *session);

gboolean model_session_ensure_speech_use_case (GDBusMethodInvocation *invocation,
                                               ModelSession          *session,
                                               const char            *method);

gboolean model_session_ensure_exact_use_case (GDBusMethodInvocation *invocation,
                                              ModelSession          *session,
                                              const char            *expected_use_case,
                                              const char            *method);

gboolean model_session_ensure_open (GDBusMethodInvocation *invocation,
                                    ModelSession          *session);

GVariant *generation_options_from_vardict (GVariant  *arg_options,
                                           GError   **error);

gboolean model_session_options_validate (GVariant  *options,
                                         GError   **error);

gboolean model_request_options_validate (GVariant  *options,
                                         GError   **error);

GVariant *model_sealed_fd_to_handle (XdpSealedFd  *sealed_fd,
                                     GUnixFDList  *fd_list,
                                     GError      **error);

gboolean model_request_export_with_impl (XdpRequest      *request,
                                         GDBusConnection *connection,
                                         GDBusProxy      *impl_proxy,
                                         GError         **error);

void model_request_emit_response (XdpRequest  *request,
                                  guint        response,
                                  const char  *error_message);

gboolean model_request_register_session_and_emit_response (XdpRequest *request,
                                                           XdpSession *session);

guint model_response_from_error (GError *error);

gboolean model_request_is_exported (XdpRequest *request);
