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

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;

namespace wivrn
{

// The user refused the screen sharing request, or it was stopped while waiting
class desktop_portal_cancelled : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

// Screen capture and remote input session through xdg-desktop-portal.
// Uses the RemoteDesktop portal when available so that mouse and keyboard events can be
// injected in the captured screen, and falls back to the ScreenCast portal otherwise.
// Uses sd-bus rather than GDBus: the server forks without exec and GDBus does not survive it.
class desktop_portal
{
public:
	struct stream_info
	{
		uint32_t node_id = 0;
		// Logical size of the captured screen, used for absolute pointer positions
		int32_t width = 0;
		int32_t height = 0;
	};

	// Blocks until the user accepted the request. Throws desktop_portal_cancelled
	// if the user refused or stop was requested, std::runtime_error on other failures.
	desktop_portal(bool choose_source, std::stop_token stop);
	~desktop_portal();

	desktop_portal(const desktop_portal &) = delete;
	desktop_portal & operator=(const desktop_portal &) = delete;

	// Screens shared by the user, at least one
	const std::vector<stream_info> & streams() const
	{
		return streams_;
	}

	bool has_remote_input() const
	{
		return remote_desktop;
	}

	// Returns a file descriptor to the PipeWire remote with the captured stream, owned by the caller
	int open_pipewire_remote();

	// Remote input, ignored when has_remote_input() is false. Thread safe.
	// Absolute positions are in the logical coordinates of the given stream.
	void pointer_motion(double dx, double dy);
	void pointer_motion_absolute(uint32_t node_id, double x, double y);
	void pointer_button(int evdev_button, bool pressed);
	void pointer_axis_discrete(int axis, int steps);
	void keyboard_keycode(int evdev_keycode, bool pressed);

private:
	sd_bus * bus = nullptr;
	// sd-bus is not thread safe, input is sent from the network thread
	std::mutex bus_mutex;
	std::string sender;
	std::string session_handle;
	bool remote_desktop = false;
	std::vector<stream_info> streams_;
	int request_counter = 0;

	struct option
	{
		const char * key;
		char type; // 's', 'u' or 'b'
		std::string s = {};
		uint32_t u = 0;
	};

	// Results of a request, read in the Response signal
	struct response
	{
		bool done = false;
		uint32_t code = 0;
		std::string session_handle;
		std::string restore_token;
		uint32_t devices = 0;
		std::vector<stream_info> streams;
	};

	// Call a portal method that answers with a Request object and wait for its response.
	// The arguments before the options dictionary are written by write_args.
	response call_request(const char * interface, const char * method, const std::function<void(sd_bus_message *)> & write_args, std::vector<option> options, std::stop_token stop);
	std::string next_token();
	void close();
	// Send a RemoteDesktop notification without waiting for a reply
	void notify(const char * method, const std::function<void(sd_bus_message *)> & write_args);
};

} // namespace wivrn
