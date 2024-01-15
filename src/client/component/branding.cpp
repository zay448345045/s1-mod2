#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include "localized_strings.hpp"
#include "scheduler.hpp"
#include "dvars.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

#include "version.hpp"

namespace branding
{
	namespace
	{
		utils::hook::detour ui_get_formatted_build_number_hook;

		const char* ui_get_formatted_build_number_stub()
		{
			const auto* const build_num = ui_get_formatted_build_number_hook.invoke<const char*>();

			return utils::string::va("%s (%s)", VERSION, build_num);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			if (game::environment::is_mp())
			{
				localized_strings::override("LUA_MENU_MULTIPLAYER_CAPS", "s1-mod: MULTIPLAYER\n");
			}
			localized_strings::override("LUA_MENU_LEGAL_COPYRIGHT", "s1-mod: " VERSION);

			dvars::override::set_string("version", utils::string::va("s1-mod %s", VERSION));

			ui_get_formatted_build_number_hook.create(
				SELECT_VALUE(0x14035B3F0, 0x1404A8950), ui_get_formatted_build_number_stub);

			scheduler::loop([]()
			{
				const auto x = 4;
				const auto y = 4;
				const auto scale = 1.0f;
				float color[4] = {0.666f, 0.666f, 0.666f, 0.666f};
				const auto* text = "s1-mod: " VERSION;

				auto* font = game::R_RegisterFont("fonts/consolefont");

				if (!font) return;

				game::R_AddCmdDrawText(text, std::numeric_limits<int>::max(), font, static_cast<float>(x),
				                       y + static_cast<float>(font->pixelHeight) * scale,
				                       scale, scale, 0.0f, color, 0);
			}, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_COMPONENT(branding::component)
