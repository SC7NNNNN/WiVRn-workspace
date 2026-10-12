/*
 * WiVRn VR streaming
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "desktop_portal.h"

#include "util/u_logging.h"
#include "utils/xdg_base_directory.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string_view>
#include <systemd/sd-bus.h>

namespace wivrn
{

namespace
{
constexpr const char * portal_bus_name = "org.freedesktop.portal.Desktop";
constexpr const char * portal_object_path = "/org/freedesktop/portal/desktop";
constexpr const char * screencast_interface = "org.freedesktop.portal.ScreenCast";
constexpr const char * remote_desktop_interface = "org.freedesktop.portal.RemoteDesktop";

// ScreenCast source types and cursor modes
constexpr uint32_t source_monitor = 1;
constexpr uint32_t cursor_embedded = 2;
// RemoteDesktop device types
constexpr uint32_t device_keyboard = 1;
constexpr uint32_t device_pointer = 2;
// Keep the permission until it is explicitly revoked
constexpr uint32_t persist_until_revoked = 2;

// Response codes of org.freedesktop.portal.Request
constexpr uint32_t response_success = 0;
constexpr uint32_t response_cancelled = 1;

void check(int r, const char * what)
{
	if (r < 0)
		throw std::runtime_error(std::string(what) + ": " + std::strerror(-r));
}

struct bus_error
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	~bus_error()
	{
		sd_bus_error_free(&error);
	}
	std::string message() const
	{
		return error.message ? error.message : (error.name ? error.name : "unknown error");
	}
};

struct message_ptr
{
	sd_bus_message * m = nullptr;
	~message_ptr()
	{
		sd_bus_message_unref(m);
	}
};

struct slot_ptr
{
	sd_bus_slot * s = nullptr;
	~slot_ptr()
	{
		sd_bus_slot_unref(s);
	}
};

std::filesystem::path restore_token_path()
{
	return xdg_config_home() / "wivrn" / "desktop_restore_token";
}

std::string load_restore_token()
{
	std::ifstream f(restore_token_path());
	std::string token;
	std::getline(f, token);
	return token;
}

void save_restore_token(const std::string & token)
{
	try
	{
		std::filesystem::create_directories(restore_token_path().parent_path());
		std::ofstream(restore_token_path()) << token;
	}
	catch (std::exception & e)
	{
		U_LOG_W("Cannot save desktop capture restore token: %s", e.what());
	}
}
// Options dictionary a{sv}, Options is a range of desktop_portal::option
template <typename Options>
void append_options(sd_bus_message * m, const Options & options)
{
	check(sd_bus_message_open_container(m, 'a', "{sv}"), "D-Bus message");
	for (const auto & o: options)
	{
		check(sd_bus_message_open_container(m, 'e', "sv"), "D-Bus message");
		check(sd_bus_message_append(m, "s", o.key), "D-Bus message");
		switch (o.type)
		{
			case 's':
				check(sd_bus_message_append(m, "v", "s", o.s.c_str()), "D-Bus message");
				break;
			case 'b':
				check(sd_bus_message_append(m, "v", "b", int(o.u)), "D-Bus message");
				break;
			default:
				check(sd_bus_message_append(m, "v", "u", o.u), "D-Bus message");
				break;
		}
		check(sd_bus_message_close_container(m), "D-Bus message");
	}
	check(sd_bus_message_close_container(m), "D-Bus message");
}
} // namespace

std::string desktop_portal::next_token()
{
	return "wivrn" + std::to_string(++request_counter);
}

desktop_portal::response desktop_portal::call_request(const char * interface, const char * method, const std::function<void(sd_bus_message *)> & write_args, std::vector<option> options, std::stop_token stop)
{
	std::string handle_token = next_token();
	std::string request_path = std::string(portal_object_path) + "/request/" + sender + "/" + handle_token;
	options.push_back({.key = "handle_token", .type = 's', .s = handle_token});

	response result;
	auto on_response = [](sd_bus_message * m, void * userdata, sd_bus_error *) -> int {
		auto & r = *static_cast<response *>(userdata);
		r.done = true;
		if (sd_bus_message_read(m, "u", &r.code) < 0)
			return 0;

		if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0)
			return 0;
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0)
		{
			const char * key = nullptr;
			sd_bus_message_read(m, "s", &key);
			std::string_view k = key ? key : "";
			const char * str = nullptr;

			if (k == "session_handle" and sd_bus_message_read(m, "v", "s", &str) >= 0)
				r.session_handle = str;
			else if (k == "restore_token" and sd_bus_message_read(m, "v", "s", &str) >= 0)
				r.restore_token = str;
			else if (k == "devices")
				sd_bus_message_read(m, "v", "u", &r.devices);
			else if (k == "streams" and sd_bus_message_enter_container(m, 'v', "a(ua{sv})") > 0)
			{
				sd_bus_message_enter_container(m, 'a', "(ua{sv})");
				while (sd_bus_message_enter_container(m, 'r', "ua{sv}") > 0)
				{
					stream_info info;
					sd_bus_message_read(m, "u", &info.node_id);
					sd_bus_message_enter_container(m, 'a', "{sv}");
					while (sd_bus_message_enter_container(m, 'e', "sv") > 0)
					{
						const char * prop = nullptr;
						sd_bus_message_read(m, "s", &prop);
						if (prop and std::string_view(prop) == "size")
							sd_bus_message_read(m, "v", "(ii)", &info.width, &info.height);
						else
							sd_bus_message_skip(m, "v");
						sd_bus_message_exit_container(m);
					}
					sd_bus_message_exit_container(m);
					sd_bus_message_exit_container(m);
					r.streams.push_back(info);
				}
				sd_bus_message_exit_container(m);
				sd_bus_message_exit_container(m);
			}
			else
			{
				sd_bus_message_skip(m, "v");
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
		return 0;
	};

	// Subscribe before calling, the response may come right after the call
	slot_ptr slot;
	{
		std::lock_guard lock(bus_mutex);
		check(sd_bus_match_signal(bus, &slot.s, portal_bus_name, request_path.c_str(), "org.freedesktop.portal.Request", "Response", on_response, &result), "Cannot subscribe to the portal response");

		message_ptr m;
		check(sd_bus_message_new_method_call(bus, &m.m, portal_bus_name, portal_object_path, interface, method), "Cannot create a portal call");
		write_args(m.m);
		append_options(m.m, options);

		bus_error error;
		message_ptr reply;
		if (sd_bus_call(bus, m.m, 0, &error.error, &reply.m) < 0)
			throw std::runtime_error(std::string(interface) + "." + method + " failed: " + error.message());
	}

	while (not result.done)
	{
		if (stop.stop_requested())
		{
			std::lock_guard lock(bus_mutex);
			sd_bus_call_method(bus, portal_bus_name, request_path.c_str(), "org.freedesktop.portal.Request", "Close", nullptr, nullptr, "");
			throw desktop_portal_cancelled("Desktop capture stopped");
		}

		int r;
		{
			std::lock_guard lock(bus_mutex);
			r = sd_bus_process(bus, nullptr);
		}
		check(r, "D-Bus error");
		if (r == 0)
			sd_bus_wait(bus, 100'000);
	}

	if (result.code != response_success)
	{
		if (result.code == response_cancelled)
			throw desktop_portal_cancelled("Screen sharing was refused on the PC");
		throw std::runtime_error(std::string(interface) + "." + method + " was interrupted");
	}

	return result;
}

desktop_portal::desktop_portal(bool choose_source, std::stop_token stop)
{
	try
	{
		check(sd_bus_open_user(&bus), "Cannot connect to the session bus");

		// Request object paths use the unique name without ':' and with '.' replaced by '_'
		const char * unique_name = nullptr;
		check(sd_bus_get_unique_name(bus, &unique_name), "Cannot get the D-Bus name");
		sender = unique_name + 1;
		std::ranges::replace(sender, '.', '_');

		{
			bus_error error;
			uint32_t version = 0;
			remote_desktop = sd_bus_get_property_trivial(bus, portal_bus_name, portal_object_path, remote_desktop_interface, "version", &error.error, 'u', &version) >= 0;
		}
		const char * session_interface = remote_desktop ? remote_desktop_interface : screencast_interface;
		std::string restore_token = choose_source ? "" : load_restore_token();
		auto no_args = [](sd_bus_message *) {};
		auto session_arg = [this](sd_bus_message * m) { check(sd_bus_message_append(m, "o", session_handle.c_str()), "D-Bus message"); };

		// CreateSession
		{
			auto r = call_request(session_interface, "CreateSession", no_args, {{.key = "session_handle_token", .type = 's', .s = next_token()}}, stop);
			session_handle = r.session_handle;
			if (session_handle.empty())
				throw std::runtime_error("The portal did not return a session");
		}

		// SelectDevices, the permission is remembered here for remote desktop sessions
		if (remote_desktop)
		{
			std::vector<option> options{
			        {.key = "types", .type = 'u', .u = device_keyboard | device_pointer},
			        {.key = "persist_mode", .type = 'u', .u = persist_until_revoked},
			};
			if (not restore_token.empty())
				options.push_back({.key = "restore_token", .type = 's', .s = restore_token});
			call_request(remote_desktop_interface, "SelectDevices", session_arg, std::move(options), stop);
		}

		// SelectSources
		{
			std::vector<option> options{
			        {.key = "types", .type = 'u', .u = source_monitor},
			        // All the screens are shared at once, the headset switches between them
			        {.key = "multiple", .type = 'b', .u = 1},
			        {.key = "cursor_mode", .type = 'u', .u = cursor_embedded},
			};
			if (not remote_desktop)
			{
				options.push_back({.key = "persist_mode", .type = 'u', .u = persist_until_revoked});
				if (not restore_token.empty())
					options.push_back({.key = "restore_token", .type = 's', .s = restore_token});
			}
			call_request(screencast_interface, "SelectSources", session_arg, std::move(options), stop);
		}

		// Start, this is where the user is asked for permission
		{
			auto r = call_request(
			        session_interface,
			        "Start",
			        [this](sd_bus_message * m) { check(sd_bus_message_append(m, "os", session_handle.c_str(), ""), "D-Bus message"); },
			        {},
			        stop);

			if (remote_desktop and not(r.devices & device_pointer))
			{
				U_LOG_W("Remote input was not allowed, mouse and keyboard use uinput");
				remote_desktop = false;
			}

			if (not r.restore_token.empty())
				save_restore_token(r.restore_token);

			streams_ = std::move(r.streams);
			if (streams_.empty())
				throw std::runtime_error("No screen was selected");

			for (const auto & stream: streams_)
				U_LOG_I("Desktop capture started: node %u, %dx%d, remote input %s", stream.node_id, stream.width, stream.height, remote_desktop ? "yes" : "no");
		}
	}
	catch (...)
	{
		close();
		throw;
	}
}

desktop_portal::~desktop_portal()
{
	close();
}

void desktop_portal::close()
{
	std::lock_guard lock(bus_mutex);
	if (bus and not session_handle.empty())
		sd_bus_call_method(bus, portal_bus_name, session_handle.c_str(), "org.freedesktop.portal.Session", "Close", nullptr, nullptr, "");
	session_handle.clear();

	if (bus)
		sd_bus_flush_close_unref(bus);
	bus = nullptr;
}

int desktop_portal::open_pipewire_remote()
{
	std::lock_guard lock(bus_mutex);

	message_ptr m;
	check(sd_bus_message_new_method_call(bus, &m.m, portal_bus_name, portal_object_path, screencast_interface, "OpenPipeWireRemote"), "Cannot create a portal call");
	check(sd_bus_message_append(m.m, "o", session_handle.c_str()), "D-Bus message");
	append_options(m.m, std::vector<option>{});

	bus_error error;
	message_ptr reply;
	if (sd_bus_call(bus, m.m, 0, &error.error, &reply.m) < 0)
		throw std::runtime_error("OpenPipeWireRemote failed: " + error.message());

	int fd = -1;
	check(sd_bus_message_read(reply.m, "h", &fd), "Cannot read the PipeWire file descriptor");

	// The descriptor belongs to the message
	int owned = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	if (owned < 0)
		throw std::runtime_error(std::string("Cannot duplicate the PipeWire file descriptor: ") + std::strerror(errno));
	return owned;
}

void desktop_portal::notify(const char * method, const std::function<void(sd_bus_message *)> & write_args)
{
	std::lock_guard lock(bus_mutex);
	if (not remote_desktop or not bus or session_handle.empty())
		return;

	try
	{
		message_ptr m;
		check(sd_bus_message_new_method_call(bus, &m.m, portal_bus_name, portal_object_path, remote_desktop_interface, method), "Cannot create a portal call");
		check(sd_bus_message_append(m.m, "o", session_handle.c_str()), "D-Bus message");
		append_options(m.m, std::vector<option>{});
		write_args(m.m);

		// Input must not wait for the compositor
		sd_bus_message_set_expect_reply(m.m, 0);
		check(sd_bus_send(bus, m.m, nullptr), "Cannot send input");
		sd_bus_flush(bus);

		// Drop incoming messages so they do not pile up
		while (sd_bus_process(bus, nullptr) > 0)
		{
		}
	}
	catch (std::exception & e)
	{
		U_LOG_W("Desktop remote input: %s", e.what());
	}
}

void desktop_portal::pointer_motion(double dx, double dy)
{
	notify("NotifyPointerMotion", [&](sd_bus_message * m) { check(sd_bus_message_append(m, "dd", dx, dy), "D-Bus message"); });
}

void desktop_portal::pointer_motion_absolute(uint32_t node_id, double x, double y)
{
	notify("NotifyPointerMotionAbsolute", [&](sd_bus_message * m) { check(sd_bus_message_append(m, "udd", node_id, x, y), "D-Bus message"); });
}

void desktop_portal::pointer_button(int evdev_button, bool pressed)
{
	notify("NotifyPointerButton", [&](sd_bus_message * m) { check(sd_bus_message_append(m, "iu", evdev_button, uint32_t(pressed)), "D-Bus message"); });
}

void desktop_portal::pointer_axis_discrete(int axis, int steps)
{
	notify("NotifyPointerAxisDiscrete", [&](sd_bus_message * m) { check(sd_bus_message_append(m, "ui", uint32_t(axis), steps), "D-Bus message"); });
}

void desktop_portal::keyboard_keycode(int evdev_keycode, bool pressed)
{
	notify("NotifyKeyboardKeycode", [&](sd_bus_message * m) { check(sd_bus_message_append(m, "iu", evdev_keycode, uint32_t(pressed)), "D-Bus message"); });
}

} // namespace wivrn
