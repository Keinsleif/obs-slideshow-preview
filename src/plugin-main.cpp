/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <obs-frontend-api.h>
#include "next-preview-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern "C" {
	extern struct obs_source_info sp_image_source_info;
	extern struct obs_source_info sp_slideshow_info;
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_register_source(&sp_image_source_info);
	obs_register_source(&sp_slideshow_info);

	obs_frontend_add_event_callback(
		[](enum obs_frontend_event e, void *) {
			if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING)
				obs_frontend_add_dock_by_id(
					"sp_next_preview",
					obs_module_text("NextSlide"),
					new NextPreviewDock());
		},
		nullptr);

	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
