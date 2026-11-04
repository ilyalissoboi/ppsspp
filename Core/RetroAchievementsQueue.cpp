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

#include <cstdlib>

#include "ext/rcheevos/include/rc_api_request.h"
#include "ext/rcheevos/include/rc_api_runtime.h"
#include "ext/rcheevos/include/rc_error.h"

#include "Common/Data/Format/JSONReader.h"
#include "Common/Data/Format/JSONWriter.h"
#include "Common/Net/URL.h"
#include "Common/StringUtils.h"
#include "Core/RetroAchievementsQueue.h"

namespace Achievements {

static bool ParseUnsigned(std::string_view str, uint32_t *out) {
	if (str.empty()) {
		return false;
	}
	std::string s(str);
	char *end = nullptr;
	unsigned long value = strtoul(s.c_str(), &end, 10);
	if (*end != '\0') {
		return false;
	}
	*out = (uint32_t)value;
	return true;
}

static bool ParseSigned(std::string_view str, int32_t *out) {
	if (str.empty()) {
		return false;
	}
	std::string s(str);
	char *end = nullptr;
	long value = strtol(s.c_str(), &end, 10);
	if (*end != '\0') {
		return false;
	}
	*out = (int32_t)value;
	return true;
}

bool ParseDeferrableRequest(std::string_view url, std::string_view postData, int64_t now, DeferredSubmission *out) {
	std::vector<std::string_view> params;
	SplitString(postData, '&', params);

	std::string r, user, hash;
	uint32_t id = 0, hardcore = 0, offset = 0;
	int32_t score = 0;
	bool hasId = false, hasScore = false;
	for (std::string_view param : params) {
		std::string_view key, value;
		if (!SplitStringOnce(param, &key, &value, '=')) {
			continue;
		}
		std::string decoded = UriDecode(value);
		if (key == "r") {
			r = decoded;
		} else if (key == "u") {
			user = decoded;
		} else if (key == "a" || key == "i") {
			hasId = ParseUnsigned(decoded, &id);
		} else if (key == "h") {
			ParseUnsigned(decoded, &hardcore);
		} else if (key == "s") {
			hasScore = ParseSigned(decoded, &score);
		} else if (key == "m") {
			hash = decoded;
		} else if (key == "o") {
			ParseUnsigned(decoded, &offset);
		}
	}

	DeferredSubmission sub;
	if (r == "awardachievement") {
		sub.type = DeferredSubmission::Type::Achievement;
		sub.hardcore = hardcore != 0;
	} else if (r == "submitlbentry") {
		if (!hasScore) {
			return false;
		}
		sub.type = DeferredSubmission::Type::Leaderboard;
		sub.score = score;
	} else {
		return false;
	}
	if (!hasId || id == 0 || user.empty()) {
		return false;
	}

	sub.url = url;
	sub.username = user;
	sub.id = id;
	sub.gameHash = hash;
	sub.time = now - offset;
	*out = sub;
	return true;
}

std::string BuildDeferredPostData(const DeferredSubmission &sub, const std::string &token, int64_t now) {
	int64_t elapsed = now - sub.time;
	// The clock may have gone backwards since. Zero leaves the offset out, which means "just now".
	uint32_t secondsSince = elapsed > 0 ? (uint32_t)elapsed : 0;

	rc_api_request_t request;
	int result;
	if (sub.type == DeferredSubmission::Type::Achievement) {
		rc_api_award_achievement_request_t params{};
		params.username = sub.username.c_str();
		params.api_token = token.c_str();
		params.achievement_id = sub.id;
		params.hardcore = sub.hardcore ? 1 : 0;
		params.game_hash = sub.gameHash.c_str();
		params.seconds_since_unlock = secondsSince;
		result = rc_api_init_award_achievement_request(&request, &params);
	} else {
		rc_api_submit_lboard_entry_request_t params{};
		params.username = sub.username.c_str();
		params.api_token = token.c_str();
		params.leaderboard_id = sub.id;
		params.score = sub.score;
		params.game_hash = sub.gameHash.c_str();
		params.seconds_since_completion = secondsSince;
		result = rc_api_init_submit_lboard_entry_request(&request, &params);
	}

	std::string postData;
	if (result == RC_OK && request.post_data) {
		postData = request.post_data;
	}
	rc_api_destroy_request(&request);
	return postData;
}

std::string SynthesizeDeferredResponse(const DeferredSubmission &sub, uint32_t score, uint32_t softcoreScore) {
	if (sub.type == DeferredSubmission::Type::Achievement) {
		// Leaving out AchievementsRemaining keeps the rc_client from declaring mastery, which it can't know about.
		return StringFromFormat("{\"Success\":true,\"Score\":%u,\"SoftcoreScore\":%u,\"AchievementID\":%u}", score, softcoreScore, sub.id);
	}
	return StringFromFormat("{\"Success\":true,\"Response\":{\"Score\":%d,\"BestScore\":%d,\"RankInfo\":{\"Rank\":0,\"NumEntries\":\"0\"},\"TopEntries\":[]}}", sub.score, sub.score);
}

bool IsRetryableHttpStatus(int httpStatus) {
	switch (httpStatus) {
	case 429:
	case 502:
	case 503:
	case 504:
	case 521:
	case 522:
	case 523:
	case 524:
	case 525:
		return true;
	default:
		return false;
	}
}

SubmissionOutcome ClassifySubmissionResponse(const DeferredSubmission &sub, int httpStatus, std::string_view body, std::string *errorMessage) {
	errorMessage->clear();
	if (httpStatus <= 0 || IsRetryableHttpStatus(httpStatus)) {
		return SubmissionOutcome::Retry;
	}

	std::string bodyStr(body);
	rc_api_server_response_t serverResponse{};
	serverResponse.body = bodyStr.c_str();
	serverResponse.body_length = bodyStr.size();
	serverResponse.http_status_code = httpStatus;

	int result;
	bool succeeded;
	if (sub.type == DeferredSubmission::Type::Achievement) {
		rc_api_award_achievement_response_t response;
		result = rc_api_process_award_achievement_server_response(&response, &serverResponse);
		succeeded = result == RC_OK && response.response.succeeded;
		if (response.response.error_message) {
			*errorMessage = response.response.error_message;
		}
		rc_api_destroy_award_achievement_response(&response);
	} else {
		rc_api_submit_lboard_entry_response_t response;
		result = rc_api_process_submit_lboard_entry_server_response(&response, &serverResponse);
		succeeded = result == RC_OK && response.response.succeeded;
		if (response.response.error_message) {
			*errorMessage = response.response.error_message;
		}
		rc_api_destroy_submit_lboard_entry_response(&response);
	}

	// Only an answer from the API itself, which always has Success, can settle a submission. Anything else (an
	// empty body, an error page from a proxy or a captive portal) may well work next time, and dropping it would
	// lose the unlock. rcheevos can't tell us, it reads a missing Success as true.
	if (bodyStr.empty() || bodyStr[0] != '{') {
		return SubmissionOutcome::Retry;
	}
	json::JsonReader reader(bodyStr.data(), bodyStr.size());
	if (!reader.ok() || !reader.root()) {
		return SubmissionOutcome::Retry;
	}
	json::JsonGet root = reader.root();
	if (root.hasChild("Success", JSON_TRUE)) {
		// Accepted, even if the rest of the response isn't what rcheevos expects.
		return SubmissionOutcome::Accepted;
	}
	if (!root.hasChild("Success", JSON_FALSE)) {
		return SubmissionOutcome::Retry;
	}
	if (succeeded) {
		// An achievement the server already had.
		return SubmissionOutcome::Accepted;
	}
	// The award achievement parser doesn't look at the error code, so check it here.
	std::string code = root.getStringOr("Code", "");
	if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED || httpStatus == 401 ||
		code == "invalid_credentials" || code == "expired_token" || code == "access_denied") {
		return SubmissionOutcome::AuthFailed;
	}
	if (errorMessage->empty()) {
		*errorMessage = rc_error_str(result);
	}
	return SubmissionOutcome::Rejected;
}

bool AddDeferredSubmission(std::vector<DeferredSubmission> *queue, const DeferredSubmission &sub) {
	for (const DeferredSubmission &queued : *queue) {
		if (queued.type != sub.type || queued.id != sub.id || !equalsNoCase(queued.username, sub.username)) {
			continue;
		}
		if (sub.type == DeferredSubmission::Type::Achievement) {
			if (queued.hardcore || !sub.hardcore) {
				return false;
			}
		} else if (queued.score == sub.score && queued.time == sub.time) {
			return false;
		}
	}
	queue->push_back(sub);
	return true;
}

std::string SerializeDeferredSubmissions(const std::vector<DeferredSubmission> &queue) {
	json::JsonWriter writer(json::JsonWriter::PRETTY);
	writer.begin();
	writer.writeInt("version", 1);
	writer.pushArray("submissions");
	for (const DeferredSubmission &sub : queue) {
		writer.pushDict();
		const bool achievement = sub.type == DeferredSubmission::Type::Achievement;
		writer.writeString("type", achievement ? "achievement" : "leaderboard");
		writer.writeString("url", sub.url);
		writer.writeString("user", sub.username);
		writer.writeUint("id", sub.id);
		if (achievement) {
			writer.writeBool("hardcore", sub.hardcore);
		} else {
			writer.writeInt("score", sub.score);
		}
		writer.writeString("hash", sub.gameHash);
		writer.writeFloat("time", (double)sub.time);
		writer.pop();
	}
	writer.pop();
	writer.end();
	return writer.str();
}

bool DeserializeDeferredSubmissions(std::string_view data, std::vector<DeferredSubmission> *queue) {
	queue->clear();
	json::JsonReader reader(data.data(), data.size());
	if (!reader.ok()) {
		return false;
	}
	json::JsonGet root = reader.root();
	if (!root) {
		return false;
	}
	const JsonNode *submissions = root.getArray("submissions");
	if (!submissions) {
		return false;
	}
	for (const JsonNode *node : submissions->value) {
		json::JsonGet entry = node->value;
		if (!entry) {
			continue;
		}
		DeferredSubmission sub;
		std::string type = entry.getStringOr("type", "");
		if (type == "achievement") {
			sub.type = DeferredSubmission::Type::Achievement;
			sub.hardcore = entry.getBoolOr("hardcore", false);
		} else if (type == "leaderboard") {
			sub.type = DeferredSubmission::Type::Leaderboard;
			sub.score = entry.getInt("score", 0);
		} else {
			continue;
		}
		sub.url = entry.getStringOr("url", "");
		sub.username = entry.getStringOr("user", "");
		sub.id = (uint32_t)entry.getFloat("id", 0.0);
		sub.gameHash = entry.getStringOr("hash", "");
		sub.time = (int64_t)entry.getFloat("time", 0.0);
		if (sub.url.empty() || sub.username.empty() || sub.id == 0) {
			continue;
		}
		queue->push_back(sub);
	}
	return true;
}

}  // namespace Achievements
