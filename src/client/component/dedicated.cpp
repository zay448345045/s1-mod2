#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "game/engine/sv_game.hpp"

#include "command.hpp"
#include "console.hpp"
#include "dvars.hpp"
#include "network.hpp"
#include "scheduler.hpp"
#include "server_list.hpp"

#include "component/gsc/script_extension.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

namespace dedicated
{
	namespace
	{
		const game::dvar_t* sv_lanOnly;

		void init_dedicated_server()
		{
			static bool initialized = false;
			if (initialized) return;
			initialized = true;

			// R_LoadGraphicsAssets
			reinterpret_cast<void(*)()>(0x1405A54F0)();
		}

		void send_heartbeat()
		{
			if (sv_lanOnly->current.enabled)
			{
				return;
			}

			game::netadr_s target{};
			if (server_list::get_master_server(target))
			{
				network::send(target, "heartbeat", "S1");
			}
		}

		std::vector<std::string>& get_startup_command_queue()
		{
			static std::vector<std::string> startup_command_queue;
			return startup_command_queue;
		}

		void execute_startup_command(int client, int /*controllerIndex*/, const char* command)
		{
			if (game::Live_SyncOnlineDataFlags(0) == 0)
			{
				game::Cbuf_ExecuteBufferInternal(0, 0, command, game::Cmd_ExecuteSingleCommand);
			}
			else
			{
				get_startup_command_queue().emplace_back(command);
			}
		}

		void execute_startup_command_queue()
		{
			const auto queue = get_startup_command_queue();
			get_startup_command_queue().clear();

			for (const auto& command : queue)
			{
				game::Cbuf_ExecuteBufferInternal(0, 0, command.data(), game::Cmd_ExecuteSingleCommand);
			}
		}

		std::vector<std::string>& get_console_command_queue()
		{
			static std::vector<std::string> console_command_queue;
			return console_command_queue;
		}

		void execute_console_command([[maybe_unused]] const int local_client_num, const char* command)
		{
			if (game::Live_SyncOnlineDataFlags(0) == 0)
			{
				command::execute(command);
			}
			else
			{
				get_console_command_queue().emplace_back(command);
			}
		}

		void execute_console_command_queue()
		{
			const auto queue = get_console_command_queue();
			get_console_command_queue().clear();

			for (const auto& command : queue)
			{
				command::execute(command);
			}
		}

		void sync_gpu_stub()
		{
			std::this_thread::sleep_for(1ms);
		}

		void sv_kill_server_f()
		{
			game::Com_Shutdown("EXE_SERVERKILLED");
		}

		void start_map(const std::string& map_name)
		{
			if (game::Live_SyncOnlineDataFlags(0) > 32)
			{
				scheduler::once([map_name]()
				{
					command::execute(std::format("map {}", map_name), false);
				}, scheduler::pipeline::main, 1s);

				return;
			}

			if (!game::SV_MapExists(map_name.c_str()))
			{
				console::info("Map '%s' doesn't exist.\n", map_name.c_str());
				return;
			}

			auto* current_mapname = game::Dvar_FindVar("mapname");
			if (current_mapname && utils::string::to_lower(current_mapname->current.string) == utils::string::to_lower(map_name) && (game::SV_Loaded() && !game::VirtualLobby_Loaded()))
			{
				console::info("Restarting map: %s\n", map_name.c_str());
				command::execute("map_restart", false);
				return;
			}

			console::info("Starting map: %s\n", map_name.c_str());

			auto* gametype = game::Dvar_FindVar("g_gametype");
			if (gametype && gametype->current.string)
			{
				command::execute(utils::string::va("ui_gametype %s", gametype->current.string), true);
			}

			command::execute(utils::string::va("ui_mapname %s", map_name.c_str()), true);

			game::SV_StartMapForParty(0, map_name.c_str(), false, false);
		}

		void gscr_is_using_match_rules_data_stub()
		{
			game::Scr_AddInt(0);
		}
	}

	void initialize()
	{
		command::execute("exec default_xboxlive.cfg", true);
		command::execute("onlinegame 1", true);
		command::execute("xblive_privatematch 1", true);
	}

	class component final : public component_interface
	{
	public:
		void* load_import(const std::string& library, const std::string& function) override
		{
			return nullptr;
		}

		void post_unpack() override
		{
			if (!game::environment::is_dedi())
			{
				return;
			}

			game::Dvar_RegisterBool("dedicated", true, game::DVAR_FLAG_READ);

			sv_lanOnly = game::Dvar_RegisterBool("sv_lanOnly", false, game::DVAR_FLAG_NONE);

			// Disable VirtualLobby
			dvars::override::register_bool("virtualLobbyEnabled", false, game::DVAR_FLAG_NONE | game::DVAR_FLAG_READ);

			// Disable r_preloadShaders
			dvars::override::register_bool("r_preloadShaders", false, game::DVAR_FLAG_NONE | game::DVAR_FLAG_READ);

			// Don't allow sv_hostname to be changed by the game
			dvars::disable::set_string("sv_hostname");

			// Hook R_SyncGpu
			utils::hook::jump(0x1405A7630, sync_gpu_stub);

			// Make GScr_IsUsingMatchRulesData return 0 so the game doesn't override the cfg
			gsc::override_function("isusingmatchrulesdata", gscr_is_using_match_rules_data_stub);

			utils::hook::jump(0x14020C6B0, init_dedicated_server);

			// delay startup commands until the initialization is done
			utils::hook::call(0x1403CDF63, execute_startup_command);

			// delay console commands until the initialization is done
			utils::hook::call(0x1403CEC35, execute_console_command);
			utils::hook::nop(0x1403CEC4B, 5);

			utils::hook::nop(0x1404AE6AE, 5); // don't load config file
			utils::hook::nop(0x1403AF719, 5); // ^
			utils::hook::set<uint8_t>(0x1403D2490, 0xC3); // don't save config file
			utils::hook::set<uint8_t>(0x14022AFC0, 0xC3); // disable self-registration
			utils::hook::set<uint8_t>(0x1404DA780, 0xC3); // init sound system (1)
			utils::hook::set<uint8_t>(0x14062BC10, 0xC3); // init sound system (2)
			utils::hook::set<uint8_t>(0x1405F31A0, 0xC3); // render thread
			utils::hook::set<uint8_t>(0x140213C20, 0xC3); // called from Com_Frame, seems to do renderer stuff
			utils::hook::set<uint8_t>(0x1402085C0, 0xC3);
			// CL_CheckForResend, which tries to connect to the local server constantly
			utils::hook::set<uint8_t>(0x14059B854, 0); // r_loadForRenderer default to 0
			utils::hook::set<uint8_t>(0x1404D6952, 0xC3); // recommended settings check - TODO: Check hook
			utils::hook::set<uint8_t>(0x1404D9BA0, 0xC3); // some mixer-related function called on shutdown
			utils::hook::set<uint8_t>(0x1403B2860, 0xC3); // dont load ui gametype stuff
			utils::hook::nop(0x14043ABB8, 6); // unknown check in SV_ExecuteClientMessage
			utils::hook::nop(0x140439F15, 4); // allow first slot to be occupied
			utils::hook::nop(0x14020E01C, 2); // properly shut down dedicated servers
			utils::hook::nop(0x14020DFE9, 2); // ^
			utils::hook::nop(0x14020E047, 5); // don't shutdown renderer

			utils::hook::set<uint8_t>(0x140057D40, 0xC3); // something to do with blendShapeVertsView
			utils::hook::nop(0x14062EA17, 8); // sound thing

			utils::hook::set<uint8_t>(0x1404D6960, 0xC3); // cpu detection stuff?
			utils::hook::set<uint8_t>(0x1405AEC00, 0xC3); // gfx stuff during fastfile loading
			utils::hook::set<uint8_t>(0x1405AEB10, 0xC3); // ^
			utils::hook::set<uint8_t>(0x1405AEBA0, 0xC3); // ^
			utils::hook::set<uint8_t>(0x140275640, 0xC3); // ^
			utils::hook::set<uint8_t>(0x1405AEB60, 0xC3); // ^
			utils::hook::set<uint8_t>(0x140572640, 0xC3); // directx stuff
			utils::hook::set<uint8_t>(0x1405A1340, 0xC3); // ^
			utils::hook::set<uint8_t>(0x140021D60, 0xC3); // ^ - mutex
			utils::hook::set<uint8_t>(0x1405A17E0, 0xC3); // ^

			utils::hook::set<uint8_t>(0x1400534F0, 0xC3); // rendering stuff
			utils::hook::set<uint8_t>(0x1405A1AB0, 0xC3); // ^
			utils::hook::set<uint8_t>(0x1405A1BB0, 0xC3); // ^
			utils::hook::set<uint8_t>(0x1405A21F0, 0xC3); // ^
			utils::hook::set<uint8_t>(0x1405A2D60, 0xC3); // ^
			utils::hook::set<uint8_t>(0x1405A3400, 0xC3); // ^

			// shaders
			utils::hook::set<uint8_t>(0x140057BC0, 0xC3); // ^
			utils::hook::set<uint8_t>(0x140057B40, 0xC3); // ^

			utils::hook::set<uint8_t>(0x1405EE040, 0xC3); // ^ - mutex

			utils::hook::set<uint8_t>(0x1404DAF30, 0xC3); // idk
			utils::hook::set<uint8_t>(0x1405736B0, 0xC3); // ^

			utils::hook::set<uint8_t>(0x1405A6E70, 0xC3); // R_Shutdown
			utils::hook::set<uint8_t>(0x1405732D0, 0xC3); // shutdown stuff
			utils::hook::set<uint8_t>(0x1405A6F40, 0xC3); // ^
			utils::hook::set<uint8_t>(0x1405A61A0, 0xC3); // ^

			utils::hook::set<uint8_t>(0x14062C550, 0xC3); // sound crashes

			utils::hook::set<uint8_t>(0x140445070, 0xC3); // disable host migration

			utils::hook::set<uint8_t>(0x1403E1A50, 0xC3); // render synchronization lock
			utils::hook::set<uint8_t>(0x1403E1990, 0xC3); // render synchronization unlock

			utils::hook::set<uint8_t>(0x1400E517B, 0xEB);
			// LUI: Unable to start the LUI system due to errors in main.lua

			utils::hook::nop(0x1404CC482, 5); // Disable sound pak file loading
			utils::hook::nop(0x1404CC471, 2); // ^
			utils::hook::set<uint8_t>(0x140279B80, 0xC3); // Disable image pak file loading

			// Reduce min required memory
			utils::hook::set<uint64_t>(0x1404D140D, 0x80000000);
			utils::hook::set<uint64_t>(0x1404D14BF, 0x80000000);

			// initialize the game after onlinedataflags is 32 (workaround)
			scheduler::schedule([=]()
			{
				if (game::Live_SyncOnlineDataFlags(0) == 32 && game::Sys_IsDatabaseReady2())
				{
					scheduler::once([]
					{
						command::execute("xstartprivateparty", true);
						command::execute("disconnect", true); // 32 -> 0
					}, scheduler::pipeline::main, 1s);
					return scheduler::cond_end;
				}

				return scheduler::cond_continue;
			}, scheduler::pipeline::main, 1s);

			scheduler::on_game_initialized([]
			{
				initialize();

				console::info("==================================\n");
				console::info("Server started!\n");
				console::info("==================================\n");

				// remove disconnect command
				game::Cmd_RemoveCommand(751);

				execute_startup_command_queue();
				execute_console_command_queue();

				// Send heartbeat to dpmaster
				scheduler::once(send_heartbeat, scheduler::pipeline::server);
				scheduler::loop(send_heartbeat, scheduler::pipeline::server, 10min);
				command::add("heartbeat", send_heartbeat);
			}, scheduler::pipeline::main, 1s);

			command::add("killserver", sv_kill_server_f);

			command::add("map", [](const command::params& argument)
			{
				if (argument.size() != 2)
				{
					return;
				}

				start_map(argument[1]);
			});

			command::add("map_restart", []()
			{
				if (!game::SV_Loaded() || game::VirtualLobby_Loaded())
				{
					return;
				}

				*reinterpret_cast<int*>(0x1488692B0) = 1; // sv_map_restart
				*reinterpret_cast<int*>(0x1488692B4) = 1; // sv_loadScripts
				*reinterpret_cast<int*>(0x1488692B8) = 0; // sv_migrate
				reinterpret_cast<void(*)(int)>(0x140437460)(0); // SV_CheckLoadGame
			});

			command::add("fast_restart", []()
			{
				if (game::SV_Loaded() && !game::VirtualLobby_Loaded())
				{
					game::SV_FastRestart(0);
				}
			});
		}
	};
}

REGISTER_COMPONENT(dedicated::component)
