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

#pragma once

#if defined(GENERALS_ONLINE)

#include "Common/AsciiString.h"
#include "Common/GameCommon.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>

class File;

/**
 * LiveObserver receives raw replay bytes (HEADER/PATCH/BODY/END) from the
 * relay server via WebSocket and writes them to a local "_live.rep" file.
 *
 * Once the HEADER arrives, it triggers RECORDERMODETYPE_PLAYBACK on the
 * Recorder — the existing playback infrastructure (readReplayHeader,
 * updatePlayback, readNextFrame, appendNextCommand) handles everything else.
 *
 * The Recorder reads from the live file while the LiveObserver's background
 * thread simultaneously appends new BODY data. Short reads in readNextFrame
 * and appendNextCommand are handled gracefully when m_isLiveStream is set.
 */
class LiveObserver
{
public:
	LiveObserver();
	~LiveObserver();

	/// Connect to the relay and begin receiving replay data.
	/// Non-blocking; spawns a background thread.
	/// @param watchUrl Full WebSocket URL (e.g. ws://host:port/watch/GAMEID)
	void connect(const AsciiString& watchUrl);

	/// Returns true once the HEADER has been received and playback can start.
	Bool isReady() const { return m_headerReceived.load(); }

	/// Returns true if connected to the relay server.
	Bool isConnected() const { return m_connected.load(); }

	/// Returns true if the streamer has ended the session.
	Bool isStreamEnded() const { return m_streamEnded.load(); }

	/// Returns the filename of the live replay file (e.g. "996C586F_live.rep").
	const AsciiString& getLiveReplayFilename() const { return m_liveFilename; }

	/// Highest frame number contained in a fully-received record ("the live edge").
	/// Published by the network thread as data arrives; O(1) to read, always fresh.
	/// Replaces the old RecorderClass::probeLiveEdge() file scan.
	UnsignedInt getMaxCompleteFrame() const { return m_maxCompleteFrame.load(); }

	/// Absolute file offset one past the last complete record. The Recorder must never
	/// read beyond this — doing so is what allowed a torn record at the growing tail to
	/// misalign the playback stream permanently.
	Int getSafeReadOffset() const { return m_safeReadOffset.load(); }

	/// Close the connection and shut down the background thread.
	void close();

private:
	/// Background thread for network I/O.
	void networkThreadFunc();

	/// Consume complete records from newly-arrived body bytes and republish the
	/// watermarks. Called on the network thread only.
	void advanceParseCursor(Int chunkOffset, const unsigned char* data, size_t dataLen);

	/// Reset the parse cursor and watermarks (new session / disconnect).
	void resetParseCursor(Int bodyStartOffset);

	/// Connect via WebSocket (called from network thread).
	bool connectToRelay();

	/// Send data over WebSocket binary (called from network thread).
	bool wsSendBinary(const unsigned char* data, size_t len);

	/// Receive data from WebSocket (non-blocking).
	bool wsRecv(std::vector<char>& outBuffer);

	/// Open the local replay file for writing.
	bool openLiveFile();

	/// Process an incoming binary frame from the relay.
	void handleFrame(unsigned char type, const char* payload, size_t len);

	std::atomic<Bool> m_connected;
	std::atomic<Bool> m_shouldRun;
	std::atomic<Bool> m_headerReceived;
	std::atomic<Bool> m_streamEnded;

	// Watermarks published by the network thread, read by the game thread.
	std::atomic<UnsignedInt> m_maxCompleteFrame;
	std::atomic<Int> m_safeReadOffset;

	// Parse-cursor state. Owned exclusively by the network thread — no locking.
	std::vector<unsigned char> m_parseTail;   // bytes after the last complete record
	Int m_parseAbsOffset;                     // absolute file offset of m_parseTail[0]
	Bool m_parseCorrupt;                      // latched: watermark frozen, see advanceParseCursor

	AsciiString m_relayUrl;
	AsciiString m_gameId;

	File* m_liveFile;
	AsciiString m_liveFilePath;
	AsciiString m_liveFilename;   // e.g. "996C586F_live.rep"

	void* m_curlEasy;
	void* m_curlMulti;

	std::thread m_networkThread;
};

extern LiveObserver* TheLiveObserver;
LiveObserver* createLiveObserver();

// ---------------------------------------------------------------------------------------
// TheSuperHackers @feature 03/08/2026 Standalone relay HTTP fetch, for the live game browser.
//
// Deliberately not routed through HTTPManager: that lives behind
// NGMP_OnlineServicesManager, which is not initialised on the main menu unless the player has
// signed in to GeneralsOnline — so every request silently no-op'd there. The browser has to
// work for someone who just wants to watch a game, signed in or not.
//
// The request runs on its own thread, so the result is collected by polling from the main
// loop rather than delivered by callback; nothing here touches gadget state.

/// Start an async GET. Returns FALSE if a fetch is already in flight.
Bool liveRelayBeginFetch(const AsciiString& url);

/// Collect a finished fetch. Returns TRUE exactly once per completed request.
Bool liveRelayPollFetch(AsciiString& outBody, Bool& outSuccess, Int& outStatusCode);

/// TRUE while a request is outstanding.
Bool liveRelayFetchInFlight();

/// Queue a live-observer session for the given watch URL (implemented in MainMenu.cpp).
///
/// The connection itself has to happen once the main menu is the active screen again — it
/// waits for the relay's HEADER and then starts playback, which needs the shell settled. So
/// this only records the intent; MainMenuUpdate performs it after the caller pops back.
void StartLiveObserverSession(const AsciiString& watchUrl);

/// Switch the replay menu into live-games mode before pushing it (ReplayMenu.cpp).
void ReplayMenuEnterLiveGamesMode(void);

// ---------------------------------------------------------------------------------------
// TheSuperHackers @build 03/08/2026 Live observer/streamer file logging.
//
// Controlled by the RTS_DEBUG_LIVE_OBSERVER cmake option (DEFAULT/ON/OFF), following the
// same DEFAULT-resolution pattern as DEBUG_LOGGING in Debug.h: on for debug/internal builds,
// off otherwise, and forceable either way. Kept separate from RTS_DEBUG_LOGGING because this
// log is far noisier — it writes per frame during playback and flushes every line so it
// survives a crash — and only matters when working on this feature.
//
// Enable in a release build with:  cmake --preset win32 -DRTS_DEBUG_LIVE_OBSERVER=ON
//
// When disabled the bodies compile away, so the ~105 call sites stay in place: they document
// how this feature behaves and are what makes a failure diagnosable when it is switched on.
#if defined(ALLOW_DEBUG_UTILS) && !defined(LIVE_OBSERVER_LOGGING) && !defined(DISABLE_LIVE_OBSERVER_LOGGING)
	#define LIVE_OBSERVER_LOGGING 1
#endif

// Identifies the build that produced a log, so a stale binary is not debugged by mistake.
// Bump on every change to the instrumentation. Defined here rather than per .cpp: it was
// previously declared separately in LiveObserver.cpp and LiveStreamer.cpp and the two drifted
// apart, which is precisely the confusion this tag exists to prevent. Any file using it must
// include this header — that also brings the LIVE_OBSERVER_LOGGING resolution above, without
// which logging silently stays off in a DEFAULT build.
#define LIVE_OBSERVER_BUILD_TAG "2026-08-03-fix26-slotlist-roster-and-real-map"

void liveObserverLog(const char* fmt, ...);
void liveObserverInitLog(const char* watchUrl);

// Gates the ad hoc LIVE_OBSERVER instrumentation so it only fires for an actual live-observer
// session, instead of on every game start (including the streamer's own local game), which used to
// pollute the shared log with unrelated noise. Expands to nothing when logging is off, so the
// arguments are not evaluated either - unlike a plain call to the (then empty) liveObserverLog.
// Defined here rather than per .cpp so every call site shares one gate; a user must have included
// Common/Recorder.h for TheRecorder/RECORDERMODETYPE_LIVE_OBSERVER.
#if defined(LIVE_OBSERVER_LOGGING)
	#define LIVE_OBSERVER_LOG(...) \
		do { if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_LIVE_OBSERVER) (liveObserverLog)(__VA_ARGS__); } while (0)
#else
	#define LIVE_OBSERVER_LOG(...) do { } while (0)
#endif

#endif // GENERALS_ONLINE
