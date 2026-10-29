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

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#ifdef SHARED_LIBZIP
#include <zip.h>
#else
#include "ext/libzip/zip.h"
#endif
#include "Core/Slang/SlangPackageImporter.h"
#include "Core/Slang/SlangPaths.h"
#include "Core/Loaders.h"
#include "Core/Config.h"
#include "Common/Net/HTTPRequest.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/Data/Format/JSONReader.h"
#include "Common/Data/Format/JSONWriter.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/Thread/ThreadUtil.h"
#include <algorithm>

const std::vector<SlangShaderPack> &GetSlangShaderPacks() {
	static const std::vector<SlangShaderPack> packs = {
		{ "libretro", "", nullptr, nullptr, "" },
		{ "lola-retro", "lola-retro", "zoran123456/lola-retro", "lola-retro-", "lola-retro/" },
		// The release also has Reshade versions; only the slang preset and its shaders are wanted.
		{ "satpixie-crt", "satpixie-crt", "Conkwer/satpixie-crt-shader", "satpixie-crt-shader-", "satpixie-crt-shader/RetroArch/shaders/shaders_slang/crt/" },
	};
	return packs;
}

Path GetSlangShaderPackDir(const SlangShaderPack &pack) {
	return *pack.dir ? GetSlangShaderDir() / pack.dir : GetSlangShaderDir();
}

bool IsSlangShaderPackInstalled(const SlangShaderPack &pack) {
	return File::Exists(GetSlangShaderPackDir(pack) / "manifest.json");
}

bool FindGitHubReleaseAsset(const std::string &json, const std::string &assetPrefix,
                            std::string *url, std::string *error) {
	json::JsonReader reader(json.data(), json.size());
	if (!reader.ok()) {
		*error = "couldn't parse the release info";
		return false;
	}
	json::JsonGet root = reader.root();
	std::string tag;
	const JsonNode *assets = root.getArray("assets");
	if (!root.getString("tag_name", &tag) || !assets) {
		*error = "the release info has no tag or assets";
		return false;
	}
	// Matching the whole name skips the release's other zips (other emulators, variants).
	const std::string wanted = assetPrefix + tag + ".zip";
	for (const JsonNode *iter : assets->value) {
		json::JsonGet asset = iter->value;
		if (wanted == asset.getStringOr("name", "") && asset.getString("browser_download_url", url) && !url->empty()) {
			return true;
		}
	}
	*error = "release " + tag + " has no " + wanted;
	return false;
}

// Empties 'dir', except for the top-level entries named in 'keep'.
static void ClearDir(const Path &dir, const std::vector<std::string> &keep) {
	if (keep.empty()) {
		File::DeleteDirRecursively(dir);
		return;
	}
	std::vector<File::FileInfo> files;
	File::GetFilesInDir(dir, &files, nullptr);
	for (const File::FileInfo &info : files) {
		if (std::find(keep.begin(), keep.end(), info.name) != keep.end()) {
			continue;
		}
		if (info.isDirectory) {
			File::DeleteDirRecursively(info.fullName);
		} else {
			File::Delete(info.fullName);
		}
	}
}

static bool WriteZipEntry(zip_t *z, zip_uint64_t index, const Path &out, std::string *error) {
	struct zip_stat st; zip_stat_init(&st);
	if (zip_stat_index(z, index, 0, &st) != 0) { *error = "zip_stat_index failed"; return false; }
	zip_file_t *zf = zip_fopen_index(z, index, 0);
	if (!zf) { *error = "zip_fopen_index failed"; return false; }
	if (!File::CreateFullPath(Path(out.GetDirectory()))) { zip_fclose(zf); *error = "mkdir failed"; return false; }
	FILE *f = File::OpenCFile(out, "wb");
	if (!f) { zip_fclose(zf); *error = "open output failed"; return false; }
	char buf[128 * 1024];
	zip_uint64_t remaining = st.size;
	bool ok = true;
	while (remaining > 0) {
		zip_int64_t n = zip_fread(zf, buf, sizeof(buf));
		if (n <= 0) { ok = false; break; }
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
		remaining -= (zip_uint64_t)n;
	}
	fclose(f);
	zip_fclose(zf);
	if (!ok) *error = "zip read/write failed";
	return ok;
}

bool ExtractSlangPackage(const Path &zipPath, const Path &destRoot,
                         const std::string &sourceUrl, int64_t unixTimestamp,
                         std::string *error,
                         const SlangExtractProgressCallback &progress,
                         const std::string &zipSubdir,
                         const std::vector<std::string> &keep) {
	// Extract directly into destRoot. We intentionally do NOT use a temp-dir + rename swap:
	// on Android scoped storage (SAF) a directory rename/move is unsupported and the copy
	// fallback recursively copies every file (minutes of thrash) and then fails, leaving an
	// empty install. PPSSPP's own GameManager::ExtractZipContents extracts straight to the
	// destination for the same reason. Trade-off: a failed/interrupted import can leave a
	// partial tree, so we clear destRoot first and remove it again on failure rather than
	// guaranteeing the previous install survives a mid-extraction error.
	ClearDir(destRoot, keep);   // start from a clean install
	if (!File::CreateFullPath(destRoot)) { *error = "could not create shader dir"; return false; }

	ZipContainer zipContainer = ZipOpenPath(zipPath);
	zip_t *z = zipContainer;
	if (!z) { *error = "could not open downloaded zip"; ClearDir(destRoot, keep); return false; }

	int fileCount = 0;
	zip_int64_t numEntries = zip_get_num_entries(z, 0);
	for (zip_int64_t i = 0; i < numEntries; i++) {
		const char *name = zip_get_name(z, (zip_uint64_t)i, 0);
		if (!name) continue;
		std::string entry(name);
		if (!zipSubdir.empty()) {
			if (!startsWith(entry, zipSubdir) || entry.size() == zipSubdir.size()) continue;
			entry = entry.substr(zipSubdir.size());
		}
		bool isDir = !entry.empty() && (entry.back() == '/' || entry.back() == '\\');
		Path outPath;
		if (!ResolveSafeZipEntryPath(destRoot, entry, isDir, &outPath)) {
			continue;  // silently skip rejected entries (traversal, wrong ext, metadata)
		}
		if (isDir) { File::CreateFullPath(outPath); continue; }
		if (!WriteZipEntry(z, (zip_uint64_t)i, outPath, error)) {
			ClearDir(destRoot, keep); return false;
		}
		fileCount++;
		// numEntries includes directory + rejected entries, so this is an approximate fraction;
		// good enough to keep the progress bar advancing during the slow per-file SAF writes.
		if (progress) progress((int)i + 1, (int)numEntries);
	}
	ZipClose(zipContainer);

	if (fileCount == 0) { *error = "no valid shader files in archive"; ClearDir(destRoot, keep); return false; }

	// Write the manifest last, so its presence marks a completed extraction.
	{
		json::JsonWriter w(json::JsonWriter::PRETTY);
		w.begin();
		w.writeString("sourceUrl", sourceUrl);
		w.writeInt("timestamp", (int)unixTimestamp);
		w.writeInt("fileCount", fileCount);
		w.end();
		File::WriteStringToFile(true, w.str(), destRoot / "manifest.json");
	}
	return true;
}

SlangPackageImporter g_SlangImporter;

SlangPackageImporter::~SlangPackageImporter() {
	if (extractThread_.joinable()) extractThread_.join();
}

bool SlangPackageImporter::Start(const SlangShaderPack &pack) {
	if (Busy()) return false;
	pack_ = pack;
	zipPath_ = Path(g_Config.memStickDirectory) / "ppsspp_slang.dl";
	error_.clear();
	extractDone_ = false;
	extractOk_ = false;
	if (pack.githubRepo) {
		// The zip's URL has the release tag in it, so ask GitHub which release is the latest first.
		sourceUrl_.clear();
		std::string apiUrl = std::string("https://api.github.com/repos/") + pack.githubRepo + "/releases/latest";
		releaseInfo_ = g_DownloadManager.StartDownload(apiUrl, Path(), http::RequestFlags::Default,
			"application/vnd.github+json", "slang_release_info");
		if (!releaseInfo_) { error_ = "failed to start download"; state_ = SlangImportState::FAILED; return false; }
		state_ = SlangImportState::DOWNLOADING;
		return true;
	}
	sourceUrl_ = g_Config.sSlangBuildbotUrl;
	if (sourceUrl_.empty()) { error_ = "no buildbot URL configured"; state_ = SlangImportState::FAILED; return false; }
	StartZipDownload();
	return state_ == SlangImportState::DOWNLOADING;
}

void SlangPackageImporter::StartZipDownload() {
	download_ = g_DownloadManager.StartDownload(sourceUrl_, zipPath_,
		http::RequestFlags::ProgressBar | http::RequestFlags::ProgressBarDelayed,
		"application/zip", "slang_shaders");
	if (!download_) { error_ = "failed to start download"; state_ = SlangImportState::FAILED; return; }
	state_ = SlangImportState::DOWNLOADING;
}

void SlangPackageImporter::Update() {
	if (state_ == SlangImportState::DOWNLOADING) {
		if (releaseInfo_) {
			if (!releaseInfo_->Done()) {
				return;
			}
			std::shared_ptr<http::Request> finished = releaseInfo_;
			releaseInfo_.reset();
			if (finished->Failed() || finished->ResultCode() != 200) {
				error_ = "couldn't look up the latest release (HTTP " + std::to_string(finished->ResultCode()) + ")";
				state_ = SlangImportState::FAILED;
				return;
			}
			std::string json;
			finished->buffer().TakeAll(&json);
			if (!FindGitHubReleaseAsset(json, pack_.assetPrefix, &sourceUrl_, &error_)) {
				state_ = SlangImportState::FAILED;
				return;
			}
			StartZipDownload();
			return;
		}
		if (download_ && download_->Done()) {
			bool ok = !download_->Failed() && download_->ResultCode() == 200 && File::Exists(zipPath_);
			std::shared_ptr<http::Request> finished = download_;
			download_.reset();
			if (!ok) {
				error_ = "download failed (HTTP " + std::to_string(finished ? finished->ResultCode() : 0) + ")";
				File::Delete(zipPath_);
				state_ = SlangImportState::FAILED;
				return;
			}
			// Kick extraction on a worker thread (extraction can be slow).
			state_ = SlangImportState::EXTRACTING;
			std::string src = sourceUrl_;
			Path zip = zipPath_;
			Path dest = GetSlangShaderPackDir(pack_);
			std::string zipSubdir = pack_.zipSubdir;
			// The libretro pack is installed at the root, around the other packs' directories.
			std::vector<std::string> keep;
			if (!*pack_.dir) {
				for (const SlangShaderPack &other : GetSlangShaderPacks()) {
					if (*other.dir) {
						keep.push_back(other.dir);
					}
				}
			}
			extractProgress_ = 0.0f;
			extractThread_ = std::thread([this, zip, src, dest, zipSubdir, keep]() {
				// On Android the shader dir is a scoped-storage (SAF) path, so File:: ops route
				// through JNI; a raw std::thread is not attached to the JVM and would abort in
				// getEnv(). Attach for the lifetime of the extraction (no-op on other platforms).
				AndroidJNIThreadContext jniContext;
				std::string err;
				// NOTE: pass 0 for timestamp; Date/time is not available in this layer without
				// plumbing. A wall-clock stamp can be added later; fileCount+url suffice for now.
				bool ok = ExtractSlangPackage(zip, dest, src, 0, &err,
					[this](int done, int total) {
						extractProgress_ = total > 0 ? (float)done / (float)total : 0.0f;
					}, zipSubdir, keep);
				threadError_ = err;
				extractOk_ = ok;
				extractDone_ = true;
			});
		}
	} else if (state_ == SlangImportState::EXTRACTING) {
		if (extractDone_.load()) {
			if (extractThread_.joinable()) extractThread_.join();
			File::Delete(zipPath_);
			error_ = threadError_;   // safe: worker wrote threadError_ before the extractDone_ release; now UI-thread-only
			state_ = extractOk_.load() ? SlangImportState::DONE : SlangImportState::FAILED;
		}
	}
}

float SlangPackageImporter::GetProgress() const {
	// Download is the first 50% of the bar, extraction the second 50% (extraction of thousands
	// of files on scoped storage takes as long as, or longer than, the download).
	if (state_ == SlangImportState::DOWNLOADING && download_) return download_->Progress() * 0.5f;
	if (state_ == SlangImportState::EXTRACTING) return 0.5f + extractProgress_.load() * 0.5f;
	if (state_ == SlangImportState::DONE) return 1.0f;
	return 0.0f;
}
