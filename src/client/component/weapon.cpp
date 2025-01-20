#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"
#include "fastfiles.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include <utils/memory.hpp>

namespace weapon
{
	namespace
	{
		utils::hook::detour g_setup_level_weapon_def_hook;
		void g_setup_level_weapon_def_stub()
		{
			// precache level weapons first
			g_setup_level_weapon_def_hook.invoke<void>();

			std::vector<game::WeaponDef*> weapons;

			// find all weapons in asset pools
			fastfiles::enum_assets(game::ASSET_TYPE_WEAPON, [&weapons](game::XAssetHeader header)
			{
				weapons.push_back(reinterpret_cast<game::WeaponDef*>(header.data));
			}, false);

			// sort weapons
			std::sort(weapons.begin(), weapons.end(), [](game::WeaponDef* weapon1, game::WeaponDef* weapon2)
			{
				return std::string_view(weapon1->name) <
					std::string_view(weapon2->name);
			});

			// precache items
			for (std::size_t i = 0; i < weapons.size(); i++)
			{
				//console::info("precaching weapon \"%s\"\n", weapons[i]->name);
				game::G_GetWeaponForName(weapons[i]->name);
			}
		}

		utils::hook::detour xmodel_get_bone_index_hook;
		int xmodel_get_bone_index_stub(game::XModel* model, game::scr_string_t name, unsigned int offset, char* index)
		{
			auto result = xmodel_get_bone_index_hook.invoke<int>(model, name, offset, index);
			if (result)
			{
				return result;
			}

			const auto original_index = *index;
			const auto original_result = result;

			if (name == game::SL_FindString("tag_weapon_right") ||
				name == game::SL_FindString("tag_knife_attach"))
			{
				const auto tag_weapon = game::SL_FindString("tag_weapon");
				result = xmodel_get_bone_index_hook.invoke<int>(model, tag_weapon, offset, index);
				if (result)
				{
					console::info("using tag_weapon instead of %s (%s, %d, %d)\n", game::SL_ConvertToString(name), model->name, offset, *index);
					return result;
				}
			}

			*index = original_index;
			result = original_result;

			return result;
		}

		void cw_mismatch_error_stub(int, const char* msg, ...)
		{
			char buffer[0x100];

			va_list ap;
			va_start(ap, msg);

			vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, msg, ap);

			va_end(ap);

			console::error(buffer);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (!game::environment::is_mp())
			{
				return;
			}

			// precache all weapons that are loaded in zones
			g_setup_level_weapon_def_hook.create(0x140340DE0, g_setup_level_weapon_def_stub);

			// use tag_weapon if tag_weapon_right or tag_knife_attach are not found on model
			xmodel_get_bone_index_hook.create(0x1404E2A50, xmodel_get_bone_index_stub);

			// make custom weapon index mismatch not drop in CG_SetupCustomWeapon
			utils::hook::call(0x1401E973D, cw_mismatch_error_stub);
		}
	};
}

REGISTER_COMPONENT(weapon::component)
