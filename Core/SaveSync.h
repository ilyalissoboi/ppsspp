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

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Save sync through the cloud, with emusync (https://github.com/ilyalissoboi/emusync).
//
// The saves on the memory stick (PSP/SAVEDATA) are synced, one save folder at a time, with every
// other device using the same storage: PPSSPP on other computers and on Android, and anything
// running `emusync watch`. A save made on one device shows up on the others; two devices playing
// different games never conflict, and when the same save was changed on two devices the newest
// version wins and the other stays in emusync's history. Folders over 32 MB (game data installed
// from the disc) are left alone.
//
// When the memory stick is read and written (with the hooks below in place):
//   Boot: OnGameStarting(), on the loader thread once the game is loaded and before it runs.
//   Newer saves from other devices are written to SAVEDATA first, waiting at most 3 s for the
//   network; offline, the game starts with the saves as they are. From then until OnGameStopped(),
//   emusync never writes to SAVEDATA (PPSSPP caches a save's PARAM.SFO while a game runs); changes
//   that arrive meanwhile wait and are applied after the game.
//   Play: OnSavedataWritten() after the game saves or deletes a save uploads the change. Snapshots
//   go to an on-disk queue first, so a save is not lost if PPSSPP exits before the upload.
//   Shutdown: OnGameStopped() ends the session, uploads, and applies waiting changes.
//
// Settings: [SaveSync] in ppsspp.ini (Enabled, RemoteURI, AccessKeyID, DeviceName), with the secret
// access key and the optional encryption passphrase in the system's credential store (Credential
// Manager, Keychain, Secret Service) through emusync, never in the INI.
//
// Without USE_EMUSYNC (emusync not found at build time) every function here is a no-op,
// IsAvailable() is false and starting fails, saying the build has no save sync.
//
// Threading: everything is thread-safe. Start() and OnGameStarting() block (local database open,
// and the network as described); every other hook returns quickly.
namespace SaveSync {

// Whether this build includes save sync.
bool IsAvailable();

struct Config {
	// Private folder for emusync's database and queue. Bound to one storage for good, so use one
	// per remote_uri (DataDirFor()).
	std::string data_dir;
	// Shown on other devices, e.g. in a save's history. Empty: the computer's name.
	std::string device_name;
	std::string emulator_version;
	// The storage, e.g. "s3://my-saves/emusync/v1?region=eu-west-1".
	std::string remote_uri;
	std::string access_key_id;
	std::string secret_access_key;
	// Optional: the storage's end-to-end encryption passphrase. Set, it creates the storage
	// encrypted if it is new, or unlocks it if it already is; storage that is not encrypted is then
	// refused. Empty, unencrypted storage is used as it is, and encrypted storage this device has
	// already unlocked keeps working (emusync keeps its key in data_dir).
	std::string passphrase;
};

// Starts syncing, replacing a client that is already running. Blocks while the local database
// opens and for up to 10 s while the storage is checked. Returns false with `error` set when sync
// cannot start (wrong address, keys rejected, unreadable data folder). Storage that cannot be
// reached right now is not an error: sync starts working once it can be reached. Does not keep
// config.passphrase; the caller should WipeSecret() its own copy afterwards.
bool Start(const Config &config, std::string *error);

// Stops syncing. Saves are left as they are. Waits up to `timeout_ms` for queued uploads.
void Stop(uint32_t timeout_ms);

bool IsRunning();

// One line about the last sync, for the settings screen.
std::string GetStatus();

// Queues a full sync: download, apply (unless a game is running), upload.
void SyncNow();

// The app went to the background (true) or came back (false): on Android, uploads what the memory
// stick holds now before the system may stop the app.
void SetAppBackgrounded(bool backgrounded);

// ---- Settings (ppsspp.ini and the system's credential store) ----------------------------

// The emusync data folder for `remote_uri`: <system folder>/emusync/<hash of the address>.
std::string DataDirFor(const std::string &remote_uri);

// Names of the secrets in the system's credential store (service "PPSSPP").
inline constexpr const char *SECRET_ACCESS_KEY = "s3-secret-access-key";
inline constexpr const char *ENCRYPTION_PASSPHRASE = "encryption-passphrase";

// Stores a secret in the system's credential store; an empty value deletes it. Records whether one
// is stored in ppsspp.ini (so the settings screen can say so without asking the store). May block
// while the system asks to unlock the keychain: not on the UI thread.
bool StoreSecret(const char *name, const std::string &value, std::string *error);

// The stored secret, if any. May block like StoreSecret().
std::optional<std::string> LoadSecret(const char *name, std::string *error);

// Whether a secret is recorded as stored (from ppsspp.ini; does not ask the store).
bool HasSecret(const char *name);

// Applies the settings on a background thread: optionally stores new secrets first, then starts
// sync with the settings, or stops it when it is switched off or incomplete. The settings are read
// from g_Config here, on the caller's thread (the UI's), so the background thread never reads
// g_Config while the UI edits it. Requests made while one runs are folded into one more run with
// the latest settings. `done`, if set, is called on that thread with an empty string when sync runs
// or is deliberately off, else a sentence for the user.
struct SecretUpdate {
	const char *name;
	std::string value;
};
void ApplySettingsAsync(std::vector<SecretUpdate> secrets = {}, std::function<void(const std::string &error)> done = {});

// Whether settings are still being applied: an ApplySettingsAsync() queued or running. Secrets saved
// by it may not be readable until it is done.
bool IsApplying();

// The error of the last ApplySettingsAsync() run, empty if it started (or stopped) cleanly.
std::string GetLastStartError();

// Starts sync from the settings at startup, and stops it at exit, giving queued uploads a few
// seconds.
void Initialize();
void Shutdown();

// Overwrites a secret's characters, then empties the string and frees its buffer. Plain clear()
// only changes the size, leaving the characters readable in memory.
void WipeSecret(std::string &secret);

// ---- Core hooks ---------------------------------------------------------------------------

// The loader thread, once the game is loaded (its PARAM.SFO read) and before it runs. Begins the
// session (see above).
void OnGameStarting();

// The game saved or deleted a save on the memory stick. Queues the upload and returns at once.
void OnSavedataWritten();

// After the game's shutdown. Ends the session; safe to call when none is open.
void OnGameStopped();

}  // namespace SaveSync
