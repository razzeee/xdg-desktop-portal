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
        assert args[1] == response.results["session_handle"]
        assert args[2] == xdp_app_info.app_id
        assert args[3] == ""
        assert args[4] == use_case
        assert args[5] == instructions

    @pytest.mark.parametrize(
        "portal,use_case",
        [
            ("Language", "speech.transcribe"),
            ("Speech", "vision.describe"),
            ("Vision", "language.summarize"),
            ("Language", "language.unknown"),
        ],
    )
    def test_get_use_case_availability_rejects_unsupported_tokens(
        self, portals, dbus_con, portal, use_case
    ):
        portal_intf = xdp.get_portal_iface(dbus_con, portal)

        is_available, code, reason = portal_intf.GetUseCaseAvailability(use_case, {})

        assert not is_available
        assert code == "unsupported_use_case"
        assert use_case in reason

    @pytest.mark.parametrize(
        "portal,use_case",
        [
            ("Language", "speech.transcribe"),
            ("Speech", "vision.describe"),
            ("Vision", "language.summarize"),
            ("Language", "language.unknown"),
        ],
    )
    def test_create_session_rejects_unsupported_tokens(
        self, portals, dbus_con, portal, use_case
    ):
        portal_intf = xdp.get_portal_iface(dbus_con, portal)

        with pytest.raises(Exception) as excinfo:
            portal_intf.CreateSession("", use_case, "", {})

        assert "unsupported use-case" in str(excinfo.value)

    @pytest.mark.parametrize("template_params", ({"language": {"expect-close": True}},))
    def test_create_session_close_propagates_to_impl_request_and_session(
        self, portals, dbus_con
    ):
        language_intf = xdp.get_portal_iface(dbus_con, "Language")
        mock_intf = xdp.get_mock_iface(dbus_con)
        session_closed_handles = []

        def cb_impl_session_closed(handle):
            session_closed_handles.append(str(handle))

        signal_match = dbus_con.add_signal_receiver(
            cb_impl_session_closed,
            "SessionClosed",
            dbus_interface="org.freedesktop.impl.portal.Mock",
        )

        request = xdp.Request(dbus_con, language_intf)
        try:
            request.schedule_close(1000)
            request.call(
                "CreateSession",
                parent_window="",
                use_case="language.summarize",
                instructions="Summarize clearly.",
                options={},
            )

            assert request.closed

            method_calls = mock_intf.GetMethodCalls("CreateSession")
            assert len(method_calls) > 0
            _, args = method_calls[-1]
            session_handle = str(args[1])
            xdp.wait_for(lambda: session_handle in session_closed_handles)
        finally:
            signal_match.remove()
