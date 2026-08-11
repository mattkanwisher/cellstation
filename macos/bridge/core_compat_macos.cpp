// Definitions the rpcs3 core expects its embedder (normally the Qt frontend) to
// provide. This is the macOS analogue of native/bridge/core_compat.cpp with the
// JNI/Android specifics removed. Keeping the embedder obligations in one file
// makes the list visible at a glance.

#include "stdafx.h"

#include "Emu/Cell/SPURecompiler.h"
#include "Emu/Io/pad_config.h"
#include "Input/mouse_gyro_handler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

// Input configuration globals normally defined in the desktop frontend
// (rpcs3.cpp / pad_settings_dialog.cpp).
cfg_input_configurations g_cfg_input_configs;
std::string g_input_config_override;

// Normally the frontend shows a dialog; here: log to stderr + abort. The core
// routes std::terminate here under -fno-exceptions.
[[noreturn]] void report_fatal_error(std::string_view text, bool /*is_html*/, bool /*include_help_text*/)
{
	std::fprintf(stderr, "[CellStation] FATAL: %.*s\n", static_cast<int>(text.size()), text.data());
	std::fflush(stderr);
	std::abort();
}

// Mouse-based gyro emulation is desktop-Qt-only (its implementation TU needs
// Qt); pad_thread still calls these two entry points.
void mouse_gyro_handler::set_enabled(bool)
{
}

void mouse_gyro_handler::apply_gyro(const std::shared_ptr<Pad>&)
{
}

#ifndef LLVM_AVAILABLE
// Real definition lives in SPULLVMRecompiler.cpp, which is empty without LLVM,
// while its caller (SPUCommonRecompiler.cpp) always references it. Without LLVM
// the SPU LLVM recompiler can never run, so a no-op is correct.
void spu_llvm_set_compile_context(spu_llvm_compile_context*) noexcept
{
}
#endif

// Upstream declares this in Emu/System.cpp and defines it in the Qt GUI, where
// it pumps the event loop while polling. A headless embedder just polls.
void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	while (!wrapped_op())
	{
		if (repeat_duration_ms == 0)
		{
			std::this_thread::yield();
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(repeat_duration_ms));
		}
	}
}
