// Copyright (c) 2026- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include "ppsspp_config.h"

#include <string>
#include <string_view>
#include <utility>

#include "Common/Crypto/sha1.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/SaveSync.h"
#include "Core/System.h"

namespace SaveSync {

// ---- Common to both builds ----------------------------------------------------------------

void WipeSecret(std::string &secret) {
	// Through a volatile pointer, so the stores can't be dropped as dead before the release.
	volatile char *p = secret.data();
	for (size_t i = 0; i < secret.size(); i++)
		p[i] = 0;
	secret.clear();
	secret.shrink_to_fit();
}

// Where emusync keeps its data: a native folder of the app's, never the memory stick, which can be
// a folder the user picked through Android's Storage Access Framework.
static Path DataRoot() {
#if PPSSPP_PLATFORM(ANDROID)
	return g_Config.internalDataDirectory;
#else
	return GetSysDirectory(DIRECTORY_SYSTEM);
#endif
}

std::string DataDirFor(const std::string &remote_uri) {
	// emusync binds a data folder to one storage for good: one folder per address, so switching to
	// another storage (a new folder for encrypted saves, say) starts a fresh one, and switching back
	// finds the old one again. SHA-1 only names the folder; nothing depends on it being secure.
	unsigned char digest[20];
	sha1((unsigned char *)remote_uri.data(), (int)remote_uri.size(), digest);
	std::string id;
	for (size_t i = 0; i < 8; i++)
		id += StringFromFormat("%02x", digest[i]);
	return (DataRoot() / "emusync" / id).ToString();
}

bool HasSecret(const char *name) {
	const std::string_view n = name;
	if (n == SECRET_ACCESS_KEY)
		return g_Config.bSaveSyncSecretStored;
	if (n == ENCRYPTION_PASSPHRASE)
		return g_Config.bSaveSyncPassphraseStored;
	return false;
}

}  // namespace SaveSync

#ifndef USE_EMUSYNC

namespace SaveSync {

static constexpr const char *NO_SYNC = "This build of PPSSPP has no save sync.";

bool IsAvailable() { return false; }
bool Start(const Config &, std::string *error) {
	if (error)
		*error = NO_SYNC;
	return false;
}
void Stop(uint32_t) {}
bool IsRunning() { return false; }
std::string GetStatus() { return {}; }
void SyncNow() {}
void SetAppBackgrounded(bool) {}
bool StoreSecret(const char *, const std::string &, std::string *error) {
	if (error)
		*error = NO_SYNC;
	return false;
}
std::optional<std::string> LoadSecret(const char *, std::string *error) {
	if (error)
		*error = NO_SYNC;
	return std::nullopt;
}
void ApplySettingsAsync(std::vector<SecretUpdate>, std::function<void(const std::string &)> done) {
	if (done)
		done(NO_SYNC);
}
bool IsApplying() { return false; }
std::string GetLastStartError() { return {}; }
void Initialize() {}
void Shutdown() {}
void OnGameStarting() {}
void OnSavedataWritten() {}
void OnGameStopped() {}

}  // namespace SaveSync

#else

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include <emusync.h>

#include "Common/Log.h"
#include "Common/System/OSD.h"
#ifdef _WIN32
#include "Common/CommonWindows.h"
#endif
#include "Common/Thread/ThreadUtil.h"
#include "Core/ELF/ParamSFO.h"

namespace SaveSync {

// How long boot waits for the network before the game starts with the saves as they are.
static constexpr uint32_t BOOT_SYNC_TIMEOUT_MS = 3000;
// The session timeout for "don't wait": 0 would mean the default.
static constexpr uint32_t NO_WAIT_MS = 1;
// A sync with no event to trigger it.
static constexpr std::chrono::minutes PERIODIC_SYNC{5};
// What exiting PPSSPP gives queued uploads.
static constexpr uint32_t EXIT_UPLOAD_MS = 5000;
// How long a boot waits for a start in progress: the start's own check of the storage takes up to
// ~10 s.
static constexpr std::chrono::seconds BOOT_WAITS_FOR_START{12};
// What going to the background gives uploads (Android).
static constexpr uint32_t SUSPEND_UPLOAD_MS = 3000;
// The credential-store service the secrets are kept under.
static constexpr const char *SECRET_SERVICE = "PPSSPP";
// Longer than any secret emusync stores (1024 bytes), plus the NUL.
static constexpr size_t SECRET_BUFFER = 1025;

// Serialises Start() and Stop() with each other.
static std::mutex s_lifecycleLock;
// The client's lifetime. Every es_* call holds it shared; Start() and Stop() hold it unique, so the
// client is never destroyed under a call. The longest shared hold is the boot sync (3 s).
static std::shared_mutex s_clientLock;
static es_client *s_client = nullptr;

// Everything else, held briefly.
static std::mutex s_stateLock;
static Config s_config;
// The memory stick's SAVEDATA folder, once registered.
static es_container_id s_container = 0;
static es_session_id s_session = 0;
static std::chrono::steady_clock::time_point s_sessionStarted;
// A game is running (from OnGameStarting to OnGameStopped).
static bool s_emulating = false;
static std::string s_status;
static std::string s_lastStartError;

// The worker thread: periodic syncs, uploads after saves, the app's background moves, and draining
// emusync's events. Saves and background moves are handed to it so the threads reporting them never
// wait for emusync.
static std::thread s_worker;
static std::mutex s_workerLock;
static std::condition_variable s_workerCv;
static bool s_workerStop = false;
static bool s_workerWake = false;
static bool s_flushPending = false;
static std::optional<bool> s_backgroundChange;

// The apply thread (ApplySettingsAsync): starts and stops the client off the UI and emulation
// threads.
static std::thread s_applyThread;
static std::mutex s_applyLock;
static std::condition_variable s_applyCv;
static bool s_applyPending = false;
// A run of the apply thread is in progress (s_applyPending is cleared when it begins).
static bool s_applyRunning = false;
static std::condition_variable s_applyIdleCv;
static bool s_applyQuit = false;
static std::vector<SecretUpdate> s_applySecrets;
static std::vector<std::function<void(const std::string &)>> s_applyDone;

// The settings an apply runs with, copied from g_Config when it is queued.
struct Settings {
	bool enabled = false;
	std::string remoteUri;
	std::string accessKeyId;
	std::string deviceName;
};
static Settings s_applySettings;

static void SetStatus(std::string status) {
	std::lock_guard<std::mutex> lock(s_stateLock);
	s_status = std::move(status);
}

static std::string TimeOfDay() {
	const std::time_t now = std::time(nullptr);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	char buf[16];
	std::strftime(buf, sizeof(buf), "%H:%M", &local);
	return buf;
}

static void LogMessage(void *, es_log_level level, const char *, const char *msg) {
	switch (level) {
	case ES_LOG_ERROR: ERROR_LOG(Log::System, "SaveSync: %s", msg); break;
	case ES_LOG_WARN: WARN_LOG(Log::System, "SaveSync: %s", msg); break;
	case ES_LOG_INFO: INFO_LOG(Log::System, "SaveSync: %s", msg); break;
	default: DEBUG_LOG(Log::System, "SaveSync: %s", msg); break;
	}
}

static void CopyTo(char *dst, size_t cap, const std::string &src) {
	const size_t n = std::min(src.size(), cap - 1);
	memcpy(dst, src.data(), n);
	dst[n] = '\0';
}

// The credential provider: the keys from Start(), kept in memory only.
static int GetCredentials(void *, const char *, es_credentials *out) {
	std::lock_guard<std::mutex> lock(s_stateLock);
	if (s_config.access_key_id.empty() || s_config.secret_access_key.empty())
		return ES_E_AUTH;
	out->kind = ES_CRED_S3;
	CopyTo(out->access_key_id, sizeof(out->access_key_id), s_config.access_key_id);
	CopyTo(out->secret_access_key, sizeof(out->secret_access_key), s_config.secret_access_key);
	return 0;
}

static void WakeWorker() {
	{
		std::lock_guard<std::mutex> lock(s_workerLock);
		s_workerWake = true;
	}
	s_workerCv.notify_one();
}

// Called by emusync on its own thread when events are waiting; must not call into emusync.
static void OnEventsWaiting(void *) {
	WakeWorker();
}

// Registers the memory stick's SAVEDATA folder (card "psp:SAVEDATA" on every device), creating it
// as PPSSPP would if the memory stick has none yet. A memory stick moved to another folder moves
// the card (ES_CF_REPLACE). Needs the client lock (shared), and no open session.
static es_container_id RegisterSavedata() {
	const Path savedata = GetSysDirectory(DIRECTORY_SAVEDATA);
	if (!File::IsDirectory(savedata))
		File::CreateFullPath(savedata);
	const std::string location = savedata.ToString();
	es_container_desc desc = ES_CONTAINER_DESC_INIT;
	desc.format = "psp.savedata";
	desc.location = location.c_str();
	desc.platform = "psp";
	desc.card = "SAVEDATA";
	desc.flags = ES_CF_REPLACE;
	es_container_id id = 0;
	const es_status st = es_container_add(s_client, &desc, &id);
	if (st != ES_OK) {
		WARN_LOG(Log::System, "SaveSync: not syncing %s: %s", location.c_str(), es_status_str(st));
		return 0;
	}
	return id;
}

// Begins a session on the SAVEDATA folder. Needs the client lock (shared). The state lock must NOT
// be held: this can wait for the network.
static es_session_id BeginSession(es_container_id container, const std::string &gameID, const std::string &titleName,
                                  uint32_t timeoutMs, es_session_info *info) {
	if (container == 0)
		return 0;
	es_title title = ES_TITLE_INIT;
	title.platform = "psp";
	// The product code ("ULUS10041") picks which saves are fetched first: their title keys are
	// "psp:ULUS10041". Homebrew has none.
	title.serial = gameID.empty() ? "HOMEBREW" : gameID.c_str();
	title.display_name = titleName.empty() ? nullptr : titleName.c_str();

	es_session_begin_args args = ES_SESSION_BEGIN_INIT;
	args.title = &title;
	args.mounted = &container;
	args.mounted_count = 1;
	args.timeout_ms = timeoutMs;
	*info = ES_SESSION_INFO_INIT;
	const es_status st = es_session_begin(s_client, &args, info);
	if (st < 0) {
		WARN_LOG(Log::System, "SaveSync: could not start syncing the memory stick in use: %s", es_status_str(st));
		return 0;
	}
	return info->id;
}

// Ends the open session, if any. Needs the client lock (shared).
static void EndSession() {
	es_session_id id;
	uint64_t playedMs;
	{
		std::lock_guard<std::mutex> lock(s_stateLock);
		id = s_session;
		s_session = 0;
		playedMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_sessionStarted).count();
	}
	if (id != 0)
		es_session_end(s_client, id, playedMs);
}

// Starts following the memory stick of a running game, when sync starts mid-game. Never waits for
// the network (the game is running).
static void FollowRunningGame() {
	EndSession();
	const es_container_id container = RegisterSavedata();
	es_session_info info;
	// 1 ms: SAVEDATA is marked in use at once; what the network brings waits for the game to end.
	const es_session_id id = BeginSession(container, g_paramSFO.GetDiscID(), g_paramSFO.GetValueString("TITLE"), NO_WAIT_MS, &info);
	std::lock_guard<std::mutex> lock(s_stateLock);
	s_container = container;
	s_session = id;
	s_sessionStarted = std::chrono::steady_clock::now();
}

static constexpr const char *LOCKED_STATUS = "This storage is encrypted. Enter its passphrase in the save sync settings to sync.";

static std::string DescribeEvent(const es_event &ev) {
	const char *save = ev.unit_name ? ev.unit_name : "a save";
	if (ev.status == ES_E_LOCKED && (ev.type == ES_EV_SYNC_FINISHED || ev.type == ES_EV_ERROR))
		return LOCKED_STATUS;
	switch (ev.type) {
	case ES_EV_SYNC_FINISHED:
		if (ev.status == ES_OK)
			return StringFromFormat("Synced at %s.", TimeOfDay().c_str());
		if (ev.status == ES_OFFLINE)
			return StringFromFormat("Offline at %s: changes upload once the storage can be reached.", TimeOfDay().c_str());
		return StringFromFormat("Sync failed at %s: %s", TimeOfDay().c_str(), ev.message ? ev.message : es_status_str(ev.status));
	case ES_EV_UNIT_APPLIED:
		return StringFromFormat("Updated %s%s at %s.", save,
			ev.device_name ? StringFromFormat(" from %s", ev.device_name).c_str() : "", TimeOfDay().c_str());
	case ES_EV_APPLY_BLOCKED:
		if (ev.status == ES_E_NO_SPACE)
			return StringFromFormat("%s does not fit on the memory stick.", save);
		if (ev.status == ES_E_BUSY)
			return {};  // the memory stick is in use; it is applied when the game ends
		return StringFromFormat("Could not update %s: %s", save, ev.message ? ev.message : es_status_str(ev.status));
	case ES_EV_AUTH_REQUIRED:
		return "The storage rejected the access key. Check it in the save sync settings.";
	case ES_EV_ERROR:
		return StringFromFormat("Sync error: %s", ev.message ? ev.message : es_status_str(ev.status));
	default:
		return {};
	}
}

// Drains emusync's events into the log and the status line. Needs the client lock (shared).
static void DrainEvents() {
	es_event ev = ES_EVENT_INIT;
	while (es_event_poll(s_client, &ev) == 1) {
		std::string text = DescribeEvent(ev);
		if (ev.type == ES_EV_UNIT_APPLIED || ev.type == ES_EV_UNIT_UPLOADED) {
			INFO_LOG(Log::System, "SaveSync: %s %s", ev.type == ES_EV_UNIT_APPLIED ? "applied" : "uploaded", ev.unit_id ? ev.unit_id : "?");
		} else if (!text.empty() && ev.type != ES_EV_SYNC_FINISHED) {
			WARN_LOG(Log::System, "SaveSync: %s", text.c_str());
		}
		if (!text.empty())
			SetStatus(std::move(text));
		es_event_release(s_client, &ev);
		ev = ES_EVENT_INIT;
	}
}

static void WorkerLoop() {
	SetCurrentThreadName("SaveSync");
	auto nextSync = std::chrono::steady_clock::now() + PERIODIC_SYNC;
	for (;;) {
		bool flush;
		std::optional<bool> background;
		{
			std::unique_lock<std::mutex> lock(s_workerLock);
			s_workerCv.wait_until(lock, nextSync, [] { return s_workerStop || s_workerWake; });
			if (s_workerStop)
				return;
			s_workerWake = false;
			flush = std::exchange(s_flushPending, false);
			background = std::exchange(s_backgroundChange, std::nullopt);
		}

		std::shared_lock<std::shared_mutex> client(s_clientLock);
		if (!s_client)
			return;
		if (flush) {
			es_container_id container;
			{
				std::lock_guard<std::mutex> lock(s_stateLock);
				container = s_container;
			}
			if (container != 0)
				es_container_flushed(s_client, container);
		}
		if (background.has_value()) {
			if (*background)
				es_app_suspend(s_client, SUSPEND_UPLOAD_MS);
			else
				es_app_resume(s_client);
		}
		if (std::chrono::steady_clock::now() >= nextSync) {
			es_sync_now(s_client);
			nextSync = std::chrono::steady_clock::now() + PERIODIC_SYNC;
		}
		DrainEvents();
	}
}

// Stops the worker and destroys the client. Needs the lifecycle lock and the client lock (unique).
static void StopLocked(std::unique_lock<std::shared_mutex> &client, uint32_t timeoutMs) {
	if (!s_client)
		return;
	{
		std::lock_guard<std::mutex> lock(s_workerLock);
		s_workerStop = true;
	}
	s_workerCv.notify_one();
	// The worker takes the client lock shared, which this thread holds unique: release it while the
	// worker winds down, so a worker blocked on it can see the stop flag. Nothing else can start or
	// stop the client meanwhile: the lifecycle lock is held.
	client.unlock();
	if (s_worker.joinable())
		s_worker.join();
	client.lock();

	es_event_set_notifier(s_client, nullptr, nullptr);
	es_client_destroy(s_client, timeoutMs);
	s_client = nullptr;

	{
		std::lock_guard<std::mutex> lock(s_workerLock);
		s_flushPending = false;
		s_backgroundChange.reset();
	}
	std::lock_guard<std::mutex> lock(s_stateLock);
	s_session = 0;
	s_container = 0;
	WipeSecret(s_config.secret_access_key);
	s_config = {};
	s_status.clear();
}

bool IsAvailable() {
#ifdef _WIN32
	// emusync.dll is delay-loaded (Windows/EmuSync.props): a call into it without the DLL would
	// crash, so every path into emusync checks this first.
	static const bool loaded = LoadLibraryW(L"emusync.dll") != nullptr;
	return loaded;
#else
	return true;
#endif
}

static constexpr const char *NO_LIBRARY = "Save sync needs emusync.dll next to PPSSPP.";

bool Start(const Config &config, std::string *error) {
	if (!IsAvailable()) {
		if (error)
			*error = NO_LIBRARY;
		return false;
	}
	std::lock_guard<std::mutex> lifecycle(s_lifecycleLock);
	std::unique_lock<std::shared_mutex> client(s_clientLock);
	StopLocked(client, 2000);

	{
		std::lock_guard<std::mutex> lock(s_stateLock);
		s_config = config;
		// Only needed below, once: not kept for the life of the client like the keys.
		WipeSecret(s_config.passphrase);
	}

	es_config cfg = ES_CONFIG_INIT;
	cfg.data_dir = config.data_dir.c_str();
	cfg.device_name = config.device_name.empty() ? nullptr : config.device_name.c_str();
	cfg.emulator_id = "ppsspp";
	cfg.emulator_version = config.emulator_version.c_str();
	// No conflict screen yet: the newest version wins and the other stays in emusync's history.
	cfg.conflict_policy = ES_POLICY_NEWEST;
	cfg.log = &LogMessage;
	cfg.log_level = ES_LOG_INFO;
	es_status st = es_client_create(&cfg, &s_client);
	if (st != ES_OK) {
		if (error)
			*error = StringFromFormat("Could not open the sync data in %s: %s", config.data_dir.c_str(), es_status_str(st));
		s_client = nullptr;
		// StopLocked() returns early without a client, so this is the only place the copied keys can
		// go.
		std::lock_guard<std::mutex> lock(s_stateLock);
		WipeSecret(s_config.secret_access_key);
		s_config = {};
		return false;
	}

	const bool encrypted = !config.passphrase.empty();
	// Applies to the es_remote_add below: create the storage encrypted, or unlock it.
	if (encrypted)
		st = es_repo_create_encrypted(s_client, config.passphrase.c_str());
	const es_credential_provider creds = {nullptr, &GetCredentials};
	if (st == ES_OK)
		st = es_remote_add(s_client, config.remote_uri.c_str(), &creds);
	// Locked: encrypted storage this device has no key for, and no passphrase was given. The remote
	// is added; sync runs and snapshots the saves, and uploads once it is unlocked.
	if (st != ES_OK && st != ES_OFFLINE && st != ES_E_LOCKED) {
		if (error) {
			if (st == ES_E_AUTH)
				*error = "The storage rejected the access key ID or secret.";
			else if (st == ES_E_PASSPHRASE)
				*error = "The passphrase does not unlock this storage.";
			else if (st == ES_E_INCOMPATIBLE && encrypted)
				*error = "This storage is not encrypted, or was set up by a newer version of emusync. "
				         "Remove the passphrase to use it, or choose a new folder for encrypted saves.";
			else if (st == ES_E_INCOMPATIBLE)
				*error = "This storage was set up by a newer version of emusync, or has changed since this device joined it.";
			else if (st == ES_E_INVALID_ARG)
				*error = "The storage address is not valid.";
			else
				*error = StringFromFormat("Could not use the storage: %s", es_status_str(st));
		}
		es_client_destroy(s_client, 0);
		s_client = nullptr;
		std::lock_guard<std::mutex> lock(s_stateLock);
		WipeSecret(s_config.secret_access_key);
		s_config = {};
		return false;
	}
	SetStatus(st == ES_OFFLINE ? "Storage not reachable yet: sync starts once it is." :
	          st == ES_E_LOCKED ? std::string(LOCKED_STATUS) : "Starting...");

	{
		std::lock_guard<std::mutex> lock(s_workerLock);
		s_workerStop = false;
		s_workerWake = false;
	}
	es_event_set_notifier(s_client, &OnEventsWaiting, nullptr);
	s_worker = std::thread(&WorkerLoop);

	// A game already running keeps its saves: follow them without touching them. One starting counts
	// too: OnGameStarting() may have run before this client existed.
	bool emulating;
	{
		std::lock_guard<std::mutex> lock(s_stateLock);
		emulating = s_emulating;
	}
	if (emulating) {
		FollowRunningGame();
	} else {
		const es_container_id container = RegisterSavedata();
		std::lock_guard<std::mutex> lock(s_stateLock);
		s_container = container;
	}
	es_sync_now(s_client);
	NOTICE_LOG(Log::System, "SaveSync: syncing saves with %s", config.remote_uri.c_str());
	return true;
}

void Stop(uint32_t timeoutMs) {
	std::lock_guard<std::mutex> lifecycle(s_lifecycleLock);
	std::unique_lock<std::shared_mutex> client(s_clientLock);
	StopLocked(client, timeoutMs);
}

bool IsRunning() {
	std::shared_lock<std::shared_mutex> client(s_clientLock);
	return s_client != nullptr;
}

std::string GetStatus() {
	std::lock_guard<std::mutex> lock(s_stateLock);
	return s_status;
}

void SyncNow() {
	std::shared_lock<std::shared_mutex> client(s_clientLock);
	if (s_client)
		es_sync_now(s_client);
}

void SetAppBackgrounded(bool backgrounded) {
	{
		std::lock_guard<std::mutex> lock(s_workerLock);
		s_backgroundChange = backgrounded;
	}
	WakeWorker();
}

// ---- Settings -----------------------------------------------------------------------------

bool StoreSecret(const char *name, const std::string &value, std::string *error) {
	if (!IsAvailable()) {
		if (error)
			*error = NO_LIBRARY;
		return false;
	}
	// Without a Secret Service (Linux), the secret goes to a file only this user can read here.
	const std::string fallback = DataRoot().ToString();
	const es_status st = value.empty() ? es_secret_delete(SECRET_SERVICE, name, fallback.c_str()) :
	                                     es_secret_set(SECRET_SERVICE, name, value.c_str(), fallback.c_str());
	if (st != ES_OK) {
		if (error)
			*error = StringFromFormat("Could not store the secret in the system's credential store: %s", es_status_str(st));
		return false;
	}
	const std::string_view n = name;
	if (n == SECRET_ACCESS_KEY)
		g_Config.bSaveSyncSecretStored = !value.empty();
	else if (n == ENCRYPTION_PASSPHRASE)
		g_Config.bSaveSyncPassphraseStored = !value.empty();
	return true;
}

std::optional<std::string> LoadSecret(const char *name, std::string *error) {
	if (!IsAvailable()) {
		if (error)
			*error = NO_LIBRARY;
		return std::nullopt;
	}
	std::string buf(SECRET_BUFFER, '\0');
	size_t len = 0;
	const std::string fallback = DataRoot().ToString();
	const es_status st = es_secret_get(SECRET_SERVICE, name, fallback.c_str(), buf.data(), buf.size(), &len);
	if (st == ES_E_NOT_FOUND) {
		WipeSecret(buf);
		return std::nullopt;
	}
	if (st != ES_OK) {
		WipeSecret(buf);
		if (error)
			*error = StringFromFormat("Could not read the secret from the system's credential store: %s", es_status_str(st));
		return std::nullopt;
	}
	std::string value(buf.c_str());
	WipeSecret(buf);
	return value;
}

std::string GetLastStartError() {
	std::lock_guard<std::mutex> lock(s_stateLock);
	return s_lastStartError;
}

// Starts sync with the settings, or stops it when it is switched off or incomplete. Returns an
// empty string when sync runs or is deliberately off, else a sentence for the user.
static std::string StartFromSettings(const Settings &s) {
	Config c;
	c.remote_uri = std::string(StripSpaces(s.remoteUri));
	c.access_key_id = std::string(StripSpaces(s.accessKeyId));
	c.device_name = std::string(StripSpaces(s.deviceName));
	if (!s.enabled || c.remote_uri.empty() || c.access_key_id.empty()) {
		Stop(2000);
		return {};
	}

	std::string error;
	std::optional<std::string> secret = LoadSecret(SECRET_ACCESS_KEY, &error);
	if (!secret.has_value()) {
		// No secret yet is "incomplete", not an error; a store that can't be read is.
		Stop(2000);
		return error;
	}
	std::optional<std::string> passphrase = LoadSecret(ENCRYPTION_PASSPHRASE, &error);
	if (!passphrase.has_value() && !error.empty()) {
		WipeSecret(*secret);
		Stop(2000);
		return error;
	}

	c.secret_access_key = std::move(*secret);
	if (passphrase.has_value())
		c.passphrase = std::move(*passphrase);
	c.data_dir = DataDirFor(c.remote_uri);
	c.emulator_version = PPSSPP_GIT_VERSION;
	if (!File::CreateFullPath(Path(c.data_dir)) || !File::IsDirectory(Path(c.data_dir))) {
		WipeSecret(c.secret_access_key);
		WipeSecret(c.passphrase);
		return StringFromFormat("Could not create the sync data folder %s.", c.data_dir.c_str());
	}
	const bool started = Start(c, &error);
	WipeSecret(c.secret_access_key);
	WipeSecret(c.passphrase);
	return started ? std::string() : error;
}

static void ApplyLoop() {
	SetCurrentThreadName("SaveSyncApply");
	for (;;) {
		std::vector<SecretUpdate> secrets;
		std::vector<std::function<void(const std::string &)>> done;
		Settings settings;
		{
			std::unique_lock<std::mutex> lock(s_applyLock);
			s_applyCv.wait(lock, [] { return s_applyPending || s_applyQuit; });
			if (!s_applyPending)
				return;
			s_applyPending = false;
			s_applyRunning = true;
			secrets = std::exchange(s_applySecrets, {});
			done = std::exchange(s_applyDone, {});
			settings = s_applySettings;
		}

		std::string error;
		for (SecretUpdate &u : secrets) {
			std::string e;
			if (!StoreSecret(u.name, u.value, &e) && error.empty())
				error = std::move(e);
			WipeSecret(u.value);
		}
		if (error.empty())
			error = StartFromSettings(settings);
		{
			std::lock_guard<std::mutex> lock(s_stateLock);
			s_lastStartError = error;
		}
		if (!error.empty())
			WARN_LOG(Log::System, "SaveSync: not syncing: %s", error.c_str());
		for (const auto &cb : done)
			cb(error);
		{
			std::lock_guard<std::mutex> lock(s_applyLock);
			s_applyRunning = false;
		}
		s_applyIdleCv.notify_all();
	}
}

// Waits, up to BOOT_WAITS_FOR_START, for a start or stop queued or in progress: at PPSSPP's startup,
// Initialize() queues one, and a game launched at once (from the command line) must not boot with
// no client and miss the saves from other devices.
static void WaitForPendingStart() {
	std::unique_lock<std::mutex> lock(s_applyLock);
	if (!s_applyIdleCv.wait_for(lock, BOOT_WAITS_FOR_START, [] { return s_applyQuit || (!s_applyPending && !s_applyRunning); }))
		WARN_LOG(Log::System, "SaveSync: still starting; the game boots with the saves as they are.");
}

void ApplySettingsAsync(std::vector<SecretUpdate> secrets, std::function<void(const std::string &)> done) {
	Settings settings;
	settings.enabled = g_Config.bSaveSyncEnabled;
	settings.remoteUri = g_Config.sSaveSyncRemoteURI;
	settings.accessKeyId = g_Config.sSaveSyncAccessKeyID;
	settings.deviceName = g_Config.sSaveSyncDeviceName;
	std::lock_guard<std::mutex> lock(s_applyLock);
	if (s_applyQuit)
		return;
	s_applySettings = std::move(settings);
	for (SecretUpdate &u : secrets)
		s_applySecrets.push_back(std::move(u));
	if (done)
		s_applyDone.push_back(std::move(done));
	s_applyPending = true;
	if (!s_applyThread.joinable())
		s_applyThread = std::thread(&ApplyLoop);
	s_applyCv.notify_one();
}

bool IsApplying() {
	std::lock_guard<std::mutex> lock(s_applyLock);
	return s_applyPending || s_applyRunning;
}

void Initialize() {
	if (g_Config.bSaveSyncEnabled)
		ApplySettingsAsync();
}

void Shutdown() {
	{
		std::lock_guard<std::mutex> lock(s_applyLock);
		s_applyQuit = true;
	}
	s_applyCv.notify_one();
	s_applyIdleCv.notify_all();
	// A start in progress finishes first (it can wait up to 10 s on the network).
	if (s_applyThread.joinable())
		s_applyThread.join();
	Stop(EXIT_UPLOAD_MS);
}

// ---- Core hooks ---------------------------------------------------------------------------

void OnGameStarting() {
	WaitForPendingStart();
	{
		std::lock_guard<std::mutex> lock(s_stateLock);
		s_emulating = true;
	}
	std::shared_lock<std::shared_mutex> client(s_clientLock);
	if (!s_client)
		return;

	EndSession();  // a session left by a game that never reached OnGameStopped()
	const es_container_id container = RegisterSavedata();
	es_session_info info;
	const es_session_id id = BeginSession(container, g_paramSFO.GetDiscID(), g_paramSFO.GetValueString("TITLE"), BOOT_SYNC_TIMEOUT_MS, &info);
	{
		std::lock_guard<std::mutex> lock(s_stateLock);
		s_container = container;
		s_session = id;
		s_sessionStarted = std::chrono::steady_clock::now();
	}
	if (id != 0 && info.units_applied > 0) {
		NOTICE_LOG(Log::System, "SaveSync: %u save(s) updated from other devices before boot.", info.units_applied);
		const std::string text = StringFromFormat("Updated %u save(s) from other devices", info.units_applied);
		g_OSD.Show(OSDType::MESSAGE_SUCCESS, text, 5.0f);
		SetStatus(StringFromFormat("%s at %s.", text.c_str(), TimeOfDay().c_str()));
	} else if (id != 0 && !info.remote_reached) {
		WARN_LOG(Log::System, "SaveSync: storage not reached before boot; starting with the saves as they are.");
	}
}

void OnSavedataWritten() {
	{
		std::lock_guard<std::mutex> lock(s_stateLock);
		// Sync is off, or SAVEDATA isn't registered.
		if (s_container == 0)
			return;
	}
	{
		std::lock_guard<std::mutex> lock(s_workerLock);
		s_flushPending = true;
	}
	WakeWorker();
}

void OnGameStopped() {
	{
		std::lock_guard<std::mutex> lock(s_stateLock);
		s_emulating = false;
	}
	std::shared_lock<std::shared_mutex> client(s_clientLock);
	if (!s_client)
		return;
	// An upload already queued by the game's last save is taken first; the session's end also
	// snapshots the memory stick once more.
	EndSession();
}

}  // namespace SaveSync

#endif  // USE_EMUSYNC
