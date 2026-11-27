// SPDX-License-Identifier: GPL-2.0-or-later
#include "routing.hpp"

#include "../source/phone-mic.hpp"
#include "plugin-support.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QFile>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

using nlohmann::json;

namespace rac::routing {

static bool ours(obs_source_t *s)
{
	return s && std::strcmp(obs_source_get_id(s), kMicSourceId) == 0;
}

bool isEveryScene(obs_source_t *src)
{
	if (!src)
		return false;
	for (uint32_t ch = kFirstChannel; ch < MAX_CHANNELS; ch++) {
		obs_source_t *o = obs_get_output_source(ch);
		const bool hit = o == src;
		obs_source_release(o);
		if (hit)
			return true;
	}
	return false;
}

bool anyEveryScene()
{
	for (uint32_t ch = kFirstChannel; ch < MAX_CHANNELS; ch++) {
		obs_source_t *o = obs_get_output_source(ch);
		const bool hit = ours(o);
		obs_source_release(o);
		if (hit)
			return true;
	}
	return false;
}

static bool putOnFreeChannel(obs_source_t *src)
{
	for (uint32_t ch = kFirstChannel; ch < MAX_CHANNELS; ch++) {
		obs_source_t *o = obs_get_output_source(ch);
		obs_source_release(o);
		if (!o) {
			obs_set_output_source(ch, src);
			return true;
		}
	}
	return false;
}

struct Hits {
	obs_source_t *src;
	std::vector<obs_sceneitem_t *> items;
};

static bool collect(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto *h = static_cast<Hits *>(data);
	if (obs_sceneitem_get_source(item) == h->src) {
		obs_sceneitem_addref(item);
		h->items.push_back(item);
	}
	if (obs_sceneitem_is_group(item))
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), collect, data);
	return true;
}

static void removeFromScenes(obs_source_t *src)
{
	Hits hits{src, {}};
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++)
		if (obs_scene_t *scene = obs_scene_from_source(scenes.sources.array[i]))
			obs_scene_enum_items(scene, collect, &hits);
	obs_frontend_source_list_free(&scenes);
	// Removed after the walk: removing while a scene is being enumerated isn't safe.
	for (auto *it : hits.items) {
		obs_sceneitem_remove(it);
		obs_sceneitem_release(it);
	}
}

void addToCurrentScene(obs_source_t *src)
{
	obs_source_t *cur = obs_frontend_get_current_scene();
	if (!cur)
		return;
	obs_scene_t *scene = obs_scene_from_source(cur);
	if (scene && !obs_scene_find_source(scene, obs_source_get_name(src)))
		obs_scene_add(scene, src);
	obs_source_release(cur);
}

void setEveryScene(obs_source_t *src, bool on)
{
	if (!src || on == isEveryScene(src))
		return;
	if (on) {
		// The channel first, so the source always has a holder; then out of the scenes, so it's
		// heard once, not twice.
		if (!putOnFreeChannel(src)) {
			obs_log(LOG_WARNING, "no free global audio channel for '%s'", obs_source_get_name(src));
			return;
		}
		removeFromScenes(src);
		obs_log(LOG_INFO, "'%s' is heard in every scene", obs_source_get_name(src));
	} else {
		addToCurrentScene(src);
		for (uint32_t ch = kFirstChannel; ch < MAX_CHANNELS; ch++) {
			obs_source_t *o = obs_get_output_source(ch);
			if (o == src)
				obs_set_output_source(ch, nullptr);
			obs_source_release(o);
		}
		obs_log(LOG_INFO, "'%s' is in the current scene only", obs_source_get_name(src));
	}
}

// ---- saved with the scene collection --------------------------------------------------------

static void onSave(obs_data_t *data, bool saving, void *)
{
	if (saving) {
		obs_data_array_t *arr = obs_data_array_create();
		for (uint32_t ch = kFirstChannel; ch < MAX_CHANNELS; ch++) {
			obs_source_t *o = obs_get_output_source(ch);
			if (ours(o)) {
				obs_data_t *sd = obs_save_source(o);
				obs_data_array_push_back(arr, sd);
				obs_data_release(sd);
			}
			obs_source_release(o);
		}
		obs_data_t *mine = obs_data_create();
		obs_data_set_array(mine, "everyScene", arr);
		obs_data_set_obj(data, "relyacam", mine);
		obs_data_release(mine);
		obs_data_array_release(arr);
		return;
	}
	obs_data_t *mine = obs_data_get_obj(data, "relyacam");
	if (!mine)
		return;
	obs_data_array_t *arr = obs_data_get_array(mine, "everyScene");
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *name = obs_data_get_string(item, "name");
		obs_source_t *src = obs_get_source_by_name(name);
		if (src && !ours(src)) {
			// Something else has that name now; leave it alone.
			obs_source_release(src);
			src = nullptr;
		} else if (!src) {
			// Filters, volume and monitoring come back with it.
			src = obs_load_source(item);
			if (src)
				obs_source_load2(src);
		}
		if (src && !isEveryScene(src))
			putOnFreeChannel(src);
		obs_source_release(src);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	obs_data_release(mine);
}

void registerSaveCallback()
{
	obs_frontend_add_save_callback(onSave, nullptr);
}

void unregisterSaveCallback()
{
	obs_frontend_remove_save_callback(onSave, nullptr);
}

// ---- use in other apps ----------------------------------------------------------------------

struct Device {
	std::string name, id;
};

static bool findCable(Device &out)
{
	struct Find {
		Device d;
		bool found = false;
	} f;
	obs_enum_audio_monitoring_devices(
		[](void *data, const char *name, const char *id) -> bool {
			std::string n = name ? name : "";
			std::string low = n;
			std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			if (low.find("cable input") == std::string::npos)
				return true;
			auto *f = static_cast<Find *>(data);
			f->d = {n, id ? id : ""};
			f->found = true;
			return false;
		},
		&f);
	if (f.found)
		out = f.d;
	return f.found;
}

static Device currentMonitor()
{
	const char *n = nullptr, *id = nullptr;
	obs_get_audio_monitoring_device(&n, &id);
	return {n ? n : "", id ? id : ""};
}

/** Sets OBS's monitoring device now and in the profile, so Settings shows it and it survives a restart. */
static void setMonitor(const Device &d)
{
	obs_set_audio_monitoring_device(d.name.c_str(), d.id.c_str());
	if (config_t *cfg = obs_frontend_get_profile_config()) {
		config_set_string(cfg, "Audio", "MonitoringDeviceName", d.name.c_str());
		config_set_string(cfg, "Audio", "MonitoringDeviceId", d.id.c_str());
		config_save_safe(cfg, "tmp", nullptr);
	}
}

static std::string stateFile()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("monitoring.json");
	std::string s = p ? p : "monitoring.json";
	bfree(p);
	return s;
}

static void savePrevious(const Device &d)
{
	QFile f(QString::fromStdString(stateFile()));
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(QByteArray::fromStdString(json{{"name", d.name}, {"id", d.id}}.dump()));
}

static bool loadPrevious(Device &d)
{
	QFile f(QString::fromStdString(stateFile()));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	json j = json::parse(f.readAll().toStdString(), nullptr, false);
	if (!j.is_object() || j.value("id", "").empty())
		return false;
	d = {j.value("name", ""), j.value("id", "")};
	return true;
}

bool inOtherApps(obs_source_t *src)
{
	if (!src || obs_source_get_monitoring_type(src) != OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT)
		return false;
	Device cable;
	return findCable(cable) && currentMonitor().id == cable.id;
}

OtherApps setInOtherApps(obs_source_t *src, bool on)
{
	if (!src)
		return OtherApps::Off;
	if (on) {
		Device cable;
		if (!findCable(cable))
			return OtherApps::NoCable;
		const Device cur = currentMonitor();
		if (cur.id != cable.id) {
			savePrevious(cur);
			setMonitor(cable);
		}
		obs_source_set_monitoring_type(src, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
		obs_log(LOG_INFO, "'%s' plays into %s for other apps", obs_source_get_name(src), cable.name.c_str());
		return OtherApps::On;
	}
	obs_source_set_monitoring_type(src, OBS_MONITORING_TYPE_NONE);
	// Give the monitoring device back once no phone mic uses the cable.
	bool left = false;
	obs_enum_sources(
		[](void *data, obs_source_t *s) {
			if (ours(s) && obs_source_get_monitoring_type(s) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT)
				*static_cast<bool *>(data) = true;
			return true;
		},
		&left);
	Device cable, prev;
	if (!left && findCable(cable) && currentMonitor().id == cable.id && loadPrevious(prev)) {
		setMonitor(prev);
		QFile::remove(QString::fromStdString(stateFile()));
	}
	return OtherApps::Off;
}

} // namespace rac::routing
