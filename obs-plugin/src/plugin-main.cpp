/*
Rely-A-Cam
Copyright (C) 2026 Niccc2007

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

#include "audio/routing.hpp"
#include "dock/dock.hpp"
#include "net/hub.hpp"
#include "source/phone-mic.hpp"
#include "source/phone-source.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Rely-A-Cam: your phone as a camera for OBS, with zoom synced on both screens.";
}

static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	// Stop before OBS tears down: every phone gets BYE and releases its camera and mic.
	if (event == OBS_FRONTEND_EVENT_EXIT)
		rac::Hub::get().stop();
	// OBS restores its saved dock layout after loading plugins, and a plugin dock starts hidden.
	// Reveal it after that, once ever, so a first-time user lands on the pairing QR.
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		QTimer::singleShot(500, [] { rac::PhoneDock::introduceOnce(); });
}

bool obs_module_load(void)
{
	rac::PhoneSource::registerSource();
	rac::PhoneMic::registerSource();
	rac::routing::registerSaveCallback();
	rac::Hub::get().start();

	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = new rac::PhoneDock(main);
	if (!obs_frontend_add_dock_by_id("relyacam_dock", obs_module_text("DockTitle"), dock))
		obs_log(LOG_WARNING, "could not add the Phone Camera dock");
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	rac::routing::unregisterSaveCallback();
	rac::Hub::get().stop();
	obs_log(LOG_INFO, "unloaded");
}
