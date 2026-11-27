// SPDX-License-Identifier: GPL-2.0-or-later
#include "phone-mic.hpp"

#include "../dock/dock.hpp"
#include "../net/hub.hpp"

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace rac {

PhoneMic::PhoneMic(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	update(settings);
	const char *saved = obs_data_get_string(settings, "remembered");
	if (saved && *saved) {
		json j = json::parse(saved, nullptr, false);
		if (j.is_object()) {
			remembered_ = true;
			mic_ = j.value("mic", true);
			micIn_ = j.value("micIn", "");
			ns_ = j.value("ns", false);
			aec_ = j.value("aec", false);
			agc_ = j.value("agc", false);
		}
	}
	Hub::get().registerMic(this);
}

PhoneMic::~PhoneMic()
{
	Hub::get().unregisterMic(this);
}

std::string PhoneMic::device() const
{
	std::lock_guard<std::mutex> l(m_);
	return device_;
}

void PhoneMic::update(obs_data_t *settings)
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

void PhoneMic::save(obs_data_t *settings)
{
	std::lock_guard<std::mutex> l(m_);
	if (!remembered_)
		return;
	json j = {{"mic", mic_}, {"micIn", micIn_}, {"ns", ns_}, {"aec", aec_}, {"agc", agc_}};
	obs_data_set_string(settings, "remembered", j.dump().c_str());
}

void PhoneMic::remember(const State &s)
{
	std::lock_guard<std::mutex> l(m_);
	remembered_ = true;
	mic_ = s.mic;
	if (!s.micIn.empty())
		micIn_ = s.micIn;
	ns_ = s.ns;
	aec_ = s.aec;
	agc_ = s.agc;
}

bool PhoneMic::hasMemory() const
{
	std::lock_guard<std::mutex> l(m_);
	return remembered_;
}

std::string PhoneMic::restoreJson() const
{
	std::lock_guard<std::mutex> l(m_);
	json j = {{"mic", mic_}, {"ns", ns_}, {"aec", aec_}, {"agc", agc_}};
	if (!micIn_.empty())
		j["micIn"] = micIn_;
	return j.dump();
}

void PhoneMic::registerSource()
{
	static obs_source_info info = {};
	info.id = kMicSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
	info.get_name = [](void *) { return obs_module_text("PhoneMic"); };
	info.create = [](obs_data_t *s, obs_source_t *src) -> void * { return new PhoneMic(s, src); };
	info.destroy = [](void *d) { delete static_cast<PhoneMic *>(d); };
	info.update = [](void *d, obs_data_t *s) { static_cast<PhoneMic *>(d)->update(s); };
	info.save = [](void *d, obs_data_t *s) { static_cast<PhoneMic *>(d)->save(s); };
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
		obs_properties_add_text(p, "help", obs_module_text("MicSourceHelp"), OBS_TEXT_INFO);
		return p;
	};
	obs_register_source(&info);
}

} // namespace rac
