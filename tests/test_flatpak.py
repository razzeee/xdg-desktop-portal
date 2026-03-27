# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
from gi.repository import GLib


@pytest.fixture
def required_templates():
    return {}


@pytest.fixture
def xdp_mocked_executables(xdp_app_info: xdp.AppInfo) -> list[xdp.ExecutableMock]:
    if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
        return []

    class FlatpakMock(xdp.ExecutableMock):
        def get_executable(self):
            # Mock flatpak commands
            return f"""#!/usr/bin/env sh
if [ "$1" = "install" ] || [ "$1" = "uninstall" ] || [ "$1" = "update" ]; then
    echo "Mocked flatpak $1 $@" >&2
    exit 0
fi
if [ "$1" = "list" ]; then
    echo "{xdp_app_info.app_id}.Extension1\tExtension One\t1.0\tA first extension"
    echo "{xdp_app_info.app_id}.Extension2\tExtension Two\t2.1\tA second extension"
    echo "org.other.App.Extension\tOther Extension\t0.1\tShould be filtered"
    exit 0
fi
exit 1
""".encode("utf-8")

    return [
        FlatpakMock(
            executable="flatpak",
            access_mode=xdp.FileAccessMode.HIDDEN,
        )
    ]


class TestFlatpak:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "FlatpakInstaller", 1)

    def _test_extension_op(self, method, dbus_con, app_id):
        flatpak_intf = xdp.get_portal_iface(dbus_con, "FlatpakInstaller")
        extensions = [f"{app_id}.Extension1", f"{app_id}.Extension2"]
        options = {"handle_token": f"test_token_{method}"}

        progress_info = []

        def on_progress(info):
            progress_info.append(info)
            if info.get("status") != 0:  # Not Running
                loop.quit()

        loop = GLib.MainLoop()

        handle = getattr(flatpak_intf, method)(extensions, options)
        assert (
            handle
            == f"/org/freedesktop/portal/FlatpakInstaller/install_monitor/{dbus_con.get_unique_name().lstrip(':').replace('.', '_')}/test_token_{method}"
        )

        monitor_obj = dbus_con.get_object("org.freedesktop.portal.Desktop", handle)
        monitor_intf = dbus.Interface(
            monitor_obj, "org.freedesktop.portal.FlatpakInstaller.InstallMonitor"
        )
        monitor_intf.connect_to_signal("Progress", on_progress)

        # In the real implementation, the subprocess runs asynchronously.
        # We wait for the finished status.
        GLib.timeout_add(2000, loop.quit)
        loop.run()

        assert len(progress_info) >= 1
        assert progress_info[-1]["status"] == 1  # Done
        assert progress_info[-1]["progress"] == 100

        monitor_intf.Close()

    def test_install_extensions(self, portals, dbus_con, xdp_app_info):
        if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
            pytest.skip("This test requires a Flatpak app info")
        self._test_extension_op("InstallExtensions", dbus_con, xdp_app_info.app_id)

    def test_uninstall_extensions(self, portals, dbus_con, xdp_app_info):
        if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
            pytest.skip("This test requires a Flatpak app info")
        self._test_extension_op("UninstallExtensions", dbus_con, xdp_app_info.app_id)

    def test_update_extensions(self, portals, dbus_con, xdp_app_info):
        if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
            pytest.skip("This test requires a Flatpak app info")
        self._test_extension_op("UpdateExtensions", dbus_con, xdp_app_info.app_id)

    def test_list_extensions(self, portals, dbus_con, xdp_app_info):
        if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
            pytest.skip("This test requires a Flatpak app info")

        flatpak_intf = xdp.get_portal_iface(dbus_con, "FlatpakInstaller")
        extensions = flatpak_intf.ListExtensions({})

        assert len(extensions) == 2
        assert extensions[0]["id"] == f"{xdp_app_info.app_id}.Extension1"
        assert extensions[0]["name"] == "Extension One"
        assert extensions[1]["id"] == f"{xdp_app_info.app_id}.Extension2"
        assert extensions[1]["summary"] == "A second extension"

    def test_install_extensions_invalid_id(self, portals, dbus_con, xdp_app_info):
        if xdp_app_info.kind != xdp.AppInfoKind.FLATPAK:
            pytest.skip("This test requires a Flatpak app info")

        flatpak_intf = xdp.get_portal_iface(dbus_con, "FlatpakInstaller")
        # Matches prefix but not followed by dot
        extensions = [f"{xdp_app_info.app_id}Suffix"]
        options: dict[str, str] = {}

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
        flatpak_intf = xdp.get_portal_iface(dbus_con, "FlatpakInstaller")

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            flatpak_intf.InstallExtensions(["some.extension"], {})

        assert (
            excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
        )
