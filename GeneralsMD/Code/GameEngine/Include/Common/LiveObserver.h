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
#include <string>

class File;

/**
 * LiveObserver receives raw replay bytes (HEADER/PATCH/BODY/END) from the
 * relay server via WebSocket and writes them to a local "_live.rep" file.
 *
 * Once the HEADER arrives, it puts the Recorder into
 * RECORDERMODETYPE_LIVE_OBSERVER — the existing playback infrastructure
 * (readReplayHeader, updatePlayback, readNextFrame, appendNextCommand) handles
 * everything else, and that mode is the single answer to "is this live?".
 *
 * The Recorder reads from the live file while this class's background thread
 * simultaneously appends new BODY data. It never reads past getSafeReadOffset(),
 * which is why it can parse a record without checking for a torn one.
 */
class LiveObserver
{
public:
	LiveObserver();
	~LiveObserver();

	/// Begin receiving replay data. Non-blocking; spawns a background thread.
	/// Start watching a livestream by GO lobby id. The relay URL comes from GO, which mints
	/// the single-use watch ticket -- callers neither know nor build it.
	void connect(const AsciiString& lobbyId);

	/// Returns true once the HEADER has been received and playback can start.
	Bool isReady() const { return m_headerReceived.load(); }

	/// Returns true if connected to the relay server.
	Bool isConnected() const { return m_connected.load(); }

	/// Returns true if the streamer has ended the session.
	Bool isStreamEnded() const { return m_streamEnded.load(); }

	/// Latched by RecorderClass::startLiveObserverPlayback() once playback is actually running.
	/// Until then the session is still being set up, and clearing game data must not end it —
	/// see liveObserverOnGameCleared().
	void notePlaybackStarted() { m_playbackStarted = TRUE; }
	Bool hasPlaybackStarted() const { return m_playbackStarted; }

	/// TRUE once the live file is safe to start playing: the header is in place, the first
	/// body record is on disk, and the buffered stream already covers the broadcast delay
	/// (or the stream has ended, so there is nothing more to wait for). The join waits for
	/// this before starting playback — that is how a not-yet-arrived first record stopped
	/// looking like the end of the replay.
	Bool isPlaybackReady() const;

	/// Whole seconds until isPlaybackReady() becomes true; 0 once it is. Shown by the shell
	/// while the join waits, replacing the in-game pre-roll countdown that this pre-start
	/// wait makes redundant.
	Int getSecondsUntilPlaybackReady() const;

	/// How long the join may wait for isPlaybackReady() before giving up: the broadcast
	/// delay plus headroom for the connection, ticket minting and the first record.
	UnsignedInt getJoinTimeoutMs() const;

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

	/// File offset of the first body byte (the header length). The Recorder rewinds its read
	/// cursor here when starting playback, because playbackFile()'s seeding read leaves it
	/// past the first record's frame field — and the live loop reads the frame itself.
	Int getBodyStartOffset() const { return m_bodyStartOffset; }

	/// Close the connection and shut down the background thread.
	void close();

	// ---- Session policy: the broadcast delay and the buffering gate --------------------
	//
	// All of this belongs to the session, not to the Recorder. The delay arrives with the
	// session (the relay's ROLE frame), the pre-roll latches against it, and every bit of it
	// must be forgotten when the session ends. It used to live on RecorderClass, where
	// ending a session meant hand-resetting a dozen fields in two separate places — and any
	// one of them surviving leaked straight into the next session. Here there is nothing to
	// reset: the state dies with the object.

	/// Re-evaluate the gate for this tick and apply the resulting pause.
	void updatePlaybackGate(UnsignedInt curFrame);

	/// The player's own pause intent, kept apart from the buffering gate's. The two are OR'd
	/// together in updatePlaybackGate() so neither can silently override the other: buffering
	/// can never undo a manual pause, nor the reverse.
	///
	/// This is live-only state. An ordinary replay's pause goes straight to GameLogic; only a
	/// live session has a second opinion to reconcile it with.
	void toggleUserPause() { m_userPaused = !m_userPaused; }
	Bool isUserPaused() const { return m_userPaused; }

	/// Whether playback must wait rather than consume more records. Valid once
	/// updatePlaybackGate() has run this tick; the Recorder applies it, it does not decide it.
	Bool shouldHoldPlayback() const { return m_holdPlayback; }

	/// Latches TRUE once the initial buffer has been built, for the rest of the session.
	Bool isPreRollComplete() const { return m_preRollComplete; }

	/// Whole seconds until the initial buffer is built; 0 once pre-roll is complete.
	Int getPreRollSecondsRemaining() const;

	/// True only when playback is held *and* the source has genuinely stopped producing —
	/// not during the normal sawtooth of maintaining the delay at the boundary.
	Bool isStalled() const { return m_stalled; }

	/// TRUE while playback sits inside the broadcast delay, i.e. as close to the live game as
	/// it is ever allowed to get. Fast-forward is refused here so it can only ever close a
	/// backlog, never catch up to the real game and spoil it. Both fast-forward key handlers
	/// ask this rather than recomputing the gap from a live edge and a delay of their own.
	Bool isWithinBroadcastDelay(UnsignedInt curFrame) const;

	/// The broadcast delay this session was started with. Configured in *seconds* — that is
	/// what a streamer thinks in, and it stays correct if the logic tick rate changes (this
	/// build runs at 60, not the original 30) — with frames derived from it.
	UnsignedInt getDelaySeconds() const { return m_delaySeconds.load(); }
	UnsignedInt getDelayFrames() const { return m_delaySeconds.load() * LOGICFRAMES_PER_SECOND; }

	/// Record the frame at which this client's simulation was first seen to diverge from the
	/// streamed one. Playback deliberately continues afterwards — the observer just needs to
	/// be told that what it is watching is no longer the real game. Only the first divergence
	/// is of interest; a desynced simulation diverges further every frame after it.
	void noteDesync(UnsignedInt frame);
	Bool isDesynced() const { return m_desyncFrame != 0; }
	UnsignedInt getDesyncFrame() const { return m_desyncFrame; }

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

	/// Ask GO for a single-use watch ticket for m_gameId, using the logged-in session token.
	/// On success outConnectUrl is the complete relay URL to connect to, ticket included.
	/// There is no fallback: without a ticket the relay refuses the connection.
	bool fetchWatchTicket(AsciiString& outConnectUrl);

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

	// How long the live edge must sit still before a hold counts as a stall rather than
	// normal delay maintenance. At the boundary the hold toggles every few ticks, so the
	// status bar needs this to avoid reading WAITING FOR FRAMES during healthy playback.
	enum { LIVE_STALL_THRESHOLD_MS = 1000 };

	// Let the game actually get on its feet before holding it. GameClient::step() — and so
	// TheDisplay->step() — is only called on ticks where logic runs, so holding immediately
	// at frame 1 leaves a loaded but never-composed scene: a black screen. A couple of
	// seconds of warmup costs a rounding error against a 3600-frame delay.
	enum { LIVE_PREROLL_WARMUP_FRAMES = 120 };

	// Hysteresis band (frames) for the near-live gate. Holding engages only once the gap has
	// fallen a full band below the delay boundary and releases only once the source has
	// pulled a full band ahead again. At equal frame rates the gap sits exactly on the
	// boundary, and a plain threshold there toggled pause/resume constantly — the stutter.
	enum { LIVE_GATE_HYSTERESIS_FRAMES = 60 };

	// Buffering-gate state. Game thread only — updatePlaybackGate() is the sole writer.
	Bool m_holdPlayback;			// playback must wait; the Recorder acts on this
	Bool m_nearLiveHeld;			// latched: the near-live gate is holding (hysteresis)
	Bool m_preRollComplete;			// latches TRUE once the initial buffer is first built
	Bool m_autoPaused;				// the buffering logic owns the current pause
	Bool m_userPaused;				// the player pressed P and wants it paused
	Bool m_stalled;					// held, and no new data has arrived for a while
	Bool m_playbackStarted;			// the Recorder is actually playing this session's file
	UnsignedInt m_lastSeenLiveEdge;
	UnsignedInt m_lastLiveEdgeChangeMs;
	UnsignedInt m_desyncFrame;		// frame of the first observed CRC divergence, 0 = none

	// Written by the network thread when the relay's ROLE frame arrives, read by the game
	// thread on every tick. Atomic because those genuinely are two different threads: as a
	// plain UnsignedInt on the Recorder this crossed the boundary unsynchronised.
	std::atomic<UnsignedInt> m_delaySeconds;

	// Watermarks published by the network thread, read by the game thread.
	std::atomic<UnsignedInt> m_maxCompleteFrame;
	std::atomic<Int> m_safeReadOffset;

	// Parse-cursor state. Owned exclusively by the network thread — no locking.
	std::vector<unsigned char> m_parseTail;   // bytes after the last complete record
	Int m_parseAbsOffset;                     // absolute file offset of m_parseTail[0]
	Int m_bodyStartOffset;                    // file offset of the first body byte (the header length)
	Bool m_parseCorrupt;                      // latched: watermark frozen, see advanceParseCursor

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

/// End the live-observer session: destroy the observer, then reset the Recorder so it forgets the
/// replay it was feeding. Deliberately not stopPlayback(), which also exits the game.
///
/// Destroying the observer *is* the cleanup — the broadcast delay, the pre-roll latch, the pause
/// claim, the live edge and the desync frame all live on that object, so they go at once and no
/// future field can fall off a list. What the Recorder still holds is its own, and TheRecorder's
/// own reset() is what puts it back; this used to be ten assignments here reimplementing exactly
/// that, only because init() had been made unsafe for live sessions.
///
/// Must run before starting another session. The live file is named after the streamer's game, so
/// rejoining a game already watched targets the same path, and Windows will not let the observer
/// recreate a file the Recorder still holds open.
void liveObserverEndSession(void);

/// Called from GameLogic::clearGameData(), the one point every game-end path converges on.
///
/// Ends the live session, but only once it has actually started playing. Clearing game data is also
/// the *first* thing a starting session does — RecorderClass::playbackFile() tears down the shell
/// map, and the shell map counts as a game — so an unconditional teardown here destroyed the
/// observer that had just connected, leaving playback with no network thread, no watermarks and no
/// broadcast delay for the whole session.
void liveObserverOnGameCleared(void);

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

// ---------------------------------------------------------------------------
// GO services calls
//
// Livestreams are orchestrated by GO, not by the relay: GO owns the list of what is being
// streamed and mints the single-use credentials for both watching and streaming. The relay
// only honours a credential GO issued, so every one of these calls needs the player's session
// token — which is why the browser now requires a sign-in that it previously did not.

/// One live game, as GO describes it, already parsed and display-ready.
///
/// The browser renders these. Keeping GO's JSON shape out of the menu means the wire format
/// is known in exactly one place, and a contract change is a change here rather than in a
/// GUI callback.
struct LiveGameEntry
{
	AsciiString lobbyId;      ///< GO's LobbyID as decimal text; also the relay's session key.
	AsciiString mapName;      ///< Display name, never a path.
	AsciiString players;      ///< Human players, comma separated.
	Int observerCount;
	Int delaySeconds;
	Int ageSeconds;

	LiveGameEntry() : observerCount(0), delaySeconds(0), ageSeconds(0) {}
};

/// Parse a GO /Livestreams reply into entries. FALSE when the body is not usable at all;
/// an empty list with TRUE simply means nobody is streaming.
Bool liveServicesParseLivestreams(const AsciiString& body, std::vector<LiveGameEntry>& outGames);

/// Full URL for a GO services endpoint, e.g. liveServicesEndpoint("Livestreams").
AsciiString liveServicesEndpoint(const char* szEndpoint);

/// The signed-in player's session token, or an empty string when not signed in.
std::string liveServicesAuthToken();

/// Blocking authenticated request against GO services. For callers already on a worker thread
/// (the observer's and streamer's own network threads) — never call this from the main loop.
/// Returns FALSE when not signed in or when the request could not be made at all; outStatusCode
/// carries GO's reply otherwise, which the caller must still check.
Bool liveServicesRequest(const AsciiString& url, Bool bPost, const char* szPostBody,
	AsciiString& outBody, Int& outStatusCode);

/// Queue a live-observer session for the given lobby (implemented in MainMenu.cpp).
///
/// Only records the intent. The connection blocks on the relay's HEADER and then starts a
/// game, neither of which may happen while a screen is still animating — so the screen the
/// player lands on performs it, via LiveObserverStartPendingSession() below.
void StartLiveObserverSession(const AsciiString& lobbyId);

/// Start a queued live-observer session if one is pending and the shell has settled; a no-op
/// otherwise. Returns TRUE when playback actually started, which means the calling screen
/// should now stand itself down so the running game is visible.
///
/// Pumped from every shell screen the browser can be reached from — today the main menu and
/// the Online welcome screen — because a session is queued from whichever one the player
/// happens to be on, and only that screen can tear itself down afterwards.
Bool LiveObserverStartPendingSession(void);

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
#define LIVE_OBSERVER_BUILD_TAG "2026-08-07-observer-shellmap-restore"

void liveObserverLog(const char* fmt, ...);
void liveObserverInitLog(const char* lobbyId);

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
