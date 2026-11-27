// SPDX-License-Identifier: GPL-2.0-or-later
#include "routing.hpp"

#include "cable.hpp"
#include "../net/hub.hpp"
#include "../source/phone-mic.hpp"
#include "plugin-support.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QFile>
#include <QObject>
#include <QTimer>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
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

namespace {

/** One mic playing into the cable. The capture callback's param is the writer itself. */
struct Tap {
	obs_weak_source_t *weak = nullptr;
	std::unique_ptr<cable::Writer> writer;
	bool hooked = false;
	uint64_t nextTry = 0;
};

std::vector<Tap> taps;
QTimer *timer = nullptr;
bool wanted = false; // the last check's verdict: some phone mic should be the default
bool taken = false;  // CABLE Output is the default because of us

constexpr uint64_t kRetryNs = 10ULL * 1000 * 1000 * 1000;

void onAudio(void *param, obs_source_t *src, const audio_data *a, bool muted)
{
	const auto ch = uint32_t(audio_output_get_channels(obs_get_audio()));
	static_cast<cable::Writer *>(param)->write(reinterpret_cast<const float *const *>(a->data), ch, a->frames,
						   obs_source_get_volume(src), muted);
}

void unhook(Tap &t)
{
	if (!t.hooked)
		return;
	if (obs_source_t *s = obs_weak_source_get_source(t.weak)) {
		// Returns once no callback is running, so the writer can go after it.
		obs_source_remove_audio_capture_callback(s, onAudio, t.writer.get());
		obs_source_release(s);
	}
	t.hooked = false;
}

void drop(Tap &t)
{
	unhook(t);
	t.writer.reset();
	obs_weak_source_release(t.weak);
	t.weak = nullptr;
}

Tap *tapFor(obs_source_t *src)
{
	for (auto &t : taps)
		if (t.weak && obs_weak_source_references_source(t.weak, src))
			return &t;
	return nullptr;
}

bool setting(obs_source_t *src, const char *key)
{
	obs_data_t *s = obs_source_get_settings(src);
	const bool on = obs_data_get_bool(s, key);
	obs_data_release(s);
	return on;
}

/** The phone behind [src] is connected and its mic is on in the app. */
bool phoneMicOn(obs_source_t *src)
{
	auto *mic = static_cast<PhoneMic *>(obs_obj_get_data(src));
	if (!mic)
		return false;
	const std::string device = mic->device();
	for (const auto &v : Hub::get().phones())
		if (v.device == device)
			return v.connected && v.state.mic;
	return false;
}

/** Opens a writer and hooks it, or schedules a retry (no cable yet, device busy). */
void tryHook(obs_source_t *src, Tap &t, uint64_t now)
{
	if (t.hooked || now < t.nextTry)
		return;
	t.writer = std::make_unique<cable::Writer>();
	if (!t.writer->open(audio_output_get_sample_rate(obs_get_audio()))) {
		t.writer.reset();
		t.nextTry = now + kRetryNs;
		return;
	}
	obs_source_add_audio_capture_callback(src, onAudio, t.writer.get());
	t.hooked = true;
	obs_log(LOG_INFO, "'%s' plays into CABLE Input for other apps", obs_source_get_name(src));
}

void reconcile()
{
	const uint64_t now = os_gettime_ns();
	std::vector<obs_source_t *> mics;
	obs_enum_sources(
		[](void *d, obs_source_t *s) {
			if (ours(s))
				if (obs_source_t *r = obs_source_get_ref(s))
					static_cast<std::vector<obs_source_t *> *>(d)->push_back(r);
			return true;
		},
		&mics);

	// Taps whose source is gone, or turned off.
	for (auto &t : taps) {
		obs_source_t *s = obs_weak_source_get_source(t.weak);
		const bool keep = s && setting(s, "otherApps");
		obs_source_release(s);
		if (!keep)
			drop(t);
	}
	taps.erase(std::remove_if(taps.begin(), taps.end(), [](const Tap &t) { return !t.weak; }), taps.end());

	bool want = false;
	for (obs_source_t *src : mics) {
		if (setting(src, "otherApps")) {
			Tap *t = tapFor(src);
			if (!t) {
				taps.push_back({obs_source_get_weak_source(src), nullptr, false, 0});
				t = &taps.back();
			}
			if (t->hooked && t->writer->failed()) {
				// The cable went away (uninstalled, or the audio service restarted): open a new one.
				obs_log(LOG_INFO, "CABLE Input stopped; reopening");
				unhook(*t);
				t->writer.reset();
				t->nextTry = now;
			}
			tryHook(src, *t, now);
			want = want || (t->hooked && setting(src, "defaultMic") && phoneMicOn(src) && !obs_source_muted(src));
		}
		obs_source_release(src);
	}

	// Windows' default mic follows the phone's mic, on each change only.
	if (want && !wanted) {
		taken = cable::takeDefaultMic();
	} else if (!want && wanted && taken) {
		cable::releaseDefaultMic();
		taken = false;
	} else if (want && taken && !cable::isDefaultMic()) {
		cable::forgetDefaultMic();
		taken = false;
	}
	wanted = want;
}

// ---- once, from the version that borrowed OBS's monitoring device ---------------------------

/** Gives OBS's monitoring device back and moves each mic's "monitor and output" to the setting. */
void migrate()
{
	char *p = obs_module_config_path("monitoring.json");
	const QString file = QString::fromUtf8(p ? p : "");
	bfree(p);
	if (!file.isEmpty() && QFile::exists(file)) {
		QFile f(file);
		if (f.open(QIODevice::ReadOnly)) {
			json j = json::parse(f.readAll().toStdString(), nullptr, false);
			f.close();
			const char *n = nullptr, *id = nullptr;
			obs_get_audio_monitoring_device(&n, &id);
			std::string cur = n ? n : "";
			std::transform(cur.begin(), cur.end(), cur.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			if (j.is_object() && !j.value("id", "").empty() && cur.find("cable input") != std::string::npos) {
				const std::string pn = j.value("name", ""), pid = j.value("id", "");
				obs_set_audio_monitoring_device(pn.c_str(), pid.c_str());
				if (config_t *cfg = obs_frontend_get_profile_config()) {
					config_set_string(cfg, "Audio", "MonitoringDeviceName", pn.c_str());
					config_set_string(cfg, "Audio", "MonitoringDeviceId", pid.c_str());
					config_save_safe(cfg, "tmp", nullptr);
				}
				obs_log(LOG_INFO, "gave OBS's monitoring device back to '%s'", pn.c_str());
			}
		}
		QFile::remove(file);
	}
	obs_enum_sources(
		[](void *, obs_source_t *s) {
			if (ours(s) && obs_source_get_monitoring_type(s) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT) {
				obs_source_set_monitoring_type(s, OBS_MONITORING_TYPE_NONE);
				obs_data_t *st = obs_source_get_settings(s);
				obs_data_set_bool(st, "otherApps", true);
				obs_data_set_bool(st, "defaultMic", true);
				obs_data_release(st);
			}
			return true;
		},
		nullptr);
}

} // namespace

void start()
{
	if (timer)
		return;
	cable::init();
	migrate();
	timer = new QTimer();
	QObject::connect(timer, &QTimer::timeout, [] { reconcile(); });
	timer->start(1000);
	reconcile();
}

void stop()
{
	if (timer) {
		timer->stop();
		delete timer;
		timer = nullptr;
	}
	for (auto &t : taps)
		drop(t);
	taps.clear();
	if (taken)
		cable::releaseDefaultMic();
	taken = wanted = false;
}

bool inOtherApps(obs_source_t *src)
{
	return src && setting(src, "otherApps");
}

bool isDefaultMic(obs_source_t *src)
{
	return src && setting(src, "defaultMic");
}

void setInOtherApps(obs_source_t *src, bool on)
{
	if (!src)
		return;
	obs_data_t *s = obs_source_get_settings(src);
	obs_data_set_bool(s, "otherApps", on);
	if (on && !obs_data_has_user_value(s, "defaultMic"))
		obs_data_set_bool(s, "defaultMic", true);
	obs_data_release(s);
	if (Tap *t = tapFor(src))
		t->nextTry = 0;
	if (!on)
		obs_log(LOG_INFO, "'%s' no longer plays into the cable", obs_source_get_name(src));
	reconcile();
}

void setDefaultMic(obs_source_t *src, bool on)
{
	if (!src)
		return;
	obs_data_t *s = obs_source_get_settings(src);
	obs_data_set_bool(s, "defaultMic", on);
	obs_data_release(s);
	reconcile();
}

void offEverywhere()
{
	obs_enum_sources(
		[](void *, obs_source_t *s) {
			if (ours(s)) {
				obs_data_t *st = obs_source_get_settings(s);
				obs_data_set_bool(st, "otherApps", false);
				obs_data_release(st);
			}
			return true;
		},
		nullptr);
	reconcile();
}

bool playing(obs_source_t *src)
{
	const Tap *t = src ? tapFor(src) : nullptr;
	return t && t->hooked && t->writer && !t->writer->failed();
}

} // namespace rac::routing
