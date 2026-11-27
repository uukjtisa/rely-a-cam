// SPDX-License-Identifier: GPL-2.0-or-later
#include "system.hpp"


#include <obs-module.h>
#include "plugin-support.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#endif

using nlohmann::json;

namespace rac::sys {

static const char *kRuleTcp = "Rely-A-Cam (phones, TCP)";
static const char *kRuleUdp = "Rely-A-Cam (discovery, UDP)";

/** Runs a program and returns its exit code and output. -1 when it could not start. */
static int run(const QString &exe, const QStringList &args, std::string *out = nullptr, int timeoutMs = 20000)
{
	QProcess p;
	p.setProcessChannelMode(QProcess::MergedChannels);
	p.start(exe, args);
	if (!p.waitForStarted(5000))
		return -1;
	if (!p.waitForFinished(timeoutMs)) {
		p.kill();
		p.waitForFinished(2000);
		return -2;
	}
	if (out)
		*out = p.readAll().toStdString();
	return p.exitCode();
}

bool firewallRulesPresent()
{
#ifdef _WIN32
	return run("netsh", {"advfirewall", "firewall", "show", "rule", QString("name=%1").arg(kRuleTcp)}) == 0;
#else
	return true;
#endif
}

bool allowThroughFirewall(uint16_t tcpPort, uint16_t udpPort)
{
#ifdef _WIN32
	const std::string obs = QCoreApplication::applicationFilePath().replace('/', '\\').toStdString();
	std::ostringstream c;
	c << "/c netsh advfirewall firewall delete rule name=\"" << kRuleTcp << "\" & "
	  << "netsh advfirewall firewall delete rule name=\"" << kRuleUdp << "\" & "
	  << "netsh advfirewall firewall add rule name=\"" << kRuleTcp << "\" dir=in action=allow protocol=TCP localport="
	  << tcpPort << " program=\"" << obs << "\" profile=any & "
	  << "netsh advfirewall firewall add rule name=\"" << kRuleUdp << "\" dir=in action=allow protocol=UDP localport="
	  << udpPort << " program=\"" << obs << "\" profile=any";
	const std::wstring params = QString::fromStdString(c.str()).toStdWString();
	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof sei;
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.lpVerb = L"runas";
	sei.lpFile = L"cmd.exe";
	sei.lpParameters = params.c_str();
	sei.nShow = SW_HIDE;
	if (!ShellExecuteExW(&sei) || !sei.hProcess)
		return false;
	WaitForSingleObject(sei.hProcess, 30000);
	CloseHandle(sei.hProcess);
	return firewallRulesPresent();
#else
	(void)tcpPort;
	(void)udpPort;
	return true;
#endif
}

bool runElevated(const std::string &exe, const std::string &args, unsigned long timeoutMs, unsigned long *exitCode)
{
#ifdef _WIN32
	const std::wstring file = QString::fromStdString(exe).toStdWString();
	const std::wstring params = QString::fromStdString(args).toStdWString();
	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof sei;
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.lpVerb = L"runas";
	sei.lpFile = file.c_str();
	sei.lpParameters = params.c_str();
	sei.nShow = SW_HIDE;
	if (!ShellExecuteExW(&sei) || !sei.hProcess)
		return false;
	WaitForSingleObject(sei.hProcess, DWORD(timeoutMs));
	DWORD code = STILL_ACTIVE;
	GetExitCodeProcess(sei.hProcess, &code);
	CloseHandle(sei.hProcess);
	if (exitCode)
		*exitCode = code;
	return true;
#else
	(void)exe;
	(void)args;
	(void)timeoutMs;
	(void)exitCode;
	return false;
#endif
}

// ---- USB (Phase 3) ------------------------------------------------------------------------

std::string adbPath()
{
	char *p = obs_module_file("adb/adb.exe");
	std::string s = p ? p : "";
	bfree(p);
	if (!s.empty() && QFileInfo::exists(QString::fromStdString(s)))
		return s;
	QString onPath = QStandardPaths::findExecutable("adb");
	return onPath.toStdString();
}

struct Device {
	std::string serial, state;
};

static std::vector<Device> devices(const QString &adb)
{
	std::string out;
	std::vector<Device> d;
	if (run(adb, {"devices"}, &out) != 0)
		return d;
	std::istringstream in(out);
	std::string line;
	while (std::getline(in, line)) {
		if (line.rfind("List of", 0) == 0 || line.find('\t') == std::string::npos)
			continue;
		auto tab = line.find('\t');
		std::string state = line.substr(tab + 1);
		while (!state.empty() && (state.back() == '\r' || state.back() == ' '))
			state.pop_back();
		d.push_back({line.substr(0, tab), state});
	}
	return d;
}

static Result pickDevice(const QString &adb, std::string &serial)
{
	auto all = devices(adb);
	for (auto &d : all)
		if (d.state == "device") {
			serial = d.serial;
			return {true, ""};
		}
	for (auto &d : all)
		if (d.state == "unauthorized")
			return {false, "The phone is asking to allow USB debugging. Unlock it, tick \"Always allow\", and tap Allow."};
	return {false, "No phone found over USB. Plug it in, then turn on USB debugging: Settings > About phone > tap "
		       "Build number 7 times, then Settings > Developer options > USB debugging."};
}

static bool appInstalled(const QString &adb, const std::string &serial)
{
	std::string out;
	run(adb, {"-s", QString::fromStdString(serial), "shell", "pm", "list", "packages", "dev.niccc2007.relyacam"}, &out);
	return out.find("package:dev.niccc2007.relyacam") != std::string::npos;
}

static Result install(const QString &adb, const std::string &serial, const std::string &apk)
{
	if (apk.empty() || !QFileInfo::exists(QString::fromStdString(apk)))
		return {false, "This plugin has no copy of the phone app. Get it from github.com/uukjtisa/rely-a-cam."};
	std::string out;
	int code = run(adb, {"-s", QString::fromStdString(serial), "install", "-r", QString::fromStdString(apk)}, &out, 120000);
	if (code != 0 || out.find("Success") == std::string::npos)
		return {false, "Installing the app failed: " + out.substr(0, 200)};
	return {true, "Installed the Rely-A-Cam app on the phone."};
}

Result connectOverUsb(const std::string &pairingUri, uint16_t port, const std::string &apkPath)
{
	const QString adb = QString::fromStdString(adbPath());
	if (adb.isEmpty())
		return {false, "adb is missing from the plugin folder. Reinstall Rely-A-Cam."};
	std::string serial;
	Result r = pickDevice(adb, serial);
	if (!r.ok)
		return r;
	const QString s = QString::fromStdString(serial);
	std::string note;
	if (!appInstalled(adb, serial)) {
		Result i = install(adb, serial, apkPath);
		if (!i.ok)
			return i;
		note = i.message + " ";
	}
	const QString fwd = QString("tcp:%1").arg(port);
	if (run(adb, {"-s", s, "reverse", fwd, fwd}) != 0)
		return {false, "adb could not forward the port to the phone. Unplug it, plug it back in, and try again."};
	if (run(adb, {"-s", s, "shell", "am", "start", "-a", "android.intent.action.VIEW", "-d",
		      QString("'%1'").arg(QString::fromStdString(pairingUri))}) != 0)
		return {false, "The phone did not open Rely-A-Cam. Open the app on the phone, then try again."};
	return {true, note + "Connecting over USB. The phone opens Rely-A-Cam and connects by itself."};
}

Result installOverUsb(const std::string &apkPath)
{
	const QString adb = QString::fromStdString(adbPath());
	if (adb.isEmpty())
		return {false, "adb is missing from the plugin folder. Reinstall Rely-A-Cam."};
	std::string serial;
	Result r = pickDevice(adb, serial);
	if (!r.ok)
		return r;
	Result i = install(adb, serial, apkPath);
	if (!i.ok)
		return i;
	run(adb, {"-s", QString::fromStdString(serial), "shell", "am", "start", "-n",
		  "dev.niccc2007.relyacam/.MainActivity"});
	return {true, i.message + " It's open on the phone now."};
}

// ---- updater (Phase 4): WinHTTP, because OBS ships Qt without a TLS backend ---------------

#ifdef _WIN32
struct UrlParts {
	std::wstring host, path;
	INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
	bool https = true;
};

static bool splitUrl(const std::string &url, UrlParts &u)
{
	std::wstring w = QString::fromStdString(url).toStdWString();
	URL_COMPONENTS c = {};
	c.dwStructSize = sizeof c;
	wchar_t host[256], path[2048];
	c.lpszHostName = host;
	c.dwHostNameLength = 256;
	c.lpszUrlPath = path;
	c.dwUrlPathLength = 2048;
	wchar_t extra[2048];
	c.lpszExtraInfo = extra;
	c.dwExtraInfoLength = 2048;
	if (!WinHttpCrackUrl(w.c_str(), 0, 0, &c))
		return false;
	u.host.assign(host, c.dwHostNameLength);
	u.path.assign(path, c.dwUrlPathLength);
	u.path.append(extra, c.dwExtraInfoLength);
	u.port = c.nPort;
	u.https = c.nScheme == INTERNET_SCHEME_HTTPS;
	return true;
}

/** GET with redirects followed; streams the body to [sink]. Returns the HTTP status. */
static int httpGet(const std::string &url, const std::function<bool(const char *, size_t, uint64_t)> &sink)
{
	std::string cur = url;
	for (int hop = 0; hop < 6; hop++) {
		UrlParts u;
		if (!splitUrl(cur, u))
			return 0;
		HINTERNET ses = WinHttpOpen(L"Rely-A-Cam-OBS", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					    WINHTTP_NO_PROXY_BYPASS, 0);
		if (!ses)
			return 0;
		HINTERNET con = WinHttpConnect(ses, u.host.c_str(), u.port, 0);
		HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
							 WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)
				    : nullptr;
		int status = 0;
		std::string location;
		if (req) {
			DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
			WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof noRedirect);
			const wchar_t *hdr = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
			if (WinHttpSendRequest(req, hdr, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			    WinHttpReceiveResponse(req, nullptr)) {
				DWORD code = 0, sz = sizeof code;
				WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz,
						    nullptr);
				status = int(code);
				if (status >= 300 && status < 400) {
					wchar_t loc[4096];
					DWORD ls = sizeof loc;
					if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, nullptr, loc, &ls, nullptr))
						location = QString::fromWCharArray(loc, int(ls / sizeof(wchar_t))).toStdString();
				} else if (status == 200) {
					wchar_t len[64];
					DWORD lsz = sizeof len;
					uint64_t total = 0;
					if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &lsz, nullptr))
						total = QString::fromWCharArray(len, int(lsz / sizeof(wchar_t))).toULongLong();
					std::vector<char> buf(64 * 1024);
					DWORD got = 0;
					while (WinHttpReadData(req, buf.data(), DWORD(buf.size()), &got) && got > 0)
						if (!sink(buf.data(), got, total)) {
							status = 0;
							break;
						}
				}
			}
			WinHttpCloseHandle(req);
		}
		if (con)
			WinHttpCloseHandle(con);
		WinHttpCloseHandle(ses);
		if (!location.empty()) {
			cur = location;
			continue;
		}
		return status;
	}
	return 0;
}
#endif

std::optional<Release> latestRelease()
{
#ifdef _WIN32
	std::string body;
	int st = httpGet("https://api.github.com/repos/uukjtisa/rely-a-cam/releases?per_page=10",
			 [&](const char *d, size_t n, uint64_t) {
				 body.append(d, n);
				 return body.size() < 4 * 1024 * 1024;
			 });
	if (st != 200)
		return std::nullopt;
	json arr = json::parse(body, nullptr, false);
	if (!arr.is_array())
		return std::nullopt;
	std::optional<Release> best;
	for (auto &r : arr) {
		if (r.value("draft", false) || r.value("prerelease", false))
			continue;
		Release x;
		x.tag = r.value("tag_name", "");
		x.name = r.value("name", "");
		x.page = r.value("html_url", "");
		if (r.contains("assets"))
			for (auto &a : r["assets"]) {
				const std::string n = a.value("name", "");
				const std::string u = a.value("browser_download_url", "");
				if (n.size() > 4 && n.substr(n.size() - 4) == ".exe")
					x.installerUrl = u;
				if (n.size() > 4 && n.substr(n.size() - 4) == ".apk")
					x.apkUrl = u;
			}
		if (!best || newerThan(x.tag, best->tag))
			best = x;
	}
	return best;
#else
	return std::nullopt;
#endif
}

static void parseVersion(const std::string &s, int v[3], std::string &pre)
{
	v[0] = v[1] = v[2] = 0;
	size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
	sscanf(s.c_str() + i, "%d.%d.%d", &v[0], &v[1], &v[2]);
	auto dash = s.find('-');
	pre = dash == std::string::npos ? "" : s.substr(dash + 1);
}

bool newerThan(const std::string &tag, const std::string &current)
{
	int a[3], b[3];
	std::string pa, pb;
	parseVersion(tag, a, pa);
	parseVersion(current, b, pb);
	for (int i = 0; i < 3; i++)
		if (a[i] != b[i])
			return a[i] > b[i];
	if (pa.empty() != pb.empty())
		return pa.empty();
	return pa > pb;
}

bool download(const std::string &url, const std::string &path, const std::function<void(int)> &progress)
{
#ifdef _WIN32
	FILE *f = _wfopen(QString::fromStdString(path).toStdWString().c_str(), L"wb");
	if (!f)
		return false;
	uint64_t done = 0;
	int last = -2;
	int st = httpGet(url, [&](const char *d, size_t n, uint64_t total) {
		if (fwrite(d, 1, n, f) != n)
			return false;
		done += n;
		int p = total ? int(done * 100 / total) : -1;
		if (p != last) {
			last = p;
			progress(p);
		}
		return true;
	});
	fclose(f);
	return st == 200;
#else
	(void)url;
	(void)path;
	(void)progress;
	return false;
#endif
}

bool runInstaller(const std::string &path)
{
	return QProcess::startDetached(QString::fromStdString(path), {});
}

} // namespace rac::sys
