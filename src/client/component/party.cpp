#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "game/engine/sv_game.hpp"
#include "game/dvars.hpp"

#include "party.hpp"
#include "console.hpp"
#include "command.hpp"
#include "network.hpp"
#include "scheduler.hpp"
#include "server_list.hpp"

#include "steam/steam.hpp"

#include <utils/string.hpp>
#include <utils/info_string.hpp>
#include <utils/cryptography.hpp>
#include <utils/hook.hpp>

#include <version.hpp>

namespace party
{
	namespace
	{
		struct
		{
			game::netadr_s host{};
			std::string challenge{};
			bool hostDefined{false};
		} connect_state;

		std::string sv_motd;

		int sv_maxclients;

		utils::info_string get_info()
		{
			utils::info_string info;

			info.set("gamename", "S1");
			info.set("hostname", dvars::get_dvar_string("sv_hostname"));
			info.set("gametype", dvars::get_dvar_string("g_gametype"));
			info.set("sv_motd", dvars::get_dvar_string("sv_motd"));
			info.set("xuid", utils::string::va("%llX", steam::SteamUser()->GetSteamID().bits));
			info.set("mapname", dvars::get_dvar_string("mapname"));
			info.set("isPrivate", dvars::get_dvar_string("g_password").empty() ? "0" : "1");
			info.set("clients", std::to_string(get_client_count()));
			info.set("bots", std::to_string(get_bot_count()));
			info.set("sv_maxclients", std::to_string(game::Dvar_FindVar("sv_maxclients")->current.integer));
			info.set("protocol", std::to_string(PROTOCOL));
			info.set("shortversion", SHORTVERSION);
			info.set("playmode", utils::string::va("%i", game::Com_GetCurrentCoDPlayMode()));
			info.set("sv_running", std::to_string(dvars::get_dvar_bool("sv_running")));
			info.set("dedicated", std::to_string(dvars::get_dvar_bool("dedicated")));

			return info;
		}

		void perform_game_initialization()
		{
			command::execute("onlinegame 1", true);
			command::execute("xstartprivateparty", true);
			command::execute("xblive_privatematch 1", true);
			command::execute("startentitlements", true);
		}

		void connect_to_party(const game::netadr_s& target, const std::string& mapname, const std::string& gametype)
		{
			if (game::environment::is_sp())
			{
				return;
			}

			if (game::Live_SyncOnlineDataFlags(0) != 0)
			{
				// initialize the game after onlinedataflags is 32 (workaround)
				if (game::Live_SyncOnlineDataFlags(0) == 32)
				{
					scheduler::once([=]()
					{
						command::execute("xstartprivateparty", true);
						command::execute("disconnect", true); // 32 -> 0

						connect_to_party(target, mapname, gametype);
					}, scheduler::pipeline::main, 1s);
					return;
				}
				else
				{
					scheduler::once([=]()
					{
						connect_to_party(target, mapname, gametype);
					}, scheduler::pipeline::main, 1s);
					return;
				}
			}

			perform_game_initialization();

			// exit from virtuallobby
			reinterpret_cast<void(*)()>(0x14020EB90)();

			// CL_ConnectFromParty
			char session_info[0x100] = {};
			reinterpret_cast<void(*)(int, char*, const game::netadr_s*, const char*, const char*)>(0x140209360)(0, session_info, &target, mapname.data(), gametype.data());
		}

		void didyouknow_stub(const char* dvar_name, const char* string)
		{
			if (!party::sv_motd.empty())
			{
				string = party::sv_motd.data();
			}

			// This function either does Dvar_SetString or Dvar_RegisterString for the given dvar
			reinterpret_cast<void(*)(const char*, const char*)>(0x1404C39B0)(dvar_name, string);
		}

		void disconnect_stub()
		{
			if (!game::VirtualLobby_Loaded())
			{
				if (game::CL_IsCgameInitialized())
				{
					game::CL_ForwardCommandToServer(0, "disconnect");
					game::CL_WritePacket(0);
				}

				game::CL_Disconnect(0);
			}
		}

		utils::hook::detour cl_disconnect_hook;

		void cl_disconnect_stub(int a1)
		{
			clear_sv_motd();
			cl_disconnect_hook.invoke<void>(a1);
		}

		const auto drop_reason_stub = utils::hook::assemble([](utils::hook::assembler& a)
		{
			a.mov(rdx, rdi);
			a.mov(ecx, 2);
			a.jmp(0x140209DD9);
		});
	}

	void clear_sv_motd()
	{
		sv_motd.clear();
	}

	int get_client_num_by_name(const std::string& name)
	{
		for (auto i = 0; !name.empty() && i < *game::mp::svs_numclients; ++i)
		{
			if (game::mp::g_entities[i].client)
			{
				char client_name[16] = {0};
				game::I_strncpyz(client_name, game::mp::g_entities[i].client->name, sizeof(client_name));
				game::I_CleanStr(client_name);

				if (client_name == name)
				{
					return i;
				}
			}
		}
		return -1;
	}

	void reset_connect_state()
	{
		connect_state = {};
	}

	int get_client_count()
	{
		auto count = 0;
		for (auto i = 0; i < *game::mp::svs_numclients; ++i)
		{
			if (game::mp::svs_clients[i].header.state >= 1)
			{
				++count;
			}
		}

		return count;
	}

	int get_bot_count()
	{
		auto count = 0;
		for (auto i = 0; i < *game::mp::svs_numclients; ++i)
		{
			if (game::mp::svs_clients[i].header.state >= 1 &&
				game::SV_BotIsBot(i))
			{
				++count;
			}
		}

		return count;
	}

	game::netadr_s& get_target()
	{
		return connect_state.host;
	}

	void connect(const game::netadr_s& target)
	{
		if (game::environment::is_sp())
		{
			return;
		}

		command::execute("lui_open_popup popup_acceptinginvite", false);

		connect_state.host = target;
		connect_state.challenge = utils::cryptography::random::get_challenge();
		connect_state.hostDefined = true;

		network::send(target, "getInfo", connect_state.challenge);
	}

	int server_client_count()
	{
		return sv_maxclients;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_sp())
			{
				return;
			}

			// hook disconnect command function
			utils::hook::jump(0x14020A010, disconnect_stub);

			// detour CL_Disconnect to clear motd
			cl_disconnect_hook.create(0x140209EC0, cl_disconnect_stub);

			if (game::environment::is_mp())
			{
				// show custom drop reason
				utils::hook::nop(0x140209D5C, 13);
				utils::hook::jump(0x140209D5C, drop_reason_stub, true);
			}
			// enable custom kick reason in GScr_KickPlayer
			utils::hook::set<uint8_t>(0x14032ED80, 0xEB);

			command::add("reconnect", [](const command::params& argument)
			{
				if (!connect_state.hostDefined)
				{
					console::info("Cannot connect to server.\n");
					return;
				}

				if (game::CL_IsCgameInitialized())
				{
					command::execute("disconnect");
					command::execute("reconnect");
				}
				else
				{
					connect(connect_state.host);
				}
			});

			command::add("connect", [](const command::params& argument)
			{
				if (argument.size() != 2)
				{
					return;
				}

				game::netadr_s target{};
				if (game::NET_StringToAdr(argument[1], &target))
				{
					connect(target);
				}
			});

			command::add("kickClient", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: kickClient <num>, <reason>(optional)\n");
					return;
				}

				if (!game::SV_Loaded() || game::VirtualLobby_Loaded())
				{
					return;
				}

				std::string reason;
				if (params.size() > 2)
				{
					reason = params.join(2);
				}
				if (reason.empty())
				{
					reason = "EXE_PLAYERKICKED";
				}

				const auto client_num = atoi(params.get(1));
				if (client_num < 0 || client_num >= *game::mp::svs_numclients)
				{
					return;
				}

				scheduler::once([client_num, reason]()
				{
					game::SV_KickClientNum(client_num, reason.data());
				}, scheduler::pipeline::server);
			});

			command::add("kick", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: kick <name>, <reason>(optional)\n");
					return;
				}

				if (!game::SV_Loaded() || game::VirtualLobby_Loaded())
				{
					return;
				}

				std::string reason;
				if (params.size() > 2)
				{
					reason = params.join(2);
				}
				if (reason.empty())
				{
					reason = "EXE_PLAYERKICKED";
				}

				const std::string name = params.get(1);
				if (name == "all"s)
				{
					for (auto i = 0; i < *game::mp::svs_numclients; ++i)
					{
						scheduler::once([i, reason]
						{
							game::SV_KickClientNum(i, reason.c_str());
						}, scheduler::pipeline::server);
					}
					return;
				}

				const auto client_num = get_client_num_by_name(name);
				if (client_num < 0 || client_num >= *game::mp::svs_numclients)
				{
					return;
				}

				scheduler::once([client_num, reason]()
				{
					game::SV_KickClientNum(client_num, reason.c_str());
				}, scheduler::pipeline::server);
			});

			scheduler::once([]()
			{
				game::Dvar_RegisterString("sv_sayName", "console", game::DVAR_FLAG_NONE);
			}, scheduler::pipeline::main);

			command::add("tell", [](const command::params& params)
			{
				if (params.size() < 3)
				{
					return;
				}

				const auto client_num = atoi(params.get(1));
				const auto message = params.join(2);
				const auto* const name = game::Dvar_FindVar("sv_sayName")->current.string;

				game::engine::SV_GameSendServerCommand(static_cast<char>(client_num), game::SV_CMD_CAN_IGNORE, utils::string::va("%c \"%s: %s\"", 84, name, message.c_str()));
				printf("%s -> %i: %s\n", name, client_num, message.c_str());
			});

			command::add("tellraw", [](const command::params& params)
			{
				if (params.size() < 3)
				{
					return;
				}

				const auto client_num = atoi(params.get(1));
				const auto message = params.join(2);

				game::engine::SV_GameSendServerCommand(static_cast<char>(client_num), game::SV_CMD_CAN_IGNORE, utils::string::va("%c \"%s\"", 84, message.c_str()));
				printf("%i: %s\n", client_num, message.c_str());
			});

			command::add("say", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					return;
				}

				const auto message = params.join(1);
				const auto* const name = game::Dvar_FindVar("sv_sayName")->current.string;

				game::engine::SV_GameSendServerCommand(-1, game::SV_CMD_CAN_IGNORE, utils::string::va("%c \"%s: %s\"", 84, name, message.c_str()));
				printf("%s: %s\n", name, message.c_str());
			});

			command::add("sayraw", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					return;
				}

				const auto message = params.join(1);

				game::engine::SV_GameSendServerCommand(-1, game::SV_CMD_CAN_IGNORE, utils::string::va("%c \"%s\"", 84, message.c_str()));
				printf("%s\n", message.data());
			});

			utils::hook::call(0x14048811C, didyouknow_stub); // allow custom didyouknow based on sv_motd

			network::on("getInfo", [](const game::netadr_s& target, const std::string& data)
			{
				utils::info_string info = get_info();
				info.set("challenge", data);

				network::send(target, "infoResponse", info.build(), '\n');
			});

			network::on("getStatus", [](const game::netadr_s& target, const std::string& data)
			{
				std::string player_list;

				utils::info_string info = get_info();
				info.set("challenge", data);

				const auto* sv_running = game::Dvar_FindVar("sv_running");
				if (!sv_running || !sv_running->current.enabled)
				{
					return;
				}

				for (auto i = 0; i < game::Dvar_FindVar("sv_maxclients")->current.integer; ++i)
				{
					auto* client = &game::mp::svs_clients[i];
					auto* self = &game::mp::g_entities[i];

					if (client->header.state < 5)
					{
						continue;
					}

					if (!self || !self->client)
					{
						continue;
					}

					if (game::SV_BotIsBot(i))
					{
						continue;
					}

					const auto score = game::G_GetClientScore(i);
					const auto ping = game::SV_GetClientPing(i);
					const std::string name = game::mp::svs_clients[i].name;

					player_list.append(std::format("{} {} \"{}\"\n", score, ping, name));
				}

				network::send(target, "statusResponse", info.build() + "\n"s + player_list + "\n"s, '\n');
			});

			if (game::environment::is_dedi())
			{
				return;
			}

			network::on("infoResponse", [](const game::netadr_s& target, const std::string& data)
			{
				const utils::info_string info(data);
				server_list::handle_info_response(target, info);

				if (connect_state.host != target)
				{
					return;
				}

				if (info.get("challenge") != connect_state.challenge)
				{
					const auto* error_msg = "Invalid challenge.";
					console::error("%s\n", error_msg);
					game::Com_Error(game::ERR_DROP, "%s", error_msg);
					return;
				}

				const auto gamename = info.get("gamename");
				if (gamename != "S1"s)
				{
					const auto* error_msg = "Invalid gamename.";
					console::error("%s\n", error_msg);
					game::Com_Error(game::ERR_DROP, "%s", error_msg);
					return;
				}

				const auto playmode = info.get("playmode");
				if (std::atoi(playmode.data()) != game::Com_GetCurrentCoDPlayMode())
				{
					const auto* error_msg = "Invalid playmode.";
					console::error("%s\n", error_msg);
					game::Com_Error(game::ERR_DROP, "%s", error_msg);
					return;
				}

				const auto sv_running = info.get("sv_running");
				if (sv_running != "1"s)
				{
					const auto* error_msg = "Server not running.";
					console::error("%s\n", error_msg);
					game::Com_Error(game::ERR_DROP, "%s", error_msg);
					return;
				}

				const auto mapname = info.get("mapname");
				if (mapname.empty())
				{
					const auto* error_msg = "Invalid map.";
					console::error("%s\n", error_msg);
					game::Com_Error(game::ERR_DROP, "%s", error_msg);
					return;
				}

				const auto gametype = info.get("gametype");
				if (gametype.empty())
				{
					const auto* error_msg = "Invalid gametype.";
					console::error("%s\n", error_msg);
					game::Com_Error(game::ERR_DROP, "%s", error_msg);
					return;
				}

				sv_motd = info.get("sv_motd");

				try
				{
					sv_maxclients = std::stoi(info.get("sv_maxclients"));
				}
				catch([[maybe_unused]] const std::exception& ex)
				{
					sv_maxclients = 1;
				}

				connect_to_party(target, mapname, gametype);
			});
		}
	};
}

REGISTER_COMPONENT(party::component)
