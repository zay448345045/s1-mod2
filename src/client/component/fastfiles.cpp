#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/dvars.hpp"

#include "command.hpp"
#include "console.hpp"
#include "fastfiles.hpp"

#include <utils/concurrency.hpp>
#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>

namespace fastfiles
{
	static utils::concurrency::container<std::string> current_fastfile;

	namespace
	{
		template <typename T>
		inline void merge(std::vector<T>* target, T* source, size_t length)
		{
			if (source)
			{
				for (size_t i = 0; i < length; ++i)
				{
					target->push_back(source[i]);
				}
			}
		}

		template <typename T>
		inline void merge(std::vector<T>* target, std::vector<T> source)
		{
			for (auto& entry : source)
			{
				target->push_back(entry);
			}
		}
	}

	namespace
	{
		utils::hook::detour db_try_load_x_file_internal_hook;
		utils::hook::detour db_find_x_asset_header_hook;

		void db_try_load_x_file_internal(const char* zone_name, const int flags)
		{
			static std::unordered_map<std::string, std::string> zone_overrides = {};

			auto override_zone_name = zone_overrides.find(zone_name);
			if (override_zone_name != zone_overrides.end())
			{
				console::info("Overriding fastfile %s with %s\n", zone_name, override_zone_name->second.c_str());
				zone_name = override_zone_name->second.c_str();
			}

			console::info("Loading fastfile %s\n", zone_name);
			current_fastfile.access([&](std::string& fastfile)
			{
				fastfile = zone_name;
			});

			return db_try_load_x_file_internal_hook.invoke<void>(zone_name, flags);
		}

		void dump_gsc_script(const std::string& name, game::XAssetHeader header)
		{
			if (!dvars::g_dump_scripts->current.enabled)
			{
				return;
			}

			std::string buffer;
			buffer.append(header.scriptfile->name, std::strlen(header.scriptfile->name) + 1);
			buffer.append(reinterpret_cast<char*>(&header.scriptfile->compressedLen), sizeof(int));
			buffer.append(reinterpret_cast<char*>(&header.scriptfile->len), sizeof(int));
			buffer.append(reinterpret_cast<char*>(&header.scriptfile->bytecodeLen), sizeof(int));
			buffer.append(header.scriptfile->buffer, header.scriptfile->compressedLen);
			buffer.append(reinterpret_cast<char*>(header.scriptfile->bytecode), header.scriptfile->bytecodeLen);

			const auto out_name = std::format("gsc_dump/{}.gscbin", name);
			utils::io::write_file(out_name, buffer);

			console::info("Dumped %s\n", out_name.data());
		}

		game::XAssetHeader db_find_x_asset_header_stub(game::XAssetType type, const char* name, int allow_create_default)
		{
			const auto start = game::Sys_Milliseconds();
			const auto result = db_find_x_asset_header_hook.invoke<game::XAssetHeader>(type, name, allow_create_default);
			const auto diff = game::Sys_Milliseconds() - start;

#ifdef DEBUG
			if (type == game::ASSET_TYPE_SCRIPTFILE)
			{
				dump_gsc_script(name, result);
			}
#endif

			if (diff > 100)
			{
				console::print(
					result.data == nullptr ? console::con_type_error : console::con_type_warning, "Waited %i msec for asset '%s' of type '%s'.\n",
					diff,
					name,
					game::g_assetNames[type]
				);
			}

			return result;
		}

		utils::hook::detour db_link_xasset_entry1_hook;
		game::XAssetEntry* db_link_xasset_entry1(game::XAssetType type, game::XAssetHeader* header)
		{
			if (type == game::ASSET_TYPE_SCRIPTFILE)
			{
				dump_gsc_script(header->scriptfile->name, *header);
			}

			return db_link_xasset_entry1_hook.invoke<game::XAssetEntry*>(type, header);
		}

		namespace mp
		{
			void skip_extra_zones_stub(utils::hook::assembler& a)
			{
				const auto skip = a.newLabel();
				const auto original = a.newLabel();

				a.pushad64();
				a.test(esi, game::DB_ZONE_CUSTOM); // allocFlags
				a.jnz(skip);

				a.bind(original);
				a.popad64();
				a.mov(rdx, 0x140809D40);
				a.mov(rcx, rbp);
				a.call(0x1406FE120);
				a.jmp(0x140271B63);

				a.bind(skip);
				a.popad64();
				a.mov(r13d, game::DB_ZONE_CUSTOM);
				a.not_(r13d);
				a.and_(esi, r13d);
				a.jmp(0x140271D02);
			}
		}

		namespace sp
		{
			void skip_extra_zones_stub(utils::hook::assembler& a)
			{
				const auto skip = a.newLabel();
				const auto original = a.newLabel();

				a.pushad64();
				a.test(edi, game::DB_ZONE_CUSTOM); // allocFlags
				a.jnz(skip);

				a.bind(original);
				a.popad64();
				a.call(0x140379360);
				a.xor_(ecx, ecx);
				a.test(eax, eax);
				a.setz(cl);
				a.jmp(0x1401802D6);

				a.bind(skip);
				a.popad64();
				a.mov(r13d, game::DB_ZONE_CUSTOM);
				a.not_(r13d);
				a.and_(edi, r13d);
				a.jmp(0x1401803EF);
			}
		}

		utils::hook::detour db_read_stream_file_hook;
		void db_read_stream_file_stub(int a1, int a2)
		{
			// always use lz4 compressor type when reading stream files
			*game::g_compressor = 4;
			return db_read_stream_file_hook.invoke<void>(a1, a2);
		}

		utils::hook::detour sys_createfile_hook;
		HANDLE sys_create_file(game::Sys_Folder folder, const char* base_filename)
		{
			const auto* fs_basepath = game::Dvar_FindVar("fs_basepath");
			const auto* fs_game = game::Dvar_FindVar("fs_game");

			const std::string dir = fs_basepath ? fs_basepath->current.string : "";
			const std::string mod_dir = fs_game ? fs_game->current.string : "";
			const std::string name = base_filename;

			if (name == "mod.ff")
			{
				if (!mod_dir.empty())
				{
					const auto path = utils::string::va("%s\\%s\\%s",
						dir.data(), mod_dir.data(), base_filename);

					if (utils::io::file_exists(path))
					{
						return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
							FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, nullptr);
					}
				}

				return INVALID_HANDLE_VALUE;
			}

			return sys_createfile_hook.invoke<HANDLE>(folder, base_filename);
		}

		HANDLE sys_create_file_stub(game::Sys_Folder folder, const char* base_filename)
		{
			return sys_create_file(folder, base_filename);
		}

		bool try_load_zone(std::string name, bool localized, bool game = false)
		{
			if (localized)
			{
				const auto language = game::SEH_GetCurrentLanguageCode();
				try_load_zone(language + "_"s + name, false);
				if (game::environment::is_mp())
				{
					try_load_zone(language + "_"s + name + "_mp"s, false);
				}
			}

			if (!fastfiles::exists(name))
			{
				return false;
			}

			game::XZoneInfo info{};
			info.name = name.data();
			info.allocFlags = (game ? game::DB_ZONE_GAME : game::DB_ZONE_COMMON) | game::DB_ZONE_CUSTOM;
			info.freeFlags = 0;
			game::DB_LoadXAssets(&info, 1u, game::DBSyncMode::DB_LOAD_ASYNC);
			return true;
		}

		void load_pre_gfx_zones(game::XZoneInfo* zoneInfo, unsigned int zoneCount, game::DBSyncMode syncMode)
		{
			//imagefiles::close_custom_handles();

			std::vector<game::XZoneInfo> data;
			merge(&data, zoneInfo, zoneCount);

			// code_pre_gfx

			//weapon::clear_modifed_enums();
			try_load_zone("mod_pre_gfx", true);
			try_load_zone("s1_mod_pre_gfx", true);

			game::DB_LoadXAssets(data.data(), static_cast<std::uint32_t>(data.size()), syncMode);
		}

		void load_post_gfx_and_ui_and_common_zones(game::XZoneInfo* zoneInfo, unsigned int zoneCount, game::DBSyncMode syncMode)
		{
			std::vector<game::XZoneInfo> data;
			merge(&data, zoneInfo, zoneCount);

			// code_post_gfx
			// ui
			// common

			try_load_zone("s1_mod_common", true);

			game::DB_LoadXAssets(data.data(), static_cast<std::uint32_t>(data.size()), syncMode);

			try_load_zone("mod_common", true);
		}

		void load_ui_zones(game::XZoneInfo* zoneInfo, unsigned int zoneCount, game::DBSyncMode syncMode)
		{
			std::vector<game::XZoneInfo> data;
			merge(&data, zoneInfo, zoneCount);

			// ui

			game::DB_LoadXAssets(data.data(), static_cast<std::uint32_t>(data.size()), syncMode);
		}

		void load_lua_file_asset_stub(void* a1)
		{
			const auto fastfile = fastfiles::get_current_fastfile();
			if (fastfile == "mod")
			{
				console::error("Mod tried to load a lua file!\n");
				return;
			}

			// TODO: add usermap LUI loading checks
			/*
			const auto usermap = fastfiles::get_current_usermap();
			if (usermap.has_value())
			{
				const auto& usermap_value = usermap.value();
				const auto usermap_load = usermap_value + "_load";

				if (fastfile == usermap_value || fastfile == usermap_load)
				{
					console::error("Usermap tried to load a lua file!\n");
					return;
				}
			}
			*/

			utils::hook::invoke<void>(0x140276480, a1);
		}

		utils::hook::detour db_level_load_add_zone_hook;
		void db_level_load_add_zone_stub(void* load, const char* name, const unsigned int alloc_flags,
			const size_t size_est)
		{
			if (!strcmp(name, "mp_terminal_cls"))
			{
				db_level_load_add_zone_hook.invoke<void>(load, name, alloc_flags | game::DB_ZONE_CUSTOM, size_est);
				return;
			}

			db_level_load_add_zone_hook.invoke<void>(load, name, alloc_flags, size_est);
;		}
	}

	std::string get_current_fastfile()
	{
		auto fastfile_copy = current_fastfile.access<std::string>([&](std::string& fastfile)
		{
			return fastfile;
		});

		return fastfile_copy;
	}

	bool exists(const std::string& zone)
	{
		const auto is_localized = game::DB_IsLocalized(zone.data());
		const auto handle = sys_create_file((is_localized ? game::SF_ZONE_LOC : game::SF_ZONE),
			utils::string::va("%s.ff", zone.data()));

		if (handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle);
			return true;
		}

		return false;
	}

	void reallocate_asset_pool(game::XAssetType Type, size_t Size)
	{
		const size_t element_size = game::DB_GetXAssetTypeSize(Type);
		auto* new_pool = utils::memory::get_allocator()->allocate(Size * element_size);
		std::memmove(new_pool, game::DB_XAssetPool[Type], game::g_poolSize[Type] * element_size);

		game::DB_XAssetPool[Type] = new_pool;
		game::g_poolSize[Type] = static_cast<int>(Size);
	}

	template <game::XAssetType Type, size_t Multiplier>
	void reallocate_asset_pool_multiplier()
	{
		reallocate_asset_pool(Type, Multiplier * game::g_poolSize[Type]);
	}

	void enum_assets(const game::XAssetType type, const std::function<void(game::XAssetHeader)>& callback, const bool include_override)
	{
		game::DB_EnumXAssets_Internal(type, static_cast<void(*)(game::XAssetHeader, void*)>([](game::XAssetHeader header, void* data)
		{
			const auto& cb = *static_cast<const std::function<void(game::XAssetHeader)>*>(data);
			cb(header);
		}), &callback, include_override);
	}

	const char* get_zone_name(const unsigned int index)
	{
		if (game::environment::is_sp())
		{
			return "";
		}
		else
		{
			return game::g_zones[index].name;
		}
	}

	void reallocate_asset_pools()
	{
		reallocate_asset_pool(game::ASSET_TYPE_FONT, 48);

		if (!game::environment::is_sp())
		{
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_LUA_FILE, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_WEAPON, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_LOCALIZE_ENTRY, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_XANIMPARTS, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_ATTACHMENT, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_FONT, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_SNDDRIVER_GLOBALS, 4>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_EQUIPMENT_SND_TABLE, 4>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_SOUND, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_LOADED_SOUND, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_LEADERBOARD, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_VERTEXDECL, 6>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_COMPUTESHADER, 4>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_REVERB_PRESET, 2>();
			reallocate_asset_pool_multiplier<game::ASSET_TYPE_IMPACT_FX, 10>();
		}
	}

	void enum_asset_entries(const game::XAssetType type, const std::function<void(game::XAssetEntry*)>& callback, bool include_override)
	{
		constexpr auto max_asset_count = 0x1D8A8;
		auto hash = &game::db_hashTable[0];
		for (auto c = 0; c < max_asset_count; c++)
		{
			for (auto i = *hash; i; )
			{
				const auto entry = &game::g_assetEntryPool[i];

				if (entry->asset.type == type)
				{
					callback(entry);

					if (include_override && entry->nextOverride)
					{
						auto next_ovveride = entry->nextOverride;
						while (next_ovveride)
						{
							const auto override = &game::g_assetEntryPool[next_ovveride];
							callback(override);
							next_ovveride = override->nextOverride;
						}
					}
				}

				i = entry->nextHash;
			}

			++hash;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			db_try_load_x_file_internal_hook.create(SELECT_VALUE(0x1401816F0, 0x1402741C0), &db_try_load_x_file_internal);

			db_find_x_asset_header_hook.create(game::DB_FindXAssetHeader, db_find_x_asset_header_stub);
			db_link_xasset_entry1_hook.create(SELECT_VALUE(0x14017F390, 0x1402708F0), db_link_xasset_entry1);
			dvars::g_dump_scripts = game::Dvar_RegisterBool("g_dumpScripts", false, game::DVAR_FLAG_NONE);

			command::add("loadzone", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: loadzone <zone>\n");
					return;
				}

				game::XZoneInfo info{};
				info.name = params.get(1);
				info.allocFlags = game::DB_ZONE_COMMON | game::DB_ZONE_CUSTOM;
				info.freeFlags = 0;
				game::DB_LoadXAssets(&info, 1u, game::DBSyncMode::DB_LOAD_SYNC);
			});

			command::add("g_poolSizes", []()
			{
				for (auto i = 0; i < game::ASSET_TYPE_COUNT; i++)
				{
					console::info("g_poolSize[%i]: %i // %s\n", i, game::g_poolSize[i], game::g_assetNames[i]);
				}
			});

#ifdef DEBUG
			//reallocate_asset_pools();
#endif

			// Allow loading of unsigned fastfiles
			if (!game::environment::is_sp())
			{
				utils::hook::nop(0x1402427A5, 2); // DB_InflateInit
			}

			// Don't load extra zones with loadzone
			if (game::environment::is_sp())
			{
				utils::hook::nop(0x1401802CA, 12);
				utils::hook::jump(0x1401802CA, utils::hook::assemble(sp::skip_extra_zones_stub), true);
			}
			else
			{
				utils::hook::nop(0x140271B54, 15);
				utils::hook::jump(0x140271B54, utils::hook::assemble(mp::skip_extra_zones_stub), true);

				// dont load localized zone for custom maps (proper patch for this is here: https://github.com/auroramod/h1-mod/blob/develop/src/client/component/fastfiles.cpp#L442)
				db_level_load_add_zone_hook.create(0x1402705C0, db_level_load_add_zone_stub);
			}

			// Allow loading of mixed compressor types
			utils::hook::nop(SELECT_VALUE(0x1401536D7, 0x140242DF7), 2);

			// Fix compressor type on streamed file load
			db_read_stream_file_hook.create(SELECT_VALUE(0x140187450, 0x14027AA70), db_read_stream_file_stub);

			// Add custom zone paths
			sys_createfile_hook.create(game::Sys_CreateFile, sys_create_file_stub);

			// load our custom ui and common zones
			utils::hook::call(SELECT_VALUE(0x140487CF8, 0x1405A562A), load_post_gfx_and_ui_and_common_zones);

			// load our custom ui zones
			utils::hook::call(SELECT_VALUE(0x1402F91D4, 0x1403D06FC), load_ui_zones);

			// prevent mod.ff from loading lua files
			if (game::environment::is_mp())
			{
				utils::hook::call(0x14024F1B4, load_lua_file_asset_stub);
			}
		}
	};
}

REGISTER_COMPONENT(fastfiles::component)
