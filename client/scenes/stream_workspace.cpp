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

#include "stream_workspace.h"

#include "imgui_internal.h"
#include "render/ui_theme.h"
#include "render/ui_widgets.h"
#include "utils/i18n.h"
#include <IconsFontAwesome7.h>
#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

namespace
{
// mouse button indices, as expected by the server (offset from BTN_MOUSE)
constexpr uint8_t button_left = 0;
constexpr uint8_t button_right = 1;

// Ignore pointer jumps larger than this between two frames (switching panel, tracking glitch)
constexpr float max_pointer_jump = 150;

struct pc_key
{
	float width;
	uint8_t code; // Linux key code, 0 for a spacer
	const char * label;
	const char * shift_label = nullptr;
};

// US ANSI physical layout, the PC applies its own keyboard layout to the key codes.
// Each row is 16.75 units wide: main block of 15, a gap, then a navigation column.
// clang-format off
const std::vector<std::vector<pc_key>> pc_layout = {
        {
                {1.25, KEY_ESC, "Esc"}, {0.25, 0, ""},
                {1, KEY_F1, "F1"}, {1, KEY_F2, "F2"}, {1, KEY_F3, "F3"}, {1, KEY_F4, "F4"},
                {1, KEY_F5, "F5"}, {1, KEY_F6, "F6"}, {1, KEY_F7, "F7"}, {1, KEY_F8, "F8"},
                {1, KEY_F9, "F9"}, {1, KEY_F10, "F10"}, {1, KEY_F11, "F11"}, {1, KEY_F12, "F12"},
                {0.25, 0, ""}, {1.5, KEY_INSERT, "Ins"}, {1.5, KEY_DELETE, "Del"},
        },
        {
                {1, KEY_GRAVE, "`", "~"}, {1, KEY_1, "1", "!"}, {1, KEY_2, "2", "@"}, {1, KEY_3, "3", "#"},
                {1, KEY_4, "4", "$"}, {1, KEY_5, "5", "%"}, {1, KEY_6, "6", "^"}, {1, KEY_7, "7", "&"},
                {1, KEY_8, "8", "*"}, {1, KEY_9, "9", "("}, {1, KEY_0, "0", ")"}, {1, KEY_MINUS, "-", "_"},
                {1, KEY_EQUAL, "=", "+"}, {2, KEY_BACKSPACE, ICON_FA_DELETE_LEFT},
                {0.25, 0, ""}, {1.5, KEY_HOME, "Home"},
        },
        {
                {1.5, KEY_TAB, "Tab"},
                {1, KEY_Q, "q", "Q"}, {1, KEY_W, "w", "W"}, {1, KEY_E, "e", "E"}, {1, KEY_R, "r", "R"},
                {1, KEY_T, "t", "T"}, {1, KEY_Y, "y", "Y"}, {1, KEY_U, "u", "U"}, {1, KEY_I, "i", "I"},
                {1, KEY_O, "o", "O"}, {1, KEY_P, "p", "P"}, {1, KEY_LEFTBRACE, "[", "{"}, {1, KEY_RIGHTBRACE, "]", "}"},
                {1.5, KEY_BACKSLASH, "\\", "|"},
                {0.25, 0, ""}, {1.5, KEY_END, "End"},
        },
        {
                {1.75, KEY_CAPSLOCK, "Caps"},
                {1, KEY_A, "a", "A"}, {1, KEY_S, "s", "S"}, {1, KEY_D, "d", "D"}, {1, KEY_F, "f", "F"},
                {1, KEY_G, "g", "G"}, {1, KEY_H, "h", "H"}, {1, KEY_J, "j", "J"}, {1, KEY_K, "k", "K"},
                {1, KEY_L, "l", "L"}, {1, KEY_SEMICOLON, ";", ":"}, {1, KEY_APOSTROPHE, "'", "\""},
                {2.25, KEY_ENTER, "Enter"},
                {0.25, 0, ""}, {1.5, KEY_PAGEUP, "PgUp"},
        },
        {
                {2.25, KEY_LEFTSHIFT, "Shift"},
                {1, KEY_Z, "z", "Z"}, {1, KEY_X, "x", "X"}, {1, KEY_C, "c", "C"}, {1, KEY_V, "v", "V"},
                {1, KEY_B, "b", "B"}, {1, KEY_N, "n", "N"}, {1, KEY_M, "m", "M"},
                {1, KEY_COMMA, ",", "<"}, {1, KEY_DOT, ".", ">"}, {1, KEY_SLASH, "/", "?"},
                {2.75, KEY_RIGHTSHIFT, "Shift"},
                {0.25, 0, ""}, {1.5, KEY_PAGEDOWN, "PgDn"},
        },
        {
                {1.25, KEY_LEFTCTRL, "Ctrl"}, {1.25, KEY_LEFTMETA, "Super"}, {1.25, KEY_LEFTALT, "Alt"},
                {5.25, KEY_SPACE, ""},
                {1.25, KEY_RIGHTALT, "AltGr"}, {1.25, KEY_RIGHTCTRL, "Ctrl"},
                {0.25, 0, ""},
                {1.25, KEY_LEFT, ICON_FA_ARROW_LEFT}, {1.25, KEY_UP, ICON_FA_ARROW_UP},
                {1.25, KEY_DOWN, ICON_FA_ARROW_DOWN}, {1.25, KEY_RIGHT, ICON_FA_ARROW_RIGHT},
        },
};
// clang-format on

constexpr float pc_layout_units = 16.75;

bool is_modifier(uint8_t code)
{
	switch (code)
	{
		case KEY_LEFTSHIFT:
		case KEY_RIGHTSHIFT:
		case KEY_LEFTCTRL:
		case KEY_RIGHTCTRL:
		case KEY_LEFTALT:
		case KEY_RIGHTALT:
		case KEY_LEFTMETA:
		case KEY_RIGHTMETA:
			return true;
		default:
			return false;
	}
}

// Convert an accumulated value to whole steps, keeping the fractional part for later
int take_whole(float & value)
{
	float whole = std::trunc(value);
	value -= whole;
	return int(whole);
}
} // namespace

pc_workspace::pc_workspace(send_fn send, pointer_fn pointer) :
        send(std::move(send)),
        pointer(std::move(pointer))
{
}

void pc_workspace::key_down(uint8_t key)
{
	if (held_keys.insert(key).second)
		send(wivrn::from_headset::hid::key_down{key});
}

void pc_workspace::key_up(uint8_t key)
{
	if (held_keys.erase(key))
		send(wivrn::from_headset::hid::key_up{key});
}

void pc_workspace::button_down(uint8_t button)
{
	if (held_buttons.insert(button).second)
		send(wivrn::from_headset::hid::button_down{button});
}

void pc_workspace::button_up(uint8_t button)
{
	if (held_buttons.erase(button))
		send(wivrn::from_headset::hid::button_up{button});
}

void pc_workspace::release_modifiers()
{
	for (uint8_t key: latched_modifiers)
		key_up(key);
	latched_modifiers.clear();
}

void pc_workspace::release_all()
{
	for (uint8_t key: std::set<uint8_t>(held_keys))
		key_up(key);
	for (uint8_t button: std::set<uint8_t>(held_buttons))
		button_up(button);

	latched_modifiers.clear();
	trigger_button.reset();
	right_button_prev = false;
	pad_hovered_prev = false;
	last_pointer = {-1, -1};
	move_remainder = {0, 0};
	scroll_remainder = {0, 0};
}

void pc_workspace::draw(float pointer_speed, bool right_button, ImTextureID desktop, ImVec2 desktop_size)
{
	draw_toolbar();

	const ImGuiStyle & style = ImGui::GetStyle();
	ImVec2 avail = ImGui::GetContentRegionAvail();

	if (keyboard_visible)
	{
		// The keyboard gets 6 rows of keys of at most 70px, the trackpad the rest
		float keyboard_height = std::min(avail.y * 0.55f, 6 * 70.f);
		draw_pointer_area({avail.x, avail.y - keyboard_height - style.ItemSpacing.y}, pointer_speed, right_button, desktop, desktop_size);
		draw_keyboard({avail.x, keyboard_height});
	}
	else
	{
		draw_pointer_area(avail, pointer_speed, right_button, desktop, desktop_size);
	}
}

void pc_workspace::draw_toolbar()
{
	using wivrn::ui::button_style;

	if (wivrn::ui::button(ICON_FA_COMPUTER_MOUSE, _("Right click"), right_click_next ? button_style::primary : button_style::secondary))
		right_click_next = not right_click_next;
	ImGui::SameLine();

	if (wivrn::ui::button(ICON_FA_KEYBOARD, keyboard_visible ? _("Hide keyboard") : _("Show keyboard"), button_style::secondary))
	{
		if (keyboard_visible)
		{
			// Do not leave keys stuck on the PC when the keyboard disappears
			for (uint8_t key: std::set<uint8_t>(held_keys))
				key_up(key);
			latched_modifiers.clear();
		}
		keyboard_visible = not keyboard_visible;
	}
}

void pc_workspace::draw_pointer_area(ImVec2 size, float pointer_speed, bool right_button, ImTextureID desktop, ImVec2 desktop_size)
{
	ImGuiIO & io = ImGui::GetIO();
	const wivrn::ui::theme & t = wivrn::ui::current();

	size.x = std::max(size.x, 1.f);
	size.y = std::max(size.y, 1.f);

	const bool show_desktop = desktop and desktop_size.x > 0 and desktop_size.y > 0;
	const ImVec2 area = ImGui::GetCursorScreenPos();
	const ImVec2 area_size = size;

	// The desktop keeps its aspect ratio, centered in the area
	ImVec2 pos = area;
	if (show_desktop)
	{
		float scale = std::min(size.x / desktop_size.x, size.y / desktop_size.y);
		ImVec2 fitted{desktop_size.x * scale, desktop_size.y * scale};
		pos = {area.x + (size.x - fitted.x) / 2, area.y + (size.y - fitted.y) / 2};
		size = fitted;
	}

	ImGui::SetCursorScreenPos(pos);
	ImGui::InvisibleButton("##pointer_area", size);
	bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	bool active = ImGui::IsItemActive();
	bool tracking = hovered or active;

	if (show_desktop)
	{
		// Absolute: the mouse goes where the controller points
		if (tracking)
		{
			ImVec2 p{std::clamp((io.MousePos.x - pos.x) / size.x, 0.f, 1.f),
			         std::clamp((io.MousePos.y - pos.y) / size.y, 0.f, 1.f)};
			if (std::abs(p.x - last_pointer.x) > 1e-4f or std::abs(p.y - last_pointer.y) > 1e-4f)
			{
				pointer(p.x, p.y);
				last_pointer = p;
			}
		}
		else
		{
			last_pointer = {-1, -1};
		}
		move_remainder = {0, 0};
	}
	// Relative: only when the pointer was already on the trackpad, so entering it does not jump
	else if (tracking and pad_hovered_prev)
	{
		ImVec2 delta = io.MouseDelta;
		if (std::abs(delta.x) < max_pointer_jump and std::abs(delta.y) < max_pointer_jump)
		{
			move_remainder.x += delta.x * pointer_speed;
			move_remainder.y += delta.y * pointer_speed;
			int dx = take_whole(move_remainder.x);
			int dy = take_whole(move_remainder.y);
			if (dx or dy)
				send(wivrn::from_headset::hid::mouse_move{float(dx), float(dy)});
		}
	}
	else
	{
		move_remainder = {0, 0};
	}
	pad_hovered_prev = tracking;

	// Scrolling with the thumbstick, one notch per unit of imgui wheel
	if (hovered)
	{
		scroll_remainder.x -= io.MouseWheelH;
		scroll_remainder.y += io.MouseWheel;
		int h = take_whole(scroll_remainder.x);
		int v = take_whole(scroll_remainder.y);
		if (h or v)
			send(wivrn::from_headset::hid::mouse_scroll{float(h), float(v)});
	}
	else
	{
		scroll_remainder = {0, 0};
	}

	// Trigger: left click, or right click if requested from the toolbar
	if (ImGui::IsItemActivated())
	{
		trigger_button = right_click_next ? button_right : button_left;
		right_click_next = false;
		button_down(*trigger_button);
	}
	if (ImGui::IsItemDeactivated() and trigger_button)
	{
		button_up(*trigger_button);
		trigger_button.reset();
	}

	// Controller button: right click while pointing at the trackpad
	if (right_button and not right_button_prev and hovered)
		button_down(button_right);
	else if (not right_button and right_button_prev)
		button_up(button_right);
	right_button_prev = right_button and (hovered or held_buttons.contains(button_right));

	// Render
	ImDrawList * draw = ImGui::GetWindowDrawList();
	ImVec2 end{pos.x + size.x, pos.y + size.y};

	if (show_desktop)
	{
		draw->AddImage(desktop, pos, end);
		if (tracking)
			draw->AddRect(pos, end, ImGui::GetColorU32(t.accent), 0, ImDrawFlags_None, 2.f);
	}
	else
	{
		ImVec4 fill = active ? t.control_active : tracking ? t.control_hovered
		                                                   : t.control;
		draw->AddRectFilled(pos, end, ImGui::GetColorU32(fill), t.card_rounding);
		draw->AddRect(pos, end, ImGui::GetColorU32(tracking ? t.accent : t.border), t.card_rounding, ImDrawFlags_None, tracking ? 3.f : 1.f);

		const std::string hint = std::string(ICON_FA_ARROW_POINTER) + "  " +
		                         _("Point here and move to control the mouse") + "\n" +
		                         _("Trigger: click / drag     B: right click     Thumbstick: scroll");
		ImVec2 hint_size = ImGui::CalcTextSize(hint.c_str());
		ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
		ImGui::RenderTextClipped(pos, end, hint.c_str(), nullptr, &hint_size, {0.5f, 0.5f});
		ImGui::PopStyleColor();
	}

	// Leave the cursor below the whole area
	ImGui::SetCursorScreenPos(area);
	ImGui::Dummy(area_size);
}

void pc_workspace::draw_keyboard(ImVec2 size)
{
	const wivrn::ui::theme & t = wivrn::ui::current();
	const float gap = 6;
	const float unit = size.x / pc_layout_units;
	const float row_height = (size.y - gap * (pc_layout.size() - 1)) / pc_layout.size();

	bool shifted = latched_modifiers.contains(KEY_LEFTSHIFT) or latched_modifiers.contains(KEY_RIGHTSHIFT);

	ImDrawList * draw = ImGui::GetWindowDrawList();
	ImVec2 origin = ImGui::GetCursorScreenPos();

	ImGui::PushID("pc_keyboard");
	for (size_t row = 0; row < pc_layout.size(); ++row)
	{
		float x = origin.x;
		float y = origin.y + row * (row_height + gap);

		for (const pc_key & k: pc_layout[row])
		{
			float w = k.width * unit;
			if (k.code == 0)
			{
				x += w;
				continue;
			}

			ImVec2 min{x + gap / 2, y};
			ImVec2 max{x + w - gap / 2, y + row_height};
			x += w;

			ImGui::SetCursorScreenPos(min);
			ImGui::PushID(k.code);
			ImGui::InvisibleButton("##key", max - min);
			bool hovered = ImGui::IsItemHovered();
			bool active = ImGui::IsItemActive();
			ImGui::PopID();

			if (ImGui::IsItemActivated())
			{
				if (is_modifier(k.code))
				{
					// Toggle the sticky modifier
					if (latched_modifiers.erase(k.code))
						key_up(k.code);
					else
					{
						latched_modifiers.insert(k.code);
						key_down(k.code);
					}
				}
				else
				{
					key_down(k.code);
				}
			}

			if (ImGui::IsItemDeactivated() and not is_modifier(k.code))
			{
				key_up(k.code);
				release_modifiers();
			}

			bool latched = latched_modifiers.contains(k.code);
			ImVec4 fill = latched ? t.accent : active ? t.control_active
			                           : hovered      ? t.control_hovered
			                                          : t.control;
			draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), t.rounding);

			const char * label = (shifted and k.shift_label) ? k.shift_label : k.label;
			ImGui::PushStyleColor(ImGuiCol_Text, latched ? t.on_accent : t.text);
			ImGui::RenderTextClipped(min, max, label, nullptr, nullptr, {0.5f, 0.5f});
			ImGui::PopStyleColor();
		}
	}
	ImGui::PopID();

	// Leave the cursor below the keyboard so the window size accounts for it
	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy(size);
}
