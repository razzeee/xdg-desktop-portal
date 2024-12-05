#!/usr/bin/env python3
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib
from itertools import count
from typing import Any, Dict, Optional, NamedTuple, Callable

import os
import dbus
import dbus.proxies
import dbusmock
import logging


DBusGMainLoop(set_as_default=True)

# Anything that takes longer than 5s needs to fail
MAX_TIMEOUT = 5000

_counter = count()

ASV = Dict[str, Any]

logger = logging.getLogger("tests")


def is_in_ci() -> bool:
    return os.environ.get("XDP_TEST_IN_CI") is not None


def is_in_container() -> bool:
    return is_in_ci() or (
        "container" in os.environ
        and (os.environ["container"] == "docker" or os.environ["container"] == "podman")
    )


def wait(ms: int):
    """
    Waits for the specified amount of milliseconds.
    """
    mainloop = GLib.MainLoop()
    GLib.timeout_add(ms, mainloop.quit)
    mainloop.run()


def wait_for(fn: Callable[[], bool]):
    """
    Waits and dispatches to mainloop until the function fn returns true. This is
    useful in combination with a lambda which captures a variable:

        my_var = False
        def callback():
            my_var = True
        do_something_later(callback)
        xdp.wait_for(lambda: my_var)
    """
    mainloop = GLib.MainLoop()
    while not fn():
        GLib.timeout_add(50, mainloop.quit)
        mainloop.run()


def get_permission_store_iface(bus: dbus.Bus):
    """
    Returns the dbus interface of the xdg-permission-store.
    """
    obj = bus.get_object(
        "org.freedesktop.impl.portal.PermissionStore",
        "/org/freedesktop/impl/portal/PermissionStore",
    )
    return dbus.Interface(obj, "org.freedesktop.impl.portal.PermissionStore")


def get_mock_iface(bus: dbus.Bus):
    """
    Returns the mock interface of the xdg-desktop-portal.
    """
    obj = bus.get_object(
        "org.freedesktop.impl.portal.Test", "/org/freedesktop/portal/desktop"
    )
    return dbus.Interface(obj, dbusmock.MOCK_IFACE)


def portal_interface_name(portal_name) -> str:
    """
    Returns the fully qualified interface for a portal name.
    """
    return f"org.freedesktop.portal.{portal_name}"


def get_portal_iface(bus: dbus.Bus, name: str) -> dbus.Interface:
    """
    Returns the dbus interface for a portal name.
    """
    name = portal_interface_name(name)
    return get_iface(bus, name)


def get_iface(bus: dbus.Bus, name: str) -> dbus.Interface:
    """
    Returns a named interface of the main portal object.
    """
    try:
        ifaces = bus._xdp_portal_ifaces
    except AttributeError:
        ifaces = bus._xdp_portal_ifaces = {}

    try:
        intf = ifaces[name]
    except KeyError:
        intf = dbus.Interface(get_xdp_dbus_object(bus), name)
        assert intf
        ifaces[name] = intf
    return intf


def get_xdp_dbus_object(bus: dbus.Bus) -> dbus.proxies.ProxyObject:
    """
    Returns the main portal object.
    """
    try:
        obj = getattr(bus, "_xdp_dbus_object")
    except AttributeError:
        obj = bus.get_object(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop"
        )
        assert obj
        bus._xdp_dbus_object = obj
    return obj


def check_version(bus: dbus.Bus, portal_name: str, expected_version: int):
    """
    Checks that the portal_name portal version is equal to expected_version.
    """
    properties_intf = dbus.Interface(
        get_xdp_dbus_object(bus), "org.freedesktop.DBus.Properties"
    )
    portal_iface_name = portal_interface_name(portal_name)
    try:
        portal_version = properties_intf.Get(portal_iface_name, "version")
        assert int(portal_version) == expected_version
    except dbus.exceptions.DBusException as e:
        logger.critical(e)
        assert e is None, str(e)


class Response(NamedTuple):
    """
    Response as returned by a completed :class:`Request`
    """

    response: int
    results: ASV


class ResponseTimeout(Exception):
    """
    Exception raised by :meth:`Request.call` if the Request did not receive a
    Response in time.
    """

    pass


class Closable:
    """
    Parent class for both Session and Request. Both of these have a Close()
    method.
    """

    def __init__(self, bus: dbus.Bus, objpath: str):
        self.objpath = objpath
        # GLib makes assertions in callbacks impossible, so we wrap all
        # callbacks into a try: except and store the error on the request to
        # be raised later when we're back in the main context
        self.error: Optional[Exception] = None

        self._mainloop: Optional[GLib.MainLoop] = None
        self._impl_closed = False
        self._bus = bus

        self._closable = type(self).__name__
        assert self._closable in ("Request", "Session")
        proxy = bus.get_object("org.freedesktop.portal.Desktop", objpath)
        self._closable_interface = dbus.Interface(
            proxy, f"org.freedesktop.portal.{self._closable}"
        )

    @property
    def bus(self) -> dbus.Bus:
        return self._bus

    @property
    def closed(self) -> bool:
        """
        True if the impl.portal was closed
        """
        return self._impl_closed

    def close(self) -> None:
        signal_match = None

        def cb_impl_closed_by_portal(handle) -> None:
            if handle == self.objpath:
                logger.debug(f"Impl{self._closable} {self.objpath} was closed")
                signal_match.remove()  # type: ignore
                self._impl_closed = True
                if self.closed and self._mainloop:
                    self._mainloop.quit()

        # See :class:`ImplRequest`, this signal is a side-channel for the
        # impl.portal template to notify us when the impl.Request was really
        # closed by the portal.
        signal_match = self._bus.add_signal_receiver(
            cb_impl_closed_by_portal,
            f"{self._closable}Closed",
            dbus_interface="org.freedesktop.impl.portal.Test",
        )

        logger.debug(f"Closing {self._closable} {self.objpath}")
        self._closable_interface.Close()

    def schedule_close(self, timeout_ms=300):
        """
        Schedule an automatic Close() on the given timeout in milliseconds.
        """
        assert 0 < timeout_ms < MAX_TIMEOUT
        GLib.timeout_add(timeout_ms, self.close)


class Request(Closable):
    """
    Helper class for executing methods that use Requests. This calls takes
    care of subscribing to the signals and invokes the method on the
    interface with the expected behaviors. A typical invocation is:

            >>> response = Request(connection, interface).call("Foo", bar="bar")
            >>> assert response.response == 0

    Requests can only be used once, to call a second method you must
    instantiate a new Request object.
    """

    def __init__(self, bus: dbus.Bus, interface: dbus.Interface):
        def sanitize(name):
            return name.lstrip(":").replace(".", "_")

        sender_token = sanitize(bus.get_unique_name())
        self._handle_token = f"request{next(_counter)}"
        self.handle = f"/org/freedesktop/portal/desktop/request/{sender_token}/{self._handle_token}"
        # The Closable
        super().__init__(bus, self.handle)

        self.interface = interface
        self.response: Optional[Response] = None
        self.used = False
        # GLib makes assertions in callbacks impossible, so we wrap all
        # callbacks into a try: except and store the error on the request to
        # be raised later when we're back in the main context
        self.error: Optional[Exception] = None

        proxy = bus.get_object("org.freedesktop.portal.Desktop", self.handle)
        self.mock_interface = dbus.Interface(proxy, dbusmock.MOCK_IFACE)
        self._proxy = bus.get_object("org.freedesktop.portal.Desktop", self.handle)

        def cb_response(response: int, results: ASV) -> None:
            try:
                logger.debug(f"Response received on {self.handle}")
                assert self.response is None
                self.response = Response(response, results)
                if self._mainloop:
                    self._mainloop.quit()
            except Exception as e:
                self.error = e

        self.request_interface = dbus.Interface(proxy, "org.freedesktop.portal.Request")
        self.request_interface.connect_to_signal("Response", cb_response)

    @property
    def handle_token(self) -> dbus.String:
        """
        Returns the dbus-ready handle_token, ready to be put into the options
        """
        return dbus.String(self._handle_token, variant_level=1)

    def call(self, methodname: str, **kwargs) -> Optional[Response]:
        """
        Semi-synchronously call method ``methodname`` on the interface given
        in the Request's constructor. The kwargs must be specified in the
        order the DBus method takes them but the handle_token is automatically
        filled in.

            >>> response = Request(connection, interface).call("Foo", bar="bar")
            >>> if response.response != 0:
            ...     print("some error occured")

        The DBus call itself is asynchronous (required for signals to work)
        but this method does not return until the Response is received, the
        Request is closed or an error occurs. If the Request is closed, the
        Response is None.

        If the "reply_handler" and "error_handler" keywords are present, those
        callbacks are called just like they would be as dbus.service.ProxyObject.
        """
        assert not self.used
        self.used = True

        # Make sure options exists and has the handle_token set
        try:
            options = kwargs["options"]
        except KeyError:
            options = dbus.Dictionary({}, signature="sv")

        if "handle_token" not in options:
            options["handle_token"] = self.handle_token

        # Anything that takes longer than 5s needs to fail
        self._mainloop = GLib.MainLoop()
        GLib.timeout_add(MAX_TIMEOUT, self._mainloop.quit)

        method = getattr(self.interface, methodname)
        assert method

        reply_handler = kwargs.pop("reply_handler", None)
        error_handler = kwargs.pop("error_handler", None)

        # Handle the normal method reply which returns is the Request object
        # path. We don't exit the mainloop here, we're waiting for either the
        # Response signal on the Request itself or the Close() handling
        def reply_cb(handle):
            try:
                logger.debug(f"Reply to {methodname} with {self.handle}")
                assert handle == self.handle

                if reply_handler:
                    reply_handler(handle)
            except Exception as e:
                self.error = e

        # Handle any exceptions during the actual method call (not the Request
        # handling itself). Can exit the mainloop if that happens
        def error_cb(error):
            try:
                logger.debug(f"Error after {methodname} with {error}")
                if error_handler:
                    error_handler(error)
                self.error = error
            except Exception as e:
                self.error = e
            finally:
                if self._mainloop:
                    self._mainloop.quit()

        # Method is invoked async, otherwise we can't mix and match signals
        # and other calls. It's still sync as seen by the caller in that we
        # have a mainloop that waits for us to finish though.
        method(
            *list(kwargs.values()),
            reply_handler=reply_cb,
            error_handler=error_cb,
        )

        self._mainloop.run()

        if self.error:
            raise self.error
        elif not self.closed and self.response is None:
            raise ResponseTimeout(f"Timed out waiting for response from {methodname}")

        return self.response


class Session(Closable):
    """
    Helper class for a Session created by a portal. This class takes care of
    subscribing to the `Closed` signals. A typical invocation is:

        >>> response = Request(connection, interface).call("CreateSession")
        >>> session = Session.from_response(response)
        # Now run the main loop and do other stuff
        # Check if the session was closed
        >>> if session.closed:
        ...    pass
        # or close the session explicitly
        >>> session.close()  # to close the session or
    """

    def __init__(self, bus: dbus.Bus, handle: str):
        assert handle
        super().__init__(bus, handle)

        self.handle = handle
        self.details = None
        # GLib makes assertions in callbacks impossible, so we wrap all
        # callbacks into a try: except and store the error on the request to
        # be raised later when we're back in the main context
        self.error = None
        self._closed_sig_received = False

        def cb_closed(details: ASV) -> None:
            try:
                logger.debug(f"Session.Closed received on {self.handle}")
                assert not self._closed_sig_received
                self._closed_sig_received = True
                self.details = details
                if self._mainloop:
                    self._mainloop.quit()
            except Exception as e:
                self.error = e

        proxy = bus.get_object("org.freedesktop.portal.Desktop", handle)
        self.session_interface = dbus.Interface(proxy, "org.freedesktop.portal.Session")
        self.session_interface.connect_to_signal("Closed", cb_closed)

    @property
    def closed(self):
        """
        Returns True if the session was closed by the backend
        """
        return self._closed_sig_received or super().closed

    @classmethod
    def from_response(cls, bus: dbus.Bus, response: Response) -> "Session":
        return cls(bus, response.results["session_handle"])
