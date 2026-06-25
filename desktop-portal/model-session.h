/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

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
                                 const char       *backend_session_id,
                                 GVariant         *options,
                                 GError          **error);

XdpSession *lookup_model_session (GDBusMethodInvocation *invocation,
                                  const char            *session_handle,
                                  ModelSessionKind       kind);

const char *model_session_get_backend_session_id (ModelSession *session);

GVariant *generation_options_from_vardict (GVariant  *arg_options,
                                           GError   **error);

gboolean create_session_options_from_vardict (GVariant  *arg_options,
                                              GVariant **out_options,
                                              GError   **error);

void end_backend_session (GObject          *impl,
                          ModelSessionKind  kind,
                          const char       *backend_session_id);
