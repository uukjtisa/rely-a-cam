// SPDX-License-Identifier: GPL-2.0-or-later
#include "cable.hpp"

#include "../sys/system.hpp"

#include <obs-module.h>
#include "plugin-support.h"
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QThread>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <wrl/client.h>
#endif

using nlohmann::json;

namespace rac::cable {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

namespace {

// PKEY_Device_FriendlyName and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out so this file needs no
// INITGUID dance.
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Windows' own (undocumented, stable since Vista) way to change the default device: the Sound
// control panel uses it, as do EarTrumpet and SoundSwitch. Only SetDefaultEndpoint is called.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown
{
public:
	virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
	virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

constexpr ERole kRoles[3] = {eConsole, eMultimedia, eCommunications};
constexpr const char *kRoleKeys[3] = {"console", "multimedia", "communications"};
constexpr const wchar_t *kRegKey = L"Software\\Rely-A-Cam";

/** COM for this thread, if it has none yet (the UI thread already has it). */
struct Com {
	HRESULT hr;
	Com() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
	~Com()
	{
		if (SUCCEEDED(hr))
			CoUninitialize();
	}
};

std::wstring lower(std::wstring s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
	return s;
}

std::string utf8(const std::wstring &w)
{
	return QString::fromStdWString(w).toStdString();
}

std::wstring wide(const std::string &s)
{
	return QString::fromStdString(s).toStdWString();
}

ComPtr<IMMDeviceEnumerator> enumerator()
{
	ComPtr<IMMDeviceEnumerator> e;
	CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
	return e;
}

/** The id of the active endpoint whose name contains [part] (lower case), or empty. */
std::wstring findEndpoint(EDataFlow flow, const wchar_t *part)
{
	auto e = enumerator();
	ComPtr<IMMDeviceCollection> c;
	if (!e || FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &c)))
		return {};
	UINT n = 0;
	c->GetCount(&n);
	for (UINT i = 0; i < n; i++) {
		ComPtr<IMMDevice> d;
		ComPtr<IPropertyStore> ps;
		if (FAILED(c->Item(i, &d)) || FAILED(d->OpenPropertyStore(STGM_READ, &ps)))
			continue;
		PROPVARIANT v;
		PropVariantInit(&v);
		bool hit = SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal &&
			   lower(v.pwszVal).find(part) != std::wstring::npos;
		PropVariantClear(&v);
		LPWSTR id = nullptr;
		if (hit && SUCCEEDED(d->GetId(&id)) && id) {
			std::wstring out = id;
			CoTaskMemFree(id);
			return out;
		}
	}
	return {};
}

std::wstring defaultId(EDataFlow flow, ERole role)
{
	auto e = enumerator();
	ComPtr<IMMDevice> d;
	LPWSTR id = nullptr;
	if (!e || FAILED(e->GetDefaultAudioEndpoint(flow, role, &d)) || FAILED(d->GetId(&id)) || !id)
		return {};
	std::wstring out = id;
	CoTaskMemFree(id);
	return out;
}

bool setDefault(const std::wstring &id, ERole role)
{
	ComPtr<IPolicyConfig> pc;
	if (id.empty() || FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pc))))
		return false;
	return SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), role));
}

/** Every default device, playback and recording, all three roles. */
struct Defaults {
	std::array<std::wstring, 3> render, capture;
};

Defaults currentDefaults()
{
	Defaults d;
	for (int r = 0; r < 3; r++) {
		d.render[r] = defaultId(eRender, kRoles[r]);
		d.capture[r] = defaultId(eCapture, kRoles[r]);
	}
	return d;
}

/** Puts [d] back wherever the default changed (a device that's gone is skipped). */
void restoreDefaults(const Defaults &d)
{
	for (int r = 0; r < 3; r++) {
		if (!d.render[r].empty() && defaultId(eRender, kRoles[r]) != d.render[r])
			setDefault(d.render[r], kRoles[r]);
		if (!d.capture[r].empty() && defaultId(eCapture, kRoles[r]) != d.capture[r])
			setDefault(d.capture[r], kRoles[r]);
	}
}

std::string savedMicFile()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("default-mic.json");
	std::string s = p ? p : "default-mic.json";
	bfree(p);
	return s;
}

QString packDir()
{
	return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Rely-A-Cam/vbcable";
}

/** Checks the publisher's signature (VB-Audio signs as its owner's sole-trader company). */
bool signedByVbAudio(const std::wstring &path, std::string &who)
{
	WINTRUST_FILE_INFO fi = {};
	fi.cbStruct = sizeof fi;
	fi.pcwszFilePath = path.c_str();
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	WINTRUST_DATA wd = {};
	wd.cbStruct = sizeof wd;
	wd.dwUIChoice = WTD_UI_NONE;
	wd.fdwRevocationChecks = WTD_REVOKE_NONE;
	wd.dwUnionChoice = WTD_CHOICE_FILE;
	wd.pFile = &fi;
	wd.dwStateAction = WTD_STATEACTION_VERIFY;
	bool ok = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd) == ERROR_SUCCESS;
	if (ok) {
		CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
		CRYPT_PROVIDER_SGNR *sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
		CRYPT_PROVIDER_CERT *cert = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
		wchar_t name[256] = {};
		if (cert && cert->pCert)
			CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
		who = utf8(name);
		const std::wstring n = lower(name);
		ok = n.find(L"burel vincent") != std::wstring::npos || n.find(L"vb-audio") != std::wstring::npos;
	}
	wd.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
	return ok;
}

/** The pack, downloaded and unpacked, with the 64-bit setup checked. Empty and [err] on failure. */
QString preparePack(std::string &err, const std::function<void(int)> &step)
{
	const QString dir = packDir();
	QDir().mkpath(dir);
	const QString zip = dir + "/VBCABLE_Driver_Pack46.zip";
	const QString pack = dir + "/pack";
	if (step)
		step(0);
	if (!sys::download(kPackUrl, zip.toStdString(), [](int) {})) {
		err = "The download from vb-audio.com failed. Check the internet connection, or install it from vb-audio.com/Cable.";
		return {};
	}
	QDir(pack).removeRecursively();
	QDir().mkpath(pack);
	// Windows' own tar (Windows 10 1803 and later) unpacks zips; called by full path.
	const QString tar = qEnvironmentVariable("SystemRoot", "C:\\Windows") + "\\System32\\tar.exe";
	QProcess p;
	p.start(tar, {"-xf", QDir::toNativeSeparators(zip), "-C", QDir::toNativeSeparators(pack)});
	if (!p.waitForFinished(60000) || p.exitCode() != 0) {
		err = "The VB-Cable download couldn't be unpacked.";
		return {};
	}
	const QString setup = QDir::toNativeSeparators(pack + "/VBCABLE_Setup_x64.exe");
	if (!QFileInfo::exists(setup)) {
		err = "The VB-Cable download doesn't have its 64-bit installer.";
		return {};
	}
	if (step)
		step(1);
	std::string who;
	if (!signedByVbAudio(setup.toStdWString(), who)) {
		obs_log(LOG_WARNING, "VB-Cable setup signature rejected (signer '%s')", who.c_str());
		err = "The download isn't signed by VB-Audio, so it wasn't run.";
		return {};
	}
	obs_log(LOG_INFO, "VB-Cable setup signed by '%s'", who.c_str());
	return setup;
}

void mark(const QString &setup)
{
	const DWORD one = 1;
	RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"VBCable", REG_DWORD, &one, sizeof one);
	const std::wstring s = setup.toStdWString();
	RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"VBCableSetup", REG_SZ, s.c_str(), DWORD((s.size() + 1) * sizeof(wchar_t)));
}

void unmark()
{
	RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegKey, L"VBCable");
	RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegKey, L"VBCableSetup");
}

} // namespace

bool installed()
{
	Com com;
	return !findEndpoint(eRender, L"cable input").empty();
}

bool installedByUs()
{
	DWORD v = 0, size = sizeof v;
	return RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"VBCable", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v == 1;
}

Result install(const std::function<void(int)> &step)
{
	Com com;
	std::string err;
	const QString setup = preparePack(err, step);
	if (setup.isEmpty())
		return {false, err};
	step(2);
	const Defaults before = currentDefaults();
	unsigned long code = 0;
	if (!sys::runElevated(setup.toStdString(), "-i -h", 180000, &code))
		return {false, "Windows' admin prompt was declined, so VB-Cable wasn't installed."};
	obs_log(LOG_INFO, "VB-Cable setup exited with %lu", code);
	mark(setup);
	// The driver can take a few seconds to show its endpoints.
	for (int i = 0; i < 30 && findEndpoint(eRender, L"cable input").empty(); i++)
		QThread::msleep(500);
	step(3);
	restoreDefaults(before);
	if (findEndpoint(eRender, L"cable input").empty())
		return {false, "VB-Cable's installer finished, but Windows doesn't list CABLE Input yet. Restart the PC, then turn Use in other apps on."};
	return {true, ""};
}

Result remove()
{
	Com com;
	QString setup;
	{
		wchar_t buf[MAX_PATH * 2] = {};
		DWORD size = sizeof buf;
		if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"VBCableSetup", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
			setup = QString::fromWCharArray(buf);
	}
	std::string err;
	if (setup.isEmpty() || !QFileInfo::exists(setup))
		setup = preparePack(err, {});
	else {
		std::string who;
		if (!signedByVbAudio(setup.toStdWString(), who))
			setup = preparePack(err, {});
	}
	if (setup.isEmpty())
		return {false, err};
	unsigned long code = 0;
	if (!sys::runElevated(setup.toStdString(), "-u -h", 180000, &code))
		return {false, "Windows' admin prompt was declined, so VB-Cable is still installed."};
	obs_log(LOG_INFO, "VB-Cable uninstall exited with %lu", code);
	unmark();
	for (int i = 0; i < 10 && !findEndpoint(eRender, L"cable input").empty(); i++)
		QThread::msleep(500);
	return {true, findEndpoint(eRender, L"cable input").empty() ? "VB-Cable is removed."
								     : "VB-Cable is removed. Restart the PC to finish."};
}

bool takeDefaultMic()
{
	const std::wstring cable = findEndpoint(eCapture, L"cable output");
	if (cable.empty())
		return false;
	const QString file = QString::fromStdString(savedMicFile());
	// Saved once: a second phone, or a crash, must never save CABLE Output as "the previous one".
	if (!QFileInfo::exists(file) && defaultId(eCapture, eConsole) != cable) {
		json j = json::object();
		for (int r = 0; r < 3; r++)
			j[kRoleKeys[r]] = utf8(defaultId(eCapture, kRoles[r]));
		QFile f(file);
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
			f.write(QByteArray::fromStdString(j.dump()));
	}
	bool ok = true;
	for (ERole r : kRoles)
		ok = setDefault(cable, r) && ok;
	obs_log(LOG_INFO, "CABLE Output is Windows' default mic%s", ok ? "" : " (partly failed)");
	return ok;
}

void releaseDefaultMic()
{
	const QString file = QString::fromStdString(savedMicFile());
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly))
		return;
	json j = json::parse(f.readAll().toStdString(), nullptr, false);
	f.close();
	const std::wstring cable = findEndpoint(eCapture, L"cable output");
	if (j.is_object())
		for (int r = 0; r < 3; r++) {
			const std::wstring prev = wide(j.value(kRoleKeys[r], ""));
			// Only while it's still the cable: a default he picked himself in the meantime stays.
			if (!prev.empty() && (cable.empty() || defaultId(eCapture, kRoles[r]) == cable))
				setDefault(prev, kRoles[r]);
		}
	QFile::remove(file);
	obs_log(LOG_INFO, "Windows' previous default mic is back");
}

bool isDefaultMic()
{
	const std::wstring cable = findEndpoint(eCapture, L"cable output");
	return !cable.empty() && defaultId(eCapture, eConsole) == cable;
}

void forgetDefaultMic()
{
	QFile::remove(QString::fromStdString(savedMicFile()));
	obs_log(LOG_INFO, "Windows' default mic was changed by hand; the phone lets it be");
}

void init()
{
	releaseDefaultMic();
}

// ---- the writer ------------------------------------------------------------------------------

struct Writer::Impl {
	ComPtr<IAudioClient> client;
	ComPtr<IAudioRenderClient> render;
	UINT32 bufferFrames = 0;
	bool started = false;
	std::atomic<bool> failed{false};
	std::mutex m;
};

Writer::Writer() : d_(std::make_unique<Impl>()) {}

Writer::~Writer()
{
	std::lock_guard<std::mutex> l(d_->m);
	if (d_->client && d_->started)
		d_->client->Stop();
}

bool Writer::open(uint32_t rate)
{
	const std::wstring id = findEndpoint(eRender, L"cable input");
	auto e = enumerator();
	ComPtr<IMMDevice> dev;
	if (id.empty() || !e || FAILED(e->GetDevice(id.c_str(), &dev)) ||
	    FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(d_->client.GetAddressOf()))))
		return false;
	WAVEFORMATEXTENSIBLE f = {};
	f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	f.Format.nChannels = 2;
	f.Format.nSamplesPerSec = rate;
	f.Format.wBitsPerSample = 32;
	f.Format.nBlockAlign = 2 * 4;
	f.Format.nAvgBytesPerSec = rate * 2 * 4;
	f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	f.Samples.wValidBitsPerSample = 32;
	f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	f.SubFormat = kFloat;
	// 200 ms of room; Windows converts to the cable's own format.
	const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	HRESULT hr = d_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &f.Format, nullptr);
	if (FAILED(hr) || FAILED(d_->client->GetBufferSize(&d_->bufferFrames)) ||
	    FAILED(d_->client->GetService(IID_PPV_ARGS(&d_->render)))) {
		obs_log(LOG_WARNING, "CABLE Input refused the stream (0x%08lx)", static_cast<unsigned long>(hr));
		d_->client.Reset();
		return false;
	}
	return true;
}

void Writer::write(const float *const *planes, uint32_t channels, uint32_t frames, float volume, bool silent)
{
	std::lock_guard<std::mutex> l(d_->m);
	if (d_->failed || !d_->render || channels == 0)
		return;
	UINT32 pad = 0;
	if (FAILED(d_->client->GetCurrentPadding(&pad))) {
		d_->failed = true;
		return;
	}
	// More than fits (OBS's clock ran ahead of the cable's) is dropped, never queued.
	const UINT32 n = std::min<UINT32>(frames, d_->bufferFrames - pad);
	if (n == 0)
		return;
	BYTE *buf = nullptr;
	if (FAILED(d_->render->GetBuffer(n, &buf))) {
		d_->failed = true;
		return;
	}
	DWORD flags = 0;
	if (silent || volume <= 0.0f || !planes[0]) {
		flags = AUDCLNT_BUFFERFLAGS_SILENT;
	} else {
		float *out = reinterpret_cast<float *>(buf);
		const float *l = planes[0];
		const float *r = channels > 1 && planes[1] ? planes[1] : planes[0];
		for (UINT32 i = 0; i < n; i++) {
			out[2 * i] = l[i] * volume;
			out[2 * i + 1] = r[i] * volume;
		}
	}
	if (FAILED(d_->render->ReleaseBuffer(n, flags))) {
		d_->failed = true;
		return;
	}
	if (!d_->started) {
		d_->started = SUCCEEDED(d_->client->Start());
	}
}

bool Writer::failed() const
{
	return d_->failed;
}

#else // not Windows: VB-Cable is a Windows driver.

bool installed() { return false; }
bool installedByUs() { return false; }
Result install(const std::function<void(int)> &) { return {false, "VB-Cable is Windows only."}; }
Result remove() { return {false, "VB-Cable is Windows only."}; }
bool takeDefaultMic() { return false; }
void releaseDefaultMic() {}
bool isDefaultMic() { return false; }
void forgetDefaultMic() {}
void init() {}
struct Writer::Impl {};
Writer::Writer() : d_(std::make_unique<Impl>()) {}
Writer::~Writer() = default;
bool Writer::open(uint32_t) { return false; }
void Writer::write(const float *const *, uint32_t, uint32_t, float, bool) {}
bool Writer::failed() const { return true; }

#endif

} // namespace rac::cable
