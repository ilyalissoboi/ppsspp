// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/Common.h"
#include "Common/Data/Text/I18n.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/GPU/thin3d.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/System/Display.h"
#include "Common/System/NativeApp.h"
#include "Common/System/OSD.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/UI/Context.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/HLE/sceUtility.h"
#include "Core/Reporting.h"
#include "Core/SaveSync.h"
#include "Core/System.h"
#include "Core/Util/PathUtil.h"
#include "UI/Background.h"
#include "UI/GameSettingsScreen.h"
#include "UI/MemStickScreen.h"
#include "UI/MiscScreens.h"
#include "UI/MiscViews.h"
#include "UI/SettingsPage.h"
#include "UI/Settings/MainSettingsPages.h"
#include "UI/Theme.h"

#if PPSSPP_PLATFORM(MAC) || PPSSPP_PLATFORM(IOS)
#include "Core/Util/DarwinFileSystemServices.h"
#endif

#if defined(_WIN32) && !PPSSPP_PLATFORM(UWP)
#pragma warning(disable:4091)  // workaround bug in VS2015 headers
#include "Windows/MainWindow.h"
#include "Windows/W32Util/ShellUtil.h"
#endif

void ReadSystemPageState(SystemPageState *state) {
	// Written back on exit, so it must be read even if the System tab is never opened.
	state->enableReports = Reporting::IsEnabled();
}

#if defined(_WIN32) && !PPSSPP_PLATFORM(UWP)
// Works out where the memstick is (installed.txt next to the exe means My Documents or a custom
// folder), and which of the two checkboxes can be changed.
void CheckMemstickInstallState(SystemPageState *state) {
	const Path &PPSSPPpath = File::GetExeDirectory();
	const Path installedFile = PPSSPPpath / "installed.txt";
	const bool myDocsExists = W32Util::UserDocumentsPath().size() != 0;

	state->installed = File::Exists(installedFile);
	state->otherinstalled = false;
	state->memstickMyDocEnabled = !PSP_IsInited();
	state->memstickOtherEnabled = false;
	if (!state->installed && myDocsExists) {
		// Disable the setting if we can't create & delete a file next to the exe.
		if (File::CreateEmptyFile(PPSSPPpath / "installedTEMP.txt")) {
			if (!(File::Delete(PPSSPPpath / "installedTEMP.txt")))
				state->memstickMyDocEnabled = false;
			else
				state->memstickOtherEnabled = !PSP_IsInited();
		} else
			state->memstickMyDocEnabled = false;
	} else {
		if (state->installed && myDocsExists) {
			FILE *testInstalled = File::OpenCFile(installedFile, "rt");
			if (testInstalled) {
				char temp[2048];
				char *tempStr = fgets(temp, sizeof(temp), testInstalled);
				// Skip UTF-8 encoding bytes if there are any. There are 3 of them.
				if (tempStr && strncmp(tempStr, "\xEF\xBB\xBF", 3) == 0) {
					tempStr += 3;
				}
				state->memstickOtherEnabled = !PSP_IsInited();
				if (tempStr && strlen(tempStr) != 0 && strcmp(tempStr, "\n") != 0) {
					state->installed = false;
					state->otherinstalled = true;
				}
				fclose(testInstalled);
			}
		} else if (!myDocsExists) {
			state->memstickMyDocEnabled = false;
		}
	}
}

static void OnMemoryStickMyDoc(SystemPageState *state) {
	const Path &PPSSPPpath = File::GetExeDirectory();
	const Path installedFile = PPSSPPpath / "installed.txt";
	state->installed = File::Exists(installedFile);
	if (state->otherinstalled) {
		File::Delete(PPSSPPpath / "installed.txt");
		File::CreateEmptyFile(PPSSPPpath / "installed.txt");
		state->otherinstalled = false;
		const std::string myDocsPath = W32Util::UserDocumentsPath() + "/PPSSPP/";
		g_Config.memStickDirectory = Path(myDocsPath);
	} else if (state->installed) {
		File::Delete(PPSSPPpath / "installed.txt");
		state->installed = false;
		g_Config.memStickDirectory = PPSSPPpath / "memstick";
	} else {
		FILE *f = File::OpenCFile(PPSSPPpath / "installed.txt", "wb");
		if (f) {
			fclose(f);
		}

		const std::string myDocsPath = W32Util::UserDocumentsPath() + "/PPSSPP/";
		g_Config.memStickDirectory = Path(myDocsPath);
		state->installed = true;
	}
}

static void OnMemoryStickOther(SystemPageState *state) {
	const Path &PPSSPPpath = File::GetExeDirectory();
	if (state->otherinstalled) {
		auto di = GetI18NCategory(I18NCat::DIALOG);
		std::string initialPath = g_Config.memStickDirectory.ToCString();
		std::string folder = W32Util::BrowseForFolder2(MainWindow::GetHWND(), di->T("Choose PPSSPP save folder"), initialPath);
		if (folder.size()) {
			g_Config.memStickDirectory = Path(folder);
			FILE *f = File::OpenCFile(PPSSPPpath / "installed.txt", "wb");
			if (f) {
				std::string utfstring("\xEF\xBB\xBF");
				utfstring.append(folder);
				fwrite(utfstring.c_str(), 1, utfstring.length(), f);
				fclose(f);
			}
			state->installed = false;
		}
		else
			state->otherinstalled = false;
	}
	else {
		File::Delete(PPSSPPpath / "installed.txt");
		state->memstickMyDocEnabled = true;
		state->otherinstalled = false;
		state->installed = false;
		g_Config.memStickDirectory = PPSSPPpath / "memstick";
	}
}

#endif

static void OnJitAffectingSetting() {
	System_PostUIMessage(UIMessage::REQUEST_CLEAR_JIT);
}

// Using SYSTEM translations
static void BuildPSPSettingsPage(SettingsPage *page) {
	SettingsPageHost *host = page->Host();

	page->Header("PSP Settings");

	// The ordering here is simply mapping directly to PSP_SYSTEMPARAM_LANGUAGE_*.
	page->Setting(&g_Config.iLanguage);
	page->Setting(&g_Config.iPSPModel).EnabledFunc([] {
		return !PSP_IsInited();
	});
	page->Custom("Nickname", [host](UI::ViewGroup *parent) {
		return AddNicknameChoice(host, parent);
	});
	page->Setting(&g_Config.bDayLightSavings);
	page->Setting(&g_Config.iDateFormat);
	page->Setting(&g_Config.iTimeFormat);
	page->Setting(&g_Config.iButtonPreference);
}

namespace {

// The storage address, shown on one row: a long address has no spaces to wrap at, so it would run
// into the label. Shown without its options (?region=...) and cropped in the middle, keeping the
// start (the bucket) and the end (the folder); the full address is in the text field.
class StorageAddressChoice : public UI::PopupTextInputChoice {
public:
	using UI::PopupTextInputChoice::PopupTextInputChoice;

protected:
	std::string ValueText(bool *shadow) const override {
		std::string text = UI::PopupTextInputChoice::ValueText(shadow);
		if (*shadow)
			return text;
		text = text.substr(0, text.find('?'));
		constexpr size_t MAX_LENGTH = 32;
		const bool ascii = std::all_of(text.begin(), text.end(), [](char c) { return (unsigned char)c < 0x80; });
		if (text.size() <= MAX_LENGTH || !ascii)
			return text;
		return text.substr(0, 16) + "..." + text.substr(text.size() - (MAX_LENGTH - 19));
	}
};

// The save sync views the status line refers to. A search result can show only some of them.
struct SaveSyncViews {
	UI::View *enabled = nullptr;
	UI::PopupTextInputChoice *secret = nullptr;
	UI::PopupTextInputChoice *passphrase = nullptr;
};

// The save sync status line, drawn as a hint of the "Sync saves" row like the other settings' tips,
// so it sits inside the section. It refreshes itself as the screen draws (syncing happens in the
// background), and keeps the secret fields' "Saved"/"Not set" in step with the store.
class SaveSyncStatusView : public SettingHint {
public:
	explicit SaveSyncStatusView(std::shared_ptr<SaveSyncViews> views)
		: SettingHint("", views->enabled), views_(views) {}

	void Update() override {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		std::string text;
		if (!SaveSync::IsAvailable()) {
			text = sy->T("This build of PPSSPP has no save sync.");
		} else if (SaveSync::IsApplying()) {
			text = sy->T("Connecting to the storage...");
		} else if (std::string error = SaveSync::GetLastStartError(); !error.empty()) {
			text = error;
		} else if (!g_Config.bSaveSyncEnabled) {
			text = sy->T("Off.");
		} else if (!SaveSync::IsRunning()) {
			text = sy->T("Not syncing: fill in the storage, access key ID and secret access key.");
		} else if (std::string status = SaveSync::GetStatus(); !status.empty()) {
			text = status;
		} else {
			text = sy->T("Syncing. Nothing has changed yet.");
		}
		SetText(text);
		if (views_->secret) {
			views_->secret->SetShadowText(SaveSync::HasSecret(SaveSync::SECRET_ACCESS_KEY) ? sy->T("Saved") : sy->T("Not set"));
		}
		if (views_->passphrase) {
			views_->passphrase->SetShadowText(SaveSync::HasSecret(SaveSync::ENCRYPTION_PASSPHRASE) ? sy->T("Saved") : sy->T("None"));
		}
		SettingHint::Update();
	}

private:
	std::shared_ptr<SaveSyncViews> views_;
};

}  // namespace

// Save sync with emusync (Core/SaveSync.h): the storage and its keys, an optional encryption
// passphrase, and a status line. Each change applies at once, in the background.
static void BuildSaveSyncSettings(SettingsPage *page, SystemPageState *state) {
	using namespace UI;
	SettingsPageHost *host = page->Host();
	auto views = std::make_shared<SaveSyncViews>();
	const auto apply = [](EventParams &) { SaveSync::ApplySettingsAsync(); };
	const auto available = []() { return SaveSync::IsAvailable(); };

	page->Header("Save sync");

	page->Bool(&g_Config.bSaveSyncEnabled, "Sync saves").OnChange(apply).EnabledFunc(available).Setup([views](View *view) {
		views->enabled = view;
	});
	page->Hint({I18NCat::SYSTEM, "Save sync Tip", "Saves made on your other devices are put on the memory stick before a game starts, and yours upload as the game saves them. Uses storage you own: Amazon S3 or an S3-compatible service."});

	page->Custom("Storage", [host, apply](ViewGroup *parent) -> View * {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		PopupTextInputChoice *storage = parent->Add(new StorageAddressChoice(host->PageRequesterToken(), &g_Config.sSaveSyncRemoteURI, sy->T("Storage"), "s3://bucket/folder?region=eu-west-1", 512, host->PageScreenManager()));
		storage->OnChange.Add(apply);
		return storage;
	}).EnabledFunc(available);
	page->Hint({I18NCat::SYSTEM, "Save sync storage Tip", "Bucket and folder, e.g. s3://my-saves/emusync/v1?region=eu-west-1. Use the same on every device. S3-compatible services take an endpoint: s3://bucket/folder?endpoint=https://...&region=auto"});

	page->Custom("Access key ID", [host, apply](ViewGroup *parent) -> View * {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		PopupTextInputChoice *keyId = parent->Add(new PopupTextInputChoice(host->PageRequesterToken(), &g_Config.sSaveSyncAccessKeyID, sy->T("Access key ID"), "", 128, host->PageScreenManager()));
		keyId->OnChange.Add(apply);
		return keyId;
	}).EnabledFunc(available);

	// The field never holds the stored secret: what is typed is handed over, then cleared.
	page->Custom("Secret access key", [host, state, views](ViewGroup *parent) -> View * {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		PopupTextInputChoice *secret = parent->Add(new PopupTextInputChoice(host->PageRequesterToken(), &state->saveSyncSecret, sy->T("Secret access key"), "", 128, host->PageScreenManager()));
		secret->SetPasswordDisplay();
		secret->OnChange.Add([state](EventParams &) {
			std::string value(StripSpaces(state->saveSyncSecret));
			SaveSync::WipeSecret(state->saveSyncSecret);
			if (!value.empty())
				SaveSync::ApplySettingsAsync({{SaveSync::SECRET_ACCESS_KEY, std::move(value)}});
		});
		views->secret = secret;
		return secret;
	}).EnabledFunc(available);

	page->Custom("Encryption passphrase", [host, state, views](ViewGroup *parent) -> View * {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		PopupTextInputChoice *passphrase = parent->Add(new PopupTextInputChoice(host->PageRequesterToken(), &state->saveSyncPassphrase, sy->T("Encryption passphrase"), "", 256, host->PageScreenManager()));
		passphrase->SetPasswordDisplay();
		// Spaces at either end are part of a passphrase: it has to match on every device.
		passphrase->SetKeepSpaces(true);
		passphrase->OnChange.Add([state](EventParams &) {
			// Not trimmed: spaces can be part of a passphrase, and it has to match on every device.
			std::string value = state->saveSyncPassphrase;
			SaveSync::WipeSecret(state->saveSyncPassphrase);
			if (!value.empty())
				SaveSync::ApplySettingsAsync({{SaveSync::ENCRYPTION_PASSPHRASE, std::move(value)}});
		});
		views->passphrase = passphrase;
		return passphrase;
	}).EnabledFunc(available);
	page->Hint({I18NCat::SYSTEM, "Save sync passphrase Tip", "Optional. Encrypts your saves on this device before they upload, so the storage provider can't read them. Use the same passphrase on every device. It can't be recovered."});

	// Show displays a saved secret, e.g. to set up another device with it.
	const auto show = [host](const char *name, std::string_view title) {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		auto di = GetI18NCategory(I18NCat::DIALOG);
		std::string error;
		std::optional<std::string> value = SaveSync::LoadSecret(name, &error);
		std::string message = value.has_value() ? *value : (error.empty() ? std::string(sy->T("Not set")) : error);
		if (value.has_value())
			SaveSync::WipeSecret(*value);
		host->PageScreenManager()->push(new MessagePopupScreen(title, message, di->T("OK"), ""));
		SaveSync::WipeSecret(message);
	};
	// While a new one is being saved, the store may still hold the old one.
	page->Choice("Show secret access key").OnClick([show](EventParams &) {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		show(SaveSync::SECRET_ACCESS_KEY, sy->T("Secret access key"));
	}).EnabledFunc([]() { return SaveSync::HasSecret(SaveSync::SECRET_ACCESS_KEY) && !SaveSync::IsApplying(); });
	page->Choice("Show encryption passphrase").OnClick([show](EventParams &) {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		show(SaveSync::ENCRYPTION_PASSPHRASE, sy->T("Encryption passphrase"));
	}).EnabledFunc([]() { return SaveSync::HasSecret(SaveSync::ENCRYPTION_PASSPHRASE) && !SaveSync::IsApplying(); });
	page->Choice("Remove encryption passphrase").OnClick([](EventParams &) {
		SaveSync::ApplySettingsAsync({{SaveSync::ENCRYPTION_PASSPHRASE, std::string()}});
	}).EnabledFunc([]() { return SaveSync::HasSecret(SaveSync::ENCRYPTION_PASSPHRASE) && !SaveSync::IsApplying(); });

	page->Custom("Device name", [host, apply](ViewGroup *parent) -> View * {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		PopupTextInputChoice *deviceName = parent->Add(new PopupTextInputChoice(host->PageRequesterToken(), &g_Config.sSaveSyncDeviceName, sy->T("Device name"), "", 64, host->PageScreenManager()));
		deviceName->SetShadowText(sy->T("This device's name"));
		deviceName->OnChange.Add(apply);
		return deviceName;
	}).EnabledFunc(available);

	page->Choice("Sync now").OnClick([](EventParams &) {
		SaveSync::SyncNow();
	}).EnabledFunc([]() { return SaveSync::IsRunning() && !SaveSync::IsApplying(); });

	// Under Sync now, with the enabled state (and so the color) of the switch, which is on whenever
	// the build has save sync.
	page->Custom("Save sync", [views](ViewGroup *parent) -> View * {
		return parent->Add(new SaveSyncStatusView(views));
	});
}

void BuildSystemSettings(SettingsPage *page, SystemPageState *state) {
	using namespace UI;

	SettingsPageHost *host = page->Host();

	page->Header();
	page->Link({I18NCat::SYSTEM, "PSP Settings"}, I18NCat::SYSTEM, &BuildPSPSettingsPage);

	page->Header("PSP Memory Stick");

	if (System_GetPropertyBool(SYSPROP_HAS_OPEN_DIRECTORY)) {
		page->Choice("Show Memory Stick folder").OnClick([](UI::EventParams &p) {
			System_LaunchUrl(LaunchUrlType::LOCAL_FOLDER, g_Config.memStickDirectory.ToString());
		});
	}

#if PPSSPP_PLATFORM(MAC) || PPSSPP_PLATFORM(IOS)
	bool showItHere = true;
#if PPSSPP_PLATFORM(IOS_APP_STORE)
	if (g_Config.memStickDirectory == DarwinFileSystemServices::defaultMemoryStickPath()) {
		// We still keep a way to access it on the developer tools screen.
		showItHere = false;
	}
#endif
	if (showItHere) {
		page->Choice("Set Memory Stick folder").OnClick([host](UI::EventParams &) {
			SetMemStickDirDarwin(host->PageRequesterToken());
		});
	}
#endif

	auto addMemstickPath = [host, &page]() {
		page->Custom("Memory Stick folder", [host](UI::ViewGroup *parent) -> UI::View * {
			Choice *memstickPath = parent->Add(new ChoiceWithCallbackValueDisplay(T(I18NCat::SYSTEM, "Memory Stick folder"), []() {
				return g_Config.memStickDirectory.ToVisualString();
			}));
			memstickPath->OnClick.Add([host](UI::EventParams &e) {
				host->PageScreenManager()->push(new MemStickScreen(false));
			});
			return memstickPath;
		}).OpensScreen().EnabledFunc([] {
			return !PSP_IsInited();
		});
	};

#if PPSSPP_PLATFORM(ANDROID)
	if (System_GetPropertyInt(SYSPROP_DEVICE_TYPE) != DEVICE_TYPE_VR) {
		addMemstickPath();
	}
#elif defined(_WIN32)
#if PPSSPP_PLATFORM(UWP)
	addMemstickPath();
#else
	page->Bool(&state->installed, "Memory Stick in My Documents").Enabled(&state->memstickMyDocEnabled).OnChange([state](UI::EventParams &e) {
		OnMemoryStickMyDoc(state);
	});
	page->Bool(&state->otherinstalled, "Memory Stick in installed.txt").Enabled(&state->memstickOtherEnabled).OnChange([state](UI::EventParams &e) {
		OnMemoryStickOther(state);
	});

	if (!System_GetPropertyBool(SYSPROP_CAN_GET_FREE_SPACE_FAST)) {
		// It might be slow, so use an option.
		page->Setting(&g_Config.bReportAccurateFreeStorageSpace);
	}
#endif
#endif
	page->Setting(&g_Config.bMemStickInserted);
	page->Setting(&g_Config.iMemStickSizeGB);

	BuildSaveSyncSettings(page, state);

	page->Header("Emulation");

	page->Setting(&g_Config.bFastMemory).OnChange([](UI::EventParams &e) {
		OnJitAffectingSetting();
	});
	page->Setting(&g_Config.bIgnoreBadMemAccess);

	if (System_GetPropertyBool(SYSPROP_ENOUGH_RAM_FOR_FULL_ISO)) {
		page->Setting(&g_Config.bCacheFullIsoInRam).EnabledFunc([] {
			return !PSP_IsInited();
		});
	}

	page->Setting(&g_Config.iIOTimingMethod);
	page->Setting(&g_Config.bForceLagSync).Disabled(&g_Config.bAutoFrameSkip);
	page->Setting(&g_Config.iLockedCPUSpeed);

#if defined(_WIN32)
	page->Header("Recording");
	page->Setting(&g_Config.bDumpFrames);
	page->Setting(&g_Config.bDumpVideoOutput);
	page->Setting(&g_Config.bDumpAudio);
	page->Setting(&g_Config.bSaveLoadResetsAVdumping);
#endif
}
