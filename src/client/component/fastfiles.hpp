#pragma once

#include "game/game.hpp"

namespace fastfiles
{
	std::string get_current_fastfile();

	bool exists(const std::string& zone);

	void enum_assets(game::XAssetType type, const std::function<void(game::XAssetHeader)>& callback, bool include_override);

	const char* get_zone_name(const unsigned int index);

	void enum_asset_entries(const game::XAssetType type, const std::function<void(game::XAssetEntry*)>& callback, bool include_override);
}
