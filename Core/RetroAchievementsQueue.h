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

#pragma once

// The offline queue for RetroAchievements: achievement unlocks and leaderboard entries that couldn't
// reach the server are kept on disk and submitted later. This file is the part that doesn't depend
// on the rc_client or the network - parsing the requests rcheevos makes, building the delayed
// requests, reading the server's answers and storing the queue. RetroAchievements.cpp drives it.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Achievements {

struct DeferredSubmission {
	enum class Type {
		Achievement,
		Leaderboard,
	};

	Type type = Type::Achievement;
	std::string url;  // The dorequest.php URL rcheevos sent it to.
	std::string username;
	uint32_t id = 0;  // Achievement or leaderboard ID.
	bool hardcore = false;  // Achievements only.
	int32_t score = 0;  // Leaderboards only.
	std::string gameHash;
	int64_t time = 0;  // When it was earned, in seconds since the epoch.

	bool operator==(const DeferredSubmission &other) const {
		return type == other.type && url == other.url && username == other.username && id == other.id &&
			hardcore == other.hardcore && score == other.score && gameHash == other.gameHash && time == other.time;
	}
};

enum class SubmissionOutcome {
	Accepted,  // Done, including an achievement the server already had.
	Retry,  // No answer, or the server is busy. Keep it and try again later.
	AuthFailed,  // The token was refused. Keep it until the user logs in again.
	Rejected,  // The server refused the submission itself. Retrying won't help.
};

// Recognizes the awardachievement and submitlbentry requests rcheevos builds, from their form-encoded
// post data. now is the current time, used with the request's "o" offset to work out when it was earned.
// Returns false for any other request.
bool ParseDeferrableRequest(std::string_view url, std::string_view postData, int64_t now, DeferredSubmission *out);

// Builds the post data to submit it with now, including the time since it was earned, so the server
// records the original time.
std::string BuildDeferredPostData(const DeferredSubmission &sub, const std::string &token, int64_t now);

// A successful server response for the rc_client to process instead of the real one, for a request
// that was put in the queue. score/softcoreScore are the user's current points.
std::string SynthesizeDeferredResponse(const DeferredSubmission &sub, uint32_t score, uint32_t softcoreScore);

// Status codes the rc_client retries on (see rc_client_should_retry).
bool IsRetryableHttpStatus(int httpStatus);

// httpStatus is 0 or negative if the request didn't get an answer at all.
SubmissionOutcome ClassifySubmissionResponse(const DeferredSubmission &sub, int httpStatus, std::string_view body, std::string *errorMessage);

// Returns false if the same submission is already queued. An achievement unlock is also a duplicate of
// a queued hardcore unlock of the same achievement, which covers softcore as well.
bool AddDeferredSubmission(std::vector<DeferredSubmission> *queue, const DeferredSubmission &sub);

std::string SerializeDeferredSubmissions(const std::vector<DeferredSubmission> &queue);
bool DeserializeDeferredSubmissions(std::string_view data, std::vector<DeferredSubmission> *queue);

}  // namespace Achievements
