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
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <queue>

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
};

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

	/// Connect to the relay server (non-blocking, spawns background thread).
	void init(const AsciiString& relayUrl);

	/// Shut down the background thread and close the connection.
	void close();

	/// Register a game session with the relay server.
	void registerForGame(
		const AsciiString& gameHash,
		const AsciiString& playerName,
		const AsciiString& mapName,
		const AsciiString& gameMode,
		Bool canStream);

	/// Called when the relay assigns a role ("streamer" or "backup" or "none").
	void onRoleAssigned(const AsciiString& role, const AsciiString& gameId, uint64_t bodyOffset);

	/// Called when this client becomes the active streamer (takeover from backup).
	/// NOTE: This is a UI-informational flag ONLY.  It does NOT gate any data flow —
	/// the IReplayStreamSink callbacks (onHeaderBytes, onBodyBytes, etc.) always
	/// send regardless of role.  Never add a role-guard to the sink methods.
	void onTakeover(uint64_t bodyOffset);

	/// UI-informational ONLY — do not use to gate data flow.
	Bool isStreaming() const { return m_isStreaming.load(); }
	Bool isBackup() const { return m_isBackup.load(); }
	Bool isConnected() const { return m_connected.load(); }
	AsciiString getGameId() const { return m_gameId; }

	static AsciiString computeGameHash(
		const AsciiString& mapName,
		const AsciiString& gameMode,
		UnsignedInt startTime,
		const AsciiString& sortedPlayerNames);

	struct QueuedFrame
	{
		unsigned char type;
		std::vector<char> data;
	};

private:
	void networkThreadFunc();
	bool connectToRelay();
	bool wsSendBinary(const unsigned char* data, size_t len);
	bool wsRecv(std::vector<char>& outBuffer);
	bool sendBinaryFrame(LiveMsgType type, const void* payload, size_t payloadLen);
	bool sendBinaryFrame(const QueuedFrame& frame);
	bool sendJsonFrame(const char* jsonStr);
	void queueFrame(LiveMsgType type, const void* data, size_t len);

	// UI-informational only — never gate data flow with these
	std::atomic<Bool> m_isStreaming;
	std::atomic<Bool> m_isBackup;
	std::atomic<Bool> m_connected;
	std::atomic<Bool> m_shouldRun;

	AsciiString m_relayUrl;
	AsciiString m_gameId;
	AsciiString m_gameHash;
	AsciiString m_playerName;

	void* m_curlEasy;
	void* m_curlMulti;

	std::thread m_networkThread;
	mutable std::mutex m_sendMutex;

	std::queue<QueuedFrame> m_outgoingQueue;

	// Header accumulation — buffered until onHeaderComplete()
	std::vector<char> m_headerBuffer;

	// Body accumulation — buffered until onBodyFlush() or threshold
	std::vector<char> m_bodyBuffer;
	static const size_t BODY_FLUSH_THRESHOLD = 4096;
	uint64_t m_bodySentOffset;   // absolute file offset for next BODY chunk
};

extern LiveStreamer* TheLiveStreamer;
LiveStreamer* createLiveStreamer();

void liveStreamLog(const char* fmt, ...);
void liveStreamerInitLog();
