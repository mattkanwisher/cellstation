#include "stdafx.h"
#include "android_pad_handler.h"

#include "Emu/Io/pad_config.h"

#include <mutex>

LOG_CHANNEL(android_pad_log, "AndroidPad");

namespace chrysalis
{
	namespace
	{
		std::mutex g_pad_mutex;
		android_pad_state g_pad_state{};
	}

	void set_android_pad_state(const android_pad_state& state)
	{
		std::lock_guard lock(g_pad_mutex);
		g_pad_state = state;
	}

	void set_android_pad_connected(bool connected)
	{
		std::lock_guard lock(g_pad_mutex);
		g_pad_state.connected = connected;
		if (!connected)
		{
			g_pad_state.values.fill(0);
		}
	}

	android_pad_state get_android_pad_state()
	{
		std::lock_guard lock(g_pad_mutex);
		return g_pad_state;
	}
}

namespace
{
	constexpr std::string_view k_device_name = "Android Gamepad";

	class android_pad_device : public PadDevice
	{
	};
}

android_pad_handler::android_pad_handler()
	: PadHandlerBase(pad_handler::keyboard)
{
	using namespace chrysalis;

	button_list =
	{
		{ btn_none,        "" },
		{ btn_cross,       "Cross" },
		{ btn_circle,      "Circle" },
		{ btn_square,      "Square" },
		{ btn_triangle,    "Triangle" },
		{ btn_l1,          "L1" },
		{ btn_r1,          "R1" },
		{ btn_l3,          "L3" },
		{ btn_r3,          "R3" },
		{ btn_start,       "Start" },
		{ btn_select,      "Select" },
		{ btn_ps,          "PS Button" },
		{ btn_dpad_up,     "Up" },
		{ btn_dpad_down,   "Down" },
		{ btn_dpad_left,   "Left" },
		{ btn_dpad_right,  "Right" },
		{ axis_l2,         "L2" },
		{ axis_r2,         "R2" },
		{ axis_ls_x_neg,   "LS X-" },
		{ axis_ls_x_pos,   "LS X+" },
		{ axis_ls_y_neg,   "LS Y-" },
		{ axis_ls_y_pos,   "LS Y+" },
		{ axis_rs_x_neg,   "RS X-" },
		{ axis_rs_x_pos,   "RS X+" },
		{ axis_rs_y_neg,   "RS Y-" },
		{ axis_rs_y_pos,   "RS Y+" },
	};

	init_configs();

	// Sticks and triggers arrive pre-scaled to 0..255 from the app.
	b_has_config = true;
	b_has_deadzones = true;
	b_has_pressure_intensity_button = false;

	m_thumb_threshold = thumb_max / 2;
}

bool android_pad_handler::Init()
{
	return true;
}

void android_pad_handler::init_config(cfg_pad* cfg)
{
	if (!cfg) return;

	using namespace chrysalis;

	cfg->ls_left.def  = ::at32(button_list, axis_ls_x_neg);
	cfg->ls_down.def  = ::at32(button_list, axis_ls_y_neg);
	cfg->ls_right.def = ::at32(button_list, axis_ls_x_pos);
	cfg->ls_up.def    = ::at32(button_list, axis_ls_y_pos);
	cfg->rs_left.def  = ::at32(button_list, axis_rs_x_neg);
	cfg->rs_down.def  = ::at32(button_list, axis_rs_y_neg);
	cfg->rs_right.def = ::at32(button_list, axis_rs_x_pos);
	cfg->rs_up.def    = ::at32(button_list, axis_rs_y_pos);

	cfg->start.def    = ::at32(button_list, btn_start);
	cfg->select.def   = ::at32(button_list, btn_select);
	cfg->ps.def       = ::at32(button_list, btn_ps);
	cfg->square.def   = ::at32(button_list, btn_square);
	cfg->cross.def    = ::at32(button_list, btn_cross);
	cfg->circle.def   = ::at32(button_list, btn_circle);
	cfg->triangle.def = ::at32(button_list, btn_triangle);
	cfg->left.def     = ::at32(button_list, btn_dpad_left);
	cfg->down.def     = ::at32(button_list, btn_dpad_down);
	cfg->right.def    = ::at32(button_list, btn_dpad_right);
	cfg->up.def       = ::at32(button_list, btn_dpad_up);
	cfg->r1.def       = ::at32(button_list, btn_r1);
	cfg->r2.def       = ::at32(button_list, axis_r2);
	cfg->r3.def       = ::at32(button_list, btn_r3);
	cfg->l1.def       = ::at32(button_list, btn_l1);
	cfg->l2.def       = ::at32(button_list, axis_l2);
	cfg->l3.def       = ::at32(button_list, btn_l3);

	cfg->pressure_intensity_button.def = ::at32(button_list, btn_none);

	cfg->lstickdeadzone.def    = 40;
	cfg->rstickdeadzone.def    = 40;
	cfg->ltriggerthreshold.def = 0;
	cfg->rtriggerthreshold.def = 0;
	cfg->lpadsquircling.def    = 0;
	cfg->rpadsquircling.def    = 0;

	cfg->from_default();
}

std::vector<pad_list_entry> android_pad_handler::list_devices()
{
	// A handheld exposes exactly one built-in controller; external pads are
	// merged into the same Android input stream by the app.
	return { pad_list_entry(std::string(k_device_name), false) };
}

std::shared_ptr<PadDevice> android_pad_handler::get_device(const std::string& device)
{
	// A handheld has exactly one input stream, and pad_thread's default config
	// binds the historical "Keyboard" device name for this handler slot, so
	// accept any non-empty name rather than forcing users to re-map.
	if (device.empty())
	{
		return nullptr;
	}

	return std::make_shared<android_pad_device>();
}

PadHandlerBase::connection android_pad_handler::update_connection(const std::shared_ptr<PadDevice>& device)
{
	if (!device)
	{
		return connection::disconnected;
	}

	// Respect the app-side connected flag so a deliberate disconnect->connect
	// cycle (EmuBridge.setPadConnected false then true) produces a real
	// connection transition. rpcs3's pad thread reacts to that edge by setting
	// CELL_PAD_STATUS_ASSIGN_CHANGES on the next poll, which is what a game's
	// "press START to assign a controller" screen waits for -- an injected
	// button press alone never satisfies it. A physical press happens to be the
	// first thing that drives this on real hardware; headless input needs to
	// reproduce the transition explicitly.
	return chrysalis::get_android_pad_state().connected ? connection::connected : connection::disconnected;
}

std::unordered_map<u32, u16> android_pad_handler::get_button_values(const std::shared_ptr<PadDevice>& device)
{
	std::unordered_map<u32, u16> values;

	if (!device)
	{
		return values;
	}

	const chrysalis::android_pad_state state = chrysalis::get_android_pad_state();

	for (u32 i = chrysalis::btn_none + 1; i < chrysalis::button_count; ++i)
	{
		values[i] = state.values[i];
	}

	return values;
}

bool android_pad_handler::get_is_left_trigger(const std::shared_ptr<PadDevice>&, u32 keyCode)
{
	return keyCode == chrysalis::axis_l2;
}

bool android_pad_handler::get_is_right_trigger(const std::shared_ptr<PadDevice>&, u32 keyCode)
{
	return keyCode == chrysalis::axis_r2;
}

bool android_pad_handler::get_is_left_stick(const std::shared_ptr<PadDevice>&, u32 keyCode)
{
	switch (keyCode)
	{
	case chrysalis::axis_ls_x_neg:
	case chrysalis::axis_ls_x_pos:
	case chrysalis::axis_ls_y_neg:
	case chrysalis::axis_ls_y_pos:
		return true;
	default:
		return false;
	}
}

bool android_pad_handler::get_is_right_stick(const std::shared_ptr<PadDevice>&, u32 keyCode)
{
	switch (keyCode)
	{
	case chrysalis::axis_rs_x_neg:
	case chrysalis::axis_rs_x_pos:
	case chrysalis::axis_rs_y_neg:
	case chrysalis::axis_rs_y_pos:
		return true;
	default:
		return false;
	}
}

std::shared_ptr<PadHandlerBase> make_android_pad_handler()
{
	android_pad_log.notice("Creating Android gamepad handler");
	return std::make_shared<android_pad_handler>();
}
