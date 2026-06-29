# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import pytest


@pytest.fixture
def required_templates():
    return {
        "language": {},
        "speech": {},
        "vision": {},
    }


class TestModelPortals:
    @pytest.mark.parametrize(
        "portal,use_case,instructions",
        [
            ("Language", "language.summarize", "Summarize clearly."),
            ("Speech", "speech.transcribe", "Transcribe accurately."),
            ("Vision", "vision.describe", "Describe clearly."),
        ],
    )
    def test_create_session_forwards_request_id(
        self, portals, dbus_con, xdp_app_info, portal, use_case, instructions
    ):
        portal_intf = xdp.get_portal_iface(dbus_con, portal)
        mock_intf = xdp.get_mock_iface(dbus_con)

        request = xdp.Request(dbus_con, portal_intf)
        response = request.call(
            "CreateSession",
            parent_window="",
            use_case=use_case,
            instructions=instructions,
            options={"session_handle_token": "session_token0"},
        )

        assert response
        assert response.response == 0
        assert response.results["session_handle"]

        method_calls = mock_intf.GetMethodCalls("CreateSession")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[0] == request.handle
        assert args[1] == xdp_app_info.app_id
        assert args[2] == ""
        assert args[3] == use_case
        assert args[4] == instructions

    @pytest.mark.parametrize("template_params", ({"language": {"expect-close": True}},))
    def test_create_session_close_propagates_to_impl_request(self, portals, dbus_con):
        language_intf = xdp.get_portal_iface(dbus_con, "Language")

        request = xdp.Request(dbus_con, language_intf)
        request.schedule_close(1000)
        request.call(
            "CreateSession",
            parent_window="",
            use_case="language.summarize",
            instructions="Summarize clearly.",
            options={},
        )

        assert request.closed
