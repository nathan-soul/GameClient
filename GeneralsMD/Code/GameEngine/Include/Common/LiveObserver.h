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
#include "Common/Recorder.h"
#include "Common/GameType.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <list>
#include <vector>

class GameMessage;

/**
 * LiveFrameData holds a single frame's worth of commands received from the relay server.
 * Each frame contains a frame number and a serialized list of GameMessages to append
 * to TheCommandList during playback.
 */
struct LiveFrameData
{
	UnsignedInt frameNumber;
	std::vector<char> serializedCommands; ///< Binary data in the same format as .rep writeToFile()
};

/**
 * LiveObserver receives live game frames from a relay server via WebSocket,
 * feeding them into TheCommandList so the game renders in real-time as a
 * live replay. It reuses the same serialization format as .rep replay files
 * so the relay can parse and forward frames from the streamer.
 *
 * This is the read-side counterpart to LiveStreamer.  The streamer writes
 * frames; the observer reads them.
 *
 * All network I/O happens on a background thread.  The main game thread
 * polls for available frames via waitForFrame() and feeds them into the
 * command list via feedCommandsToCommandList().
 *
 * Usage:
 *   1. Call connect(relayUrl, gameId) to start the background thread.
 *   2. Call receiveGameMetadata() to block until metadata arrives.
 *   3. Each game tick: call waitForFrame(targetFrame), then feedCommandsToCommandList().
 *   4. Call close() when done.
 */
class LiveObserver
{
public:
	LiveObserver();
	~LiveObserver();

	/// Connect to the relay server and join the specified game session.
	void connect(const AsciiString& relayUrl, const AsciiString& gameId);

	/// Block until game metadata (map, players, CRC info) is received.
	/// Returns true on success, false on timeout/disconnect.
	Bool receiveGameMetadata();

	/// Returns a pointer to the reconstructed ReplayGameInfo for game initialization.
	/// Valid only after receiveGameMetadata() returns true.
	ReplayGameInfo* getGameInfo() { return &m_replayGameInfo; }

	/// Returns the game options string (S=... format) received from the relay.
	/// This can be fed to ParseAsciiStringToGameInfo() to populate player slots.
	const AsciiString& getGameOptions() const { return m_gameOptionsStr; }

	/// Block until at least one frame >= targetFrame is available, or timeout.
	/// Returns true if a frame is available, false on timeout/disconnect.
	Bool waitForFrame(UnsignedInt targetFrame);

	/// Feed the commands for a specific frame to TheCommandList.
	/// Only processes the single frame matching targetFrame if it exists.
	void feedCommandsToCommandList(UnsignedInt targetFrame);

	/// Insert a placeholder empty frame so the game can advance past gaps.
	/// Used when waitForFrame times out but future frames are already buffered.
	void insertPlaceholderFrame(UnsignedInt frameNum);

	/// Returns how many frames behind the streamer the observer currently is.
	/// Negative means ahead (shouldn't happen).
	Int getBufferDelay() const;

	/// Returns true if connected to the relay server.
	Bool isConnected() const { return m_connected.load(); }

	/// Returns true if metadata has been received and the observer is ready.
	Bool isReady() const { return m_metadataReceived.load(); }

	/// Returns the current frame number of the streamer (from metadata updates).
	UnsignedInt getStreamerFrame() const { return m_streamerFrame.load(); }

	/// Returns the streamer's reported FPS (0 if unknown).
	Int getStreamerFps() const { return m_streamerFps.load(); }

	/// Close the connection and shut down the background thread.
	void close();

private:
	/// Background thread entry point for all network I/O.
	void networkThreadFunc();

	/// Connect to relay via WebSocket (called from network thread).
	bool connectToRelay();

	/// Attempt to reconnect to the relay after a disconnect.
	bool reconnectToRelay();

	/// Send data over WebSocket (called from network thread).
	bool wsSend(const void* data, size_t len);

	/// Receive data from WebSocket (non-blocking, called from network thread).
	bool wsRecv(std::vector<char>& outBuffer);

	/// Send a JSON message over WebSocket.
	bool sendJsonMessage(const AsciiString& jsonMsg);

	/// Parse an incoming JSON frame message from the relay.
	void parseFrameMessage(const AsciiString& json);

	/// Parse an incoming catchup_bulk message (array of frames) from the relay.
	void parseBulkCatchup(const AsciiString& json);

	/// Parse an incoming JSON metadata message from the relay.
	void parseMetadataMessage(const AsciiString& json);

	/// Deserialize binary commands into a LiveFrameData and push to m_pendingFrames.
	void deserializeFrame(UnsignedInt frameNum, const char* payload, Int payloadSize);

	// --- State ---
	std::atomic<Bool> m_connected;
	std::atomic<Bool> m_shouldRun;
	std::atomic<Bool> m_metadataReceived;

	AsciiString m_relayUrl;
	AsciiString m_gameId;

	UnsignedInt m_lastReceivedFrame;    ///< Highest frame number received from relay
	UnsignedInt m_lastProcessedFrame;   ///< Highest frame number fed to TheCommandList
	std::atomic<UnsignedInt> m_streamerFrame;  ///< Current frame of the streamer
	std::atomic<Int> m_streamerFps;            ///< Framerate of the streamer

	// CURL WebSocket handles (owned by the background thread)
	void* m_curlEasy;
	void* m_curlMulti;

	// Background thread
	std::thread m_networkThread;
	mutable std::mutex m_pendingMutex;

	/// Buffered frames waiting to be consumed by the game thread.
	std::list<LiveFrameData> m_pendingFrames;

	/// Reconstructed game info from metadata.
	ReplayGameInfo m_replayGameInfo;

	/// Game options string (S=... format) from relay metadata.
	/// Used by ParseAsciiStringToGameInfo() to populate player slots.
	AsciiString m_gameOptionsStr;

	/// Reconnection state
	Int m_reconnectAttempts;
	bool m_isReconnecting;
	static const Int MAX_RECONNECT_ATTEMPTS = 10;
	static const Int RECONNECT_DELAY_MS = 2000;
};

extern LiveObserver* TheLiveObserver;
LiveObserver* createLiveObserver();

/// Log a message to live_observer_debug.log (opens file on first call).
void liveObserverLog(const char* fmt, ...);

/// Write initial config header to live_observer_debug.log.
/// Called at game start when -livewatch is specified.
void liveObserverInitLog(const char* watchUrl);

#endif // GENERALS_ONLINE
