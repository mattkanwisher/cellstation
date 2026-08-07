// CellStation JNI bridge: thin marshalling layer between the Kotlin app and
// the unmodified RPCS3 core (rpcs3_emu). Modeled on upstream's
// headless_application/main_application callback wiring and the archived
// rpcs3-android alpha's JNI surface (see docs/PATCH-INVENTORY.md).
//
// CellStation is based on RPCS3 (GPL-2.0), (c) RPCS3 team and contributors.

#include "stdafx.h"
#include "util/types.hpp"
#include "util/logs.hpp"
#include "util/asm.hpp"
#include "Utilities/Thread.h"
#include "Utilities/File.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/system_progress.hpp"
#include "Emu/vfs_config.h"
#include "Emu/IdManager.h"
#include "Emu/VFS.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/MouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Emu/Audio/Cubeb/CubebBackend.h"
#include "Emu/Audio/Cubeb/cubeb_enumerator.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/Null/NullGSRender.h"
#if defined(HAVE_VULKAN)
#include "Emu/RSX/VK/VKGSRender.h"
#endif
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Input/pad_thread.h"
#include "android_pad_handler.h"
#include "Loader/ISO.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Crypto/unself.h"
#include "Crypto/key_vault.h"
#include "util/video_source.h"
#include "rpcs3_version.h"

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctime>

#include <deque>

#include <mutex>
#include <condition_variable>

#include "Emu/Io/pad_config.h"
#include "Input/mouse_gyro_handler.h"

LOG_CHANNEL(cellstation_log, "CELLSTATION");

namespace chrysalis { std::string describe_current_exception(); } // fatal_report.cpp

// mouse_gyro_handler's implementation lives in a Qt translation unit upstream
// (QEvent-driven desktop feature). pad_thread only needs these two members;
// mouse-based gyro stays inert on Android.
void mouse_gyro_handler::set_enabled(bool enabled)
{
	m_enabled = enabled;
}

void mouse_gyro_handler::apply_gyro(const std::shared_ptr<Pad>&)
{
}

// Upstream implements this in the Qt layer (main_window.cpp) with a
// QEventLoop on the GUI thread; emucore calls it to poll a condition.
// This mirrors upstream's own non-GUI-thread branch.
void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	ensure(wrapped_op);

	while (!wrapped_op())
	{
		if (repeat_duration_ms == 0)
		{
			std::this_thread::yield();
		}
		else if (thread_ctrl::get_current())
		{
			thread_ctrl::wait_for(repeat_duration_ms * 1000);
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(repeat_duration_ms));
		}
	}
}

// Defaults cached by Emu.Init() (Emu/System.cpp), as used by rpcs3qt.
extern std::string g_cfg_defaults;

// Defined in Utilities/File.cpp for the Android embedder to fill in.
extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;

// Upstream defines these in Qt-side translation units (rpcs3.cpp,
// rpcs3qt/pad_settings_dialog.cpp); the emucore/Input objects reference them.
std::string g_input_config_override;
cfg_input_configurations g_cfg_input_configs;

// Rebuilds the RSX performance overlay from g_cfg. Declared the way
// main_application.cpp declares it (rather than pulling in the overlay
// headers), since that is the only piece of OnEmuSettingsChange we need.
namespace rsx::overlays
{
	extern void reset_performance_overlay();
}

// RtMidi's Android backend calls JNI_GetCreatedJavaVMs, which is not a public
// NDK export. We know our VM from JNI_OnLoad, so provide the symbol here.
static JavaVM* g_java_vm = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
	g_java_vm = vm;
	return JNI_VERSION_1_6;
}

extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vm_buf, jsize buf_len, jsize* n_vms)
{
	jsize count = 0;
	if (g_java_vm && buf_len > 0)
	{
		vm_buf[0] = g_java_vm;
		count = 1;
	}
	if (n_vms)
		*n_vms = count;
	return JNI_OK;
}

[[noreturn]] void report_fatal_error(std::string_view text, bool /*is_html*/ = false, bool /*include_help_text*/ = true)
{
	__android_log_print(ANDROID_LOG_FATAL, "CellStation", "FATAL: %.*s", static_cast<int>(text.size()), text.data());

	// The core routes std::terminate here, so an unhandled exception is still
	// active and carries the only description of what actually went wrong.
	// Without this the crash reads as a bare "abnormally terminated".
	if (const std::string what = chrysalis::describe_current_exception(); !what.empty())
	{
		__android_log_print(ANDROID_LOG_FATAL, "CellStation", "FATAL: unhandled exception -> %s", what.c_str());
	}

	std::abort();
}

namespace
{
	// ---- logcat sink -------------------------------------------------------
	class logcat_listener final : public logs::listener
	{
	public:
		void log(u64 /*stamp*/, const logs::message& msg, std::string_view prefix, std::string_view text) override
		{
			int prio = ANDROID_LOG_INFO;
			switch (static_cast<logs::level>(msg))
			{
			case logs::level::always:  prio = ANDROID_LOG_INFO; break;
			case logs::level::fatal:   prio = ANDROID_LOG_FATAL; break;
			case logs::level::error:   prio = ANDROID_LOG_ERROR; break;
			case logs::level::todo:    prio = ANDROID_LOG_WARN; break;
			case logs::level::success: prio = ANDROID_LOG_INFO; break;
			case logs::level::warning: prio = ANDROID_LOG_WARN; break;
			case logs::level::notice:  prio = ANDROID_LOG_DEBUG; break;
			case logs::level::trace:   prio = ANDROID_LOG_VERBOSE; break;
			}
			__android_log_print(prio, "RPCS3", "%.*s: %.*s",
				static_cast<int>(prefix.size()), prefix.data(),
				static_cast<int>(text.size()), text.data());
		}
	};

	logcat_listener g_logcat;

	// ---- "main thread" pump ------------------------------------------------
	// The core expects a main/UI thread it can queue work onto
	// (EmuCallbacks::call_from_main_thread). A dedicated Java thread calls
	// runMainLoop() which drains this queue forever.
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
	};

	main_pump g_pump;

	// ---- surface state -----------------------------------------------------
	atomic_t<ANativeWindow*> g_native_window{nullptr};

	// ---- ADPF (Android Dynamic Performance Framework) ----------------------
	//
	// The power HAL cannot distinguish a latency-sensitive workload from a
	// merely busy one. A hint session names the threads that have to meet a
	// deadline and reports how long their work actually took, so the governor
	// holds clocks rather than inferring demand from utilisation alone.
	//
	// That distinction matters more here than it might elsewhere: profiling
	// showed the SPU threads spend much of their time spinning on range locks
	// (docs/PROFILE-doa5.md), so a utilisation-based governor sees fully busy
	// cores that are achieving nothing and can draw the opposite of the right
	// conclusion.
	//
	// The NDK in this toolchain ships no <android/performance_hint.h>, so the
	// API is resolved from libandroid.so at runtime. It is API 31+; on older
	// devices the symbols are simply absent and everything here is a no-op.
	//
	// This lives on the native side rather than in Kotlin because the report
	// is only meaningful once per frame, and the flip callback below is the
	// only place that knows when a frame ended.
	namespace adpf
	{
		struct manager_t;
		struct session_t;

		manager_t* (*get_manager)() = nullptr;
		session_t* (*create_session)(manager_t*, const s32*, usz, s64) = nullptr;
		int (*report_actual)(session_t*, s64) = nullptr;
		int (*set_threads)(session_t*, const s32*, usz) = nullptr;
		void (*close_session)(session_t*) = nullptr;

		session_t* g_session = nullptr;

		// Set from the Kotlin side before boot. Experimental, so it is worth
		// being able to A/B a scene with and without it.
		atomic_t<bool> g_enabled{true};

		// The set the session was last told about, kept sorted so it can be
		// compared cheaply against a fresh scan.
		std::vector<s32> g_tids;

		// 60 Hz. Reporting an actual duration longer than this is the signal
		// that asks for more clock, which is the honest state of affairs for a
		// game we cannot yet run at full speed.
		constexpr s64 target_ns = 16'666'666;

		s64 now_ns()
		{
			timespec ts{};
			clock_gettime(CLOCK_MONOTONIC, &ts);
			return static_cast<s64>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
		}

		// The emulated CPUs and the renderer, by thread name. These do not
		// exist until a game is running, which is why the session is built on
		// the first flip rather than at startup.
		std::vector<s32> emu_thread_ids()
		{
			std::vector<s32> out;

			DIR* d = opendir("/proc/self/task");
			if (!d) return out;

			while (dirent* e = readdir(d))
			{
				const int tid = atoi(e->d_name);
				if (tid <= 0) continue;

				char path[64]{};
				snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);

				const int fd = open(path, O_RDONLY);
				if (fd < 0) continue;

				char comm[32]{};
				const ssize_t n = read(fd, comm, sizeof(comm) - 1);
				close(fd);
				if (n <= 0) continue;

				if (!std::strncmp(comm, "PPU[", 4) || !std::strncmp(comm, "SPU[", 4) ||
				    !std::strncmp(comm, "rsx::thread", 11))
				{
					out.push_back(tid);
				}
			}

			closedir(d);
			std::sort(out.begin(), out.end());
			return out;
		}

		bool resolve()
		{
			void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
			if (!lib) return false;

			get_manager    = reinterpret_cast<decltype(get_manager)>(dlsym(lib, "APerformanceHint_getManager"));
			create_session = reinterpret_cast<decltype(create_session)>(dlsym(lib, "APerformanceHint_createSession"));
			report_actual  = reinterpret_cast<decltype(report_actual)>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
			close_session  = reinterpret_cast<decltype(close_session)>(dlsym(lib, "APerformanceHint_closeSession"));

			// API 34. Without it the session can only be re-made from scratch.
			set_threads    = reinterpret_cast<decltype(set_threads)>(dlsym(lib, "APerformanceHint_setThreads"));

			if (!get_manager || !create_session || !report_actual)
			{
				cellstation_log.notice("ADPF: unavailable (needs API 31)");
				return false;
			}

			return true;
		}

		// The thread set is not stable: SPU threads are created as the game
		// starts using them, long after the first frame is presented, and come
		// and go over a session. A set fixed at the first flip would name the
		// PPU and RSX threads only — which is precisely the case this misses,
		// since the SPU threads are the ones under real deadline pressure.
		void sync_threads()
		{
			std::vector<s32> tids = emu_thread_ids();

			if (tids.empty() || tids == g_tids)
				return;

			if (g_session && set_threads)
			{
				if (set_threads(g_session, tids.data(), tids.size()) == 0)
				{
					cellstation_log.notice("ADPF: tracking %u threads (was %u)", tids.size(), g_tids.size());
					g_tids = std::move(tids);
					return;
				}
			}

			// No setThreads, or it refused: rebuild the session.
			if (g_session && close_session)
				close_session(g_session);
			g_session = nullptr;

			manager_t* mgr = get_manager();
			if (!mgr)
			{
				cellstation_log.notice("ADPF: no hint manager (device opted out)");
				return;
			}

			g_session = create_session(mgr, tids.data(), tids.size(), target_ns);

			if (g_session)
			{
				cellstation_log.notice("ADPF: hint session for %u threads, target %d us", tids.size(), static_cast<int>(target_ns / 1000));
				g_tids = std::move(tids);
			}
			else
			{
				cellstation_log.notice("ADPF: createSession failed for %u threads", tids.size());
				g_tids.clear();
			}
		}

		void stop();

		// Called from the RSX thread on every flip, so no synchronisation is
		// needed on the statics below.
		void on_flip()
		{
			static bool usable = [] { return resolve(); }();
			static s64 last = 0;
			static u32 frames = 0;

			if (!usable) return;

			if (!g_enabled)
			{
				// Toggled off mid-session: drop the hints rather than leaving a
				// stale session reporting nothing, which would keep whatever
				// boost it last asked for.
				if (g_session) stop();
				return;
			}

			// Rescanning /proc costs a few dozen small reads, so do it every
			// couple of seconds rather than per frame. Startup is when the set
			// actually churns, so scan eagerly for the first few frames.
			if (frames < 8 || frames % 240 == 0)
				sync_threads();

			frames++;

			const s64 now = now_ns();

			if (g_session && last)
			{
				const s64 elapsed = now - last;

				// A frame boundary that long means the emulator stalled on
				// something other than rendering — shader compilation, or a
				// load. Reporting it would tell the governor a deadline was
				// missed by seconds and is not useful.
				if (elapsed > 0 && elapsed < 1'000'000'000)
					report_actual(g_session, elapsed);
			}

			last = now;
		}

		void stop()
		{
			if (g_session && close_session)
				close_session(g_session);
			g_session = nullptr;
			g_tids.clear();
		}
	}

	// ---- GS frame over ANativeWindow --------------------------------------
	class android_gs_frame final : public GSFrameBase
	{
	public:
		void close() override {}
		void reset() override {}
		bool shown() override { return g_native_window != nullptr; }
		void hide() override {}
		void show() override {}
		void toggle_fullscreen() override {}

		void delete_context(draw_context_t) override {}
		draw_context_t make_context() override { return nullptr; }
		void set_current(draw_context_t) override {}
		void flip(draw_context_t, bool /*skip_frame*/) override { adpf::on_flip(); }

		int client_width() override
		{
			if (ANativeWindow* w = g_native_window)
				return ANativeWindow_getWidth(w);
			return 1280;
		}

		int client_height() override
		{
			if (ANativeWindow* w = g_native_window)
				return ANativeWindow_getHeight(w);
			return 720;
		}

		f64 client_display_rate() override { return 60.; }
		bool has_alpha() override { return false; }

		display_handle_t handle() const override
		{
			return display_handle_t{g_native_window.load()};
		}

		bool can_consume_frame() const override { return false; }
		void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
		void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
		void update_title(double) override {}
	};

	// ---- helpers -----------------------------------------------------------
	std::string jstr(JNIEnv* env, jstring s)
	{
		if (!s) return {};
		const char* c = env->GetStringUTFChars(s, nullptr);
		std::string out = c ? c : "";
		env->ReleaseStringUTFChars(s, c);
		return out;
	}

	void raise_rlimits()
	{
		rlimit lim{};
		lim.rlim_cur = RLIM_INFINITY;
		lim.rlim_max = RLIM_INFINITY;
		setrlimit(RLIMIT_MEMLOCK, &lim);
		lim.rlim_cur = 65536;
		lim.rlim_max = 65536;
		setrlimit(RLIMIT_NOFILE, &lim);
	}

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
			if (force_quit)
			{
				if (on_exit)
					on_exit();
				return true;
			}
			return false;
		};

		callbacks.init_kb_handler = []()
		{
			ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager()));
		};

		callbacks.init_mouse_handler = []()
		{
			ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager()));
		};

		callbacks.init_pad_handler = [](std::string_view title_id)
		{
			ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id));
			while (!pad::g_started && !Emu.IsStopped())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		};

		callbacks.init_gs_render = [](utils::serial* ar)
		{
			switch (const video_renderer type = g_cfg.video.renderer)
			{
			case video_renderer::null:
			{
				g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
				break;
			}
#if defined(HAVE_VULKAN)
			case video_renderer::vulkan:
			{
				g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
				break;
			}
#endif
			default:
			{
				fmt::throw_exception("Invalid video renderer: %s", type);
			}
			}
		};

		callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase>
		{
			return std::make_unique<android_gs_frame>();
		};
		callbacks.close_gs_frame = []() { adpf::stop(); };

		callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base> { return std::make_shared<null_camera_handler>(); };
		callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base> { return std::make_shared<null_music_handler>(); };

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
				cellstation_log.error("Audio backend %s failed to initialize, falling back to null output", result->GetName());
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

		callbacks.get_msg_dialog                 = []() -> std::shared_ptr<MsgDialogBase> { return std::shared_ptr<MsgDialogBase>(); };
		callbacks.get_osk_dialog                 = []() -> std::shared_ptr<OskDialogBase> { return std::shared_ptr<OskDialogBase>(); };
		callbacks.get_save_dialog                = []() -> std::unique_ptr<SaveDialogBase> { return std::unique_ptr<SaveDialogBase>(); };
		callbacks.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return std::unique_ptr<TrophyNotificationBase>(); };

		callbacks.on_run    = [](bool) {};
		callbacks.on_pause  = []() {};
		callbacks.on_resume = []() {};
		callbacks.on_stop   = []() {};
		callbacks.on_ready  = []() {};
		callbacks.on_missing_fw = []() { cellstation_log.error("No PS3 firmware installed"); };

		callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>> closed_successfully, int)
		{
			if (!closed_successfully || !*closed_successfully)
			{
				report_fatal_error("Stopping the emulator took too long (deadlock?)");
			}
		};

		callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};

		callbacks.enable_disc_eject  = [](bool) {};
		callbacks.enable_disc_insert = [](bool) {};
		callbacks.handle_taskbar_progress = [](s32, s32) {};

		// Only the strings the boot overlay shows. Everything else stays empty,
		// as it was — the app supplies its own text for anything it renders.
		callbacks.get_localized_string = [](localized_string_id id, const char*) -> std::string
		{
			switch (id)
			{
			case localized_string_id::PROGRESS_DIALOG_SCANNING_PPU_EXECUTABLE: return "Scanning game code";
			case localized_string_id::PROGRESS_DIALOG_ANALYZING_PPU_EXECUTABLE: return "Analyzing game code";
			case localized_string_id::PROGRESS_DIALOG_SCANNING_PPU_MODULES: return "Scanning modules";
			case localized_string_id::PROGRESS_DIALOG_LOADING_PPU_MODULES: return "Loading compiled modules";
			case localized_string_id::PROGRESS_DIALOG_COMPILING_PPU_MODULES: return "Compiling game code";
			case localized_string_id::PROGRESS_DIALOG_LINKING_PPU_MODULES: return "Linking modules";
			case localized_string_id::PROGRESS_DIALOG_APPLYING_PPU_CODE: return "Applying game code";
			case localized_string_id::PROGRESS_DIALOG_BUILDING_SPU_CACHE: return "Building SPU cache";
			default: return {};
			}
		};
		callbacks.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return {}; };
		callbacks.get_localized_setting   = [](const cfg::_base*, u32) -> std::string { return {}; };

		callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
		callbacks.add_breakpoint = [](u32) {};

		callbacks.display_sleep_control_supported = []() { return false; };
		callbacks.enable_display_sleep = [](bool) {};
		callbacks.check_microphone_permissions = []() {};
		callbacks.make_video_source = []() { return nullptr; };

		// Every std::function member must be callable: the tree builds with
		// -fno-exceptions, so an empty one aborts via bad_function_call.
		callbacks.update_emu_settings = []() {};
		callbacks.save_emu_settings = []() { Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID()); };
		callbacks.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return {}; };
		callbacks.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return {}; };
		callbacks.get_photo_path = [](std::string_view) -> std::string { return {}; };
		callbacks.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) -> bool { return false; };
		callbacks.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) -> bool { return false; };
		// The overlay renderer (progress dialogs, messages, the home menu) throws
		// a fatal if it can't find any font, which kills the RSX thread and
		// leaves a black screen. Its only other source is dev_flash, so before
		// firmware is installed there is nothing to fall back on. Android always
		// ships fonts here, so offer them to every lookup.
		callbacks.get_font_dirs = []() -> std::vector<std::string>
		{
			return {"/system/fonts/", "/product/fonts/", "/system_ext/fonts/"};
		};
		callbacks.on_install_pkgs = [](const std::vector<std::string>&) { return false; };
		callbacks.enable_gamemode = [](bool) {};
		callbacks.get_database_config = [](const std::string&) -> std::string { return {}; };

		Emu.SetCallbacks(std::move(callbacks));
	}

	// ---- global config (config.yml) ----------------------------------------
	// Settings the app exposes are read-modify-written on a scratch cfg_root
	// (upstream does the same in rpcs3qt/emu_settings.cpp) instead of on g_cfg:
	// the live config is reset by Emu.Init(), reloaded from disk by every
	// Emu.Load(), and read by the RSX thread each frame — so editing it in
	// place would either be silently discarded or write a defaults-derived
	// file over the user's config.yml.
	std::string global_config_path()
	{
		return fs::get_config_dir(true) + "config.yml";
	}

	std::unique_ptr<cfg_root> load_global_config()
	{
		auto cfg = std::make_unique<cfg_root>();

		// Emu.Init() caches the defaults *after* picking the default renderer;
		// start from those so keys missing from the file keep that choice.
		if (!g_cfg_defaults.empty())
		{
			cfg->from_string(g_cfg_defaults);
		}

		if (const fs::file file{global_config_path()})
		{
			if (!cfg->from_string(file.to_string()))
			{
				cellstation_log.error("Failed to parse %s, falling back to defaults", global_config_path());
			}
		}

		return cfg;
	}

	void save_global_config(const cfg_root& cfg)
	{
		Emulator::SaveSettings(cfg.to_string(), {});
	}

	// A serial reaches the filesystem via get_custom_config_path, so keep it to
	// a bare identifier rather than something that can escape the config dir.
	bool valid_serial(const std::string& serial)
	{
		return !serial.empty()
			&& serial.find('/') == std::string::npos
			&& serial.find('\\') == std::string::npos
			&& serial.find("..") == std::string::npos;
	}

	// Resolves a "Section/Setting" path (the names as they appear in the YAML)
	// to its config node, so the app can drive settings without the bridge
	// hardcoding a type for each one.
	cfg::_base* find_config_node(cfg::node& root, std::string_view path)
	{
		cfg::_base* current = &root;

		for (usz pos = 0; pos <= path.size();)
		{
			const usz sep = path.find('/', pos);
			const std::string_view part = path.substr(pos, sep == umax ? umax : sep - pos);

			if (current->get_type() != cfg::type::node)
			{
				return nullptr;
			}

			cfg::_base* found = nullptr;
			for (cfg::_base* child : static_cast<cfg::node*>(current)->get_nodes())
			{
				if (child->get_name() == part)
				{
					found = child;
					break;
				}
			}

			if (!found)
			{
				return nullptr;
			}

			current = found;

			if (sep == umax)
			{
				break;
			}

			pos = sep + 1;
		}

		return current == &root ? nullptr : current;
	}

	// Newline-separated list of accepted values for a config path, taken from a
	// defaults-constructed tree so it does not depend on what is on disk. Empty
	// when the path does not resolve or the type has no fixed value list.
	std::string config_options(const std::string& path)
	{
		// Heap, not stack: cfg_root is the whole settings tree (hundreds of
		// entries, each carrying std::strings), and this runs on whichever
		// thread the app called from. load_global_config allocates it the same
		// way for the same reason.
		const auto defaults = std::make_unique<cfg_root>();
		const cfg::_base* node = find_config_node(*defaults, path);

		if (!node)
			return {};

		// to_list() covers enums; booleans have no list of their own.
		if (node->get_type() == cfg::type::_bool)
			return "false\ntrue";

		std::string result;

		for (const std::string& value : node->to_list())
		{
			if (!result.empty()) result += '\n';
			result += value;
		}

		return result;
	}

	// Per-game config as the core will see it: the global config with the
	// game's overrides applied on top, matching what Emulator::Load does for
	// cfg_mode::custom.
	std::unique_ptr<cfg_root> load_game_config(const std::string& serial)
	{
		auto cfg = load_global_config();

		if (const fs::file file{rpcs3::utils::get_custom_config_path(serial)})
		{
			if (!cfg->from_string(file.to_string()))
			{
				cellstation_log.error("Failed to parse the custom config for %s", serial);
			}
		}

		return cfg;
	}

	// Lean port of main_window::HandlePupInstallation (no dialogs/progress UI).
	bool install_firmware(const std::string& path)
	{
		fs::file pup_f(path);
		if (!pup_f)
		{
			cellstation_log.error("Firmware install: cannot open '%s' (%s)", path, fs::g_tls_error);
			return false;
		}

		pup_object pup(std::move(pup_f));
		if (pup.operator pup_error() != pup_error::ok)
		{
			cellstation_log.error("Firmware install: invalid PUP: %s", pup.get_formatted_error());
			return false;
		}

		fs::file update_files_f = pup.get_file(0x300);
		if (!update_files_f || !update_files_f.size())
		{
			cellstation_log.error("Firmware install: missing update files database");
			return false;
		}

		tar_object update_files(update_files_f);
		auto update_filenames = update_files.get_filenames();
		update_filenames.erase(std::remove_if(update_filenames.begin(), update_filenames.end(),
			[](const std::string& s) { return s.find("dev_flash_") == umax; }), update_filenames.end());

		if (update_filenames.empty())
		{
			cellstation_log.error("Firmware install: no dev_flash_* packages found");
			return false;
		}

		// Used by tar_object::extract() as destination directory
		vfs::mount("/dev_flash", g_cfg_vfs.get_dev_flash());

		for (const auto& update_filename : update_filenames)
		{
			auto update_file_stream = update_files.get_file(update_filename);

			if (update_file_stream->m_file_handler)
			{
				// Forcefully read all the data
				update_file_stream->m_file_handler->handle_file_op(*update_file_stream, 0, update_file_stream->get_size(umax), nullptr);
			}

			fs::file update_file = fs::make_stream(std::move(update_file_stream->data));

			SCEDecrypter self_dec(update_file);
			self_dec.LoadHeaders();
			self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
			self_dec.DecryptData();

			auto dev_flash_tar_f = self_dec.MakeFile();
			if (dev_flash_tar_f.size() < 3)
			{
				cellstation_log.error("Firmware install: PUP contents are invalid (%s)", update_filename);
				return false;
			}

			tar_object dev_flash_tar(dev_flash_tar_f[2]);
			if (!dev_flash_tar.extract())
			{
				cellstation_log.error("Firmware install: TAR extraction failed (%s)", update_filename);
				return false;
			}
		}

		cellstation_log.success("Firmware installed: %s", utils::get_firmware_version());
		return true;
	}
} // namespace

namespace chrysalis
{
	bool vk_dispatch_init(const char* custom_driver_dir, const char* custom_driver_name,
		const char* hook_lib_dir, const char* tmp_lib_dir);
}

namespace
{
	// Custom GPU driver selection, stashed by setGpuDriver() until initialize()
	// wires the Vulkan dispatch (driver changes need a process restart).
	std::string g_gpu_driver_dir, g_gpu_driver_name, g_gpu_hook_dir, g_gpu_tmp_dir;
}

// ---- JNI surface (nu.hyperworks.cellstation.EmuBridge) ---------------------

extern "C"
{

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_setGpuDriver(
	JNIEnv* env, jclass, jstring dir, jstring name, jstring hook_dir, jstring tmp_dir)
{
	g_gpu_driver_dir = dir ? jstr(env, dir) : std::string();
	g_gpu_driver_name = name ? jstr(env, name) : std::string();
	g_gpu_hook_dir = hook_dir ? jstr(env, hook_dir) : std::string();
	g_gpu_tmp_dir = tmp_dir ? jstr(env, tmp_dir) : std::string();
}

JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_initialize(JNIEnv* env, jclass, jstring root_dir, jstring user)
{
	static bool s_initialized = false;
	if (s_initialized)
		return JNI_TRUE;

	// Before any Vulkan call: route the core's Vulkan imports through either
	// the system loader or an adrenotools-hooked one (custom driver).
	chrysalis::vk_dispatch_init(
		g_gpu_driver_dir.empty() ? nullptr : g_gpu_driver_dir.c_str(),
		g_gpu_driver_name.empty() ? nullptr : g_gpu_driver_name.c_str(),
		g_gpu_hook_dir.empty() ? nullptr : g_gpu_hook_dir.c_str(),
		g_gpu_tmp_dir.empty() ? nullptr : g_gpu_tmp_dir.c_str());

	const std::string root = jstr(env, root_dir);
	// Trailing slashes are load-bearing: fs::get_config_dir()/get_cache_dir()
	// return these verbatim and the core appends names directly
	// ("configdev_hdd0/" vs "config/dev_hdd0/").
	g_android_executable_dir = root;
	g_android_config_dir = root + "/config/";
	g_android_cache_dir = root + "/cache/";
	fs::create_path(g_android_config_dir);
	fs::create_path(g_android_cache_dir);

	raise_rlimits();

	logs::listener::add(&g_logcat);

	std::string usr = jstr(env, user);
	if (usr.empty())
		usr = "00000001";

	Emu.SetHasGui(false);
	Emu.SetUsr(usr);
	// Without this the core's supported-renderer list stays at its {null}
	// default and System.cpp forces any Vulkan config back to Null.
	Emu.SetSupportedRenderers({
		video_renderer::null,
#if defined(HAVE_VULKAN)
		video_renderer::vulkan,
#endif
	});

	// Fresh installs get their config.yml from these defaults, so a Null
	// default means every first-run boots to a black screen. Mirror
	// main_application: enumerate Vulkan and make it the default when a
	// device is present (mandatory on the Snapdragon targets).
	Emu.SetDefaultRenderer(video_renderer::null);
#if defined(HAVE_VULKAN)
	{
		vk::instance vulkan_probe;
		if (vulkan_probe.create("CellStation", true))
		{
			vulkan_probe.bind();
			if (const auto& gpus = vulkan_probe.enumerate_devices(); !gpus.empty())
			{
				const std::string adapter = gpus[0].get_name();
				cellstation_log.notice("Default renderer: Vulkan ('%s')", adapter);
				Emu.SetDefaultRenderer(video_renderer::vulkan);
				Emu.SetDefaultGraphicsAdapter(adapter);
			}
		}
	}
#endif
	// Emu.Init() writes config.yml when it is missing, so a first run has to be
	// detected before it, not after.
	const bool fresh_config = !fs::is_file(fs::get_config_dir(true) + "config.yml");

	Emu.Init();

	// Android-appropriate defaults, applied only to a fresh config so a user's
	// own settings are never overwritten.
	//
	// llvm_threads=0 means "one compiler per core". Each PPU LLVM worker holds
	// a whole module (thousands of functions in a big title), so on an 8-core
	// phone that spikes past what malloc can map and aborts mid-compile —
	// heavy titles never finish their first boot. Two workers keep the compile
	// parallel without the spike.
	if (fresh_config)
	{
		g_cfg.core.llvm_threads.set(2);

		// Upstream defaults to async_with_interpreter, which precompiles 570
		// shader-interpreter pipeline variants before a game can start. That
		// costs ~5 minutes on this class of hardware and reads as a hang.
		// The interpreter only smooths over shaders that are still compiling;
		// plain async recompilation trades a little first-encounter stutter
		// for a boot that actually finishes.
		g_cfg.video.shadermode.set(shader_mode::async_recompiler);

		Emulator::SaveSettings(g_cfg.to_string(), {});
		cellstation_log.notice("Fresh config: LLVM threads = 2, shader mode = async recompiler");
	}

#ifdef ARCH_ARM64
	// Scale busy-wait budgets to the (usually 19.2MHz) arm generic timer,
	// as upstream's main() does.
	utils::init_arm_timer_scale();
#endif

	init_callbacks();

	s_initialized = true;
	cellstation_log.success("CellStation core initialized (root=%s)", root);
	return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_runMainLoop(JNIEnv*, jclass)
{
	thread_ctrl::scoped_priority high_prio(+1);
	g_pump.run();
}

JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_installFirmware(JNIEnv*, jclass, jint fd)
{
	// Note: reopening a SAF/content-provider fd via /proc/self/fd fails with
	// EACCES on scoped-storage FUSE mounts; prefer installFirmwarePath with a
	// file the app itself can open (e.g. a copy in cacheDir).
	return install_firmware(fmt::format("/proc/self/fd/%d", fd)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_installFirmwarePath(JNIEnv* env, jclass, jstring jpath)
{
	return install_firmware(jstr(env, jpath)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_nu_hyperworks_cellstation_EmuBridge_firmwareVersion(JNIEnv* env, jclass)
{
	return env->NewStringUTF(utils::get_firmware_version().c_str());
}

// Deletes a game's compiled caches: rpcs3 keys them by TITLE_ID under
// <cache>/cache/<serial>/ (PPU LLVM object cache, SPU cache, shader cache).
// Deleting makes the next boot recompile from scratch — the fix for a cache
// poisoned by a crash mid-compile, or one built by an older core.
// Returns the number of bytes freed.
JNIEXPORT jlong JNICALL Java_nu_hyperworks_cellstation_EmuBridge_clearGameCache(JNIEnv* env, jclass, jstring jserial)
{
	const std::string serial = jstr(env, jserial);

	// Empty serial would resolve to the whole cache root; refuse rather than
	// wiping every game's caches from a per-game action.
	if (serial.empty() || serial.find('/') != std::string::npos || !Emu.IsStopped())
		return 0;

	const std::string dir = rpcs3::utils::get_cache_dir() + serial + "/";
	if (!fs::is_dir(dir))
		return 0;

	u64 freed = 0;
	for (const auto& entry : fs::dir(dir))
	{
		if (!entry.is_directory)
		{
			freed += entry.size;
			continue;
		}
		if (entry.name != "." && entry.name != "..")
		{
			for (const auto& sub : fs::dir(dir + entry.name))
			{
				if (!sub.is_directory) freed += sub.size;
			}
		}
	}

	if (!fs::remove_all(dir, true))
	{
		cellstation_log.error("Failed to clear cache for %s", serial);
		return 0;
	}

	cellstation_log.success("Cleared %s cache (%lluK)", serial, freed / 1024);
	return static_cast<jlong>(freed);
}

// Boot progress, for the loading overlay. The core already tracks this — it is
// what drives the desktop GUI's progress dialog — so this just reads the same
// globals. Two counters exist: "pieces" (ptotal/pdone) count compiled PPU/SPU
// modules, "files" (ftotal/fdone) count file operations like a firmware or disc
// install. The desktop dialog prefers pieces when a compile is running, so
// mirror that rather than inventing a different rule.
//
// Returns "done\ttotal\ttext", or "" when nothing is in progress.
JNIEXPORT jstring JNICALL Java_nu_hyperworks_cellstation_EmuBridge_bootProgress(JNIEnv* env, jclass)
{
	const u32 ptotal = g_progr_ptotal;
	const u32 pdone = g_progr_pdone;
	const u32 ftotal = g_progr_ftotal;
	const u32 fdone = g_progr_fdone;

	const std::string text = g_progr_text;

	u32 done = 0;
	u32 total = 0;

	if (ptotal)
	{
		done = pdone;
		total = ptotal;
	}
	else if (ftotal)
	{
		done = fdone;
		total = ftotal;
	}

	// A phase can publish text before it knows its totals; still worth showing,
	// so treat text alone as progress rather than reporting nothing.
	if (!total && text.empty())
	{
		return env->NewStringUTF("");
	}

	return env->NewStringUTF(fmt::format("%u\t%u\t%s", done, total, text).c_str());
}

// Per-game settings live in config/custom_configs/config_<SERIAL>.yml, which
// Emulator::Load already applies on top of the global config when booting with
// cfg_mode::custom (what boot() uses). These four entry points are all the app
// needs to create, read, edit and drop one.

JNIEXPORT jstring JNICALL Java_nu_hyperworks_cellstation_EmuBridge_gameConfigGet(JNIEnv* env, jclass, jstring jserial, jstring jpath)
{
	const std::string serial = jstr(env, jserial);
	const std::string path = jstr(env, jpath);

	if (!valid_serial(serial))
		return env->NewStringUTF("");

	auto cfg = load_game_config(serial);
	const cfg::_base* node = find_config_node(*cfg, path);

	return env->NewStringUTF(node ? node->to_string().c_str() : "");
}

// Newline-separated list of accepted values, so the app can render a picker
// without duplicating rpcs3's enum names.
JNIEXPORT jstring JNICALL Java_nu_hyperworks_cellstation_EmuBridge_gameConfigOptions(JNIEnv* env, jclass, jstring jpath)
{
	return env->NewStringUTF(config_options(jstr(env, jpath)).c_str());
}

JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_gameConfigSet(JNIEnv* env, jclass, jstring jserial, jstring jpath, jstring jvalue)
{
	const std::string serial = jstr(env, jserial);
	const std::string path = jstr(env, jpath);
	const std::string value = jstr(env, jvalue);

	if (!valid_serial(serial))
		return JNI_FALSE;

	auto cfg = load_game_config(serial);
	cfg::_base* node = find_config_node(*cfg, path);

	if (!node || !node->from_string(value))
	{
		cellstation_log.error("Rejected per-game setting %s = '%s' for %s", path, value, serial);
		return JNI_FALSE;
	}

	// SaveSettings writes through fs::pending_file, which does not create
	// missing parents — on a fresh install nothing has made custom_configs/ yet.
	const std::string dir = rpcs3::utils::get_custom_config_dir();
	if (!fs::is_dir(dir) && !fs::create_path(dir))
	{
		cellstation_log.error("Failed to create %s: %s", dir, fs::g_tls_error);
		return JNI_FALSE;
	}

	// Writes the whole config, matching how rpcs3 stores custom configs: a
	// snapshot of the global settings with this game's changes folded in.
	const std::string file = rpcs3::utils::get_custom_config_path(serial);
	Emulator::SaveSettings(cfg->to_string(), serial);

	// SaveSettings reports failure to the log and returns void, so confirm the
	// file actually landed rather than telling the app it worked.
	if (!fs::is_file(file))
	{
		cellstation_log.error("Per-game config for %s was not written", serial);
		return JNI_FALSE;
	}

	cellstation_log.success("Per-game setting for %s: %s = %s", serial, path, value);
	return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_gameConfigExists(JNIEnv* env, jclass, jstring jserial)
{
	const std::string serial = jstr(env, jserial);

	if (!valid_serial(serial))
		return JNI_FALSE;

	return fs::is_file(rpcs3::utils::get_custom_config_path(serial)) ? JNI_TRUE : JNI_FALSE;
}

// Drops the override file so the game falls back to the global config.
JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_gameConfigReset(JNIEnv* env, jclass, jstring jserial)
{
	const std::string serial = jstr(env, jserial);

	if (!valid_serial(serial))
		return JNI_FALSE;

	const std::string path = rpcs3::utils::get_custom_config_path(serial);

	if (!fs::is_file(path))
		return JNI_TRUE;

	if (!fs::remove_file(path))
	{
		cellstation_log.error("Failed to remove the custom config for %s", serial);
		return JNI_FALSE;
	}

	cellstation_log.success("Removed the custom config for %s", serial);
	return JNI_TRUE;
}

// The same three operations against the global config (config/config.yml),
// addressed by the same "Section/Setting" paths. Nested sections are spelled
// out in full, e.g. "Video/Performance Overlay/Enabled" — find_config_node
// walks every '/'-separated component, so depth is not special-cased here.

JNIEXPORT jstring JNICALL Java_nu_hyperworks_cellstation_EmuBridge_globalConfigGet(JNIEnv* env, jclass, jstring jpath)
{
	const std::string path = jstr(env, jpath);

	auto cfg = load_global_config();
	const cfg::_base* node = find_config_node(*cfg, path);

	return env->NewStringUTF(node ? node->to_string().c_str() : "");
}

JNIEXPORT jstring JNICALL Java_nu_hyperworks_cellstation_EmuBridge_globalConfigOptions(JNIEnv* env, jclass, jstring jpath)
{
	return env->NewStringUTF(config_options(jstr(env, jpath)).c_str());
}

JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_globalConfigSet(JNIEnv* env, jclass, jstring jpath, jstring jvalue)
{
	const std::string path = jstr(env, jpath);
	const std::string value = jstr(env, jvalue);

	auto cfg = load_global_config();
	cfg::_base* node = find_config_node(*cfg, path);

	if (!node || !node->from_string(value))
	{
		cellstation_log.error("Rejected global setting %s = '%s'", path, value);
		return JNI_FALSE;
	}

	// A running game holds its own copy of the config (Emu.Load reloads it at
	// boot), so persisting alone would leave the change waiting for the next
	// launch. Settings rpcs3 marks dynamic are re-read while a game runs, so
	// assign the live one too, as setStretchToDisplayArea does. Non-dynamic
	// settings are deliberately left for the next boot; the core makes no
	// promise about picking those up mid-game.
	//
	// Done before the write because SaveSettings snapshots g_cfg into
	// g_backup_cfg (what the in-game home menu diffs against): updating the
	// live value first keeps the two in step. SaveSettings also merges into
	// g_cfg itself, but only when g_cfg came from config.yml — a game booted
	// with a per-game override is left out, which is exactly the case this
	// assignment covers.
	cfg::_base* live = find_config_node(g_cfg, path);
	const bool applied_live = live && live->get_is_dynamic();

	if (applied_live)
	{
		live->from_string(value);
	}

	save_global_config(*cfg);

	if (applied_live)
	{
		// The perf overlay is not polled per frame: it is created, configured
		// and torn down by reset_performance_overlay(), which upstream calls
		// from main_application::OnEmuSettingsChange after any settings edit.
		// Do the same, on the main thread, so a live change actually shows up.
		// It is a no-op when no renderer is up.
		g_pump.push([]() { rsx::overlays::reset_performance_overlay(); });
	}

	cellstation_log.success("Global setting: %s = %s (live: %s)", path, value, applied_live);
	return JNI_TRUE;
}

// Pulls PARAM.SFO / ICON0.PNG out of a disc image into out_dir using the
// core's own ISO reader, so the library can show real titles and tile art
// for .iso games. Uses the same virtual-device slot as booting an ISO does,
// hence the stopped-emulator guard.
JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_extractIsoAssets(JNIEnv* env, jclass, jstring jiso, jstring jout)
{
	const std::string iso = jstr(env, jiso);
	const std::string out = jstr(env, jout);

	if (!Emu.IsStopped() || !is_iso_file(iso))
		return JNI_FALSE;

	fs::set_virtual_device("iso_overlay_fs_dev", stx::make_shared<iso_device>(iso));

	bool any = false;
	for (const char* name : {"PS3_GAME/PARAM.SFO", "PS3_GAME/ICON0.PNG"})
	{
		fs::file src(iso_device::virtual_device_name + "/" + name);
		if (!src)
			continue;

		const std::string base = std::string(name).substr(std::string(name).find('/') + 1);
		if (fs::file dst(out + "/" + base, fs::rewrite); dst)
		{
			dst.write(src.to_vector<u8>());
			any = true;
		}
	}

	fs::set_virtual_device("iso_overlay_fs_dev", stx::shared_ptr<iso_device>());
	return any ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_nu_hyperworks_cellstation_EmuBridge_boot(JNIEnv* env, jclass, jstring jpath)
{
	const std::string path = jstr(env, jpath);

	// Homebrew boots without installed PS3 firmware: force HLE for every
	// firmware module so Emulator::Load's LLE check does not demand
	// /dev_flash (mirrors native/host/main.cpp). Persisted via SaveSettings
	// because Load() re-reads config.yml. Symmetric: we restore the default
	// only if the config still equals exactly the set we wrote, so a user's
	// hand-tuned library list is never touched.
	{
		extern const std::map<std::string_view, int> g_prx_list;
		std::set<std::string> hle_all;
		for (const auto& [name, flag] : g_prx_list)
		{
			hle_all.emplace(std::string(name) + ":hle");
		}

		const bool no_fw = utils::get_firmware_version().empty();

		auto cfg = load_global_config();
		const bool is_hle_all = cfg->core.libraries_control.get_set() == hle_all;

		if (no_fw && !is_hle_all)
		{
			cellstation_log.notice("No firmware installed: forcing HLE firmware modules for homebrew boot");
			cfg->core.libraries_control.set_set(std::move(hle_all));
			save_global_config(*cfg);
		}
		else if (!no_fw && is_hle_all)
		{
			cellstation_log.notice("Firmware present: restoring default firmware library mode");
			cfg->core.libraries_control.set_set({});
			save_global_config(*cfg);
		}
	}

	atomic_t<u32> done = 0;
	game_boot_result result = game_boot_result::generic_error;

	g_pump.push([&]()
	{
		result = Emu.BootGame(path, "", true);
		done = 1;
		done.notify_one();
	});

	done.wait(0);
	if (is_error(result))
	{
		cellstation_log.error("Boot failed for '%s': %s", path, result);
	}
	else
	{
		// Stable marker asserted by ci/integration-test.sh - keep in sync.
		cellstation_log.success("Boot OK: %s", path);
	}
	return static_cast<jint>(result);
}

JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_stretchToDisplayArea(JNIEnv*, jclass)
{
	return load_global_config()->video.stretch_to_display_area.get() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_setStretchToDisplayArea(JNIEnv*, jclass, jboolean enabled, jboolean persist)
{
	const bool value = enabled == JNI_TRUE;

	if (persist == JNI_TRUE)
	{
		auto cfg = load_global_config();
		cfg->video.stretch_to_display_area.set(value);
		save_global_config(*cfg);
	}

	// "Stretch To Display Area" is a dynamic setting — VK/GLPresent re-read it
	// on every flip — so assigning it here applies mid-game, and lets a
	// per-launch override take effect without touching the persisted value.
	g_cfg.video.stretch_to_display_area.set(value);

	cellstation_log.notice("Stretch to display area: %s (persisted: %s)", value, persist == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL Java_nu_hyperworks_cellstation_EmuBridge_surfaceEvent(JNIEnv* env, jclass, jobject surface, jint event)
{
	if (event == 0 /* ready */ && surface)
	{
		ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
		if (ANativeWindow* old = g_native_window.exchange(w))
			ANativeWindow_release(old);
		return JNI_TRUE;
	}

	// destroyed
	if (ANativeWindow* old = g_native_window.exchange(nullptr))
		ANativeWindow_release(old);
	return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_setAdpfEnabled(JNIEnv*, jclass, jboolean enabled)
{
	adpf::g_enabled = enabled == JNI_TRUE;
	cellstation_log.notice("ADPF: %s by setting", enabled == JNI_TRUE ? "enabled" : "disabled");
}

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_setPadState(JNIEnv* env, jclass, jbyteArray jvalues)
{
	if (!jvalues) return;

	chrysalis::android_pad_state state{};
	state.connected = true;

	const jsize len = std::min<jsize>(env->GetArrayLength(jvalues), static_cast<jsize>(state.values.size()));
	env->GetByteArrayRegion(jvalues, 0, len, reinterpret_cast<jbyte*>(state.values.data()));

	chrysalis::set_android_pad_state(state);

}

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_setPadConnected(JNIEnv*, jclass, jboolean connected)
{
	chrysalis::set_android_pad_connected(connected == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_kill(JNIEnv*, jclass)
{
	g_pump.push([]() { Emu.Kill(false); });
}

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_pause(JNIEnv*, jclass)
{
	g_pump.push([]() { Emu.Pause(); });
}

JNIEXPORT void JNICALL Java_nu_hyperworks_cellstation_EmuBridge_resume(JNIEnv*, jclass)
{
	g_pump.push([]() { Emu.Resume(); });
}

JNIEXPORT jint JNICALL Java_nu_hyperworks_cellstation_EmuBridge_getState(JNIEnv*, jclass)
{
	return static_cast<jint>(Emu.GetStatus(false));
}

JNIEXPORT jstring JNICALL Java_nu_hyperworks_cellstation_EmuBridge_getVersion(JNIEnv* env, jclass)
{
	return env->NewStringUTF(rpcs3::get_verbose_version().c_str());
}

} // extern "C"
