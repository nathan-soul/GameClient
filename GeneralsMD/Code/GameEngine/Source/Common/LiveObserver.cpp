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
#include "Common/MessageStream.h"
#include "Common/GlobalData.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/GameEngine.h"
#include "GameLogic/GameLogic.h"
#include "Common/AsciiString.h"
#include "Common/Recorder.h"

#include "GameNetwork/GameMessageParser.h"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/multi.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/websockets.h"

#include <algorithm>
#include <cstdio>

extern GameLogic* TheGameLogic;
extern CommandList* TheCommandList;

// ============================================================================
// findSubstring — helper: find pattern in AsciiString, return index or -1
// AsciiString::find(char) only accepts a single character. For substring
// searches we use strstr on the raw buffer and convert to an Int offset.
// ============================================================================
static Int findSubstring(const AsciiString& str, const char* pattern, Int startPos = 0)
{
	const char* haystack = str.str() + startPos;
	const char* result = strstr(haystack, pattern);
	if (!result)
		return -1;
	return (Int)(result - str.str());
}

// ============================================================================
// base64Decode — decode base64 string to binary data
// ============================================================================

static int base64CharToVal(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static void base64Decode(const char* encoded, size_t len, std::vector<char>& out)
{
	out.clear();
	int val = 0, bits = -8;
	for (size_t i = 0; i < len; i++)
	{
		char c = encoded[i];
		if (c == '=' || c == ' ' || c == '\n' || c == '\r')
			continue;
		int v = base64CharToVal(c);
		if (v < 0) continue;
		val = (val << 6) + v;
		bits += 6;
		if (bits >= 0)
		{
			out.push_back((char)((val >> bits) & 0xff));
			bits -= 8;
		}
	}
}
// ============================================================================
// readArgumentFromBuffer — read a single argument from a binary buffer
// Same switch as Recorder::readArgument(), but reads from memory instead of file.
// ============================================================================

static void readArgumentFromBuffer(GameMessageArgumentDataType type, GameMessage* msg, const char*& pos, Int& remaining)
{
	switch (type) {
	case ARGUMENTDATATYPE_INTEGER: {
		if (remaining < (Int)sizeof(Int)) return;
		Int val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendIntegerArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_REAL: {
		if (remaining < (Int)sizeof(Real)) return;
		Real val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendRealArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_BOOLEAN: {
		if (remaining < (Int)sizeof(Bool)) return;
		Bool val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendBooleanArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_OBJECTID: {
		if (remaining < (Int)sizeof(ObjectID)) return;
		ObjectID val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendObjectIDArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_DRAWABLEID: {
		if (remaining < (Int)sizeof(DrawableID)) return;
		DrawableID val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendDrawableIDArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_TEAMID: {
		if (remaining < (Int)sizeof(UnsignedInt)) return;
		UnsignedInt val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendTeamIDArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_LOCATION: {
		if (remaining < (Int)sizeof(Coord3D)) return;
		Coord3D val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendLocationArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_PIXEL: {
		if (remaining < (Int)sizeof(ICoord2D)) return;
		ICoord2D val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendPixelArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_PIXELREGION: {
		if (remaining < (Int)sizeof(IRegion2D)) return;
		IRegion2D val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendPixelRegionArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_TIMESTAMP: {
		if (remaining < (Int)sizeof(UnsignedInt)) return;
		UnsignedInt val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendTimestampArgument(val);
		break;
	}
	case ARGUMENTDATATYPE_WIDECHAR: {
		if (remaining < (Int)sizeof(WideChar)) return;
		WideChar val;
		memcpy(&val, pos, sizeof(val));
		pos += sizeof(val);
		remaining -= sizeof(val);
		msg->appendWideCharArgument(val);
		break;
	}
	default:
		break;
	}
}

// ============================================================================
// Singleton
// ============================================================================

LiveObserver* TheLiveObserver = nullptr;

LiveObserver* createLiveObserver()
{
	return MSGNEW("LiveObserver") LiveObserver();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

LiveObserver::LiveObserver()
	: m_connected(FALSE)
	, m_shouldRun(FALSE)
	, m_metadataReceived(FALSE)
	, m_lastReceivedFrame(0)
	, m_lastProcessedFrame(0)
	, m_streamerFrame(0)
	, m_streamerFps(0)
	, m_curlEasy(nullptr)
	, m_curlMulti(nullptr)
	, m_reconnectAttempts(0)
	, m_isReconnecting(false)
{
}

LiveObserver::~LiveObserver()
{
	close();
}

// ============================================================================
// connect — set relay URL/game ID and start the background network thread
// ============================================================================

void LiveObserver::connect(const AsciiString& relayUrl, const AsciiString& gameId)
{
	if (m_shouldRun)
		return;

	m_relayUrl = relayUrl;
	m_gameId = gameId;
	m_shouldRun = TRUE;
	m_connected = FALSE;
	m_metadataReceived = FALSE;
	m_lastReceivedFrame = 0;
	m_lastProcessedFrame = 0;
	m_reconnectAttempts = 0;

	DEBUG_LOG(("LiveObserver::connect() - connecting to %s game %s", relayUrl.str(), gameId.str()));

	m_networkThread = std::thread(&LiveObserver::networkThreadFunc, this);
}

// ============================================================================
// close — shut down the background thread
// ============================================================================

void LiveObserver::close()
{
	if (!m_shouldRun)
		return;

	m_shouldRun = FALSE;
	m_connected = FALSE;

	if (m_networkThread.joinable())
	{
		m_networkThread.join();
	}

	// Clear any pending frames
	{
		std::lock_guard<std::mutex> lock(m_pendingMutex);
		m_pendingFrames.clear();
	}

	DEBUG_LOG(("LiveObserver::close() - shut down"));
}

// ============================================================================
// receiveGameMetadata — block until metadata arrives from the relay
// ============================================================================

Bool LiveObserver::receiveGameMetadata()
{
	// Wait up to 10 seconds for metadata
	const Int timeoutMs = 10000;
	const Int pollIntervalMs = 50;
	Int elapsed = 0;

	while (!m_metadataReceived && m_connected && elapsed < timeoutMs)
	{
		Sleep(pollIntervalMs);
		elapsed += pollIntervalMs;
	}

	if (m_metadataReceived)
	{
		DEBUG_LOG(("LiveObserver::receiveGameMetadata() - metadata received"));
		return TRUE;
	}

	DEBUG_LOG(("LiveObserver::receiveGameMetadata() - timeout or disconnect"));
	return FALSE;
}

// ============================================================================
// waitForFrame — block until the target frame is available
// ============================================================================

Bool LiveObserver::waitForFrame(UnsignedInt targetFrame)
{
	// Check if we already have the frame buffered
	{
		std::lock_guard<std::mutex> lock(m_pendingMutex);
		for (const LiveFrameData& fd : m_pendingFrames)
		{
			if (fd.frameNumber >= targetFrame)
				return TRUE;
		}
	}

	// If not connected, return false
	if (!m_connected)
		return FALSE;

	// Wait for the frame to arrive (with timeout)
	const Int timeoutMs = 5000;
	const Int pollIntervalMs = 10;
	Int elapsed = 0;

	while (elapsed < timeoutMs && m_connected)
	{
		Sleep(pollIntervalMs);
		elapsed += pollIntervalMs;

		std::lock_guard<std::mutex> lock(m_pendingMutex);
		for (const LiveFrameData& fd : m_pendingFrames)
		{
			if (fd.frameNumber >= targetFrame)
				return TRUE;
		}
	}

	// Timeout — might need reconnect
	if (!m_connected)
	{
		DEBUG_LOG(("LiveObserver::waitForFrame() - disconnected, attempting reconnect"));
		m_reconnectAttempts++;
		if (m_reconnectAttempts <= MAX_RECONNECT_ATTEMPTS)
		{
			// Signal reconnect in background thread
			// The background thread handles reconnection
		}
	}

	return FALSE;
}

// ============================================================================
// feedCommandsToCommandList — append commands for the current frame
// ============================================================================

void LiveObserver::feedCommandsToCommandList()
{
	UnsignedInt currentFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;

	std::lock_guard<std::mutex> lock(m_pendingMutex);

	// Find and consume all frames that should be executed on or before the current frame
	// We iterate in order since frames are inserted sorted
	auto it = m_pendingFrames.begin();
	while (it != m_pendingFrames.end())
	{
		if (it->frameNumber <= currentFrame)
		{
			// Deserialize the binary commands and append to TheCommandList
			if (!it->serializedCommands.empty() && TheCommandList)
			{
				const char* data = it->serializedCommands.data();
				Int dataSize = (Int)it->serializedCommands.size();
				Int pos = 0;

				// Binary format (same as Recorder::writeToFile / LiveStreamer::serializeFrame):
				// [4-byte frame][4-byte type][4-byte playerIndex][1-byte numTypes][type entries...][arguments...]
				while (pos + 13 <= dataSize) // minimum: 4 frame + 4 type + 4 player + 1 numTypes = 13
				{
					// Read frame number (4 bytes)
					UnsignedInt frameNum;
					memcpy(&frameNum, data + pos, sizeof(frameNum));
					pos += sizeof(frameNum);

					// Read message type (4 bytes)
					GameMessage::Type msgType;
					memcpy(&msgType, data + pos, sizeof(msgType));
					pos += sizeof(msgType);

					// Read player index (4 bytes)
					Int playerIndex;
					memcpy(&playerIndex, data + pos, sizeof(playerIndex));
					pos += sizeof(playerIndex);

					// Read number of argument type entries (1 byte)
					if (pos >= dataSize)
						break;
					UnsignedByte numTypes = (UnsignedByte)data[pos];
					pos += sizeof(numTypes);

					// Read argument type info entries: each is [1-byte type][1-byte argCount]
					if (pos + numTypes * 2 > dataSize)
						break;

					GameMessageParser* parser = newInstance(GameMessageParser)();
					Int totalArgs = 0;
					for (UnsignedByte i = 0; i < numTypes; ++i)
					{
						UnsignedByte argType = (UnsignedByte)data[pos];
						pos += 1;
						UnsignedByte argc = (UnsignedByte)data[pos];
						pos += 1;
						parser->addArgType((GameMessageArgumentDataType)argType, argc);
						totalArgs += argc;
					}

					// Create the GameMessage using the correct API
					GameMessage* msg = newInstance(GameMessage)(msgType);
					msg->friend_setPlayerIndex(playerIndex);

					// Read arguments using parser's linked-list type info (same as Recorder::appendNextCommand)
					GameMessageParserArgumentType* parserArgType = parser->getFirstArgumentType();
					GameMessageArgumentDataType lastType = ARGUMENTDATATYPE_UNKNOWN;
					Int argsLeft = 0;
					if (parserArgType != nullptr)
					{
						lastType = parserArgType->getType();
						argsLeft = parserArgType->getArgCount();
					}
					for (Int j = 0; j < totalArgs; ++j)
					{
						if (pos >= dataSize)
							break;
						readArgumentFromBuffer(lastType, msg, data, pos, dataSize);

						--argsLeft;
						if (argsLeft == 0)
						{
							if (parserArgType != nullptr)
								parserArgType = parserArgType->getNext();
							if (parserArgType != nullptr)
							{
								argsLeft = parserArgType->getArgCount();
								lastType = parserArgType->getType();
							}
						}
					}

					deleteInstance(parser);

					// Append to command list (skip CRC messages, as Recorder does)
					if (msgType != GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
					{
						TheCommandList->appendMessage(msg);
					}
					else
					{
						deleteInstance(msg);
					}
				}
			}

			m_lastProcessedFrame = it->frameNumber;
			it = m_pendingFrames.erase(it);
		}
		else
		{
			break; // Frames are sorted, no need to check further
		}
	}
}

// ============================================================================
// getBufferDelay — frames behind the streamer
// ============================================================================

Int LiveObserver::getBufferDelay() const
{
	return (Int)m_streamerFrame - (Int)m_lastProcessedFrame;
}

// ============================================================================
// networkThreadFunc — background thread for all network I/O
// ============================================================================

void LiveObserver::networkThreadFunc()
{
	DEBUG_LOG(("LiveObserver::networkThreadFunc() - thread started"));

	curl_global_init(CURL_GLOBAL_DEFAULT);

	while (m_shouldRun)
	{
		if (!m_connected)
		{
			// Try to connect or reconnect
			if (reconnectToRelay())
			{
				m_reconnectAttempts = 0;

				// Send a reconnect message with our last known frame
				if (m_lastProcessedFrame > 0)
				{
					AsciiString json;
					json.format(
						"{\"type\":\"reconnect\",\"game_id\":\"%s\",\"last_frame\":%u}",
						m_gameId.str(), m_lastProcessedFrame);
					sendJsonMessage(json);
				}
			}
			else
			{
				m_reconnectAttempts++;
				if (m_reconnectAttempts > MAX_RECONNECT_ATTEMPTS)
				{
					DEBUG_LOG(("LiveObserver::networkThreadFunc() - max reconnect attempts reached"));
					break;
				}
				Sleep(RECONNECT_DELAY_MS);
				continue;
			}
		}

		// --- Receive incoming messages ---
		std::vector<char> recvBuffer;
		if (wsRecv(recvBuffer) && !recvBuffer.empty())
		{
			// All messages are now JSON text frames
			AsciiString incoming(recvBuffer.data(), (Int)recvBuffer.size());

			if (findSubstring(incoming, "\"type\"") != -1)
			{
				if (findSubstring(incoming, "\"type\":\"frame\"") != -1)
				{
					// JSON frame message — extract and decode commands
					parseFrameMessage(incoming);
				}
				else
				{
					// Metadata or other JSON message
					parseMetadataMessage(incoming);
				}
			}
		}

		// Small sleep to avoid busy-waiting (1ms)
		Sleep(1);
	}

	// Cleanup
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

	curl_global_cleanup();

	m_connected = FALSE;
	DEBUG_LOG(("LiveObserver::networkThreadFunc() - thread exiting"));
}

// ============================================================================
// connectToRelay — establish WebSocket connection
// ============================================================================

bool LiveObserver::connectToRelay()
{
	CURL* easy = curl_easy_init();
	if (!easy)
	{
		DEBUG_LOG(("LiveObserver::connectToRelay() - curl_easy_init failed"));
		return false;
	}

	CURLM* multi = curl_multi_init();
	if (!multi)
	{
		DEBUG_LOG(("LiveObserver::connectToRelay() - curl_multi_init failed"));
		curl_easy_cleanup(easy);
		return false;
	}

	// Configure the WebSocket connection
	AsciiString url;
	if (m_isReconnecting)
		url.format("%s/watch-reconnect/%s", m_relayUrl.str(), m_gameId.str());
	else
		url.format("%s/watch/%s", m_relayUrl.str(), m_gameId.str());
	curl_easy_setopt(easy, CURLOPT_URL, url.str());
	curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L); // 2 = use WebSocket protocol
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);

	// Add to multi handle
	curl_multi_add_handle(multi, easy);

	// Perform the connection
	int runningHandles = 0;
	CURLMcode mc = curl_multi_perform(multi, &runningHandles);

	// Wait for connection to complete
	int numfds = 0;
	mc = curl_multi_wait(multi, nullptr, 0, 1000, &numfds);

	// Check if connected
	int infoRunning = 0;
	CURLMsg* infoMsg = curl_multi_info_read(multi, &infoRunning);
	if (infoMsg)
	{
		CURLcode res = infoMsg->data.result;
		if (res != CURLE_OK)
		{
			DEBUG_LOG(("LiveObserver::connectToRelay() - connection failed: %s", curl_easy_strerror(res)));
			curl_multi_remove_handle(multi, easy);
			curl_easy_cleanup(easy);
			curl_multi_cleanup(multi);
			return false;
		}
	}

	m_curlEasy = easy;
	m_curlMulti = multi;
	m_connected = TRUE;

	DEBUG_LOG(("LiveObserver::connectToRelay() - connected to %s", url.str()));
	return true;
}

// ============================================================================
// reconnectToRelay — attempt to reconnect after a disconnect
// ============================================================================

bool LiveObserver::reconnectToRelay()
{
	// Clean up old handles
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

	m_connected = FALSE;

	DEBUG_LOG(("LiveObserver::reconnectToRelay() - attempt %d", m_reconnectAttempts + 1));

	m_isReconnecting = true;
	bool result = connectToRelay();
	m_isReconnecting = false;
	return result;
}

// ============================================================================
// wsSend — send data over WebSocket
// ============================================================================

bool LiveObserver::wsSend(const void* data, size_t len)
{
	if (!m_curlEasy || !m_connected)
		return false;

	size_t sent = 0;
	CURLcode res = curl_ws_send((CURL*)m_curlEasy, data, len, &sent, 0, CURLWS_BINARY);

	if (res == CURLE_OK)
		return true;

	if (res == CURLE_AGAIN)
		return true; // not a real error, will retry next tick

	DEBUG_LOG(("LiveObserver::wsSend() - error: %s", curl_easy_strerror(res)));
	m_connected = FALSE;
	return false;
}

// ============================================================================
// wsRecv — receive data from WebSocket (non-blocking)
// ============================================================================

bool LiveObserver::wsRecv(std::vector<char>& outBuffer)
{
	if (!m_curlEasy || !m_connected)
		return false;

	char buf[4096];
	size_t nread = 0;
	const struct curl_ws_frame* meta = nullptr;

	CURLcode res = curl_ws_recv((CURL*)m_curlEasy, buf, sizeof(buf), &nread, &meta);

	if (res == CURLE_OK && nread > 0)
	{
		outBuffer.assign(buf, buf + nread);
		return true;
	}

	if (res == CURLE_AGAIN)
		return false; // no data available

	if (res != CURLE_OK)
	{
		DEBUG_LOG(("LiveObserver::wsRecv() - error: %s", curl_easy_strerror(res)));
		m_connected = FALSE;
	}

	return false;
}

// ============================================================================
// sendJsonMessage — convenience wrapper
// ============================================================================

bool LiveObserver::sendJsonMessage(const AsciiString& jsonMsg)
{
	if (!m_curlEasy || !m_connected)
		return false;
	size_t sent = 0;
	CURLcode res = curl_ws_send((CURL*)m_curlEasy, jsonMsg.str(), strlen(jsonMsg.str()), &sent, 0, CURLWS_TEXT);
	if (res == CURLE_OK)
		return true;
	if (res == CURLE_AGAIN)
		return true;
	DEBUG_LOG(("LiveObserver::sendJsonMessage() - error: %s", curl_easy_strerror(res)));
	m_connected = FALSE;
	return false;
}

// ============================================================================
// parseFrameMessage — process a JSON frame message from the relay
// ============================================================================

void LiveObserver::parseFrameMessage(const AsciiString& json)
{
	// Extract frame number
	Int framePos = findSubstring(json, "\"frame\":");
	if (framePos == -1)
		return;

	Int start = framePos + 8;
	Int end = findSubstring(json, ",", start);
	if (end == -1)
		end = findSubstring(json, "}", start);
	if (end == -1)
		return;

	AsciiString frameStr(json.str() + start, end - start);
	UnsignedInt frameNum = (UnsignedInt)atoi(frameStr.str());

	// Extract base64-encoded commands
	Int cmdPos = findSubstring(json, "\"commands\":\"");
	if (cmdPos == -1)
		return;

	start = cmdPos + 12;
	end = findSubstring(json, "\"", start);
	if (end == -1)
		return;

	AsciiString b64Commands(json.str() + start, end - start);

	// Base64 decode
	std::vector<char> decodedCommands;
	base64Decode(b64Commands.str(), b64Commands.getLength(), decodedCommands);

	if (decodedCommands.empty())
		return;

	// Update streamer frame tracking
	if (frameNum > m_streamerFrame)
		m_streamerFrame = frameNum;

	// Deserialize the payload and add to pending frames
	deserializeFrame(frameNum, decodedCommands.data(), (Int)decodedCommands.size());

	// Update last received frame
	if (frameNum > m_lastReceivedFrame)
		m_lastReceivedFrame = frameNum;

	DEBUG_LOG(("LiveObserver::parseFrameMessage() - frame %u, decoded %d bytes", frameNum, (Int)decodedCommands.size()));
}

// ============================================================================
// parseMetadataMessage — process a JSON metadata message
// ============================================================================

void LiveObserver::parseMetadataMessage(const AsciiString& json)
{
	// Simple JSON parsing — extract fields we need
	DEBUG_LOG(("LiveObserver::parseMetadataMessage() - %s", json.str()));

	// Check message type
	Int typePos = findSubstring(json, "\"type\":\"");
	if (typePos == -1)
		return;

	AsciiString msgType;
	Int start = typePos + 8;
	Int end = findSubstring(json, "\"", start);
	if (end != -1)
		msgType = AsciiString(json.str() + start, end - start);

	if (msgType == "metadata")
	{
		// Extract streamer frame
		Int framePos = findSubstring(json, "\"current_frame\":");
		if (framePos != -1)
		{
			start = framePos + 16;
			end = findSubstring(json, ",", start);
			if (end == -1)
				end = findSubstring(json, "}", start);
			if (end != -1)
			{
				AsciiString frameStr(json.str() + start, end - start);
				m_streamerFrame = (UnsignedInt)atoi(frameStr.str());
			}
		}

		// Extract FPS
		Int fpsPos = findSubstring(json, "\"fps\":");
		if (fpsPos != -1)
		{
			start = fpsPos + 6;
			end = findSubstring(json, ",", start);
			if (end == -1)
				end = findSubstring(json, "}", start);
			if (end != -1)
			{
				AsciiString fpsStr(json.str() + start, end - start);
				m_streamerFps = atoi(fpsStr.str());
			}
		}

		// Extract map name
		Int mapPos = findSubstring(json, "\"map_name\":\"");
		if (mapPos != -1)
		{
			start = mapPos + 12;
			end = findSubstring(json, "\"", start);
			if (end != -1)
			{
				AsciiString mapName(json.str() + start, end - start);
				m_replayGameInfo.setMap(mapName);
			}
		}

		// Extract exeCRC and iniCRC
		Int exeCRCPos = findSubstring(json, "\"exe_crc\":");
		if (exeCRCPos != -1)
		{
			start = exeCRCPos + 10;
			end = findSubstring(json, ",", start);
			if (end == -1)
				end = findSubstring(json, "}", start);
			if (end != -1)
			{
				AsciiString crcStr(json.str() + start, end - start);
				// Store CRC info if needed
			}
		}

		m_metadataReceived = TRUE;
		DEBUG_LOG(("LiveObserver::parseMetadataMessage() - metadata parsed, frame=%u", m_streamerFrame.load()));
	}
	else if (msgType == "disconnect")
	{
		// Streamer disconnected — we'll keep buffering from reconnect
		DEBUG_LOG(("LiveObserver::parseMetadataMessage() - streamer disconnected"));
	}
}

// ============================================================================
// deserializeFrame — convert binary payload to LiveFrameData and buffer it
// ============================================================================

void LiveObserver::deserializeFrame(UnsignedInt frameNum, const char* payload, Int payloadSize)
{
	LiveFrameData fd;
	fd.frameNumber = frameNum;
	fd.serializedCommands.assign(payload, payload + payloadSize);

	std::lock_guard<std::mutex> lock(m_pendingMutex);

	// Insert in sorted order by frame number
	auto it = m_pendingFrames.begin();
	while (it != m_pendingFrames.end() && it->frameNumber < frameNum)
		++it;

	// If a frame with this number already exists, replace it
	if (it != m_pendingFrames.end() && it->frameNumber == frameNum)
	{
		*it = fd;
	}
	else
	{
		m_pendingFrames.insert(it, fd);
	}

	// Limit buffer size to prevent memory issues (keep at most 300 frames = 5 seconds at 60fps)
	while (m_pendingFrames.size() > 300)
	{
		m_pendingFrames.pop_front();
	}
}

#endif // GENERALS_ONLINE
