// CellStation macOS bridge — C-ABI implementation over the unmodified rpcs3
// core. Adapted from the Android bridge (native/bridge/chrysalis_boot.cpp +
// chrysalis_jni.cpp + the main-thread pump in bridge.cpp), with JNI and Android
// specifics removed. See cellstation_bridge.h for the contract.
//
// CellStation is based on RPCS3 (GPL-2.0), (c) RPCS3 team and contributors.

#include "stdafx.h"

#include "util/types.hpp"
#include "util/logs.hpp"
#include "util/sysinfo.hpp"
#include "Utilities/File.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"
#include "Emu/VFS.h"
#include "Emu/savestate_utils.hpp"
#include "Emu/IdManager.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/MouseHandler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Emu/Audio/Cubeb/CubebBackend.h"
#include "Emu/Audio/Cubeb/cubeb_enumerator.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Input/pad_thread.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Crypto/unself.h"
#include "util/video_source.h"
#include "rpcs3_version.h"

#include "cellstation_bridge.h"
#include "macos_pad.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <condition_variable>

LOG_CHANNEL(cellstation_log, "CELLSTATION");

void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op); // core_compat_macos.cpp

// Firmware module list (Emu/Cell/lv2/sys_prx.cpp). Declared at global scope so
// it resolves to the real symbol, not an anonymous-namespace one.
extern const std::map<std::string_view, int> g_prx_list;

namespace
{
	// ---- stderr log sink ---------------------------------------------------
	class stderr_listener final : public logs::listener
	{
	public:
		void log(u64, const logs::message& msg, std::string_view prefix, std::string_view text) override
		{
			const char* tag = "I";
			switch (static_cast<logs::level>(msg))
			{
			case logs::level::always:  tag = "="; break;
			case logs::level::fatal:   tag = "F"; break;
			case logs::level::error:   tag = "E"; break;
			case logs::level::todo:    tag = "T"; break;
			case logs::level::success: tag = "+"; break;
			case logs::level::warning: tag = "W"; break;
			case logs::level::notice:  tag = "N"; break;
			case logs::level::trace:   return; // too noisy for the console
			}
			if (prefix.empty())
				std::fprintf(stderr, "[%s] %.*s\n", tag, static_cast<int>(text.size()), text.data());
			else
				std::fprintf(stderr, "[%s] {%.*s} %.*s\n", tag,
					static_cast<int>(prefix.size()), prefix.data(),
					static_cast<int>(text.size()), text.data());
		}
	};

	stderr_listener g_stderr;

	// ---- main-thread work pump --------------------------------------------
	// The core posts work to the "main thread" via call_from_main_thread;
	// cs_run_main_loop() drains this queue. Mirrors native/bridge/bridge.cpp.
	struct main_pump
	{
		std::mutex mtx;
		std::condition_variable cv;
		std::deque<std::function<void()>> q;
		atomic_t<bool> quit{false};

		void push(std::function<void()> f)
		{
			{
				std::lock_guard lock(mtx);
				q.emplace_back(std::move(f));
			}
			cv.notify_one();
		}

		void run()
		{
			for (;;)
			{
				std::function<void()> f;
				{
					std::unique_lock lock(mtx);
					cv.wait(lock, [this] { return quit || !q.empty(); });
					if (quit && q.empty())
						return;
					f = std::move(q.front());
					q.pop_front();
				}
				f();
			}
		}

		void stop()
		{
			quit = true;
			cv.notify_one();
		}
	};

	main_pump g_pump;

	// ---- headless GS frame -------------------------------------------------
	// The Null renderer still asks for a frame object; give it an inert one.
	class null_gs_frame final : public GSFrameBase
	{
	public:
		void close() override {}
		void reset() override {}
		bool shown() override { return true; }
		void hide() override {}
		void show() override {}
		void toggle_fullscreen() override {}
		void delete_context(draw_context_t) override {}
		draw_context_t make_context() override { return nullptr; }
		void set_current(draw_context_t) override {}
		void flip(draw_context_t, bool) override {}
		int client_width() override { return 1280; }
		int client_height() override { return 720; }
		f64 client_display_rate() override { return 60.; }
		bool has_alpha() override { return false; }
		display_handle_t handle() const override { return {}; }
		bool can_consume_frame() const override { return false; }
		void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
		void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
		void update_title(double) override {}
	};

	// ---- pad snapshot storage ---------------------------------------------
	std::mutex g_pad_mtx;
	cellstation::pad_snapshot g_pad_state;

	// ---- init state --------------------------------------------------------
	bool g_initialized = false;
	std::string g_status_scratch;

	void init_callbacks()
	{
		EmuCallbacks callbacks{};

		callbacks.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
		{
			g_pump.push([func = std::move(func), wake_up]()
			{
				func();
				if (wake_up)
				{
					*wake_up = true;
					wake_up->notify_one();
				}
			});
		};

		callbacks.try_to_quit = [](bool force_quit, std::function<void()> on_exit) -> bool
		{
			if (force_quit && on_exit)
				on_exit();
			return force_quit;
		};

		callbacks.update_emu_settings = []() {};
		callbacks.save_emu_settings = []() { Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID()); };

		callbacks.init_kb_handler = []() { ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager())); };
		callbacks.init_mouse_handler = []() { ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager())); };

		callbacks.init_pad_handler = [](std::string_view title_id)
		{
			ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id));
			qt_events_aware_op(0, []() { return !!pad::g_started; });
		};

		callbacks.init_gs_render = [](utils::serial* ar)
		{
			// Headless bring-up: always the Null renderer (the Vulkan/Metal path
			// arrives with the window in M3).
			if (const video_renderer type = g_cfg.video.renderer; type != video_renderer::null)
				cellstation_log.warning("Configured renderer is %s; forcing Null for headless bring-up", type);

			g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
		};

		callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase> { return std::make_unique<null_gs_frame>(); };
		callbacks.close_gs_frame = []() {};

		callbacks.get_audio = []() -> std::shared_ptr<AudioBackend>
		{
			std::shared_ptr<AudioBackend> result;
			switch (g_cfg.audio.renderer.get())
			{
			case audio_renderer::null: result = std::make_shared<NullAudioBackend>(); break;
			default: result = std::make_shared<CubebBackend>(); break;
			}
			if (!result->Initialized())
			{
				cellstation_log.error("Audio backend %s failed to initialize; falling back to Null", result->GetName());
				result = std::make_shared<NullAudioBackend>();
			}
			return result;
		};

		callbacks.get_audio_enumerator = [](u64 renderer) -> std::shared_ptr<audio_device_enumerator>
		{
			switch (static_cast<audio_renderer>(renderer))
			{
			case audio_renderer::null: return std::make_shared<null_enumerator>();
			default: return std::make_shared<cubeb_enumerator>();
			}
		};

		callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base> { return std::make_shared<null_camera_handler>(); };
		callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base> { return std::make_shared<null_music_handler>(); };

		callbacks.get_msg_dialog                 = []() -> std::shared_ptr<MsgDialogBase> { return {}; };
		callbacks.get_osk_dialog                 = []() -> std::shared_ptr<OskDialogBase> { return {}; };
		callbacks.get_save_dialog                = []() -> std::unique_ptr<SaveDialogBase> { return {}; };
		callbacks.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return {}; };

		callbacks.on_run    = [](bool) {};
		callbacks.on_pause  = []() {};
		callbacks.on_resume = []() {};
		callbacks.on_stop   = []() {};
		callbacks.on_ready  = []() {};

		callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>> closed_successfully, int seconds)
		{
			if (!closed_successfully || !*closed_successfully)
				cellstation_log.error("Emulation stop is taking too long (%d s)...", seconds);
		};

		callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};

		callbacks.enable_disc_eject  = [](bool) {};
		callbacks.enable_disc_insert = [](bool) {};
		callbacks.on_missing_fw = []() { cellstation_log.error("No PS3 firmware installed"); };
		callbacks.handle_taskbar_progress = [](s32, s32) {};

		callbacks.get_localized_string    = [](localized_string_id, const char*) -> std::string { return {}; };
		callbacks.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return {}; };
		callbacks.get_localized_setting   = [](const cfg::_base*, u32) -> std::string { return {}; };

		callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
		callbacks.add_breakpoint = [](u32) {};

		callbacks.display_sleep_control_supported = []() { return false; };
		callbacks.enable_display_sleep = [](bool) {};
		callbacks.check_microphone_permissions = []() {};
		callbacks.make_video_source = []() -> std::unique_ptr<video_source> { return nullptr; };

		callbacks.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) -> bool { return false; };
		callbacks.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) -> bool { return false; };

		// Remaining EmuCallbacks members. Every std::function must be callable:
		// the tree is -fno-exceptions, so an empty one aborts via bad_function_call
		// the moment the core invokes it (e.g. the boot overlay calls get_font_dirs).
		callbacks.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return {}; };
		callbacks.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return {}; };
		callbacks.get_photo_path = [](std::string_view) -> std::string { return {}; };
		// The overlay renderer throws a fatal if it can't find any font. macOS
		// always ships these, so offer them to every lookup.
		callbacks.get_font_dirs = []() -> std::vector<std::string>
		{
			return {"/System/Library/Fonts/", "/System/Library/Fonts/Supplemental/", "/Library/Fonts/"};
		};
		callbacks.on_install_pkgs = [](const std::vector<std::string>&) { return false; };
		callbacks.enable_gamemode = [](bool) {};
		callbacks.get_database_config = [](const std::string&) -> std::string { return {}; };

		Emu.SetCallbacks(std::move(callbacks));
	}

	// Force HLE for every firmware module when no firmware is installed, so a
	// homebrew ELF boots without /dev_flash. Mirrors native/bridge/bridge.cpp.
	void ensure_homebrew_hle()
	{
		std::set<std::string> hle_all;
		for (const auto& [name, flag] : g_prx_list)
			hle_all.emplace(std::string(name) + ":hle");

		const bool no_fw = utils::get_firmware_version().empty();
		const bool is_hle_all = g_cfg.core.libraries_control.get_set() == hle_all;

		if (no_fw && !is_hle_all)
		{
			cellstation_log.notice("No firmware installed: forcing HLE firmware modules for homebrew boot");
			g_cfg.core.libraries_control.set_set(std::move(hle_all));
			Emulator::SaveSettings(g_cfg.to_string(), {});
		}
		else if (!no_fw && is_hle_all)
		{
			g_cfg.core.libraries_control.set_set({});
			Emulator::SaveSettings(g_cfg.to_string(), {});
		}
	}

	// Qt-free port of chrysalis_jni install_firmware. Returns "" on success.
	std::string install_firmware(const std::string& path)
	{
		if (path.empty()) return "The provided path is empty.";

		fs::file pup_f(path);
		if (!pup_f) return "The selected firmware file couldn't be opened.";

		pup_object pup(std::move(pup_f));
		switch (pup.operator pup_error())
		{
		case pup_error::header_read: return "The provided file is empty.";
		case pup_error::header_magic: return "The provided file is not a PUP file.";
		case pup_error::expected_size: return "The provided file is incomplete.";
		case pup_error::header_file_count:
		case pup_error::file_entries:
		case pup_error::stream: return "The provided file is corrupted: " + pup.get_formatted_error();
		case pup_error::hash_mismatch: return "The provided file's contents are corrupted (hash check failed).";
		case pup_error::ok: break;
		}

		fs::file update_files_f = pup.get_file(0x300);
		if (!update_files_f || !update_files_f.size()) return "Couldn't find the installation packages database.";

		tar_object update_files(update_files_f);
		auto update_filenames = update_files.get_filenames();
		update_filenames.erase(std::remove_if(update_filenames.begin(), update_filenames.end(),
			[](const std::string& s) { return s.find("dev_flash_") == umax; }), update_filenames.end());
		if (update_filenames.empty()) return "No dev_flash_* packages were found in the file.";

		vfs::mount("/dev_flash", g_cfg_vfs.get_dev_flash());

		for (const auto& update_filename : update_filenames)
		{
			auto update_file_stream = update_files.get_file(update_filename);
			if (update_file_stream->m_file_handler)
				update_file_stream->m_file_handler->handle_file_op(*update_file_stream, 0, update_file_stream->get_size(umax), nullptr);

			fs::file update_file = fs::make_stream(std::move(update_file_stream->data));

			SCEDecrypter self_dec(update_file);
			self_dec.LoadHeaders();
			self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
			self_dec.DecryptData();

			auto dev_flash_tar_f = self_dec.MakeFile();
			if (dev_flash_tar_f.size() < 3) return "Firmware could not be decompressed.";

			tar_object dev_flash_tar(dev_flash_tar_f[2]);
			if (!dev_flash_tar.extract()) return "The firmware contents could not be extracted.";
		}

		cellstation_log.success("Installed PS3 firmware version %s", utils::get_firmware_version());
		return {};
	}

	// Newest savestate file for the given slot's title/boot, honouring the
	// possible compression suffixes rpcs3 writes.
	std::string newest_savestate(const std::string& title, const std::string& boot)
	{
		std::string base = get_savestate_file(title, boot, 0); // ...".SAVESTAT"
		for (const char* suffix : {"", ".zst", ".gz"})
		{
			const std::string p = base + suffix;
			if (fs::is_file(p)) return p;
		}
		return {};
	}
}

namespace cellstation
{
	void set_pad_state(const pad_snapshot& state)
	{
		std::lock_guard lock(g_pad_mtx);
		g_pad_state = state;
	}
	void set_pad_connected(bool connected)
	{
		std::lock_guard lock(g_pad_mtx);
		g_pad_state.connected = connected;
	}
	pad_snapshot current_pad_state()
	{
		std::lock_guard lock(g_pad_mtx);
		return g_pad_state;
	}
}

// ---- C ABI -----------------------------------------------------------------

extern "C" {

int cs_initialize(const char* data_dir, const char* user)
{
	if (g_initialized) return 1;

	// Isolated app-data tree when requested: point HOME at it so the core's
	// macOS path logic (Utilities/File.cpp) nests config + cache underneath,
	// keeping a dev build clear of any real rpcs3 install. Must happen before
	// the first fs::get_config_dir() (a magic static).
	if (data_dir && *data_dir)
	{
		fs::create_path(data_dir);
		setenv("HOME", data_dir, 1);
	}

	logs::listener::add(&g_stderr);

	init_callbacks();
	Emu.SetHasGui(false);
	Emu.SetHeadless(true);
	Emu.SetUsr((user && *user) ? user : "00000001");
	Emu.Init();

#ifndef LLVM_AVAILABLE
	// This build has no LLVM (interpreter-only). rpcs3's default config selects
	// the LLVM recompilers for PPU and SPU; honoring that with no LLVM leaves
	// null recompiler objects that fault on teardown. Force the interpreters
	// (_static) — the correct, reference execution path for this build — and
	// persist so Emu.Load (which re-reads config.yml at boot) sees them too.
	if (g_cfg.core.ppu_decoder.get() != ppu_decoder_type::_static ||
	    g_cfg.core.spu_decoder.get() != spu_decoder_type::_static)
	{
		g_cfg.core.ppu_decoder.set(ppu_decoder_type::_static);
		g_cfg.core.spu_decoder.set(spu_decoder_type::_static);
		Emulator::SaveSettings(g_cfg.to_string(), {});
		cellstation_log.notice("No LLVM in this build: forced PPU/SPU interpreter decoders");
	}
#endif

	g_initialized = true;
	cellstation_log.success("CellStation core initialized (config: %s)", fs::get_config_dir());
	return 1;
}

void cs_run_main_loop(void)
{
	thread_ctrl::scoped_priority high_prio(+1);
	g_pump.run();
}

void cs_stop_main_loop(void)
{
	g_pump.stop();
}

int cs_boot(const char* path)
{
	if (!path || !*path) return static_cast<int>(game_boot_result::invalid_file_or_folder);

	const std::string p = path;

	atomic_t<u32> done = 0;
	game_boot_result result = game_boot_result::generic_error;

	g_pump.push([&]()
	{
		ensure_homebrew_hle();
		Emu.SetForceBoot(true);
		result = Emu.BootGame(p, "", true);
		done = 1;
		done.notify_one();
	});

	done.wait(0);

	if (is_error(result))
		cellstation_log.error("Boot failed for '%s': %s", p, result);
	else
		cellstation_log.success("Boot OK: %s", p); // stable marker for scripts

	return static_cast<int>(result);
}

void cs_kill(void)
{
	if (Emu.IsStopped()) return;

	// Synchronous: post the Kill and wait until the core is fully stopped, so a
	// subsequent cs_boot() doesn't race an in-flight teardown and get
	// still_running. cs_kill is called off the main-loop thread (the pump drains
	// the posted Kill), so waiting here can't deadlock.
	atomic_t<u32> done = 0;
	g_pump.push([&]() { Emu.Kill(false); done = 1; done.notify_one(); });
	done.wait(0);
	qt_events_aware_op(5, []() { return Emu.IsStopped(true); });
}

void cs_pause(void)  { g_pump.push([]() { Emu.Pause(); }); }
void cs_resume(void) { g_pump.push([]() { Emu.Resume(); }); }
void cs_reset(void)  { g_pump.push([]() { Emu.Restart(); }); }

int cs_get_state(void) { return static_cast<int>(Emu.GetStatus(false)); }

const char* cs_status_text(void)
{
	switch (Emu.GetStatus(true))
	{
	case system_state::stopped:  g_status_scratch = "stopped";  break;
	case system_state::running:  g_status_scratch = "running";  break;
	case system_state::paused:   g_status_scratch = "paused";   break;
	case system_state::ready:    g_status_scratch = "ready";    break;
	case system_state::starting: g_status_scratch = "starting"; break;
	case system_state::stopping: g_status_scratch = "stopping"; break;
	default:                     g_status_scratch = "unknown";  break;
	}
	return g_status_scratch.c_str();
}

double cs_get_fps(void)
{
	// The perf overlay's fps counter is an RSX-thread quantity; it is not
	// meaningful under the Null renderer. Real values arrive with the M3
	// Metal render path.
	return 0.0;
}

const char* cs_install_firmware(const char* pup_path)
{
	static std::string err;
	err = install_firmware(pup_path ? pup_path : "");
	return err.c_str();
}

const char* cs_firmware_version(void)
{
	static std::string ver;
	ver = utils::get_firmware_version();
	return ver.c_str();
}

void cs_set_pad_state(const uint8_t* bytes, int len)
{
	if (!bytes || len <= 0) return;
	cellstation::pad_snapshot s;
	s.connected = true;
	const int n = std::min<int>(len, static_cast<int>(s.values.size()));
	for (int i = 0; i < n; ++i) s.values[i] = bytes[i];
	cellstation::set_pad_state(s);
}

void cs_set_pad_connected(int connected)
{
	cellstation::set_pad_connected(connected != 0);
}

int cs_save_state(int slot)
{
	if (Emu.IsStopped()) return 0;

	// Capture before Kill clears them.
	const std::string title = Emu.GetTitleID();
	const std::string boot  = Emu.GetBoot();

	atomic_t<u32> done = 0;
	g_pump.push([&]()
	{
		Emu.Kill(false /*allow_autoexit*/, true /*savestate*/);
		done = 1;
		done.notify_one();
	});
	done.wait(0);

	// Wait until fully stopped so the file is on disk.
	qt_events_aware_op(5, []() { return Emu.IsStopped(true); });

	const std::string produced = newest_savestate(title, boot);
	if (produced.empty())
	{
		cellstation_log.error("Save state: no savestate file was produced");
		return 0;
	}

	const std::string slot_path = rpcs3::utils::get_savestates_dir() + fmt::format("cellstation_slot_%d.SAVESTAT", slot);
	fs::create_path(rpcs3::utils::get_savestates_dir());
	if (!fs::copy_file(produced, slot_path, true /*overwrite*/))
	{
		cellstation_log.error("Save state: failed to copy %s -> %s", produced, slot_path);
		return 0;
	}

	cellstation_log.success("Saved state to slot %d (%s)", slot, slot_path);
	return 1;
}

int cs_load_state(int slot)
{
	const std::string slot_path = rpcs3::utils::get_savestates_dir() + fmt::format("cellstation_slot_%d.SAVESTAT", slot);
	if (!fs::is_file(slot_path))
	{
		cellstation_log.error("Load state: slot %d has no savestate (%s)", slot, slot_path);
		return 0;
	}

	atomic_t<u32> done = 0;
	game_boot_result result = game_boot_result::generic_error;
	g_pump.push([&]()
	{
		Emu.SetForceBoot(true);
		result = Emu.BootGame(slot_path, "", true);
		done = 1;
		done.notify_one();
	});
	done.wait(0);

	if (is_error(result))
	{
		cellstation_log.error("Load state slot %d failed: %s", slot, result);
		return 0;
	}
	cellstation_log.success("Loaded state from slot %d", slot);
	return 1;
}

void cs_surface_changed(void* /*metal_layer*/, int /*width*/, int /*height*/)
{
	// M3: hand the CAMetalLayer to the Vulkan (MoltenVK) swapchain path.
}

int cs_capture_screenshot(const char* /*png_path*/)
{
	// M3: read back the last presented Metal frame. Null renderer has none.
	return 0;
}

const char* cs_version(void)
{
	static std::string ver;
	ver = rpcs3::get_verbose_version();
	return ver.c_str();
}

} // extern "C"
