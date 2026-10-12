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

#include "desktop_streamer.h"

#include "desktop_portal.h"
#include "util/u_logging.h"
#include "utils/overloaded.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <linux/input-event-codes.h>
#include <optional>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <x264.h>

extern "C"
{
#include <libswscale/swscale.h>
}

namespace wivrn
{
namespace
{
using state_t = to_headset::desktop_status::state_t;

// Fit the captured size in the requested maximum, keeping the aspect ratio, with even dimensions for 4:2:0
std::pair<int, int> fit_size(int width, int height, int max_width, int max_height)
{
	double scale = 1;
	if (max_width > 0)
		scale = std::min(scale, double(max_width) / width);
	if (max_height > 0)
		scale = std::min(scale, double(max_height) / height);

	int w = std::max(2, int(width * scale) & ~1);
	int h = std::max(2, int(height * scale) & ~1);
	return {w, h};
}

std::optional<AVPixelFormat> to_av_format(spa_video_format format)
{
	switch (format)
	{
		case SPA_VIDEO_FORMAT_BGRx:
			return AV_PIX_FMT_BGR0;
		case SPA_VIDEO_FORMAT_RGBx:
			return AV_PIX_FMT_RGB0;
		case SPA_VIDEO_FORMAT_BGRA:
			return AV_PIX_FMT_BGRA;
		case SPA_VIDEO_FORMAT_RGBA:
			return AV_PIX_FMT_RGBA;
		default:
			return std::nullopt;
	}
}

struct i420_frame
{
	int width = 0;
	int height = 0;
	std::vector<uint8_t> data;

	void resize(int w, int h)
	{
		width = w;
		height = h;
		data.resize(size_t(w) * h * 3 / 2);
	}

	uint8_t * plane(int i)
	{
		size_t luma = size_t(width) * height;
		switch (i)
		{
			case 0:
				return data.data();
			case 1:
				return data.data() + luma;
			default:
				return data.data() + luma + luma / 4;
		}
	}

	int stride(int i) const
	{
		return i == 0 ? width : width / 2;
	}
};

// Receives the captured screen from PipeWire, in shared memory
class pw_capture
{
public:
	using callback = std::function<void(const uint8_t * data, int width, int height, int stride, AVPixelFormat)>;

	pw_capture(int fd, uint32_t node_id, callback cb) :
	        cb(std::move(cb))
	{
		pw_init(nullptr, nullptr);

		loop = pw_thread_loop_new("wivrn-desktop", nullptr);
		if (not loop)
			throw std::runtime_error("Cannot create the PipeWire loop");
		context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
		if (not context)
		{
			pw_thread_loop_destroy(loop);
			throw std::runtime_error("Cannot create the PipeWire context");
		}

		pw_thread_loop_lock(loop);
		try
		{
			// Takes ownership of the file descriptor
			core = pw_context_connect_fd(context, fd, nullptr, 0);
			if (not core)
				throw std::runtime_error("Cannot connect to the PipeWire remote of the screen capture");

			stream = pw_stream_new(core,
			                       "WiVRn desktop",
			                       pw_properties_new(
			                               PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
			if (not stream)
				throw std::runtime_error("Cannot create the PipeWire stream");

			pw_stream_add_listener(stream, &listener, &events, this);

			// Formats without modifiers, the compositor sends frames in shared memory
			uint8_t buffer[1024];
			spa_pod_builder builder{};
			spa_pod_builder_init(&builder, buffer, sizeof(buffer));
			std::array formats{SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA};
			std::array<const spa_pod *, formats.size()> params;
			for (size_t i = 0; i < formats.size(); ++i)
			{
				spa_video_info_raw info{};
				info.format = formats[i];
				params[i] = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
			}

			if (pw_stream_connect(stream,
			                      PW_DIRECTION_INPUT,
			                      node_id,
			                      pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
			                      params.data(),
			                      params.size()) < 0)
				throw std::runtime_error("Cannot connect to the screen capture stream");
		}
		catch (...)
		{
			pw_thread_loop_unlock(loop);
			cleanup();
			throw;
		}
		pw_thread_loop_unlock(loop);

		if (pw_thread_loop_start(loop) < 0)
		{
			cleanup();
			throw std::runtime_error("Cannot start the PipeWire loop");
		}
	}

	~pw_capture()
	{
		cleanup();
	}

	std::optional<std::string> error()
	{
		std::lock_guard lock(error_mutex);
		return error_;
	}

private:
	void cleanup()
	{
		if (loop)
		{
			pw_thread_loop_lock(loop);
			if (stream)
				pw_stream_destroy(stream);
			stream = nullptr;
			if (core)
				pw_core_disconnect(core);
			core = nullptr;
			pw_thread_loop_unlock(loop);
			pw_thread_loop_stop(loop);
		}
		if (context)
			pw_context_destroy(context);
		context = nullptr;
		if (loop)
			pw_thread_loop_destroy(loop);
		loop = nullptr;
	}

	void set_error(std::string message)
	{
		std::lock_guard lock(error_mutex);
		if (not error_)
			error_ = std::move(message);
	}

	static void on_state_changed(void * data, pw_stream_state old, pw_stream_state state, const char * error)
	{
		auto self = static_cast<pw_capture *>(data);
		U_LOG_I("Desktop capture stream: %s -> %s", pw_stream_state_as_string(old), pw_stream_state_as_string(state));

		if (state == PW_STREAM_STATE_ERROR)
			self->set_error(std::string("Screen capture error: ") + (error ? error : "unknown"));
		else if (state == PW_STREAM_STATE_UNCONNECTED and old != PW_STREAM_STATE_UNCONNECTED)
			self->set_error("Screen sharing was stopped on the PC");
	}

	static void on_param_changed(void * data, uint32_t id, const spa_pod * param)
	{
		auto self = static_cast<pw_capture *>(data);
		if (id != SPA_PARAM_Format or not param)
			return;

		spa_video_info_raw info{};
		if (spa_format_video_raw_parse(param, &info) < 0)
			return;
		self->format = info;
		U_LOG_I("Desktop capture format: %dx%d, format %d", info.size.width, info.size.height, info.format);
	}

	static void on_process(void * data)
	{
		auto self = static_cast<pw_capture *>(data);

		// Only the most recent frame is useful
		pw_buffer * b = pw_stream_dequeue_buffer(self->stream);
		if (not b)
			return;
		while (pw_buffer * next = pw_stream_dequeue_buffer(self->stream))
		{
			pw_stream_queue_buffer(self->stream, b);
			b = next;
		}

		spa_data & d = b->buffer->datas[0];
		auto av_format = to_av_format(self->format.format);
		int width = self->format.size.width;
		int height = self->format.size.height;

		if (not d.data)
		{
			if (not self->warned_unmapped)
				U_LOG_W("Desktop capture: unsupported buffer type %u", d.type);
			self->warned_unmapped = true;
		}
		else if (d.chunk->size > 0 and not(d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) and av_format and width > 0 and height > 0)
		{
			int stride = d.chunk->stride > 0 ? d.chunk->stride : width * 4;
			self->cb(static_cast<const uint8_t *>(d.data) + d.chunk->offset, width, height, stride, *av_format);
		}

		pw_stream_queue_buffer(self->stream, b);
	}

	static constexpr pw_stream_events events = {
	        .version = PW_VERSION_STREAM_EVENTS,
	        .state_changed = on_state_changed,
	        .param_changed = on_param_changed,
	        .process = on_process,
	};

	callback cb;
	pw_thread_loop * loop = nullptr;
	pw_context * context = nullptr;
	pw_core * core = nullptr;
	pw_stream * stream = nullptr;
	spa_hook listener{};
	spa_video_info_raw format{};
	bool warned_unmapped = false;

	std::mutex error_mutex;
	std::optional<std::string> error_;
};
} // namespace

// Converts captured frames to I420 and encodes them with x264 on its own thread
struct desktop_streamer::encoder_state
{
	struct params
	{
		int max_width;
		int max_height;
		int fps;
		uint32_t bitrate;
	};

	encoder_state(frame_fn send_frame, std::function<void(int, int)> on_size, const from_headset::desktop_request & request) :
	        send_frame(std::move(send_frame)),
	        on_size(std::move(on_size))
	{
		set_params(request);
		if (const char * path = std::getenv("WIVRN_DESKTOP_DUMP"))
			dump.open(path, std::ios::binary | std::ios::trunc);
		thread = std::jthread([this](std::stop_token stop) { run(stop); });
	}

	~encoder_state()
	{
		thread.request_stop();
		cv.notify_all();
		if (thread.joinable())
			thread.join();
		if (sws)
			sws_freeContext(sws);
	}

	void set_params(const from_headset::desktop_request & request)
	{
		std::lock_guard lock(mutex);
		wanted = {
		        .max_width = request.max_width,
		        .max_height = request.max_height,
		        .fps = std::clamp<int>(request.fps, 1, 120),
		        .bitrate = std::clamp<uint32_t>(request.bitrate, 500'000, 200'000'000),
		};
	}

	void request_idr()
	{
		std::lock_guard lock(mutex);
		force_idr = true;
		cv.notify_all();
	}

	// PipeWire thread
	void push(const uint8_t * src, int width, int height, int stride, AVPixelFormat format)
	{
		params p;
		{
			std::lock_guard lock(mutex);
			p = wanted;
		}

		auto [w, h] = fit_size(width, height, p.max_width, p.max_height);
		int flags = (w == width and h == height) ? SWS_POINT : SWS_BICUBIC;
		sws = sws_getCachedContext(sws, width, height, format, w, h, AV_PIX_FMT_YUV420P, flags, nullptr, nullptr, nullptr);
		if (not sws)
			return;
		// Full range RGB to limited range BT.709, matching the VUI written by the encoder
		sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_ITU709), 1, sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);

		scratch.resize(w, h);
		uint8_t * dst[3] = {scratch.plane(0), scratch.plane(1), scratch.plane(2)};
		int dst_stride[3] = {scratch.stride(0), scratch.stride(1), scratch.stride(2)};
		sws_scale(sws, &src, &stride, 0, height, dst, dst_stride);

		std::lock_guard lock(mutex);
		std::swap(scratch, pending);
		pending_ready = true;
		cv.notify_all();
	}

private:
	void close_encoder()
	{
		if (encoder)
			x264_encoder_close(encoder);
		encoder = nullptr;
	}

	void open_encoder(int width, int height, const params & p)
	{
		close_encoder();

		x264_param_t param;
		x264_param_default_preset(&param, "superfast", "zerolatency");
		param.i_log_level = X264_LOG_WARNING;
		param.i_width = width;
		param.i_height = height;
		param.i_csp = X264_CSP_I420;
		param.i_fps_num = p.fps;
		param.i_fps_den = 1;
		param.i_threads = 4;
		param.i_keyint_max = p.fps * 4;
		param.b_repeat_headers = 1;
		param.b_annexb = 1;

		param.vui.i_colorprim = 1; // BT.709
		param.vui.i_transfer = 1;
		param.vui.i_colmatrix = 1;
		param.vui.b_fullrange = 0;

		param.rc.i_rc_method = X264_RC_ABR;
		param.rc.i_bitrate = p.bitrate / 1000; // kbit/s
		param.rc.i_vbv_max_bitrate = param.rc.i_bitrate;
		param.rc.i_vbv_buffer_size = std::max(1, param.rc.i_bitrate * 2 / p.fps);

		x264_param_apply_profile(&param, "high");

		encoder = x264_encoder_open(&param);
		if (not encoder)
			throw std::runtime_error("Cannot open the x264 encoder for the desktop");

		encoder_width = width;
		encoder_height = height;
		encoder_params = p;
		U_LOG_I("Desktop encoder: %dx%d, %d fps, %u kbit/s", width, height, p.fps, p.bitrate / 1000);
		on_size(width, height);
	}

	void run(std::stop_token stop)
	{
		using clock = std::chrono::steady_clock;
		i420_frame current;
		clock::time_point last_encode{};
		uint64_t frame_idx = 0;

		while (not stop.stop_requested())
		{
			bool idr;
			params p;
			{
				std::unique_lock lock(mutex);
				cv.wait(lock, stop, [&] { return pending_ready or force_idr; });
				if (stop.stop_requested())
					break;

				// Frame rate cap, newer frames replace the pending one while waiting
				auto next = last_encode + std::chrono::nanoseconds(1'000'000'000 / wanted.fps);
				if (clock::now() < next)
				{
					cv.wait_until(lock, stop, next, [] { return false; });
					if (stop.stop_requested())
						break;
				}

				if (pending_ready)
				{
					std::swap(current, pending);
					pending_ready = false;
				}
				idr = force_idr;
				force_idr = false;
				p = wanted;
			}

			// Nothing captured yet, an IDR will come with the first frame
			if (current.width == 0)
				continue;

			try
			{
				if (not encoder or current.width != encoder_width or current.height != encoder_height or
				    p.fps != encoder_params.fps or p.bitrate != encoder_params.bitrate)
					open_encoder(current.width, current.height, p);

				x264_picture_t pic;
				x264_picture_init(&pic);
				pic.img.i_csp = X264_CSP_I420;
				pic.img.i_plane = 3;
				for (int i = 0; i < 3; ++i)
				{
					pic.img.plane[i] = current.plane(i);
					pic.img.i_stride[i] = current.stride(i);
				}
				pic.i_pts = frame_idx;
				pic.i_type = idr ? X264_TYPE_IDR : X264_TYPE_AUTO;

				x264_nal_t * nals = nullptr;
				int num_nals = 0;
				x264_picture_t pic_out;
				int size = x264_encoder_encode(encoder, &nals, &num_nals, &pic, &pic_out);
				last_encode = clock::now();

				if (size > 0)
				{
					// The payloads of all the NALs are contiguous
					to_headset::desktop_frame frame{
					        .frame_idx = frame_idx,
					        .idr = bool(pic_out.b_keyframe),
					        .data = std::vector<uint8_t>(nals[0].p_payload, nals[0].p_payload + size),
					};
					if (dump.is_open())
						dump.write(reinterpret_cast<const char *>(frame.data.data()), frame.data.size());
					send_frame(std::move(frame));
				}
				++frame_idx;
			}
			catch (std::exception & e)
			{
				U_LOG_E("Desktop encoder error: %s", e.what());
				close_encoder();
			}
		}

		close_encoder();
	}

	frame_fn send_frame;
	std::function<void(int, int)> on_size;

	std::mutex mutex;
	std::condition_variable_any cv;
	params wanted{};
	i420_frame pending;
	bool pending_ready = false;
	bool force_idr = false;

	// PipeWire thread only
	SwsContext * sws = nullptr;
	i420_frame scratch;

	// Encoder thread only
	x264_t * encoder = nullptr;
	int encoder_width = 0;
	int encoder_height = 0;
	params encoder_params{};
	std::ofstream dump;

	std::jthread thread;
};

desktop_streamer::desktop_streamer(status_fn send_status, frame_fn send_frame) :
        send_status_(std::move(send_status)),
        send_frame(std::move(send_frame))
{
	thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

desktop_streamer::~desktop_streamer()
{
	{
		std::lock_guard lock(mutex);
		session_stop.request_stop();
	}
	thread.request_stop();
	cv.notify_all();
	if (thread.joinable())
		thread.join();
}

void desktop_streamer::send_status(state_t state, const std::string & message)
{
	send_status_(to_headset::desktop_status{
	        .state = state,
	        .message = message,
	});
}

void desktop_streamer::request(const from_headset::desktop_request & request)
{
	std::lock_guard lock(mutex);
	bool was_enabled = wanted.enabled;
	wanted = request;

	if (not request.enabled)
	{
		session_stop.request_stop();
	}
	else if (request.choose_source)
	{
		choose_source_pending = true;
		session_stop.request_stop();
	}
	else if (was_enabled and encoder)
	{
		// A different monitor is picked up by the session loop
		// Same session: apply the new settings, and send a full frame for a new decoder
		encoder->set_params(request);
		encoder->request_idr();
	}
	cv.notify_all();
}

void desktop_streamer::pointer(const from_headset::desktop_pointer & p)
{
	std::lock_guard lock(input_mutex);
	if (not input_portal or not input_portal->has_remote_input())
		return;

	input_portal->pointer_motion_absolute(input_node, std::clamp(p.x, 0.f, 1.f) * input_width, std::clamp(p.y, 0.f, 1.f) * input_height);
}

bool desktop_streamer::handle_input(const from_headset::hid::input & e)
{
	std::lock_guard lock(input_mutex);
	if (not input_portal or not input_portal->has_remote_input())
		return false;

	desktop_portal & portal = *input_portal;
	std::visit(utils::overloaded{
	                   [&](const from_headset::hid::key_down & e) { portal.keyboard_keycode(e.key, true); },
	                   [&](const from_headset::hid::key_up & e) { portal.keyboard_keycode(e.key, false); },
	                   [&](const from_headset::hid::button_down & e) { portal.pointer_button(BTN_MOUSE + e.button, true); },
	                   [&](const from_headset::hid::button_up & e) { portal.pointer_button(BTN_MOUSE + e.button, false); },
	                   [&](const from_headset::hid::mouse_move & e) { portal.pointer_motion(e.x, e.y); },
	                   [&](const from_headset::hid::mouse_scroll & e) {
		                   // Wayland scrolls down for positive steps, evdev wheels scroll up
		                   if (int v = e.v)
			                   portal.pointer_axis_discrete(0, -v);
		                   if (int h = e.h)
			                   portal.pointer_axis_discrete(1, h);
	                   },
	           },
	           e.input_data);
	return true;
}

void desktop_streamer::run_session(bool choose_source, std::stop_token session_stop)
{
	send_status(state_t::waiting_permission);

	desktop_portal portal(choose_source, session_stop);
	const bool remote_input = portal.has_remote_input();
	const auto & streams = portal.streams();
	const auto monitor_count = uint8_t(std::min<size_t>(streams.size(), 255));

	// Screen currently streamed, shared with the status callback of the encoder
	auto monitor = std::make_shared<std::atomic<uint8_t>>(0);

	auto enc = std::make_shared<encoder_state>(
	        send_frame,
	        [this, remote_input, monitor_count, monitor](int width, int height) {
		        send_status_(to_headset::desktop_status{
		                .state = state_t::streaming,
		                .width = uint16_t(width),
		                .height = uint16_t(height),
		                .remote_input = remote_input,
		                .monitor_count = monitor_count,
		                .monitor = *monitor,
		        });
	        },
	        [&] {
		        std::lock_guard lock(mutex);
		        return wanted;
	        }());

	{
		std::lock_guard lock(mutex);
		encoder = enc;
	}

	auto teardown = [&] {
		{
			std::lock_guard lock(mutex);
			encoder.reset();
		}
		std::lock_guard lock(input_mutex);
		input_portal = nullptr;
	};

	try
	{
		std::optional<pw_capture> capture;
		int current = -1;

		while (not session_stop.stop_requested())
		{
			int requested;
			{
				std::lock_guard lock(mutex);
				requested = std::min<int>(wanted.monitor, streams.size() - 1);
			}

			// Switching screens reuses the portal session, no new permission is needed
			if (requested != current)
			{
				capture.reset();
				current = requested;
				*monitor = current;
				const auto & stream = streams[current];
				{
					std::lock_guard lock(input_mutex);
					input_portal = &portal;
					input_node = stream.node_id;
					input_width = stream.width;
					input_height = stream.height;
				}
				capture.emplace(portal.open_pipewire_remote(), stream.node_id, [enc](const uint8_t * data, int width, int height, int stride, AVPixelFormat format) {
					enc->push(data, width, height, stride, format);
				});
				// A screen of the same size does not reopen the encoder, the new decoder needs a full frame
				enc->request_idr();
			}

			if (auto error = capture->error())
				throw std::runtime_error(*error);

			std::unique_lock lock(mutex);
			cv.wait_for(lock, session_stop, std::chrono::milliseconds(200), [&] { return std::min<int>(wanted.monitor, streams.size() - 1) != current; });
		}
	}
	catch (...)
	{
		teardown();
		throw;
	}
	teardown();
}

void desktop_streamer::run(std::stop_token stop)
{
	while (not stop.stop_requested())
	{
		bool choose_source;
		std::stop_source current_stop;
		{
			std::unique_lock lock(mutex);
			cv.wait(lock, stop, [&] { return wanted.enabled; });
			if (stop.stop_requested())
				return;

			choose_source = choose_source_pending;
			choose_source_pending = false;
			session_stop = std::stop_source{};
			current_stop = session_stop;
		}

		// Server shutdown also ends the session
		std::stop_callback on_stop(stop, [current_stop]() mutable { current_stop.request_stop(); });
		std::stop_token session_token = current_stop.get_token();

		try
		{
			run_session(choose_source, session_token);
			send_status(state_t::stopped);
		}
		catch (desktop_portal_cancelled & e)
		{
			if (session_token.stop_requested())
			{
				send_status(state_t::stopped);
			}
			else if (choose_source)
			{
				// The screen picker was closed: keep using the screens shared before
				U_LOG_W("Desktop capture: %s, restoring the previous screens", e.what());
			}
			else
			{
				U_LOG_W("Desktop capture: %s", e.what());
				send_status(state_t::error, e.what());
				std::lock_guard lock(mutex);
				wanted.enabled = false;
			}
		}
		catch (std::exception & e)
		{
			U_LOG_E("Desktop capture failed: %s", e.what());
			send_status(state_t::error, e.what());
			std::lock_guard lock(mutex);
			wanted.enabled = false;
		}
	}
}

} // namespace wivrn
