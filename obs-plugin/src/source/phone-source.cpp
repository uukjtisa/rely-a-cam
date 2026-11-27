// SPDX-License-Identifier: GPL-2.0-or-later
#include "phone-source.hpp"

#include "../dock/dock.hpp"
#include "../net/hub.hpp"

#include <graphics/matrix4.h>
#include <nlohmann/json.hpp>

#include <cmath>

using nlohmann::json;

namespace rac {

PhoneSource::PhoneSource(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	update(settings);
	// Remembered camera settings, as last saved with the scene collection.
	const char *saved = obs_data_get_string(settings, "remembered");
	if (saved && *saved) {
		json j = json::parse(saved, nullptr, false);
		if (j.is_object()) {
			remembered_ = true;
			cam_ = j.value("cam", "");
			mode_ = j.value("mode", "");
			codec_ = j.value("codec", "");
			rMirror_ = j.value("mirror", false);
			rFlip_ = j.value("flip", false);
			rLock_ = j.value("lock", false);
			rMicOnly_ = j.value("micOnly", false);
			if (j.contains("zooms") && j["zooms"].is_object())
				for (auto &[k, v] : j["zooms"].items())
					if (v.is_number())
						zooms_[k] = v.get<double>();
		}
	}
	registerHotkeys();
	Hub::get().registerSource(this);
}

PhoneSource::~PhoneSource()
{
	Hub::get().unregisterSource(this);
	obs_enter_graphics();
	if (tex_)
		gs_texture_destroy(tex_);
	obs_leave_graphics();
}

std::string PhoneSource::device() const
{
	std::lock_guard<std::mutex> l(m_);
	return device_;
}

void PhoneSource::update(obs_data_t *settings)
{
	std::string d = obs_data_get_string(settings, "device");
	bool changed;
	{
		std::lock_guard<std::mutex> l(m_);
		changed = d != device_;
		device_ = d;
	}
	if (changed)
		Hub::get().sourceChanged();
}

void PhoneSource::save(obs_data_t *settings)
{
	std::lock_guard<std::mutex> l(m_);
	if (!remembered_)
		return;
	json j = {{"cam", cam_},    {"mode", mode_},  {"codec", codec_},       {"mirror", rMirror_},
		  {"flip", rFlip_}, {"lock", rLock_}, {"micOnly", rMicOnly_}, {"zooms", zooms_}};
	obs_data_set_string(settings, "remembered", j.dump().c_str());
}

void PhoneSource::pushPicture(Picture &p)
{
	std::lock_guard<std::mutex> l(m_);
	// Swap rather than copy: the decoder gets the old buffer back, so nothing is allocated per frame.
	std::swap(pending_, p);
	hasPending_ = true;
}

void PhoneSource::setTransform(int rot, bool mirror, bool flip)
{
	std::lock_guard<std::mutex> l(m_);
	rot_ = ((rot % 360) + 360) % 360;
	mirror_ = mirror;
	flip_ = flip;
}

void PhoneSource::remember(const State &s)
{
	std::lock_guard<std::mutex> l(m_);
	remembered_ = true;
	if (!s.cam.empty()) {
		cam_ = s.cam;
		zooms_[s.cam] = std::round(s.zoom * 1000) / 1000;
	}
	if (!s.mode.empty())
		mode_ = s.mode;
	codec_ = s.codec;
	rMirror_ = s.mirror;
	rFlip_ = s.flip;
	rLock_ = s.lock;
	rMicOnly_ = s.micOnly;
}

void PhoneSource::setConnected(bool on)
{
	std::lock_guard<std::mutex> l(m_);
	connected_ = on;
}

void PhoneSource::setCameraOff(bool off)
{
	std::lock_guard<std::mutex> l(m_);
	cameraOff_ = off;
}

std::string PhoneSource::restoreJson()
{
	std::lock_guard<std::mutex> l(m_);
	if (!remembered_)
		return "{}";
	json j = {{"mirror", rMirror_}, {"flip", rFlip_}, {"lock", rLock_}, {"micOnly", rMicOnly_}};
	if (!cam_.empty())
		j["cam"] = cam_;
	if (!mode_.empty())
		j["mode"] = mode_;
	if (!codec_.empty())
		j["codec"] = codec_;
	if (!zooms_.empty())
		j["zooms"] = zooms_;
	return j.dump();
}

uint32_t PhoneSource::width()
{
	std::lock_guard<std::mutex> l(m_);
	return uint32_t(rot_ % 180 ? th_ : tw_);
}

uint32_t PhoneSource::height()
{
	std::lock_guard<std::mutex> l(m_);
	return uint32_t(rot_ % 180 ? tw_ : th_);
}

void PhoneSource::render(gs_effect_t *)
{
	int rot;
	bool mirror, flip, connected, off;
	{
		std::lock_guard<std::mutex> l(m_);
		if (hasPending_ && pending_.width > 0) {
			if (!tex_ || tw_ != pending_.width || th_ != pending_.height) {
				if (tex_)
					gs_texture_destroy(tex_);
				tex_ = gs_texture_create(uint32_t(pending_.width), uint32_t(pending_.height), GS_BGRA, 1,
							 nullptr, GS_DYNAMIC);
				tw_ = pending_.width;
				th_ = pending_.height;
			}
			if (tex_)
				gs_texture_set_image(tex_, pending_.bgra.data(), uint32_t(pending_.linesize), false);
			hasPending_ = false;
		}
		rot = rot_;
		mirror = mirror_;
		flip = flip_;
		connected = connected_;
		off = cameraOff_;
	}
	if (!tex_ || !connected || off)
		return;

	const float tw = float(tw_), th = float(th_);
	const float ow = rot % 180 ? th : tw;
	const float oh = rot % 180 ? tw : th;

	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(eff, "image"), tex_);

	// Turn about the centre, then mirror/flip in the upright picture (PROTOCOL.md, STATE).
	gs_matrix_push();
	gs_matrix_translate3f(ow / 2, oh / 2, 0);
	gs_matrix_scale3f(mirror ? -1.0f : 1.0f, flip ? -1.0f : 1.0f, 1.0f);
	gs_matrix_rotaa4f(0, 0, 1, RAD(float(rot)));
	gs_matrix_translate3f(-tw / 2, -th / 2, 0);
	while (gs_effect_loop(eff, "Draw"))
		gs_draw_sprite(tex_, 0, uint32_t(tw), uint32_t(th));
	gs_matrix_pop();

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

// ---- hotkeys: per source, so each phone gets its own in Settings > Hotkeys ----------------

namespace {
struct HotkeyDef {
	const char *name;
	const char *label;
	void (*act)(const std::string &device);
};

const HotkeyDef kHotkeys[] = {
	{"relyacam.zoom_in", "Zoom in", [](const std::string &d) { Hub::get().nudgeZoom(d, 1.12); }},
	{"relyacam.zoom_out", "Zoom out", [](const std::string &d) { Hub::get().nudgeZoom(d, 1 / 1.12); }},
	{"relyacam.preset1", "Zoom preset 1", [](const std::string &d) { Hub::get().preset(d, 0); }},
	{"relyacam.preset2", "Zoom preset 2", [](const std::string &d) { Hub::get().preset(d, 1); }},
	{"relyacam.preset3", "Zoom preset 3", [](const std::string &d) { Hub::get().preset(d, 2); }},
	{"relyacam.preset4", "Zoom preset 4", [](const std::string &d) { Hub::get().preset(d, 3); }},
	{"relyacam.preset5", "Zoom preset 5", [](const std::string &d) { Hub::get().preset(d, 4); }},
	{"relyacam.preset6", "Zoom preset 6", [](const std::string &d) { Hub::get().preset(d, 5); }},
	{"relyacam.preset7", "Zoom preset 7", [](const std::string &d) { Hub::get().preset(d, 6); }},
	{"relyacam.preset8", "Zoom preset 8", [](const std::string &d) { Hub::get().preset(d, 7); }},
	{"relyacam.preset9", "Zoom preset 9", [](const std::string &d) { Hub::get().preset(d, 8); }},
	{"relyacam.torch", "Torch on/off", [](const std::string &d) { Hub::get().toggle(d, "torch"); }},
	{"relyacam.next_cam", "Next camera", [](const std::string &d) { Hub::get().nextCam(d); }},
	{"relyacam.mirror", "Mirror on/off", [](const std::string &d) { Hub::get().toggle(d, "mirror"); }},
};
} // namespace

void PhoneSource::registerHotkeys()
{
	static_assert(sizeof(kHotkeys) / sizeof(kHotkeys[0]) == sizeof(hotkeys_) / sizeof(hotkeys_[0]));
	for (size_t i = 0; i < sizeof(kHotkeys) / sizeof(kHotkeys[0]); i++) {
		hotkeys_[i] = obs_hotkey_register_source(
			source_, kHotkeys[i].name, kHotkeys[i].label,
			[](void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed) {
				if (!pressed)
					return;
				auto *self = static_cast<PhoneSource *>(data);
				for (size_t k = 0; k < sizeof(kHotkeys) / sizeof(kHotkeys[0]); k++)
					if (self->hotkeys_[k] == id)
						kHotkeys[k].act(self->device());
			},
			this);
	}
}

// ---- registration -------------------------------------------------------------------------

void PhoneSource::registerSource()
{
	static obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	// No OBS_SOURCE_AUDIO: the mic is its own source, so it can stay on while another camera shows.
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = [](void *) { return obs_module_text("PhoneCamera"); };
	info.create = [](obs_data_t *s, obs_source_t *src) -> void * { return new PhoneSource(s, src); };
	info.destroy = [](void *d) { delete static_cast<PhoneSource *>(d); };
	info.update = [](void *d, obs_data_t *s) { static_cast<PhoneSource *>(d)->update(s); };
	info.save = [](void *d, obs_data_t *s) { static_cast<PhoneSource *>(d)->save(s); };
	info.video_render = [](void *d, gs_effect_t *e) { static_cast<PhoneSource *>(d)->render(e); };
	info.get_width = [](void *d) { return static_cast<PhoneSource *>(d)->width(); };
	info.get_height = [](void *d) { return static_cast<PhoneSource *>(d)->height(); };
	info.get_properties = [](void *) -> obs_properties_t * {
		obs_properties_t *p = obs_properties_create();
		obs_property_t *list = obs_properties_add_list(p, "device", obs_module_text("Phone"), OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_STRING);
		const auto phones = Hub::get().phones();
		if (phones.empty()) {
			obs_property_list_add_string(list, obs_module_text("NoPhoneYet"), "");
		} else {
			obs_property_list_add_string(list, obs_module_text("FirstPhoneToConnect"), "");
			for (auto &ph : phones)
				obs_property_list_add_string(list, ph.name.c_str(), ph.device.c_str());
		}
		obs_properties_add_button2(
			p, "pair", obs_module_text(phones.empty() ? "PairPhone" : "OpenDock"),
			[](obs_properties_t *, obs_property_t *, void *) {
				PhoneDock::reveal(Hub::get().phones().empty());
				return false;
			},
			nullptr);
		obs_properties_add_text(p, "help", obs_module_text("SourceHelp"), OBS_TEXT_INFO);
		return p;
	};
	obs_register_source(&info);
}

} // namespace rac
