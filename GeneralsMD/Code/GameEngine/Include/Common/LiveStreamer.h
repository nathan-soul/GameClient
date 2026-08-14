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

#include "Common/AsciiString.h"
#include "Common/GameCommon.h"
#include "Common/ReplayStreamSink.h"
#include "Common/UnicodeString.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <queue>
#include <deque>
#include <string>

/**
 * Binary message types sent over WebSocket between streamer/observer and relay.
 */
enum LiveMsgType : unsigned char {
	LIVE_MSG_REGISTER = 0,
	LIVE_MSG_HEADER    = 1,
	LIVE_MSG_PATCH     = 2,
	LIVE_MSG_BODY      = 3,
	LIVE_MSG_END       = 4,
	LIVE_MSG_ROLE      = 5,
	LIVE_MSG_ERROR     = 6,
	LIVE_MSG_CHAT      = 7,  // player chat: [frame u32][textLen u32][UTF-8 text][color u32]
	LIVE_MSG_SPECTATOR_CHAT = 8,  // spectator chat: [nameLen u32][UTF-8 name][textLen u32][UTF-8 text]
	LIVE_MSG_TICK      = 9,  // frame heartbeat: [frame u32]
};

class LiveStreamer;

/**
 * Everything the relay needs to open a session, assembled by the pre-game lobby.
 *
 * The lobby is the only place that can see a GeneralsOnline lobby in full, and the Recorder is
 * the only place that knows when a match has actually begun. This struct is the handover between
 * the two: the lobby fills it in and leaves it pending (liveStreamSetPendingRegistration), and
 * the Recorder sends it without needing to know what a GeneralsOnline lobby even is — which is
 * what keeps Recorder.cpp free of any GO includes.
 *
 * The host's registration is authoritative. Every player in a lobby registers, because every one
 * of them is a potential source of replay bytes, but only the host describes the game: the lobby
 * block and the broadcast delay are host-only fields. Without that rule the description of a game
 * would be whichever client's REGISTER happened to arrive first — they start within milliseconds
 * of each other, so it was genuinely arbitrary which one won.
 */
struct LiveStreamRegistration
{
	/// GO's LobbyID as plain decimal. This is the relay's session key and the id an observer
	/// watches by (/watch/<lobbyId>) — the same value, as the same text, that GO's own /Lobbies
	/// JSON prints for LobbyID, so a relay session and a GO lobby are trivially matched up.
	AsciiString lobbyId;
	/// Local player, for the relay's logs only. Never used to identify the session.
	AsciiString playerName;
	/// TRUE when the local player owns this lobby. Gates the two authoritative fields below.
	Bool isHost;
	/// Whether this client is willing to upload replay bytes. Per-client by nature — it is this
	/// machine's bandwidth — so unlike the fields below it is not the host's to decide.
	Bool canStream;

	/// HOST ONLY. A complete JSON object literal describing the lobby, in GO's own key spelling.
	/// Already escaped by its builder; empty on every non-host, which sends the lobby id alone.
	std::string lobbyJson;
	/// HOST ONLY. The broadcast delay every observer of this game is held behind — the host's
	/// spoiler window, so it is theirs to set. Negative means "not mine to say".
	Int delaySeconds;

	LiveStreamRegistration() : isHost(FALSE), canStream(FALSE), delaySeconds(-1) {}

	Bool isValid() const { return !lobbyId.isEmpty(); }
};

/// Hand a completed registration to the Recorder. Safe to call repeatedly — the last one wins,
/// so a lobby that changes between the player arriving and the match starting is not a problem.
void liveStreamSetPendingRegistration(const LiveStreamRegistration& registration);

/// Drop anything pending. Called when a lobby is left without starting a match, so a later,
/// unrelated recording cannot pick up a stale lobby's registration.
void liveStreamClearPendingRegistration();

Bool liveStreamHasPendingRegistration();

/// Open the pending session: create TheLiveStreamer, connect it to the relay and send REGISTER.
/// Returns the streamer for the caller to hook in as a replay sink, or nullptr when nothing is
/// pending or live streaming is switched off — in which case the game simply records as usual.
LiveStreamer* liveStreamStartPendingSession();

/**
 * LiveStreamer implements IReplayStreamSink and forwards raw replay bytes
 * to the relay server via WebSocket using a simple binary envelope.
 *
 * It has NO knowledge of the replay file format — it just receives raw
 * header/body/patch bytes from the Recorder and sends them over the wire.
 */
class LiveStreamer : public IReplayStreamSink
{
public:
	LiveStreamer();
	virtual ~LiveStreamer();

	/// IReplayStreamSink — called by Recorder during recording
	virtual void onHeaderBytes(const void* data, Int size) override;
	virtual void onHeaderComplete() override;
	virtual void onHeaderPatch(Int offset, const void* data, Int size) override;
	virtual void onBodyBytes(const void* data, Int size) override;
	virtual void onBodyFlush() override;
	virtual void onRecordingEnded() override;

	/// Start the network thread, which registers the stream with GO and connects to whatever
	/// relay URL GO returns. Non-blocking.
	void init();

	/// Shut down the background thread and close the connection.
	void close();

	/// Register a session with the relay server. See LiveStreamRegistration.
	void registerForGame(const LiveStreamRegistration& registration);

	/// Called when the relay confirms the session ("streamer" or "backup" or "none").
	/// While the role is "backup" this client stops uploading replay data (the backup gate
	/// below) but keeps recording locally, so it can take over later if promoted.
	void onRoleAssigned(const AsciiString& role, const AsciiString& lobbyId, uint64_t bodyOffset);

	/// Called when this client becomes the active streamer (takeover from backup).
	/// Backfills the relay's missing bytes from the local recorded copy starting at
	/// bodyOffset, then resumes live streaming. This is what makes a re-promoted
	/// backup seamless: it kept recording the whole match while demoted.
	void onTakeover(uint64_t bodyOffset);

	/// UI-informational flags. NOTE: unlike the original design, m_isBackup DOES gate data
	/// flow: while backup, the sink drops HEADER/PATCH/BODY (END is still sent) so a demoted
	/// streamer stops wasting its uplink. See plans/relay/streamer-allpush-demotion.md.
	Bool isStreaming() const { return m_isStreaming.load(); }
	Bool isBackup() const { return m_isBackup.load(); }
	Bool isConnected() const { return m_connected.load(); }
	AsciiString getLobbyId() const { return m_lobbyId; }

	/// IReplayStreamSink::onChat — forward a displayed global chat line to the relay
	/// (MSG_CHAT). Called by the Recorder from Core's ConnectionManager::processChat; see
	/// plans/relay/live-observer-chat.md.
	virtual void onChat(UnsignedInt frame, const UnicodeString& text, UnsignedInt colorArgb) override;

	/// IReplayStreamSink::onTick — publish our current logic frame (MSG_TICK).
	///
	/// The replay body only carries records for frames that have input, so in quiet play the
	/// stream is silent except for one CRC record every REPLAY_CRC_INTERVAL frames. An
	/// observer deriving the live edge from those records therefore learns where the game is
	/// in ~1.7 s jumps and starves between them. This states the frame directly.
	///
	/// The ordering is what makes it safe to act on: the Recorder calls this immediately
	/// after onBodyFlush() for the same frame, and frames leave on one WebSocket in queue
	/// order, so "tick N arrived" proves "every record with frame <= N arrived". The observer
	/// may simulate to N without guessing.
	virtual void onTick(UnsignedInt frame) override;

	/// Drain spectator chat received from the relay (MSG_SPECTATOR_CHAT) into the HUD
	/// message log. Called once per logic frame while recording a live-streamed game.
	/// See plans/relay/live-observer-spectator-chat.md.
	void pumpSpectatorChat();


	struct QueuedFrame
	{
		unsigned char type;
		std::vector<char> data;
	};

private:
	void networkThreadFunc();

	/// Ask GO to register this livestream and mint our single-use stream token, returning the
	/// relay URL to connect to. Blocking, so network thread only.
	bool requestStreamUrl(AsciiString& outUrl);

	bool connectToRelay();

	/// Tri-state send outcome: Sent = frame handed to the socket, WouldBlock = socket buffer
	/// full (CURLE_AGAIN — nothing was sent, retry the same frame later), Error = connection
	/// is gone. WouldBlock is a pause, never a failure: the relay is merely reading slowly.
	enum class WsSendResult { Sent, WouldBlock, Error };
	WsSendResult wsSendBinary(const unsigned char* data, size_t len);
	bool wsRecv(std::vector<char>& outBuffer);
	WsSendResult sendBinaryFrame(LiveMsgType type, const void* payload, size_t payloadLen);
	WsSendResult sendBinaryFrame(const QueuedFrame& frame);
	void queueFrame(LiveMsgType type, const void* data, size_t len);

	// UI-informational flags; m_isBackup additionally gates data flow (see onRoleAssigned)
	std::atomic<Bool> m_isStreaming;
	std::atomic<Bool> m_isBackup;
	std::atomic<Bool> m_connected;
	std::atomic<Bool> m_shouldRun;

	// Relay liveness watchdog (the streamer's mirror of LiveObserver's): the relay (or the
	// reverse proxy in front of it) pings this websocket every ~20 s, so any frame — stream
	// data or a ping — proves the connection is alive. Written by the network thread in
	// wsRecv on every received frame, read by the same thread's loop. Zero means "nothing
	// received yet" (still joining — the watchdog must not fire before the first ROLE).
	std::atomic<UnsignedInt> m_lastFrameReceivedMs{ 0 };

	// Relay liveness watchdog threshold: no frame of any kind for this long while connected
	// means the relay (or the path to it) is gone. The streamer must then wind down and say
	// so — without this it keeps "streaming" into a dead socket forever, and nothing on the
	// relay side exists anymore to consume the upload. 120 s = ~6 missed pings.
	enum { LIVE_STREAM_WATCHDOG_MS = 120000 };

	// Why the network thread ended (one of: shutdown / relay-silent / send-failed /
	// relay-error). Filled by the wind-down sites, printed in the thread-end summary so a
	// dead stream is always attributable to a side.
	AsciiString m_endReason;

	// Bytes and frames actually put on the wire, for the thread-end summary.
	size_t m_sentBytes;
	size_t m_sentFrames;

	AsciiString m_lobbyId;
	/// Host-only fields kept from the registration, because the stream is registered with GO
	/// from the network thread and the registration struct is gone by then.
	Bool m_isHost;
	Int m_delaySeconds;
	AsciiString m_playerName;

	void* m_curlEasy;
	void* m_curlMulti;

	std::thread m_networkThread;
	mutable std::mutex m_sendMutex;

	// Spectator chat inbound (MSG_SPECTATOR_CHAT): written by the network thread, drained
	// by pumpSpectatorChat on the game thread. This client is a *source* — spectator chat
	// is received (never sent, v1) because in-game observers are part of the audience.
	struct SpectatorChatEntry
	{
		UnicodeString displayName;
		UnicodeString text;
	};
	std::deque<SpectatorChatEntry> m_spectatorChatQueue;
	mutable std::mutex m_spectatorChatMutex;

	// deque, not queue: on CURLE_AGAIN (socket buffer full) the network thread must put
	// unsent frames back at the FRONT of the line — order is data, a misordered stream is
	// corrupt — and only a queue that supports front insertion can do that.
	std::deque<QueuedFrame> m_outgoingQueue;
	/// Bytes currently sitting in m_outgoingQueue, and whether the budget has been blown.
	/// Frames are queued before the relay connection exists (that is what lets the sink attach
	/// the moment a match starts), so a registration GO refuses would otherwise let the queue
	/// grow for the entire match with nothing ever draining it.
	size_t m_queuedBytes;
	bool m_queueOverflowed;

	// Header accumulation — buffered until onHeaderComplete()
	std::vector<char> m_headerBuffer;

	// Body accumulation — buffered until onBodyFlush() or threshold.
	//
	// One buffer, both roles (see plans/relay/streamer-allpush-demotion.md): while streaming it is
	// flushed to the wire every BODY_FLUSH_THRESHOLD bytes. While backup (demoted) onBodyFlush
	// is a no-op, so the same buffer accumulates the body from the demotion point onward —
	// which is the backfill source a later takeover needs. m_bodySentOffset is frozen at the
	// absolute offset of buffer[0] while backup, so takeover offsets stay absolute-correct.
	// Guarded by m_sendMutex: onBodyBytes/onBodyFlush (game thread) vs onTakeover (network
	// thread).
	std::vector<char> m_bodyBuffer;
	static const size_t BODY_FLUSH_THRESHOLD = 4096;
	uint64_t m_bodySentOffset;   // absolute file offset for next BODY chunk
	/// Hard ceiling on the backup accumulation. Matches are tens-to-hundreds of KB; this is
	/// generous headroom. On overflow the oldest bytes are dropped and m_bodySentOffset
	/// advances, so a takeover from an offset older than the retained window degrades to
	/// skip-forward.
	static const size_t BODY_BUFFER_MAX = 8 * 1024 * 1024;
};

extern LiveStreamer* TheLiveStreamer;
LiveStreamer* createLiveStreamer();

void liveStreamLog(const char* fmt, ...);
void liveStreamerInitLog();

/// Escape a string so it can be embedded in a JSON string literal.
///
/// The REGISTER payload carries free-form user text (lobby names such as "[eu][*] cazino 2v2")
/// and Windows paths full of backslashes. Both used to go out raw, which the relay could only
/// paper over by retrying the parse with every backslash doubled — a hack that a quote or a
/// newline in a lobby name would still have defeated. UTF-8 bytes are passed through unchanged;
/// they are already valid inside a JSON string.
std::string liveStreamJsonEscape(const char* str);
