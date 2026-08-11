/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/LiveObserver.h"
#include "Common/Recorder.h"
#include "Common/GlobalData.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "GameLogic/GameLogic.h"	// the buffering gate pauses the game; see updatePlaybackGate
#include "GameClient/ClientInstance.h"
#include "GameClient/InGameUI.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/multi.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/websockets.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <fstream>		// cacert.pem presence check, see connectToRelay
#include <string>
#include <thread>
#include <windows.h>

// ============================================================================
// liveObserverLog
// ============================================================================
// LIVE_OBSERVER_BUILD_TAG and the LIVE_OBSERVER_LOGGING gate both live in LiveObserver.h.

void liveObserverLog(const char* fmt, ...) {
#if !defined(LIVE_OBSERVER_LOGGING)
    // Compiled out by RTS_DEBUG_LIVE_OBSERVER=OFF (and by default in release builds).
    // The call sites remain, so switching the option on restores full diagnostics without
    // touching any source. See LiveObserver.h.
    (void)fmt;
#else
    static FILE* logFile = NULL;
    if (!logFile) {
        // TheSuperHackers @fix Give this log a per-instance name, instead of a bare
        // relative filename in the exe's cwd (the install dir). Streamer and observer
        // are the same exe and can share a cwd/install; without this, both processes'
        // calls to liveObserverLog() (this file AND GameLogic.cpp's unconditional
        // LIVE_OBSERVER instrumentation) truncate the same file.
        AsciiString path;
        path.format("live_observer_debug_Instance%.2u.log", rts::ClientInstance::getInstanceId());
        logFile = fopen(path.str(), "w");
        if (logFile)
            fprintf(logFile, "LIVE_OBSERVER_BUILD_TAG=%s\n", LIVE_OBSERVER_BUILD_TAG);
    }
    if (logFile) {
        va_list args;
        va_start(args, fmt);
        vfprintf(logFile, fmt, args);
        va_end(args);
        fflush(logFile);
    }
#endif // LIVE_OBSERVER_LOGGING
}

void liveObserverInitLog(const char* lobbyId) {
    liveObserverLog("=== Live Observer Init ===\n");
    liveObserverLog("Lobby: %s\n", lobbyId ? lobbyId : "(empty)");
    liveObserverLog("Observer mode activated\n");
}

// ============================================================================
// LiveObserver
// ============================================================================
LiveObserver* TheLiveObserver = nullptr;

LiveObserver::LiveObserver()
    : m_connected(false)
    , m_shouldRun(false)
    , m_headerReceived(false)
    , m_streamEnded(false)
    , m_maxCompleteFrame(0)
    , m_safeReadOffset(0)
    , m_parseAbsOffset(0)
    , m_bodyStartOffset(0)
    , m_parseCorrupt(false)
    , m_holdPlayback(FALSE)
    , m_nearLiveHeld(FALSE)
    , m_preRollComplete(FALSE)
    , m_autoPaused(FALSE)
    , m_userPaused(FALSE)
    , m_stalled(FALSE)
    , m_playbackStarted(FALSE)
    , m_lastSeenLiveEdge(0)
    , m_lastLiveEdgeChangeMs(timeGetTime())
    , m_desyncFrame(0)
    , m_delaySeconds(LIVE_DELAY_SECONDS_DEFAULT)
    , m_serverHeld(FALSE)
    , m_delayWaitActive(FALSE)
    , m_delayWaitDeadlineMs(0)
    , m_expectedDelaySeconds(-1)
    , m_spectatorChatMode(SPECTATOR_CHAT_AUTO)
    , m_liveFile(nullptr)
    , m_curlEasy(nullptr)
    , m_curlMulti(nullptr)
{
    // Every field above is session state, and this constructor is the only place it is ever
    // initialised. That is the point of the object: a session begins when one is created and
    // ends when it is destroyed, so there is no "reset the previous session" step to forget.
}

// ============================================================================
// Standalone relay HTTP fetch (live game browser)
// ============================================================================

namespace
{
	std::mutex s_fetchMutex;
	std::atomic<bool> s_fetchInFlight(false);
	std::atomic<bool> s_fetchReady(false);
	std::string s_fetchBody;
	bool s_fetchSuccess = false;
	long s_fetchStatus = 0;

	size_t liveRelayWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		std::string* out = static_cast<std::string*>(userdata);
		out->append(ptr, size * nmemb);
		return size * nmemb;
	}

	// Shared setup for every GO services call made from this file. Identical CA handling and
	// timeouts whether the caller is the browser's background fetch, the observer asking for a
	// watch ticket, or the streamer registering a stream — one place to get this right.
	//
	// Returns the header list, which the caller owns and must curl_slist_free_all().
	curl_slist* liveServicesConfigureCurl(CURL* easy, std::string* outBody, const std::string& authToken)
	{
		curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, liveRelayWriteCb);
		curl_easy_setopt(easy, CURLOPT_WRITEDATA, outBody);
		curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
		// Keep this short: these run behind a menu the user is looking at, or in the moment a
		// match starts, and a hung service must not leave either of them hanging.
		curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);

		// Same CA handling as connectToRelay — this libcurl is OpenSSL-backed and has
		// no trust anchors of its own, so https:// fails without an explicit bundle.
		std::ifstream certFile("cacert.pem");
		if (certFile.good())
		{
			certFile.close();
			curl_easy_setopt(easy, CURLOPT_CAINFO, "cacert.pem");
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
		}
		else
		{
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
		}

		curl_slist* headers = curl_slist_append(nullptr, "Accept: application/json");
		if (!authToken.empty())
		{
			const std::string authHeader = "Authorization: Bearer " + authToken;
			headers = curl_slist_append(headers, authHeader.c_str());
		}
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
		return headers;
	}

	void liveRelayFetchThread(std::string url, std::string authToken)
	{
		std::string body;
		bool success = false;
		long status = 0;

		CURL* easy = curl_easy_init();
		if (easy)
		{
			curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
			curl_slist* headers = liveServicesConfigureCurl(easy, &body, authToken);

			CURLcode res = curl_easy_perform(easy);
			curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
			success = (res == CURLE_OK);
			if (!success)
				liveObserverLog("liveRelayFetch: curl failed (result=%d) for %s\n", (int)res, url.c_str());
			curl_slist_free_all(headers);
			curl_easy_cleanup(easy);
		}

		{
			std::lock_guard<std::mutex> lock(s_fetchMutex);
			s_fetchBody = body;
			s_fetchSuccess = success;
			s_fetchStatus = status;
		}
		s_fetchReady.store(true);
		s_fetchInFlight.store(false);
	}
}

std::string liveServicesAuthToken()
{
	NGMP_OnlineServices_AuthInterface* pAuthInterface =
		NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	if (pAuthInterface == nullptr || !pAuthInterface->IsLoggedIn())
		return std::string();

	return pAuthInterface->GetAuthToken();
}

AsciiString liveServicesEndpoint(const char* szEndpoint)
{
	// Static on the manager, so this resolves whether or not the player has signed in.
	return AsciiString(NGMP_OnlineServicesManager::GetAPIEndpoint(szEndpoint).c_str());
}

Bool liveServicesParseLivestreams(const AsciiString& body, std::vector<LiveGameEntry>& outGames)
{
	outGames.clear();

	try
	{
		// GO answers with { "livestreams": [ ... ] }, already filtered to what this player may
		// watch -- lobbies in progress whose relay session is live.
		nlohmann::json response = nlohmann::json::parse(body.str());
		if (!response.is_object() || !response.contains("livestreams"))
			return FALSE;

		const nlohmann::json& games = response["livestreams"];
		if (!games.is_array())
			return FALSE;

		for (const auto& game : games)
		{
			if (!game.is_object())
				continue;

			LiveGameEntry entry;

			// lobby_id is a number in GO's JSON, and the relay keys its sessions by the same
			// value as decimal text -- so it is formatted, not read as a string.
			if (game.contains("lobby_id") && game["lobby_id"].is_number_integer())
				entry.lobbyId.format("%lld", (long long)game["lobby_id"].get<long long>());
			if (entry.lobbyId.isEmpty())
				continue;

			// map_name is already a display name, not the raw path the old relay field carried,
			// so it needs no leaf/extension stripping. A game missing metadata is still watchable,
			// so fall back rather than dropping the row.
			const std::string mapName = game.value("map_name", std::string(""));
			entry.mapName = mapName.empty() ? "(unknown map)" : mapName.c_str();

			// The lobby's display name — used for the password popup title on passworded rows.
			const std::string lobbyName = game.value("name", std::string(""));
			entry.name = lobbyName.empty() ? entry.mapName : lobbyName.c_str();

			// players[] arrives already reduced to the humans in the lobby, so unlike the relay's
			// old members[] there are no empty slots to filter out.
			std::string playerList;
			if (game.contains("players") && game["players"].is_array())
			{
				for (const auto& player : game["players"])
				{
					if (!player.is_string())
						continue;

					const std::string name = player.get<std::string>();
					if (name.empty())
						continue;

					if (!playerList.empty())
						playerList += ", ";
					playerList += name;
				}
			}
			entry.players = playerList.empty() ? "?" : playerList.c_str();

			// delay_seconds and age_seconds are nullable in GO's contract, so present-but-null has
			// to be treated as absent -- value() would throw on it.
			entry.observerCount = game.value("observer_count", 0);
			entry.delaySeconds = (game.contains("delay_seconds") && game["delay_seconds"].is_number_integer())
				? game["delay_seconds"].get<Int>() : (Int)LIVE_DELAY_SECONDS_DEFAULT;
			entry.ageSeconds = (game.contains("age_seconds") && game["age_seconds"].is_number_integer())
				? game["age_seconds"].get<Int>() : 0;

			// state/passworded/pending_observer_count are new in the expanded /Livestreams list
			// (pre-game lobbies included). Defaults keep an old GO's live-only rows behaving as
			// before: live, not passworded, no waiting count.
			entry.state = (game.contains("state") && game["state"].is_number_integer())
				? game["state"].get<Int>() : 1;
			entry.passworded = game.value("passworded", false) ? TRUE : FALSE;
			entry.pendingObserverCount = game.value("pending_observer_count", 0);

			// watch_action is GO's per-viewer directive (0 observe / 1 wait / 2 join). Absent
			// on older GO, fall back to the old state-derived rule: live rows join, pre-game
			// rows observe.
			entry.watchAction = (game.contains("watch_action") && game["watch_action"].is_number_integer())
				? game["watch_action"].get<Int>() : (entry.state == 1 ? 2 : 0);
			entry.delayRemainingSeconds = (game.contains("delay_remaining_seconds") &&
				game["delay_remaining_seconds"].is_number_integer())
				? game["delay_remaining_seconds"].get<Int>() : 0;

			// priority: GO latches the lobby when a user_priority = Player creates/joins.
			// Absent on older GO = not priority.
			entry.priority = game.value("priority", false) ? TRUE : FALSE;

			outGames.push_back(entry);
		}
	}
	catch (const nlohmann::json::exception&)
	{
		outGames.clear();
		return FALSE;
	}

	return TRUE;
}

Bool liveServicesRequest(const AsciiString& url, Bool bPost, const char* szPostBody,
	AsciiString& outBody, Int& outStatusCode)
{
	outBody = AsciiString::TheEmptyString;
	outStatusCode = 0;

	const std::string authToken = liveServicesAuthToken();
	if (authToken.empty())
	{
		liveObserverLog("liveServicesRequest: %s refused (not signed in)\n", url.str());
		return FALSE;
	}

	CURL* easy = curl_easy_init();
	if (easy == nullptr)
	{
		liveObserverLog("liveServicesRequest: %s failed (curl init)\n", url.str());
		return FALSE;
	}

	std::string body;
	curl_easy_setopt(easy, CURLOPT_URL, url.str());
	curl_slist* headers = liveServicesConfigureCurl(easy, &body, authToken);

	if (bPost)
	{
		// GO reads the body itself rather than through a model binder, so an empty POST still
		// needs a real (zero-length) body rather than no body at all.
		const char* szBody = (szPostBody != nullptr) ? szPostBody : "";
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(easy, CURLOPT_POST, 1L);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDS, szBody);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(szBody));
	}

	const CURLcode res = curl_easy_perform(easy);
	long status = 0;
	curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(easy);

	outBody = body.c_str();
	outStatusCode = (Int)status;

	if (res != CURLE_OK)
	{
		liveObserverLog("liveServicesRequest: %s failed (result=%d)\n", url.str(), (int)res);
		return FALSE;
	}

	return TRUE;
}

Bool liveRelayBeginFetch(const AsciiString& url)
{
	bool expected = false;
	if (!s_fetchInFlight.compare_exchange_strong(expected, true))
		return FALSE;	// one already running

	// Read the token here, on the calling thread: the auth interface is not safe to reach from
	// the fetch thread, and it is a plain string by the time it crosses over.
	const std::string authToken = liveServicesAuthToken();
	if (authToken.empty())
	{
		// GO gates the livestream list behind a GameClient session, so there is nothing to ask
		// for when signed out. Release the in-flight flag rather than leaving the browser
		// believing a request is running.
		s_fetchInFlight.store(false);
		liveObserverLog("liveRelayFetch: skipped %s (not signed in)\n", url.str());
		return FALSE;
	}

	s_fetchReady.store(false);
	liveObserverLog("liveRelayFetch: GET %s\n", url.str());

	std::thread(liveRelayFetchThread, std::string(url.str()), authToken).detach();
	return TRUE;
}

Bool liveRelayPollFetch(AsciiString& outBody, Bool& outSuccess, Int& outStatusCode)
{
	if (!s_fetchReady.load())
		return FALSE;

	std::lock_guard<std::mutex> lock(s_fetchMutex);
	// Re-check under the lock so two callers in one frame cannot both consume the result.
	if (!s_fetchReady.load())
		return FALSE;

	outBody = s_fetchBody.c_str();
	outSuccess = s_fetchSuccess ? TRUE : FALSE;
	outStatusCode = (Int)s_fetchStatus;
	s_fetchReady.store(false);
	return TRUE;
}

Bool liveRelayFetchInFlight()
{
	return s_fetchInFlight.load() ? TRUE : FALSE;
}

// ============================================================================
// Replay-record scanner
// ============================================================================
//
// TheSuperHackers @feature One replay record on disk is laid out as:
//   [UnsignedInt frame][GameMessage::Type type][Int playerIndex][UnsignedByte numTypes]
//   { [UnsignedByte argType][UnsignedByte numArgs] } x numTypes
//   [argument payload]
//
// This has to agree byte-for-byte with RecorderClass::appendNextCommand(), which consumes those
// records during playback. It lived in Recorder.cpp for exactly that reason, but the only caller
// is advanceParseCursor() below, on this file's network thread — so keeping it there put a
// hundred lines of observer parsing in a class that never calls it. The coupling is a comment,
// not a location: if appendNextCommand()/readArgument() ever change what they read, change this
// with them.
//
// It fails closed. An unparseable record stalls the watermark rather than poisoning it — the old
// probeLiveEdge() instead treated an unrecognised argument type as zero-width, which desynced the
// parse and made it report float payload bytes as frame numbers.

enum ScanRecordResult CPP_11(: Int)
{
    SCANRECORD_OK,              ///< a complete record is present; outSize/outFrame are valid
    SCANRECORD_INCOMPLETE,      ///< the buffer holds a valid prefix — more bytes needed
    SCANRECORD_CORRUPT          ///< unparseable (e.g. unknown argument type)
};

// Size of one replay argument on disk. Must match RecorderClass::readArgument() exactly — that
// function is the authority on what gets read for each type. Returns -1 for anything
// unrecognised so callers can fail closed instead of skipping zero bytes and desyncing the rest
// of the parse.
static Int replayArgumentSize(UnsignedByte argType)
{
    switch ((GameMessageArgumentDataType)argType) {
        case ARGUMENTDATATYPE_INTEGER:      return sizeof(Int);
        case ARGUMENTDATATYPE_REAL:         return sizeof(Real);
        case ARGUMENTDATATYPE_BOOLEAN:      return sizeof(Bool);
        case ARGUMENTDATATYPE_OBJECTID:     return sizeof(ObjectID);
        case ARGUMENTDATATYPE_DRAWABLEID:   return sizeof(DrawableID);
        case ARGUMENTDATATYPE_TEAMID:       return sizeof(UnsignedInt);
        case ARGUMENTDATATYPE_LOCATION:     return sizeof(Coord3D);
        case ARGUMENTDATATYPE_PIXEL:        return sizeof(ICoord2D);
        case ARGUMENTDATATYPE_PIXELREGION:  return sizeof(IRegion2D);
        case ARGUMENTDATATYPE_TIMESTAMP:    return sizeof(UnsignedInt);
        case ARGUMENTDATATYPE_WIDECHAR:     return sizeof(WideChar);
        default:                            return -1;
    }
}

/// Scan one replay record from buf[0..len). Never reads past len. outSize/outFrame may be null.
static ScanRecordResult scanReplayRecord(const unsigned char* buf, Int len, Int* outSize, UnsignedInt* outFrame)
{
    // numTypes and numArgs are single bytes, so a well-formed record cannot exceed
    // 9 + 255*2 + 255*255*sizeof(IRegion2D) bytes. Anything claiming more than this is
    // misparsed data rather than a record we are merely waiting on — say so, instead of
    // stalling forever waiting for bytes that will never make it complete.
    const Int MAX_SANE_RECORD_SIZE = 2 * 1024 * 1024;

    const Int fixedSize = sizeof(UnsignedInt) + sizeof(GameMessage::Type) + sizeof(Int) + sizeof(UnsignedByte);
    if (len < fixedSize)
        return SCANRECORD_INCOMPLETE;

    UnsignedInt frame;
    memcpy(&frame, buf, sizeof(frame));

    Int pos = sizeof(UnsignedInt) + sizeof(GameMessage::Type) + sizeof(Int);
    UnsignedByte numTypes = buf[pos];
    pos += sizeof(UnsignedByte);

    // All (argType, numArgs) pairs are written consecutively, and only then the argument
    // payload for every type in order — see appendNextCommand(), which reads the full pair
    // list into the parser before its readArgument() loop. Accumulate the payload size and
    // add it once, after the pair list. (The old probeLiveEdge() skipped each type's payload
    // inside this loop instead, which is correct only when numTypes == 1 and desynced the
    // parse for every message carrying two or more argument types.)
    Int payloadSize = 0;
    for (UnsignedByte i = 0; i < numTypes; ++i) {
        if (pos + 2 > len)
            return SCANRECORD_INCOMPLETE;

        UnsignedByte argType = buf[pos];
        UnsignedByte numArgs = buf[pos + 1];
        pos += 2;

        Int argSize = replayArgumentSize(argType);
        if (argSize < 0)
            return SCANRECORD_CORRUPT;

        payloadSize += argSize * (Int)numArgs;
        if (payloadSize > MAX_SANE_RECORD_SIZE)
            return SCANRECORD_CORRUPT;
    }

    pos += payloadSize;
    if (pos > len)
        return SCANRECORD_INCOMPLETE;

    if (outSize)
        *outSize = pos;
    if (outFrame)
        *outFrame = frame;
    return SCANRECORD_OK;
}

// ============================================================================
// Parse cursor — publishes the live-edge and safe-read watermarks
// ============================================================================

void LiveObserver::resetParseCursor(Int bodyStartOffset)
{
    m_parseTail.clear();
    m_parseAbsOffset = bodyStartOffset;
    m_bodyStartOffset = bodyStartOffset;
    m_parseCorrupt = false;
    m_maxCompleteFrame.store(0);
    m_safeReadOffset.store(bodyStartOffset);
}

void LiveObserver::advanceParseCursor(Int chunkOffset, const unsigned char* data, size_t dataLen)
{
    if (m_parseCorrupt || dataLen == 0)
        return;

    // The relay appends body data strictly in order (see server.py apply_body, which only
    // accepts offset == body_len), so chunks normally arrive contiguously. The exception is
    // the observer-join race documented in plans/live-observer-900-frame-broadcast-delay.md
    // §8: a live chunk can be broadcast to a freshly-registered observer before catch-up has
    // sent the earlier chunks, leaving a hole in the file. Parsing across a hole would feed
    // uninitialised bytes to the scanner, so stall the watermark instead. The cursor resumes
    // by itself once the missing bytes are backfilled and a contiguous chunk arrives.
    Int expected = m_parseAbsOffset + (Int)m_parseTail.size();
    if (chunkOffset != expected)
    {
        liveObserverLog("LiveObserver: parse cursor gap — chunkOffset=%d expected=%d, watermark stalled\n",
            chunkOffset, expected);
        return;
    }

    m_parseTail.insert(m_parseTail.end(), data, data + dataLen);

    Int consumed = 0;
    UnsignedInt maxFrame = m_maxCompleteFrame.load();
    const Int tailSize = (Int)m_parseTail.size();

    while (consumed < tailSize)
    {
        Int recSize = 0;
        UnsignedInt recFrame = 0;
        ScanRecordResult r = scanReplayRecord(&m_parseTail[consumed], tailSize - consumed, &recSize, &recFrame);

        if (r == SCANRECORD_INCOMPLETE)
            break;

        if (r == SCANRECORD_CORRUPT)
        {
            // Fail closed. Freezing the watermark stops playback advancing into data we
            // cannot trust, which is recoverable and diagnosable; the old behaviour skipped
            // zero bytes and reported garbage frame numbers indefinitely.
            m_parseCorrupt = true;
            liveObserverLog("LiveObserver: parse cursor CORRUPT at abs offset %d — watermark frozen at frame %u\n",
                m_parseAbsOffset + consumed, maxFrame);
            break;
        }

        consumed += recSize;
        if (recFrame > maxFrame)
            maxFrame = recFrame;
    }

    if (consumed > 0)
    {
        m_parseTail.erase(m_parseTail.begin(), m_parseTail.begin() + consumed);
        m_parseAbsOffset += consumed;
        // Publish the frame before the offset: a reader that sees the new safe offset must
        // never see a stale live edge for the records it is now allowed to read.
        m_maxCompleteFrame.store(maxFrame);
        m_safeReadOffset.store(m_parseAbsOffset);
    }
}

// ============================================================================
// Buffering gate
// ============================================================================
//
// The observer never plays closer to the live game than the broadcast delay. Holding it
// there is the whole feature, and the decision is made here because every input to it —
// the delay, the live edge, whether the initial buffer has been built — belongs to this
// session. The Recorder only carries out what this decides.

void LiveObserver::updatePlaybackGate(UnsignedInt curFrame)
{
    // Never hold the game before it has actually started. The map load and object creation
    // run inside GameLogic::update() (see the m_startNewGame branch there), and the pause
    // stops update() from being called at all — so pausing this early means the game never
    // starts, while TheGameClient keeps updating above the halt. That mismatch is what made
    // a pre-roll join glitch, whereas joining after the buffer was already full was fine.
    // Frames barely advance before the start completes, so nothing is lost by waiting.
    // The warmup extends the same reasoning past the start itself: the scene is not composed
    // until logic has run for a few ticks, and a hold at frame 1 renders nothing at all —
    // not the map, and not the buffering countdown that is supposed to explain the wait.
    if (TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame()
        || TheGameLogic->isStartingNewGame()
        || curFrame < (UnsignedInt)LIVE_PREROLL_WARMUP_FRAMES)
    {
        if (TheGameLogic != nullptr && m_autoPaused && TheGameLogic->isGamePaused())
        {
            TheGameLogic->setGamePaused(FALSE, FALSE, FALSE);
            m_autoPaused = FALSE;
        }
        // The gate is not being evaluated, so it must not keep reporting a hold from the
        // last tick it was.
        m_holdPlayback = FALSE;
        m_nearLiveHeld = FALSE;
        return;
    }

    const UnsignedInt liveEdge = getMaxCompleteFrame();
    const UnsignedInt gap = (liveEdge > curFrame) ? (liveEdge - curFrame) : 0;
    const UnsignedInt delayFrames = getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND;
    const Bool streamEnded = m_streamEnded.load();

    // The near-live gate keeps the observer a full broadcast delay behind the live edge. It
    // is latched with hysteresis: holding engages only once the gap has fallen a whole band
    // below the delay boundary, and releases only once the source has pulled a whole band
    // ahead again. Between the bounds the gate keeps its previous decision instead of
    // re-evaluating every tick — with both sides at the same frame rate the gap sits exactly
    // on the boundary, and a plain threshold there toggled pause/resume constantly, which
    // is the stutter every second the observer reported.
    const UnsignedInt holdBand = LIVE_GATE_HYSTERESIS_FRAMES;
    const UnsignedInt engageBelow = (delayFrames > holdBand) ? (delayFrames - holdBand) : 0;
    const UnsignedInt releaseAbove = delayFrames + holdBand;

    // Existing fast-forward auto-disable, now driven by a live edge that is actually real.
    // Uses the release bound: inside the hysteresis band the observer is close enough to the
    // edge that a fast-forward would spoil the live game, so it must stay disabled there too.
    if (gap <= releaseAbove)
    {
        if (TheWritableGlobalData)
            TheWritableGlobalData->m_TiVOFastMode = FALSE;
    }

    // Pre-roll: hold playback until the initial buffer has been built once. Sticky for the
    // rest of the session — after this the delay is maintained by the near-live gate below.
    // A finished stream is the escape hatch for a game that ends before ever buffering a
    // full delay; without it a short game would pre-roll-pause forever.
    if (!m_preRollComplete && (gap >= delayFrames || streamEnded))
        m_preRollComplete = TRUE;

    // The gate is purely a function of the gap — deliberately not of whether a record
    // happened to be readable this tick. Holding whenever we are inside the delay window IS
    // the broadcast delay; an "did we hit EOF" term would also make the two callers of this
    // function disagree (the poll runs before GameLogic::UPDATE() and cannot know), which
    // oscillated the pause every tick and let playback creep forward while starved.
    //
    // The latched hold replaces what used to be a tight sawtooth around the boundary: paused
    // at gap == delayFrames, released the moment the source pulled ahead. At equal frame
    // rates that boundary is crossed constantly, so the observer now settles on whichever
    // side of the hysteresis band it reaches and the gate only moves when the two sides'
    // rates genuinely diverge.
    if (m_preRollComplete && !streamEnded)
    {
        if (gap <= engageBelow)
            m_nearLiveHeld = TRUE;
        else if (gap > releaseAbove)
            m_nearLiveHeld = FALSE;
    }
    else
    {
        m_nearLiveHeld = FALSE;
    }

    const Bool preRollGate = !m_preRollComplete;
    m_holdPlayback = (preRollGate || m_nearLiveHeld) && !streamEnded;

    // Distinguish normal delay-holding from a genuine stall for the status bar's benefit.
    // At the boundary the hold toggles constantly, which is healthy; what the observer
    // actually wants flagged is the source having stopped producing data altogether.
    const UnsignedInt nowMs = timeGetTime();
    if (liveEdge != m_lastSeenLiveEdge)
    {
        m_lastSeenLiveEdge = liveEdge;
        m_lastLiveEdgeChangeMs = nowMs;
    }
    m_stalled = m_holdPlayback && !streamEnded && (nowMs - m_lastLiveEdgeChangeMs) > LIVE_STALL_THRESHOLD_MS;

    // The user's intent and ours are independent inputs to one decision, so a manual pause
    // can never be silently undone by buffering, nor vice versa.
    const Bool shouldBePaused = m_userPaused || m_holdPlayback;
    if (shouldBePaused != TheGameLogic->isGamePaused())
    {
        TheGameLogic->setGamePaused(shouldBePaused, FALSE, FALSE);
        m_autoPaused = shouldBePaused && !m_userPaused;
    }
}

void LiveObserver::noteDesync(UnsignedInt frame)
{
    if (m_desyncFrame != 0)
        return;

    m_desyncFrame = frame;

    // Logged with the gate's state, because the interesting question about a divergence is
    // always whether we had run out of data when it happened.
    liveObserverLog("DESYNC: observer diverged from the stream at frame %u. curFrame=%u liveEdge=%u "
        "delayFrames=%u holdPlayback=%d stalled=%d preRoll=%d\n",
        frame, TheGameLogic ? TheGameLogic->getFrame() : 0, getMaxCompleteFrame(), getDelayFrames(),
        m_holdPlayback ? 1 : 0, m_stalled ? 1 : 0, m_preRollComplete ? 1 : 0);
}

Int LiveObserver::getPreRollSecondsRemaining() const
{
    if (m_preRollComplete)
        return 0;

    const UnsignedInt curFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;
    const UnsignedInt liveEdge = getMaxCompleteFrame();
    const UnsignedInt gap = (liveEdge > curFrame) ? (liveEdge - curFrame) : 0;
    const UnsignedInt delayFrames = getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND;
    if (gap >= delayFrames)
        return 0;

    // Round up, so the countdown only reads 0 when playback is genuinely about to start.
    return (Int)((delayFrames - gap + LOGICFRAMES_PER_SECOND - 1) / LOGICFRAMES_PER_SECOND);
}

Bool LiveObserver::isWithinBroadcastDelay(UnsignedInt curFrame) const
{
    const UnsignedInt liveEdge = getMaxCompleteFrame();
    const UnsignedInt gap = (liveEdge > curFrame) ? (liveEdge - curFrame) : 0;
    // The release bound of the gate's hysteresis band: the observer may sit a full band
    // past the delay while playing, and fast-forward must stay refused for all of it.
    return gap <= getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND + LIVE_GATE_HYSTERESIS_FRAMES;
}

Bool LiveObserver::isPlaybackReady() const
{
    if (!m_headerReceived.load())
        return false;
    if (m_streamEnded.load())
        return true;

    // Playback may only start once the file is safe to read: the header plus at least the
    // first body record, and enough complete records to cover the whole broadcast delay.
    // The delay boundary proves the buffer is built because records arrive in order — and
    // it also guarantees the first record is present, which the Recorder's seeding read
    // depends on. A zero delay still needs that first record, hence the offset check.
    if (m_safeReadOffset.load() <= m_bodyStartOffset)
        return false;
    return getMaxCompleteFrame() >= getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND;
}

Int LiveObserver::getBroadcastDelayRemainingSeconds() const
{
    if (!m_delayWaitActive.load())
        return 0;

    const UnsignedInt nowMs = timeGetTime();
    const UnsignedInt deadline = m_delayWaitDeadlineMs.load();
    if (deadline <= nowMs)
        return 0;

    // Round up, so the countdown only reads 0 when the hold is genuinely over.
    return (Int)((deadline - nowMs + 999) / 1000);
}

Int LiveObserver::getSecondsUntilPlaybackReady() const
{
    // The GO admission hold replaces the pre-roll wait: while the ticket itself is held
    // behind the broadcast delay there is no file yet, so the countdown must come from the
    // hold deadline, not from the buffer.
    if (isWaitingForBroadcastDelay())
        return getBroadcastDelayRemainingSeconds();

    if (isPlaybackReady())
        return 0;

    // Before the ticket/ROLE arrive there is no authoritative delay yet: use the expected
    // lobby delay (pre-seeded at connect), which is what the countdown should show while
    // GO is still holding. Once connected, the relay/GO values apply.
    const UnsignedInt delaySeconds = m_connected.load() ? getEffectiveDelaySeconds() : getExpectedDelaySeconds();
    const UnsignedInt delayFrames = delaySeconds * LOGICFRAMES_PER_SECOND;
    const UnsignedInt edge = getMaxCompleteFrame();
    const UnsignedInt remaining = (delayFrames > edge) ? (delayFrames - edge) : 0;
    // Round up so the countdown only reads 0 when playback can genuinely start.
    return (Int)((remaining + LOGICFRAMES_PER_SECOND - 1) / LOGICFRAMES_PER_SECOND);
}

UnsignedInt LiveObserver::getJoinTimeoutMs() const
{
    // While GO holds the ticket behind the broadcast delay the wait can be minutes long
    // (the host's delay, up to 600 s). The timeout must cover the remaining hold plus
    // headroom, or the join pump would abandon a perfectly healthy wait.
    if (m_delayWaitActive.load())
    {
        return getBroadcastDelayRemainingSeconds() * 1000 + 60000;
    }

    // A server-held stream never needs the client's pre-roll buffer (effective delay 0),
    // so the whole wait is connection + first record — headroom only.
    if (m_serverHeld.load())
    {
        return 60000;
    }

    // Before the ticket/ROLE arrive, time out on the expected lobby delay (pre-seeded at
    // connect) rather than the ROLE default: the pre-live phase can legitimately last the
    // whole delay once the join is queued at game start.
    const UnsignedInt delaySeconds = m_connected.load() ? getDelaySeconds() : getExpectedDelaySeconds();

    // Ceiling for how long the join may wait before giving up — nothing expects to reach it.
    // A game already past the broadcast delay is playable the moment its catch-up arrives,
    // because isPlaybackReady() compares the live edge against the delay; it does not sit out
    // the delay itself. Only a freshly-started game approaches this ceiling, and there the
    // wait really is the delay: the stream must produce a full delay's worth of records before
    // playback may begin. The delay in real seconds plus headroom for the connection, ticket
    // minting and the first record bounds that worst case. 60s of headroom also covers the
    // lobby-observer flow, where the ticket retry can wait out the stream going live.
    return delaySeconds * 1000 + 60000;
}

UnsignedInt LiveObserver::getJoinDeadlineMs() const
{
    // While GO holds the ticket behind the broadcast delay the wait is the hold itself, so
    // the deadline is the (absolute) hold end plus headroom. The hold deadline is refreshed
    // forward on every 423, so it never moves backward while the hold is running — an
    // elapsed-since-connect budget would, which is how the pump used to abandon a healthy
    // hold just as it was about to end (the log read "timed out" with the hold still active).
    if (m_delayWaitActive.load())
    {
        return m_delayWaitDeadlineMs.load() + 60000;
    }

    // Non-held: join start plus the ordinary timeout budget, measured once against the
    // absolute baseline set at connect.
    return m_joinStartedAtMs + getJoinTimeoutMs();
}

LiveObserver::~LiveObserver()
{
    // Anything this session paused dies with it. The pause is global game state, so unlike
    // every other field here it does not simply disappear when the object does — and a
    // session left holding it would hand the next one a game that is already halted.
    if (m_autoPaused && TheGameLogic != nullptr && TheGameLogic->isGamePaused())
        TheGameLogic->setGamePaused(FALSE, FALSE, FALSE);

    close();
}

LiveObserver* createLiveObserver()
{
    return new LiveObserver();
}

void liveObserverEndSession(void)
{
    liveObserverLog("liveObserverEndSession: observer=%s\n", TheLiveObserver ? "destroying" : "(none)");

    if (TheLiveObserver)
    {
        TheLiveObserver->close();
        delete TheLiveObserver;
        TheLiveObserver = nullptr;
    }

    // Closes the playback file and parks the playback cursor, but deliberately does not
    // reset() the Recorder: the score screen runs immediately after the session ends and
    // consults TheRecorder->isMultiplayer() to pick between the multiplayer and the
    // single-player layout — the single-player one overrides the player names with "player".
    // Keeping LIVE_OBSERVER mode and the header's game-info slots makes that call report the
    // streamer's game truthfully, exactly like a normal replay that floats in PLAYBACK mode
    // until the next game starts. Safe to call when there was no session: the teardown is a
    // no-op on a recorder that never opened a file.
    if (TheRecorder)
        TheRecorder->endLivePlayback();

    // Every way a session ends returns the player to the shell (stream end, exit game, join
    // aborted), and the shell map is the shell's backdrop. This restore used to live only in
    // stopPlayback()'s live branch, which is why quitting via the in-game exit button left a
    // mapless shell — the clearGameData() path ended the session without touching the flag.
    // Done here, all end-session call sites behave alike. A session that is still starting
    // (playbackFile() clearing the shell map) never reaches this function: the startup guard
    // in liveObserverOnGameCleared() returns before calling it.
    if (TheWritableGlobalData)
        TheWritableGlobalData->m_shellMapOn = TRUE;
}

void liveObserverOnGameCleared(void)
{
    if (TheLiveObserver == nullptr)
        return;

    // A session that has not started playing is a session still being set up, and the thing
    // clearing game data right now is that very setup: playbackFile() unloads the shell map
    // before it reads the header. Ending the session here would destroy the observer that just
    // finished connecting - see the declaration in LiveObserver.h.
    if (!TheLiveObserver->hasPlaybackStarted())
    {
        liveObserverLog("liveObserverOnGameCleared: session is still starting, keeping it\n");
        return;
    }

    liveObserverEndSession();
}

// ============================================================================
// Network setup
// ============================================================================

bool LiveObserver::fetchWatchTicket(AsciiString& outConnectUrl)
{
    // GO owns admission to a livestream: it checks the session, confirms the lobby really is
    // being streamed, and asks the relay for a single-use ticket on the player's behalf. What
    // comes back is a complete connect URL, so nothing here needs to know the relay's address
    // -- which is why nothing here derives an origin from a caller-supplied URL.
    AsciiString url;
    url.format("%s/observe/%s", liveServicesEndpoint("Livestreams").str(), m_gameId.str());

    AsciiString body;
    Int statusCode = 0;

    // The POST body carries the lobby password when one was supplied; unpassworded streams
    // keep the empty body. Built once: nlohmann's dump() escapes it, so a quote in a
    // password cannot break the JSON.
    std::string postBody;
    if (!m_password.empty())
    {
        nlohmann::json pwPayload;
        pwPayload["password"] = m_password;
        postBody = pwPayload.dump();
    }

    // The observer can arrive just as the stream goes live (the pre-game lobby view hands off
    // on the stream-live push, but there is a race: the relay may not hold the header yet, or
    // GO may not have processed its liveness report). GO answers 404 for that window. Retry on
    // a short cadence for a bounded time instead of aborting — a normal Watch Live join is
    // unaffected (the ticket is minted in the first request), and the retry loop aborts as
    // soon as the session is cancelled, so LEAVE does not hang behind it.
    //
    // 401 is different and must NOT retry: it means the stream is password-protected and the
    // supplied password was missing or wrong. Latched and returned immediately so the join
    // pump can reprompt.
    //
    // 423 is the broadcast-delay admission hold (plans/live-observer-server-delay.md): GO
    // will not mint the ticket until the stream has been live for the host's delay. The
    // retry deadline is re-armed past the end of the hold on every 423, and the client
    // sleeps to the end of the hold instead of polling: GO re-computes the remaining hold
    // on every request, so a single retry right after the deadline mints the ticket. Long
    // holds sleep in 30s steps, which keeps the 404 "stream ended" case detectable within
    // half a minute.
    const int64_t kTicketRetryWindowMs = 40000;
    const auto retryStart = std::chrono::steady_clock::now();
    auto retryDeadline = retryStart + std::chrono::milliseconds(kTicketRetryWindowMs);
    for (;;)
    {
        if (!m_shouldRun.load())
        {
            liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s cancelled\n",
                m_gameId.str());
            return false;
        }

        if (!liveServicesRequest(url, TRUE, postBody.c_str(), body, statusCode))
        {
            liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s failed (request not sent)\n",
                m_gameId.str());
            return false;
        }

        if (statusCode == 200)
        {
            // GO owns the broadcast delay: the ticket was only minted once the stream
            // outlived it, so the relay stream is already delayed and this client must not
            // hold playback itself. Absent field (older GO) keeps the client-side hold.
            try
            {
                nlohmann::json ticketResponse = nlohmann::json::parse(body.str());
                if (ticketResponse.is_object() && ticketResponse.contains("server_held")
                    && ticketResponse["server_held"].is_boolean())
                {
                    m_serverHeld.store(ticketResponse["server_held"].get<bool>() ? TRUE : FALSE);
                }
            }
            catch (const nlohmann::json::exception&) { }
            m_delayWaitActive.store(FALSE);
            break;
        }

        if (statusCode == 401)
        {
            // Wrong or missing password for a password-protected stream.
            m_passwordRejected.store(TRUE);
            liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s password rejected (status=%d)\n",
                m_gameId.str(), statusCode);
            return false;
        }

        if (statusCode == 423)
        {
            Int holdRemainingSeconds = 0;
            try
            {
                nlohmann::json holdResponse = nlohmann::json::parse(body.str());
                if (holdResponse.is_object() && holdResponse.contains("delay_remaining_seconds")
                    && holdResponse["delay_remaining_seconds"].is_number_integer())
                {
                    holdRemainingSeconds = holdResponse["delay_remaining_seconds"].get<Int>();
                }
            }
            catch (const nlohmann::json::exception&) { }

            if (holdRemainingSeconds > 0)
            {
                m_delayWaitDeadlineMs.store(timeGetTime() + (UnsignedInt)holdRemainingSeconds * 1000);
                m_delayWaitActive.store(TRUE);
                retryDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds((int64_t)holdRemainingSeconds * 1000 + 30000);

                // Sleep to the end of the hold (plus a small margin so the retry lands after
                // the window opens) rather than polling at 1s. Holds longer than 30s sleep in
                // 30s steps: each wake re-requests, gets the fresh remaining hold, and re-arms
                // — so a hold that ends early (stream gone, viewer granted priority mid-wait)
                // is still picked up within half a minute.
                Int64 holdSleepMs = (Int64)holdRemainingSeconds * 1000 + 500;
                if (holdSleepMs > 30000)
                {
                    holdSleepMs = 30000;
                }
                liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s held behind the "
                    "broadcast delay (%ds remaining), retrying in %lldms\n",
                    m_gameId.str(), holdRemainingSeconds, holdSleepMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(holdSleepMs));
                continue;
            }
        }

        if (std::chrono::steady_clock::now() > retryDeadline)
        {
            // 404 is the ordinary "that stream is over" answer: the game was listed a moment
            // ago, but the relay has closed it since. Anything else is a real failure.
            liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s gave up (status=%d) %s\n",
                m_gameId.str(), statusCode, body.str());
            return false;
        }

        liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s not watchable yet (status=%d), retrying\n",
            m_gameId.str(), statusCode);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    bool success = false;
    try
    {
        nlohmann::json response = nlohmann::json::parse(body.str());
        if (response.is_object() && response.contains("url") && response["url"].is_string())
        {
            const std::string ticketUrl = response["url"].get<std::string>();
            if (!ticketUrl.empty())
            {
                outConnectUrl = ticketUrl.c_str();
                success = true;
            }
        }
    }
    catch (const nlohmann::json::exception&)
    {
    }

    liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s %s (status=%d)\n",
        m_gameId.str(), success ? "succeeded" : "failed", statusCode);
    return success;
}

void LiveObserver::connect(const AsciiString& lobbyId, const std::string& password,
    Int expectedDelaySeconds)
{
    m_shouldRun.store(true);
    m_password = password;
    m_passwordRejected.store(FALSE);
    m_expectedDelaySeconds = expectedDelaySeconds;
    m_joinStartedAtMs = timeGetTime();

    // The lobby id is all this needs. Where to connect is not knowable here and never was
    // ours to assemble: admission runs through GO, which mints a single-use ticket for this
    // player and answers with the complete relay URL (see fetchWatchTicket). This used to be
    // handed a fabricated wss://<relay>/watch/<id>, purely so the id could be parsed back out
    // of it -- and that URL could not be connected to, because an unticketed /watch is
    // refused by the relay.
    m_gameId = lobbyId.isEmpty() ? AsciiString("unknown") : lobbyId;
    if (lobbyId.isEmpty())
    {
        // No id means no session to ask GO about. Keep the filename unique to this instance
        // anyway: a shared "_live.rep" is the worst possible name to collide on.
        m_liveFilename.format("unknown_Instance%.2u_live.rep",
            rts::ClientInstance::getInstanceId());
        liveObserverLog("LiveObserver::connect: no lobby id supplied\n");
    }
    else
    {
        m_liveFilename.format("%s_live.rep", m_gameId.str());
    }

    liveObserverLog("LiveObserver::connect game=%s (file=%s)\n",
        m_gameId.str(), m_liveFilename.str());

    m_networkThread = std::thread(&LiveObserver::networkThreadFunc, this);
}

void LiveObserver::close()
{
    m_shouldRun.store(false);

    if (m_networkThread.joinable())
        m_networkThread.join();

    if (m_liveFile)
    {
        m_liveFile->close();
        m_liveFile = nullptr;
    }

    m_connected.store(false);
    m_headerReceived.store(false);
    m_streamEnded.store(false);
}

// ============================================================================
// Live file management
// ============================================================================

bool LiveObserver::openLiveFile()
{
    AsciiString filepath = RecorderClass::getReplayDir();
    filepath.concat(m_liveFilename);

    m_liveFilePath = filepath;

    // TheSuperHackers @fix 03/08/2026 Start from a genuinely empty file.
    //
    // File::WRITE does not imply truncation here — TRUNCATE is a separate flag — so opening
    // an existing file leaves everything past the new session's data in place. That matters
    // because these files are named by game id and never cleaned up, so rejoining a game, or
    // any session after a crash, lands on the previous session's file. A shorter session then
    // inherits a longer one's tail, and reads that should hit end-of-stream instead return
    // stale bytes as replay records.
    //
    // A failed delete usually means another process still holds the file — the streamer and
    // observer are the same exe and can share an install. Two writers at absolute offsets in
    // one file corrupt each other, so refuse rather than proceed.
    if (remove(filepath.str()) == 0)
    {
        liveObserverLog("LiveObserver::openLiveFile removed leftover %s (previous session did not clean up)\n",
            filepath.str());
    }
    else if (errno != ENOENT)
    {
        liveObserverLog("LiveObserver::openLiveFile could NOT remove %s (errno=%d) — refusing to reuse it\n",
            filepath.str(), errno);
        return false;
    }

    m_liveFile = TheFileSystem->openFile(filepath.str(),
        File::WRITE | File::CREATE | File::TRUNCATE | File::BINARY);
    if (!m_liveFile)
    {
        liveObserverLog("LiveObserver::openLiveFile FAILED for %s\n", filepath.str());
        return false;
    }

    liveObserverLog("LiveObserver::openLiveFile opened %s\n", filepath.str());
    return true;
}

// ============================================================================
// Chat helpers (plans/relay/live-observer-chat.md + live-observer-spectator-chat.md)
//
// These three are MIRRORED in LiveStreamer.cpp (wideToUtf8/utf8ToWide/appendU32LE) —
// keep both copies in sync; like the build tag, they have already drifted once.
// ============================================================================

static std::string chatWideToUtf8(const UnicodeString& text)
{
    const wchar_t* src = text.str();
    const int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)       // nothing but the terminator, or a failure
        return std::string();
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, src, -1, &out[0], len, nullptr, nullptr);
    return out;
}

static UnicodeString chatUtf8ToWide(const std::string& utf8)
{
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (len <= 0)
        return UnicodeString::TheEmptyString;
    std::wstring tmp(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &tmp[0], len);
    return UnicodeString(tmp.c_str());
}

static void chatAppendU32LE(std::vector<char>& out, unsigned int value)
{
    out.push_back((char)(value & 0xFF));
    out.push_back((char)((value >> 8) & 0xFF));
    out.push_back((char)((value >> 16) & 0xFF));
    out.push_back((char)((value >> 24) & 0xFF));
}

namespace
{
    /// The signed-in user's display name, for spectator chat sends. Cached: the auth
    /// interface outlives every live-observer session.
    UnicodeString observerDisplayName()
    {
        static UnicodeString s_cached;
        if (s_cached.isEmpty())
        {
            NGMP_OnlineServices_AuthInterface* auth =
                NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
            if (auth)
                s_cached.set(auth->GetDisplayNameW().c_str());
        }
        return s_cached;
    }
}

/// Fixed spectator-chat display color (light blue) — matches the streamer-side style.
static const unsigned int SPECTATOR_CHAT_COLOR = 0x72ADF2u;

void LiveObserver::displayChat(const ChatEntry& entry)
{
    RGBColor c;
    c.setFromInt((Int)entry.colorArgb);
    TheInGameUI->messageColor(true, &c, UnicodeString(L"%ls"), entry.text.str());
}

Bool LiveObserver::isSpectatorGateOpen(UnsignedInt curFrame) const
{
    // The 5-second spoiler rule: live spectator chat is shown only while the observer is
    // within ~5s of the broadcast-delay boundary, i.e. effectively watching live. Far
    // behind (pre-roll, stalls, long pauses) the live chat is dropped — it would spoil
    // the game it describes. See plans/relay/live-observer-spectator-chat.md.
    const UnsignedInt liveEdge = m_maxCompleteFrame.load();
    if (liveEdge <= curFrame)
        return TRUE;    // at or past the live edge — as live as it gets
    const UnsignedInt gap = liveEdge - curFrame;
    return gap <= getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND + 5 * LOGICFRAMES_PER_SECOND;
}

void LiveObserver::pollChatMessages(UnsignedInt curFrame)
{
    if (!TheInGameUI)
        return;

    std::deque<ChatEntry> batch;
    {
        std::lock_guard<std::mutex> lock(m_chatMutex);
        if (m_chatQueue.empty())
            return;
        batch.swap(m_chatQueue);
    }

    const Bool interactive = TheGameLogic && TheGameLogic->isInInteractiveGame();
    const Bool gateOpen = isSpectatorGateOpen(curFrame);
    std::deque<ChatEntry> holdback;
    for (auto& entry : batch)
    {
        if (entry.spectator)
        {
            // Live meta-chat, shown per the F7 mode: auto = inside the spoiler window only,
            // forced ON = always (spoilers accepted), OFF = never. Outside the window in
            // auto mode it is dropped, never held — "if you are there during a livestream
            // you can see chat, otherwise you missed it".
            Bool showSpectator = FALSE;
            if (m_spectatorChatMode == SPECTATOR_CHAT_FORCED_ON)
                showSpectator = interactive;
            else if (m_spectatorChatMode == SPECTATOR_CHAT_AUTO)
                showSpectator = interactive && gateOpen;
            if (showSpectator)
            {
                displayChat(entry);
                liveObserverLog("LiveObserver: displayed spectator chat\n");
            }
            else
            {
                liveObserverLog("LiveObserver: DROPPED spectator chat (mode=%d interactive=%d gateOpen=%d)\n",
                    (int)m_spectatorChatMode, (int)interactive, (int)gateOpen);
            }
        }
        else if (interactive && entry.frame <= curFrame)
        {
            // Player chat is frame-gated: released exactly when the observed game
            // reaches the moment the streamer sent it — held behind the same broadcast
            // delay as the video.
            displayChat(entry);
        }
        else
        {
            holdback.push_back(entry);
        }
    }
    if (!holdback.empty())
    {
        std::lock_guard<std::mutex> lock(m_chatMutex);
        // Reinsert at the FRONT: these are the oldest entries and must drain in order.
        m_chatQueue.insert(m_chatQueue.begin(), holdback.begin(), holdback.end());
    }
}

void LiveObserver::sendSpectatorChat(const UnicodeString& text)
{
    if (!m_connected.load() || !m_shouldRun.load())
    {
        liveObserverLog("LiveObserver::sendSpectatorChat DROPPED (not connected)\n");
        return;
    }

    // [nameLen u32 LE][UTF-8 name][textLen u32 LE][UTF-8 text] — see
    // plans/relay/live-observer-spectator-chat.md.
    std::string utf8Name = chatWideToUtf8(observerDisplayName());
    std::string utf8Text = chatWideToUtf8(text);
    std::vector<char> payload;
    payload.reserve(8 + utf8Name.size() + utf8Text.size());
    chatAppendU32LE(payload, (unsigned int)utf8Name.size());
    payload.insert(payload.end(), utf8Name.begin(), utf8Name.end());
    chatAppendU32LE(payload, (unsigned int)utf8Text.size());
    payload.insert(payload.end(), utf8Text.begin(), utf8Text.end());

    std::lock_guard<std::mutex> lock(m_outboundChatMutex);
    if (m_outboundChatQueue.size() < 100)
    {
        m_outboundChatQueue.push_back(payload);
        liveObserverLog("LiveObserver::sendSpectatorChat queued %zu bytes (name=%zu, text=%zu)\n",
            payload.size(), utf8Name.size(), utf8Text.size());
    }
}

// ============================================================================
// Frame handler
// ============================================================================

void LiveObserver::handleFrame(unsigned char type, const char* payload, size_t len)
{
    switch (type)
    {
    case 1: // LIVE_MSG_HEADER
    {
        if (!openLiveFile())
            return;

        // Write header bytes
        if (len > 0)
            m_liveFile->write(payload, len);
        m_liveFile->flush();

        // Close write handle so the Recorder can open the file for reading.
        // Do NOT reopen here — the Recorder needs exclusive access to read
        // the header during playbackFile().  Reopen lazily when the first
        // BODY or PATCH frame arrives.
        m_liveFile->close();
        m_liveFile = nullptr;

        // Body records start immediately after the header, so that is where the parse
        // cursor begins. Reset here rather than only in the constructor, so a second
        // live-observer session in the same process cannot inherit a stale watermark.
        resetParseCursor((Int)len);

		m_headerReceived.store(true);
		liveObserverLog("LiveObserver: HEADER received (%zu bytes), ready for playback\n", len);
        break;
    }

    case 2: // LIVE_MSG_PATCH
    {
        if (len < 8)
            return;

        // Lazy-open the file for read/write (no truncation)
        if (!m_liveFile)
        {
            m_liveFile = TheFileSystem->openFile(m_liveFilePath.str(), File::READWRITE | File::BINARY);
        }
        if (!m_liveFile)
            return;

        // Parse offset and data length from payload
        const unsigned char* p = (const unsigned char*)payload;
        Int offset = (Int)p[0] | ((Int)p[1] << 8) | ((Int)p[2] << 16) | ((Int)p[3] << 24);
        Int dataLen = (Int)p[4] | ((Int)p[5] << 8) | ((Int)p[6] << 16) | ((Int)p[7] << 24);

        if (dataLen <= 0 || (size_t)(8 + dataLen) > len)
            return;

        Int fileSize = (Int)m_liveFile->size();
        Int seekRes = m_liveFile->seek(offset, File::seekMode::START);
        if (seekRes == offset)
        {
            m_liveFile->write(payload + 8, dataLen);
            m_liveFile->seek(fileSize, File::seekMode::START);
        }

		liveObserverLog("LiveObserver: PATCH offset=%d size=%d fileSize=%d\n", offset, dataLen, (int)m_liveFile->size());
        break;
    }

    case 3: // LIVE_MSG_BODY
    {
        // BODY payload: [8B offset uint64 LE][data]
        if (len < 8)
        {
            liveObserverLog("LiveObserver: BODY frame too short (len=%d)\n", (int)len);
            return;
        }

        const unsigned char* p = (const unsigned char*)payload;
        // Read 8-byte absolute file offset (little-endian)
        Int offset = (Int)(p[0] | ((unsigned long long)p[1] << 8)
            | ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24)
            | ((unsigned long long)p[4] << 32) | ((unsigned long long)p[5] << 40)
            | ((unsigned long long)p[6] << 48) | ((unsigned long long)p[7] << 56));
        size_t dataLen = len - 8;
        if (dataLen == 0)
        {
            liveObserverLog("LiveObserver: BODY frame with zero dataLen at offset=%d\n", offset);
            return;
        }

        // Lazy-open the file for read/write (no truncation, file exists from HEADER handler)
        if (!m_liveFile)
        {
            m_liveFile = TheFileSystem->openFile(m_liveFilePath.str(), File::READWRITE | File::BINARY);
        }
        if (!m_liveFile)
        {
            liveObserverLog("LiveObserver: BODY openFile FAILED for %s\n", m_liveFilePath.str());
            return;
        }

        // Seek to the specified offset and write the data
        m_liveFile->seek(offset, File::seekMode::START);
        m_liveFile->write(payload + 8, (Int)dataLen);
        m_liveFile->flush();

        // Scan the bytes we just committed, so the game thread's live edge and safe-read
        // limit are up to date the moment the data is readable.
        advanceParseCursor(offset, (const unsigned char*)(payload + 8), dataLen);

        // Guarded rather than relying on the empty liveObserverLog: this fires per chunk
        // (~30/s) and m_liveFile->size() is a real file-system query, which would otherwise
        // still run in a build with logging switched off.
#if defined(LIVE_OBSERVER_LOGGING)
        liveObserverLog("LiveObserver: BODY written offset=%d dataLen=%d fileSize=%d liveEdge=%u safeOffset=%d\n",
            offset, (int)dataLen, (int)m_liveFile->size(), m_maxCompleteFrame.load(), m_safeReadOffset.load());
#endif
        break;
    }

    case 5: // LIVE_MSG_ROLE — session config, sent by the relay ahead of the HEADER
    {
        // The broadcast delay originates with the streamer and reaches us here. It must be
        // applied before playback starts, because the pre-roll buffer latches against it and
        // there is no un-latching once a session is running — which is exactly why the relay
        // sends this frame before the HEADER that triggers the game start.
        std::string json(payload, len);
        liveObserverLog("LiveObserver: ROLE received: %s\n", json.c_str());

        const char* delayStart = strstr(json.c_str(), "\"delay_seconds\":");
        if (delayStart)
        {
            delayStart += 16; // skip "delay_seconds":
            Int delaySeconds = (Int)strtol(delayStart, nullptr, 10);
            if (delaySeconds >= 0 && delaySeconds <= LIVE_DELAY_SECONDS_MAX)
            {
                m_delaySeconds.store((UnsignedInt)delaySeconds);
                liveObserverLog("LiveObserver: broadcast delay set to %d seconds\n", delaySeconds);
            }
            else
            {
                liveObserverLog("LiveObserver: ignoring out-of-range delay_seconds=%d, keeping %u\n",
                    delaySeconds, m_delaySeconds.load());
            }
        }
        // No delay_seconds (older relay) simply leaves the built-in default in place.
        break;
    }

    case 4: // LIVE_MSG_END
    {
        liveObserverLog("LiveObserver: END received\n");
        m_streamEnded.store(true);

        if (m_liveFile)
        {
            m_liveFile->flush();
            m_liveFile->close();
            m_liveFile = nullptr;
        }
        break;
    }

    case 6: // LIVE_MSG_ERROR
    {
        AsciiString errMsg;
        if (len > 0)
            errMsg.set(payload, len);
        liveObserverLog("LiveObserver: ERROR from relay: %s\n", errMsg.str());
        m_streamEnded.store(true);
        break;
    }

    case 7: // LIVE_MSG_CHAT — player chat, frame-stamped by the streamer
    {
        // [frame u32 LE][textLen u32 LE][UTF-8 text][color u32 LE]
        if (len < 12)
            break;
        const unsigned char* p = (const unsigned char*)payload;
        unsigned int frame = (unsigned int)p[0]
            | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
        unsigned int textLen = (unsigned int)p[4]
            | ((unsigned int)p[5] << 8) | ((unsigned int)p[6] << 16) | ((unsigned int)p[7] << 24);
        if ((uint64_t)len < 12ull + textLen)
            break;
        unsigned int colorArgb = (unsigned int)p[8 + textLen]
            | ((unsigned int)p[9 + textLen] << 8)
            | ((unsigned int)p[10 + textLen] << 16)
            | ((unsigned int)p[11 + textLen] << 24);

        ChatEntry entry;
        entry.frame = frame;
        entry.colorArgb = colorArgb;
        entry.spectator = FALSE;
        entry.text = chatUtf8ToWide(std::string(payload + 8, textLen));
        {
            std::lock_guard<std::mutex> lock(m_chatMutex);
            if (m_chatQueue.size() < 1000)
                m_chatQueue.push_back(entry);
        }
        liveObserverLog("LiveObserver: chat frame=%u bytes=%u\n", frame, textLen);
        break;
    }

    case 8: // LIVE_MSG_SPECTATOR_CHAT — live spectator meta-chat
    {
        // [nameLen u32 LE][UTF-8 name][textLen u32 LE][UTF-8 text]
        if (len < 8)
            break;
        const unsigned char* p = (const unsigned char*)payload;
        unsigned int nameLen = (unsigned int)p[0]
            | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
        if ((uint64_t)len < 8ull + nameLen)
            break;
        const unsigned char* t = p + 4 + nameLen;
        unsigned int textLen = (unsigned int)t[0]
            | ((unsigned int)t[1] << 8) | ((unsigned int)t[2] << 16) | ((unsigned int)t[3] << 24);
        if ((uint64_t)len < 8ull + nameLen + textLen)
            break;

        ChatEntry entry;
        entry.frame = 0;
        entry.colorArgb = SPECTATOR_CHAT_COLOR;
        entry.spectator = TRUE;
        UnicodeString name = chatUtf8ToWide(std::string(payload + 4, nameLen));
        UnicodeString text = chatUtf8ToWide(std::string((const char*)(t + 4), textLen));
        entry.text.format(L"[%ls] %ls", name.str(), text.str());
        {
            std::lock_guard<std::mutex> lock(m_chatMutex);
            if (m_chatQueue.size() < 1000)
                m_chatQueue.push_back(entry);
        }
        liveObserverLog("LiveObserver: spectator chat from '%ls'\n", name.str());
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// WebSocket I/O
// ============================================================================

bool LiveObserver::wsSendBinary(const unsigned char* data, size_t len)
{
    if (!m_curlEasy)
        return false;

    size_t sent = 0;
    CURLcode rc = curl_ws_send(m_curlEasy, data, len, &sent, 0, CURLWS_BINARY);
    return (rc == CURLE_OK && sent == len);
}

bool LiveObserver::wsRecv(std::vector<char>& outBuffer)
{
    if (!m_curlEasy)
        return false;

    outBuffer.clear();
    outBuffer.resize(65536);

    const struct curl_ws_frame* meta = nullptr;
    size_t nread = 0;
    CURLcode rc = curl_ws_recv(m_curlEasy, outBuffer.data(), outBuffer.size(), &nread, &meta);
    if (rc == CURLE_AGAIN)
    {
        outBuffer.clear();
        return false;
    }
    if (rc != CURLE_OK)
    {
        liveObserverLog("LiveObserver::wsRecv error: %d\n", (int)rc);
        outBuffer.clear();
        return false;
    }

    // TheSuperHackers @fix Only binary payloads belong in the envelope reassembly buffer. meta was
    // being ignored entirely, so a PING/PONG/TEXT/CLOSE payload surfaced by libcurl would be
    // appended straight into the byte stream and misparse everything after it - and the relay's
    // server sends keepalive pings on a timer, which is exactly the kind of thing that shows up
    // minutes into an otherwise healthy session and never before.
    if (meta != nullptr && (meta->flags & CURLWS_BINARY) == 0)
    {
        liveObserverLog("LiveObserver::wsRecv: dropped a non-binary websocket frame (flags=0x%X, %d bytes)\n",
            (unsigned)meta->flags, (int)nread);
        outBuffer.clear();
        return false;
    }

    outBuffer.resize(nread);
    return nread > 0;
}

bool LiveObserver::connectToRelay()
{
    if (m_curlEasy)
    {
        curl_easy_cleanup((CURL*)m_curlEasy);
        m_curlEasy = nullptr;
    }
    if (m_curlMulti)
    {
        curl_multi_cleanup((CURLM*)m_curlMulti);
        m_curlMulti = nullptr;
    }

	// No ticket, no connection. There is deliberately no fallback: the relay refuses any
	// /watch without a valid ?ticket=, so connecting anyway would turn "GO would not admit
	// you" into a connect that opens and is then rejected -- which reads as a relay fault.
	AsciiString connectUrl;
	if (!fetchWatchTicket(connectUrl))
	{
		liveObserverLog("LiveObserver::connectToRelay game=%s aborted (no watch ticket)\n",
			m_gameId.str());
		return false;
	}

    CURL* easy = curl_easy_init();
    if (!easy)
    {
        liveObserverLog("LiveObserver::connectToRelay curl_easy_init failed\n");
        return false;
    }

    curl_easy_setopt(easy, CURLOPT_URL, connectUrl.str());
    curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);

    // TheSuperHackers @fix 03/08/2026 wss:// relays need a CA bundle. This libcurl is built
    // against OpenSSL (libssl-3.dll / libcrypto-3.dll ship beside it), and unlike Schannel
    // OpenSSL does not consult the Windows certificate store — with no trust anchors it
    // rejects every certificate, including valid ones, as CURLE_PEER_FAILED_VERIFICATION (60).
    // Same approach as HTTPRequest.cpp; see the TODO_NGMP there about moving to Schannel.
    {
        std::ifstream certFile("cacert.pem");
        if (certFile.good())
        {
            certFile.close();
            curl_easy_setopt(easy, CURLOPT_CAINFO, "cacert.pem");
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        }
        else
        {
            // Matches the existing fallback rather than failing the connection outright.
            // Worth knowing this is an unverified link, so say so in the log.
            liveObserverLog("LiveObserver: cacert.pem not found — TLS certificate verification DISABLED\n");
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }
    }

    CURLM* multi = curl_multi_init();
    if (!multi)
    {
        curl_easy_cleanup(easy);
        liveObserverLog("LiveObserver::connectToRelay curl_multi_init failed\n");
        return false;
    }

    curl_multi_add_handle(multi, easy);

    int stillRunning = 0;
    CURLMcode mc = curl_multi_perform(multi, &stillRunning);
    while (mc == CURLM_OK && stillRunning > 0)
    {
        mc = curl_multi_poll(multi, NULL, 0, 1000, NULL);
        if (mc == CURLM_OK)
            mc = curl_multi_perform(multi, &stillRunning);
    }

    if (mc != CURLM_OK)
    {
        liveObserverLog("LiveObserver::connectToRelay failed: %d\n", (int)mc);
        curl_multi_remove_handle(multi, easy);
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
        return false;
    }

    int infoRunning = 0;
    CURLMsg* infoMsg = curl_multi_info_read(multi, &infoRunning);
    if (!infoMsg || infoMsg->data.result != CURLE_OK)
    {
        int result = infoMsg ? (int)infoMsg->data.result : -1;
        liveObserverLog("LiveObserver::connectToRelay: handshake failed (result=%d)\n", result);
        curl_multi_remove_handle(multi, easy);
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
        return false;
    }

    m_curlEasy = easy;
    m_curlMulti = multi;
    m_connected.store(true);
    liveObserverLog("LiveObserver::connectToRelay connected (game=%s)\n", m_gameId.str());
    return true;
}

// ============================================================================
// Background network thread
// ============================================================================

void LiveObserver::networkThreadFunc()
{
    liveObserverLog("LiveObserver::networkThreadFunc started\n");

    if (!connectToRelay())
    {
        liveObserverLog("LiveObserver::networkThreadFunc connectToRelay failed\n");
        m_shouldRun.store(false);
        return;
    }

    m_connected.store(true);

    // Persistent buffer for incoming data — multiple frames may arrive in
    // one wsRecv call, or a frame may be split across multiple calls.
    std::vector<char> buf;
    size_t totalBytesReceived = 0;
    size_t totalFramesProcessed = 0;

    while (m_shouldRun.load() && m_connected.load())
    {
        {
            // TheSuperHackers @fix curl_multi_poll's out-param is numfds (fds with
            // activity during this poll), NOT "transfers still running" — it can be 0
            // on a timeout even though curl_multi_perform() would still have work to
            // do. curl_multi_perform() must be called unconditionally every iteration,
            // or received bytes can sit unprocessed forever and curl_ws_recv() keeps
            // returning CURLE_AGAIN even though data already arrived on the socket.
            int numfds = 0;
            CURLMcode mpoll = curl_multi_poll(m_curlMulti, NULL, 0, 50, &numfds);
            if (mpoll != CURLM_OK)
            {
                liveObserverLog("LiveObserver: curl_multi_poll failed (%d), connection lost\n", (int)mpoll);
                m_connected.store(false);
                break;
            }
            int runningHandles = 0;
            curl_multi_perform((CURLM*)m_curlMulti, &runningHandles);
        }

        // Append any newly received data to our persistent buffer
        {
            std::vector<char> tmp;
            if (wsRecv(tmp) && !tmp.empty())
            {
                totalBytesReceived += tmp.size();
                if (totalBytesReceived % 10240 < tmp.size() || totalBytesReceived < 1024)
                    liveObserverLog("LiveObserver: received %zu bytes from relay (total=%zu, queued=%zu)\n",
                        tmp.size(), totalBytesReceived, buf.size() + tmp.size());
                buf.insert(buf.end(), tmp.begin(), tmp.end());
            }
        }

        // Process as many complete frames as possible from the buffer
        while (buf.size() >= 5)
        {
            // TheSuperHackers @fix `buf` is std::vector<char>, and char is signed on MSVC.
            // Casting a byte >= 0x80 straight to unsigned int sign-extends it (e.g. 0x87 ->
            // 0xFFFFFF87) instead of zero-extending, corrupting the length for roughly half of
            // all possible values. Must cast through (unsigned char) first to zero-extend.
            unsigned char msgType = (unsigned char)buf[0];
            unsigned int msgLen = (unsigned int)(unsigned char)buf[1]
                | ((unsigned int)(unsigned char)buf[2] << 8)
                | ((unsigned int)(unsigned char)buf[3] << 16)
                | ((unsigned int)(unsigned char)buf[4] << 24);

            if ((uint64_t)buf.size() < 5ull + msgLen)
            {
                // TheSuperHackers @fix Log once per distinct incomplete-frame situation
                // (not every ~50ms poll) so we can tell a legitimately large still-arriving
                // frame apart from a corrupted/garbage length that would wait forever.
                static unsigned int s_lastWaitMsgLen = 0xFFFFFFFFu;
                static unsigned char s_lastWaitMsgType = 0xFF;
                if (msgLen != s_lastWaitMsgLen || msgType != s_lastWaitMsgType)
                {
                    liveObserverLog("LiveObserver: waiting for frame type=%d len=%u — have %zu of %llu bytes\n",
                        (int)msgType, msgLen, buf.size(), (unsigned long long)(5ull + msgLen));
                    s_lastWaitMsgLen = msgLen;
                    s_lastWaitMsgType = msgType;
                }
                break; // Partial frame — wait for more data
            }

            const char* payload = (msgLen > 0) ? buf.data() + 5 : nullptr;
            handleFrame(msgType, payload, msgLen);
            ++totalFramesProcessed;

            // Remove the processed frame from the buffer (the length was validated against
            // buf.size() in 64-bit above, so this cannot wrap)
            buf.erase(buf.begin(), buf.begin() + 5 + (size_t)msgLen);
        }

        // Send any queued spectator chat. Drained here because the curl handle is
        // network-thread-owned; chat is sparse, so one frame per loop pass is plenty.
        {
            std::vector<char> outbound;
            {
                std::lock_guard<std::mutex> lock(m_outboundChatMutex);
                if (!m_outboundChatQueue.empty())
                {
                    outbound = m_outboundChatQueue.front();
                    m_outboundChatQueue.pop_front();
                }
            }
            if (!outbound.empty())
            {
                // The relay expects the binary envelope [1B type][4B length][payload] — the
                // same framing the streamer's sendBinaryFrame applies. Sending the bare
                // payload made unpack_frame read the payload's first bytes as type/length
                // and the frame was silently dropped.
                std::vector<char> framed;
                framed.reserve(5 + outbound.size());
                framed.push_back((char)8);   // LIVE_MSG_SPECTATOR_CHAT (see LiveStreamer.h)
                chatAppendU32LE(framed, (unsigned int)outbound.size());
                framed.insert(framed.end(), outbound.begin(), outbound.end());
                const bool sent = wsSendBinary((const unsigned char*)framed.data(), framed.size());
                liveObserverLog("LiveObserver: sent spectator chat %zu bytes -> %s\n",
                    framed.size(), sent ? "OK" : "FAILED");
            }
        }
    }

    // Cleanup
    if (m_liveFile)
    {
        m_liveFile->close();
        m_liveFile = nullptr;
    }
    if (m_curlMulti)
    {
        if (m_curlEasy)
            curl_multi_remove_handle((CURLM*)m_curlMulti, (CURL*)m_curlEasy);
        curl_multi_cleanup((CURLM*)m_curlMulti);
        m_curlMulti = nullptr;
    }
    if (m_curlEasy)
    {
        curl_easy_cleanup((CURL*)m_curlEasy);
        m_curlEasy = nullptr;
    }
    m_connected.store(false);
    liveObserverLog("LiveObserver::networkThreadFunc ended — totalBytes=%zu totalFrames=%zu\n", totalBytesReceived, totalFramesProcessed);
}

#endif // GENERALS_ONLINE
