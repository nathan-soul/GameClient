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
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <queue>

class GameMessage;

/**
 * LiveStreamer streams game commands to a relay server via WebSocket.
 * Uses the same serialization format as .rep replay files so the relay
 * server can parse and forward frames to observers in real time.
 *
 * All network I/O happens on a background thread so the game loop is
 * never blocked.  If the connection fails the game continues normally.
 */
class LiveStreamer
{
public:
	LiveStreamer();
	~LiveStreamer();

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
	void onRoleAssigned(const AsciiString& role, const AsciiString& gameId);

	/// Stream one frame of commands (same format as .rep file writeToFile).
	void streamFrame(UnsignedInt frame, GameMessage* cmdList, Int currentFps);

	/// Send game metadata (map, players, CRC info, version, etc.).
	void sendMetadata();

	/// Called when this client becomes the active streamer (takeover from backup).
	void onTakeover();

	/// Process incoming messages from the relay (called from background thread).
	void tick();

	Bool isStreaming() const { return m_isStreaming; }
	Bool isBackup() const { return m_isBackup; }
	Bool isConnected() const { return m_connected; }
	AsciiString getGameId() const { return m_gameId; }

	/// Compute a deterministic game hash from game parameters.
	static AsciiString computeGameHash(
		const AsciiString& mapName,
		const AsciiString& gameMode,
		UnsignedInt startTime,
		const AsciiString& sortedPlayerNames);

private:
	/// Background thread entry point.
	void networkThreadFunc();

	/// Connect to relay (called from background thread).
	bool connectToRelay();

	/// Send raw data over WebSocket (called from background thread).
	bool wsSend(const void* data, size_t len);

	/// Receive data from WebSocket (called from background thread).
	bool wsRecv(std::vector<char>& outBuffer);

	/// Send a JSON message over WebSocket.
	bool sendJsonMessage(const AsciiString& jsonMsg);

	/// Serialize a frame into a binary buffer (same format as .rep writeToFile).
	void serializeFrame(UnsignedInt frame, GameMessage* cmdList, std::vector<char>& outBuffer);

	/// Serialize a single GameMessage argument.
	void serializeArgument(Int argType, const void* argData, std::vector<char>& outBuffer);

	// --- State ---
	std::atomic<Bool> m_isStreaming;
	std::atomic<Bool> m_isBackup;
	std::atomic<Bool> m_connected;
	std::atomic<Bool> m_shouldRun;

	AsciiString m_relayUrl;
	AsciiString m_gameId;
	AsciiString m_gameHash;
	AsciiString m_playerName;

	// CURL WebSocket handles (owned by the background thread)
	void* m_curlEasy;
	void* m_curlMulti;

	// Background thread
	std::thread m_networkThread;
	mutable std::mutex m_sendMutex;

	// Outgoing message queue (game thread pushes, network thread pops)
	struct QueuedMessage
	{
		std::vector<char> data;
		Bool isBinary;
	};
	std::queue<QueuedMessage> m_outgoingQueue;

	// Frame counter for throttling metadata sends
	UnsignedInt m_lastMetadataFrame;
};

extern LiveStreamer* TheLiveStreamer;
LiveStreamer* createLiveStreamer();

/// Log a message to live_streamer_debug.log (opens file on first call).
void liveStreamLog(const char* fmt, ...);

/// Write initial config header to live_streamer_debug.log.
/// Should be called at game start, BEFORE any streaming decision is made.
void liveStreamerInitLog();
