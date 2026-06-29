# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

from tests.templates.xdp_utils import Response, init_logger, ImplRequest

import dbus.service
from dataclasses import dataclass


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Language"


logger = init_logger(__name__)


@dataclass
class LanguageParameters:
    delay: int
    session_id: str
    expect_close: bool


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    assert not hasattr(mock, "language_params")
    mock.language_params = LanguageParameters(
        delay=parameters.get("delay", 200),
        session_id=parameters.get("session-id", "language-session-1"),
        expect_close=parameters.get("expect-close", False),
    )


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="(bss)")
def GetUseCaseAvailability(self, app_id, use_case):
    logger.debug(f"GetUseCaseAvailability({app_id}, {use_case})")
    return (True, "available", "available")


@dbus.service.method(
    MAIN_IFACE,
    in_signature="sssss",
    out_signature="s",
    async_callbacks=("cb_success", "cb_error"),
)
def CreateSession(
    self,
    request_id,
    app_id,
    parent_window,
    use_case,
    instructions,
    cb_success,
    cb_error,
):
    logger.debug(
        f"CreateSession({request_id}, {app_id}, {parent_window}, {use_case}, {instructions})"
    )
    params = self.language_params
    request = ImplRequest(
        self,
        BUS_NAME,
        request_id,
        logger,
        lambda response, results: cb_success(params.session_id),
        cb_error,
    )

    if params.expect_close:
        request.wait_for_close()
    else:
        request.respond(Response(0, {}), delay=params.delay)


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ss",
    out_signature="",
    async_callbacks=("cb_success", "cb_error"),
)
def Prewarm(self, request_id, session_id, cb_success, cb_error):
    request = ImplRequest(
        self, BUS_NAME, request_id, logger, lambda response, results: cb_success(), cb_error
    )
    request.respond(Response(0, {}), delay=self.language_params.delay)


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="")
def EndSession(self, session_id):
    logger.debug(f"EndSession({session_id})")
