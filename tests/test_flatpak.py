# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
from gi.repository import GLib
import os
from pathlib import Path


@pytest.fixture
def required_templates():
    return {}


@pytest.fixture
def xdp_mocked_executables(xdp_app_info: xdp.AppInfo) -> list[xdp.ExecutableMock]:
    if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
        return []

    class FlatpakMock(xdp.ExecutableMock):
        def get_executable(self):
            # Mock flatpak install to succeed
            return b"""#!/usr/bin/env sh
if [ "$1" = "install" ]; then
    echo "Mocked flatpak install $@" >&2
    exit 0
fi
exit 1
"""

    return [
        FlatpakMock(
            executable="flatpak",
            access_mode=xdp.FileAccessMode.HIDDEN,
        )
    ]


class TestFlatpak:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Flatpak", 1)

    def test_install_extensions(self, portals, dbus_con, xdp_app_info):
        if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
            pytest.skip("This test requires a Flatpak app info")

        flatpak_intf = xdp.get_portal_iface(dbus_con, "Flatpak")
        app_id = xdp_app_info.app_id
        extensions = [f"{app_id}.Extension1", f"{app_id}.Extension2"]
        options = {"handle_token": "test_token"}

        progress_info = []

        def on_progress(info):
            progress_info.append(info)
            if info.get("status") != 0:  # Not Running
                loop.quit()

        loop = GLib.MainLoop()

        handle = flatpak_intf.InstallExtensions(extensions, options)
        assert (
            handle
            == f"/org/freedesktop/portal/Flatpak/install_monitor/{dbus_con.get_unique_name().lstrip(':').replace('.', '_')}/test_token"
        )

        monitor_obj = dbus_con.get_object("org.freedesktop.portal.Desktop", handle)
        monitor_intf = dbus.Interface(
            monitor_obj, "org.freedesktop.portal.Flatpak.InstallMonitor"
        )
        monitor_intf.connect_to_signal("Progress", on_progress)

        # In the real implementation, the subprocess runs asynchronously.
        # We wait for the finished status.
        GLib.timeout_add(2000, loop.quit)
        loop.run()

        assert len(progress_info) >= 1
        # We expect at least the "Done" signal, and possibly a "Running" signal before it.
        assert progress_info[-1]["status"] == 1  # Done
        assert progress_info[-1]["progress"] == 100

        monitor_intf.Close()

    def test_install_extensions_invalid_id(self, portals, dbus_con, xdp_app_info):
        if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
            pytest.skip("This test requires a Flatpak app info")

        flatpak_intf = xdp.get_portal_iface(dbus_con, "Flatpak")
        # Matches prefix but not followed by dot
        extensions = [f"{xdp_app_info.app_id}Suffix"]
        options = {}

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            flatpak_intf.InstallExtensions(extensions, options)

        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

        # Does not match prefix at all
        extensions = ["com.other.App.Extension"]
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            flatpak_intf.InstallExtensions(extensions, options)

        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

    def test_install_extensions_host_forbidden(self, portals, dbus_con):
        # The xdp_app_info fixture will iterate, but we can also use it as host
        flatpak_intf = xdp.get_portal_iface(dbus_con, "Flatpak")

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            flatpak_intf.InstallExtensions(["some.extension"], {})

        assert excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
