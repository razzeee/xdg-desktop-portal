# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates.xdp_utils import Response, init_logger, ImplRequest, ImplSession

import dbus.service
from dataclasses import dataclass


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Speech"


logger = init_logger(__name__)


@dataclass
class SpeechParameters:
    delay: int
    session_id: str


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "speech_params")
    mock.speech_params = SpeechParameters(
        delay=parameters.get("delay", 200),
        session_id=parameters.get("session-id", "speech-session-1"),
    )
    mock.speech_sessions = {}


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="(bss)")
def GetUseCaseAvailability(self, app_id, use_case):
    logger.debug(f"GetUseCaseAvailability({app_id}, {use_case})")
    return (True, "available", "available")


@dbus.service.method(
    MAIN_IFACE,
    in_signature="oossss",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def CreateSession(
    self,
    request_id,
    session_handle,
    app_id,
    parent_window,
    use_case,
    instructions,
    cb_success,
    cb_error,
):
    logger.debug(
        f"CreateSession({request_id}, {session_handle}, {app_id}, {parent_window}, {use_case}, {instructions})"
    )
    params = self.speech_params
    self.speech_sessions[session_handle] = ImplSession(
        self, BUS_NAME, session_handle, app_id
    ).export(lambda: self.speech_sessions.pop(session_handle, None))
    request = ImplRequest(
        self,
        BUS_NAME,
        request_id,
        logger,
        lambda response, results: cb_success(),
        cb_error,
    )
    request.respond(Response(0, {}), delay=params.delay)
