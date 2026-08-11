// Pad snapshot layout — a byte-for-byte mirror of the Android app's
// chrysalis::android_pad_state (native/bridge/android_pad_handler.h), so a
// scripted test can reuse the exact same button indices on both platforms.
//
// M2 stores the latest snapshot here. Delivering it to the emulated pad needs a
// PadHandlerBase subclass registered with pad_thread (the macOS analogue of
// android_pad_handler) — that is M3 work; the storage seam below is where it
// will read from.

#ifndef CELLSTATION_MACOS_PAD_H
#define CELLSTATION_MACOS_PAD_H

#include "util/types.hpp"

#include <array>

namespace cellstation
{
	// Button/axis indices in the snapshot. MUST stay in sync with the Android
	// side (chrysalis::android_pad_button) and with the AppleScript button names
	// documented in macos/README.md.
	enum pad_button : u32
	{
		btn_none = 0,

		btn_cross,
		btn_circle,
		btn_square,
		btn_triangle,
		btn_l1,
		btn_r1,
		btn_l3,
		btn_r3,
		btn_start,
		btn_select,
		btn_ps,
		btn_dpad_up,
		btn_dpad_down,
		btn_dpad_left,
		btn_dpad_right,

		axis_l2,
		axis_r2,
		axis_ls_x_neg,
		axis_ls_x_pos,
		axis_ls_y_neg,
		axis_ls_y_pos,
		axis_rs_x_neg,
		axis_rs_x_pos,
		axis_rs_y_neg,
		axis_rs_y_pos,

		button_count
	};

	// Values are 0..255: digital buttons use 0/255, analog triggers/sticks keep
	// their resolution.
	struct pad_snapshot
	{
		std::array<u8, button_count> values{};
		bool connected = false;
	};

	// Storage seam. cs_set_pad_state()/cs_set_pad_connected() write here; a
	// future macos_pad_handler reads current_pad_state() from pad_thread's poll.
	void set_pad_state(const pad_snapshot& state);
	void set_pad_connected(bool connected);
	pad_snapshot current_pad_state();
}

#endif // CELLSTATION_MACOS_PAD_H
