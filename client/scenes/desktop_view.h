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

#include "imgui.h"
#include "utils/thread_safe.h"
#include "wivrn_packets.h"

#include <memory>
#include <vulkan/vulkan_raii.hpp>

class imgui_textures;

// Decodes the desktop video of the workspace tab and exposes the latest frame as a GUI texture.
// Hardware decoding is only implemented on Android, elsewhere no image is shown.
class desktop_view
{
public:
	desktop_view(vk::raii::Device & device,
	             vk::raii::PhysicalDevice & physical_device,
	             thread_safe<vk::raii::Queue> & queue,
	             uint32_t queue_family_index,
	             imgui_textures & textures);
	~desktop_view();

	desktop_view(const desktop_view &) = delete;
	desktop_view & operator=(const desktop_view &) = delete;

	static bool supported();

	// Network thread: restart decoding for a stream of this size, frames are skipped until the next IDR
	void reset(uint16_t width, uint16_t height);
	// Network thread: decode a frame
	void push_frame(wivrn::to_headset::desktop_frame && frame);
	// Network thread: stop decoding
	void stop();

	// Render thread: convert the latest decoded frame for the GUI.
	// Returns the texture, or 0 if no frame was decoded yet.
	ImTextureID update();
	// Size of the decoded image, valid when update returned a texture
	ImVec2 size() const;

	struct impl;

private:
	std::unique_ptr<impl> d;
};
