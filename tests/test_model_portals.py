# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest


@pytest.fixture
def required_templates():
    return {
        "language": {},
        "speech": {},
        "vision": {},
    }


class TestModelPortals:
    def create_speech_session(self, dbus_con, use_case):
        speech_intf = xdp.get_portal_iface(dbus_con, "Speech")
        response = xdp.Request(dbus_con, speech_intf).call(
            "CreateSession",
            parent_window="",
            use_case=use_case,
            instructions="",
            options={},
        )
        assert response and response.response == 0
        return speech_intf, response.results["session_handle"]

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

    def test_speech_version_is_two(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Speech", 2)

    def test_stream_synthesize_forwards_options_and_terminal_audio(
        self, portals, dbus_con
    ):
        speech_intf, session_handle = self.create_speech_session(
            dbus_con, "speech.synthesize"
        )
        mock_intf = xdp.get_mock_iface(dbus_con)
        received = []

        def audio_received(
            request_handle,
            signal_session_handle,
            audio,
            sample_rate,
            channels,
            sample_format,
            done,
        ):
            received.append(
                (
                    str(request_handle),
                    str(signal_session_handle),
                    bytes(audio),
                    int(sample_rate),
                    int(channels),
                    str(sample_format),
                    bool(done),
                )
            )

        signal_match = dbus_con.add_signal_receiver(
            audio_received,
            "AudioReceived",
            dbus_interface="org.freedesktop.portal.Speech",
        )
        request = xdp.Request(dbus_con, speech_intf)
        try:
            response = request.call(
                "StreamSynthesize",
                session_handle=session_handle,
                text="Hello.",
                options={
                    "voice_id": dbus.String("default", variant_level=1),
                    "language_hint": dbus.String("en", variant_level=1),
                    "execution_mode": dbus.String("interactive", variant_level=1),
                },
            )
        finally:
            signal_match.remove()

        assert response and response.response == 0
        assert [chunk[2] for chunk in received] == [
            b"\x01\x00\x02\x00",
            b"\x03\x00\x04\x00",
            b"",
        ]
        assert [chunk[6] for chunk in received] == [False, False, True]
        assert all(chunk[0] == request.handle for chunk in received)
        assert all(chunk[1] == str(session_handle) for chunk in received)
        assert all(chunk[3:6] == (24000, 1, "s16le") for chunk in received)

        method_calls = mock_intf.GetMethodCalls("StreamSynthesize")
        assert len(method_calls) == 1
        _, args = method_calls[0]
        assert args[0] == request.handle
        assert args[1] == session_handle
        assert args[2] == "Hello."
        assert tuple(args[3]) == ("default", "en", "interactive")

    def test_stream_synthesize_rejects_transcription_session(
        self, portals, dbus_con
    ):
        speech_intf, session_handle = self.create_speech_session(
            dbus_con, "speech.transcribe"
        )

        with pytest.raises(Exception) as excinfo:
            xdp.Request(dbus_con, speech_intf).call(
                "StreamSynthesize",
                session_handle=session_handle,
                text="Hello.",
                options={},
            )

        assert "requires use-case speech.synthesize" in str(excinfo.value)

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
