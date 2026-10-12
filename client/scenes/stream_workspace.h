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
#include "wivrn_packets.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>

// Desktop workspace: a trackpad and a PC keyboard drawn with the controller pointers,
// sending mouse and key events to the PC through the hid forwarding packets.
class pc_workspace
{
public:
	using send_fn = std::function<void(wivrn::from_headset::hid::input_t)>;
	// Absolute mouse position in the desktop image, in [0, 1]
	using pointer_fn = std::function<void(float x, float y)>;

	pc_workspace(send_fn send, pointer_fn pointer);

	// Draw the toolbar and the pointer area in the remaining space of the current window.
	// right_button is the state of the controller button used for right click.
	// When desktop is set, the PC desktop image replaces the trackpad and the mouse follows the pointer on it.
	void draw(float pointer_speed, bool right_button, ImTextureID desktop = 0, ImVec2 desktop_size = {});

	// Draw the PC keyboard at the cursor of the current window, meant for its own panel
	void draw_keyboard(ImVec2 size);
	bool is_keyboard_visible() const
	{
		return keyboard_visible;
	}

	// Release every key and mouse button still held on the PC
	void release_all();

private:
	void draw_toolbar();
	void draw_pointer_area(ImVec2 size, float pointer_speed, bool right_button, ImTextureID desktop, ImVec2 desktop_size);

	void key_down(uint8_t key);
	void key_up(uint8_t key);
	void button_down(uint8_t button);
	void button_up(uint8_t button);
	void release_modifiers();

	send_fn send;
	pointer_fn pointer;
	// Last absolute position sent, negative when the pointer is not on the desktop
	ImVec2 last_pointer{-1, -1};

	// Sub-pixel motion and scroll carried over to the next frame
	ImVec2 move_remainder{0, 0};
	ImVec2 scroll_remainder{0, 0};
	bool pad_hovered_prev = false;

	// Mouse button pressed with the trigger on the trackpad
	std::optional<uint8_t> trigger_button;
	bool right_button_prev = false;
	// The next trigger press on the trackpad is a right click
	bool right_click_next = false;

	bool keyboard_visible = true;

	// Keys and mouse buttons currently down on the PC
	std::set<uint8_t> held_keys;
	std::set<uint8_t> held_buttons;
	// Sticky modifiers, released after the next non-modifier key
	std::set<uint8_t> latched_modifiers;
};
