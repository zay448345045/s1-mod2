#include <std_include.hpp>
#include "game.hpp"

#include <utils/flags.hpp>

namespace game
{
	int Cmd_Argc()
	{
		return cmd_args->argc[cmd_args->nesting];
	}

	const char* Cmd_Argv(const int index)
	{
		return cmd_args->argv[cmd_args->nesting][index];
	}

	int SV_Cmd_Argc()
	{
		return sv_cmd_args->argc[sv_cmd_args->nesting];
	}

	const char* SV_Cmd_Argv(const int index)
	{
		return sv_cmd_args->argv[sv_cmd_args->nesting][index];
	}

	bool VirtualLobby_Loaded()
	{
		return !game::environment::is_sp() && *mp::virtualLobby_loaded == 1;
	}

	HANDLE Sys_OpenFileReliable(const char* filename)
	{
		return ::CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, nullptr,
		                     OPEN_EXISTING,
		                     FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, nullptr);
	}

	namespace environment
	{
		launcher::mode mode = launcher::mode::none;

		launcher::mode translate_surrogate(const launcher::mode _mode)
		{
			switch (_mode)
			{
			case launcher::mode::survival:
			case launcher::mode::zombies:
				return launcher::mode::multiplayer;
			default:
				return _mode;
			}
		}

		launcher::mode get_real_mode()
		{
			if (mode == launcher::mode::none)
			{
				throw std::runtime_error("Launcher mode not valid. Something must be wrong.");
			}

			return mode;
		}

		launcher::mode get_mode()
		{
			return translate_surrogate(get_real_mode());
		}

		bool is_sp()
		{
			return get_mode() == launcher::mode::singleplayer;
		}

		bool is_mp()
		{
			return get_mode() == launcher::mode::multiplayer;
		}

		bool is_dedi()
		{
			return get_mode() == launcher::mode::server;
		}

		void set_mode(const launcher::mode _mode)
		{
			mode = _mode;
		}

		std::string get_string()
		{
			const auto current_mode = get_real_mode();
			switch (current_mode)
			{
			case launcher::mode::server:
				return "dedicated server";

			case launcher::mode::zombies:
				return "zombies";

			case launcher::mode::survival:
				return "survival";

			case launcher::mode::multiplayer:
				return "multiplayer";

			case launcher::mode::singleplayer:
				return "singleplayer";

			case launcher::mode::none:
				return "none";

			default:
				return "unknown (" + std::to_string(static_cast<int>(mode)) + ")";
			}
		}
	}

	bool is_headless()
	{
		static const auto headless = utils::flags::has_flag("headless");
		return headless;
	}

	void show_error(const std::string& text, const std::string& title)
	{
		if (is_headless())
		{
			puts(text.data());
		}
		else
		{
			MessageBoxA(nullptr, text.data(), title.data(), MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
		}
	}
}
