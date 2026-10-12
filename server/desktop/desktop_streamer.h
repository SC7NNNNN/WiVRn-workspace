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

#include "wivrn_packets.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace wivrn
{
class desktop_portal;

// Streams the PC desktop to the headset for the workspace tab: the screen is captured
// through xdg-desktop-portal and PipeWire, then encoded with x264.
class desktop_streamer
{
public:
	using status_fn = std::function<void(to_headset::desktop_status &&)>;
	using frame_fn = std::function<void(to_headset::desktop_frame &&)>;

	desktop_streamer(status_fn send_status, frame_fn send_frame);
	~desktop_streamer();

	// Called from the network thread
	void request(const from_headset::desktop_request &);
	void pointer(const from_headset::desktop_pointer &);

	// Inject a forwarded mouse or keyboard event in the captured desktop.
	// Returns false if there is no capture session with remote input.
	bool handle_input(const from_headset::hid::input &);

	struct encoder_state;

private:
	void run(std::stop_token stop);
	void run_session(bool choose_source, std::stop_token session_stop);
	void send_status(to_headset::desktop_status::state_t state, const std::string & message = {});

	status_fn send_status_;
	frame_fn send_frame;

	std::mutex mutex;
	std::condition_variable_any cv;
	from_headset::desktop_request wanted{};
	bool choose_source_pending = false;
	// Stops the current capture session when the request changes
	std::stop_source session_stop;

	// Portal and screen of the active session, for remote input
	std::mutex input_mutex;
	desktop_portal * input_portal = nullptr;
	uint32_t input_node = 0;
	int input_width = 0;
	int input_height = 0;

	std::shared_ptr<encoder_state> encoder;

	std::jthread thread;
};

} // namespace wivrn
