// cellstation_harness — a headless CLI that boots a PS3 payload on the macOS
// build of the rpcs3 core and drives it over a stdin command REPL. This is the
// M2 proof that our patched core runs natively and the C-ABI bridge works, and
// the "thin fallback" automation surface whose verbs mirror the AppleScript
// vocabulary the windowed app will expose in M3/M4 (see macos/README.md).
//
// Usage:
//   cellstation_harness [--data-dir DIR] [--boot PATH] [--run-seconds N] [--quit-after-boot]
//   ... then type commands on stdin (see `help`).

#include "../bridge/cellstation_bridge.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <array>
#include <unordered_map>

namespace
{
	// Button name -> snapshot index. Keep in sync with cellstation::pad_button
	// (macos/bridge/macos_pad.h) and the AppleScript button names.
	const std::unordered_map<std::string, int> g_button_index = {
		{"cross", 1}, {"circle", 2}, {"square", 3}, {"triangle", 4},
		{"l1", 5}, {"r1", 6}, {"l3", 7}, {"r3", 8},
		{"start", 9}, {"select", 10}, {"ps", 11},
		{"up", 12}, {"down", 13}, {"left", 14}, {"right", 15},
		{"l2", 16}, {"r2", 17},
	};

	constexpr int kSnapshotLen = 26; // cellstation::button_count

	std::array<uint8_t, kSnapshotLen> g_pad{}; // current held snapshot

	void push_pad()
	{
		cs_set_pad_state(g_pad.data(), kSnapshotLen);
	}

	bool set_button(const std::string& name, uint8_t value)
	{
		auto it = g_button_index.find(name);
		if (it == g_button_index.end())
		{
			std::fprintf(stderr, "unknown button '%s'\n", name.c_str());
			return false;
		}
		g_pad[it->second] = value;
		push_pad();
		return true;
	}

	void print_help()
	{
		std::puts(
			"commands (mirror of the AppleScript verbs):\n"
			"  boot <path>              boot an ELF/SELF/ISO/game dir\n"
			"  press <button> [ms]      hold a button for ms (default 120), then release\n"
			"  hold <button>            hold a button until released\n"
			"  release <button>         release a held button\n"
			"  pad <b0,b1,...>          set the raw snapshot (comma-separated 0..255 bytes)\n"
			"  save <slot>              save state to slot N\n"
			"  load <slot>              load state from slot N\n"
			"  reset                    restart the current title\n"
			"  pause | resume           pause / resume emulation\n"
			"  kill                     stop emulation\n"
			"  status                   print run status\n"
			"  fps                      print emulated fps\n"
			"  fw <pup>                 install firmware PUP\n"
			"  fwver | version          print firmware / core version\n"
			"  buttons                  list button names\n"
			"  help                     this text\n"
			"  quit                     stop the core and exit");
	}
}

int main(int argc, char** argv)
{
	std::string data_dir, boot_path;
	int run_seconds = -1;
	bool quit_after_boot = false;

	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
		else if (a == "--boot" && i + 1 < argc) boot_path = argv[++i];
		else if (a == "--run-seconds" && i + 1 < argc) run_seconds = std::atoi(argv[++i]);
		else if (a == "--quit-after-boot") quit_after_boot = true;
		else if (a == "--help") { print_help(); return 0; }
		else std::fprintf(stderr, "ignoring unknown arg '%s'\n", a.c_str());
	}

	if (!cs_initialize(data_dir.c_str(), "00000001"))
	{
		std::fprintf(stderr, "cs_initialize failed\n");
		return 1;
	}
	std::fprintf(stderr, "core version: %s\n", cs_version());

	// The control thread drives the core; the main thread runs the pump loop
	// (cs_run_main_loop) that services call_from_main_thread and boot/kill.
	std::thread control([&]()
	{
		if (!boot_path.empty())
		{
			const int r = cs_boot(boot_path.c_str());
			std::fprintf(stderr, "boot result: %d (%s)\n", r, r == 0 ? "no_errors" : "error");

			if (quit_after_boot)
			{
				cs_kill();
				cs_stop_main_loop();
				return;
			}
			if (run_seconds >= 0)
			{
				std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
				cs_kill();
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				cs_stop_main_loop();
				return;
			}
		}

		print_help();
		std::string line;
		while (std::fprintf(stderr, "> "), std::getline(std::cin, line))
		{
			std::istringstream ss(line);
			std::string cmd; ss >> cmd;
			if (cmd.empty()) continue;

			if (cmd == "quit") { break; }
			else if (cmd == "help") print_help();
			else if (cmd == "buttons")
			{
				for (const auto& [name, idx] : g_button_index) std::fprintf(stderr, "  %s=%d\n", name.c_str(), idx);
			}
			else if (cmd == "boot") { std::string p; ss >> p; std::fprintf(stderr, "boot result: %d\n", cs_boot(p.c_str())); }
			else if (cmd == "press")
			{
				std::string b; int ms = 120; ss >> b; if (!(ss >> ms)) ms = 120;
				if (set_button(b, 255))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(ms));
					set_button(b, 0);
				}
			}
			else if (cmd == "hold") { std::string b; ss >> b; set_button(b, 255); }
			else if (cmd == "release") { std::string b; ss >> b; set_button(b, 0); }
			else if (cmd == "pad")
			{
				std::string list; ss >> list;
				std::array<uint8_t, kSnapshotLen> snap{};
				std::stringstream ls(list); std::string tok; int i = 0;
				while (std::getline(ls, tok, ',') && i < kSnapshotLen) snap[i++] = static_cast<uint8_t>(std::atoi(tok.c_str()));
				g_pad = snap; push_pad();
			}
			else if (cmd == "save") { int s = 0; ss >> s; std::fprintf(stderr, "save slot %d: %s\n", s, cs_save_state(s) ? "ok" : "fail"); }
			else if (cmd == "load") { int s = 0; ss >> s; std::fprintf(stderr, "load slot %d: %s\n", s, cs_load_state(s) ? "ok" : "fail"); }
			else if (cmd == "reset") cs_reset();
			else if (cmd == "pause") cs_pause();
			else if (cmd == "resume") cs_resume();
			else if (cmd == "kill") cs_kill();
			else if (cmd == "status") std::fprintf(stderr, "status: %s (state=%d)\n", cs_status_text(), cs_get_state());
			else if (cmd == "fps") std::fprintf(stderr, "fps: %.1f\n", cs_get_fps());
			else if (cmd == "fw") { std::string p; ss >> p; const char* e = cs_install_firmware(p.c_str()); std::fprintf(stderr, "firmware: %s\n", (e && *e) ? e : "ok"); }
			else if (cmd == "fwver") std::fprintf(stderr, "firmware version: '%s'\n", cs_firmware_version());
			else if (cmd == "version") std::fprintf(stderr, "%s\n", cs_version());
			else std::fprintf(stderr, "unknown command '%s' (try help)\n", cmd.c_str());
		}

		cs_kill();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		cs_stop_main_loop();
	});

	cs_run_main_loop();
	control.join();
	std::fprintf(stderr, "bye\n");

	// The core has been stopped cleanly by cs_kill (all threads joined, fxo
	// objects cleared). Exit immediately rather than unwinding global static
	// destructors: rpcs3's global fxo typemap asserts it was torn down through
	// its own controlled path, and letting its destructor run at process exit
	// trips that assertion. This is the standard emulator shutdown pattern.
	std::fflush(stdout);
	std::fflush(stderr);
	std::_Exit(0);
}
