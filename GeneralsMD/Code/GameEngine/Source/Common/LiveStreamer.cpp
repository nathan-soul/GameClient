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

#include "Common/LiveStreamer.h"
#include "Common/MessageStream.h"
#include "Common/GlobalData.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/GameEngine.h"
#include "GameLogic/GameLogic.h"
#include "Common/AsciiString.h"
#include "Common/CRCDebug.h"

#include "GameNetwork/GameMessageParser.h"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/multi.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/websockets.h"

#include <algorithm>
#include <cstdio>
#include <cstdarg>

extern GameLogic* TheGameLogic;
extern CommandList* TheCommandList;

// ============================================================================
// liveDebugLog — write diagnostic messages to live_streamer_debug.log
// ============================================================================
static void liveDebugLog(const char* fmt, ...) {
    static FILE* logFile = NULL;
    if (!logFile) {
        logFile = fopen("live_streamer_debug.log", "w");
    }
    if (logFile) {
        va_list args;
        va_start(args, fmt);
        vfprintf(logFile, fmt, args);
        va_end(args);
        fflush(logFile);
    }
}

// ============================================================================
// findSubstring — wrapper around strstr() for AsciiString (AsciiString::find
// only takes a single char, not a string pattern)
// ============================================================================
static Int findSubstring(const AsciiString& haystack, const char* needle, Int startPos = 0)
{
	const char* str = haystack.str();
	if (!str || !needle || startPos < 0)
		return -1;
	Int len = (Int)strlen(str);
	if (startPos >= len)
		return -1;
	const char* found = strstr(str + startPos, needle);
	if (!found)
		return -1;
	return (Int)(found - str);
}

// ============================================================================
// base64Encode — encode binary data to base64 string
// ============================================================================

static void base64Encode(const char* data, size_t len, AsciiString& out)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	out.clear();
	unsigned char input[3];
	unsigned char output[4];
	int inputIdx = 0;

	for (size_t i = 0; i < len; i++)
	{
		input[inputIdx++] = (unsigned char)data[i];
		if (inputIdx == 3)
		{
			output[0] = (input[0] & 0xfc) >> 2;
			output[1] = ((input[0] & 0x03) << 4) | ((input[1] & 0xf0) >> 4);
			output[2] = ((input[1] & 0x0f) << 2) | ((input[2] & 0xc0) >> 6);
			output[3] = input[2] & 0x3f;
			char buf[5] = { table[output[0]], table[output[1]], table[output[2]], table[output[3]], 0 };
			out.concat(buf);
			inputIdx = 0;
		}
	}
	if (inputIdx > 0)
	{
		for (int j = inputIdx; j < 3; j++)
			input[j] = 0;
		output[0] = (input[0] & 0xfc) >> 2;
		output[1] = ((input[0] & 0x03) << 4) | ((input[1] & 0xf0) >> 4);
		output[2] = ((input[1] & 0x0f) << 2) | ((input[2] & 0xc0) >> 6);
		char buf[5] = { table[output[0]], table[output[1]], '=', '=', 0 };
		if (inputIdx >= 2) buf[2] = table[output[2]];
		out.concat(buf);
	}
}

/**
 * The singleton live streamer instance.
 */
LiveStreamer* TheLiveStreamer = nullptr;

LiveStreamer* createLiveStreamer()
{
	return MSGNEW("LiveStreamer") LiveStreamer();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

LiveStreamer::LiveStreamer()
	: m_isStreaming(FALSE)
	, m_isBackup(FALSE)
	, m_connected(FALSE)
	, m_shouldRun(FALSE)
	, m_curlEasy(nullptr)
	, m_curlMulti(nullptr)
	, m_lastMetadataFrame(0)
{
}

LiveStreamer::~LiveStreamer()
{
	close();
}

// ============================================================================
// computeGameHash — deterministic hash from game parameters
// ============================================================================

AsciiString LiveStreamer::computeGameHash(
	const AsciiString& mapName,
	const AsciiString& gameMode,
	UnsignedInt startTime,
	const AsciiString& sortedPlayerNames)
{
	// Simple FNV-1a hash — deterministic, fast, good enough for game identification.
	// All clients in the same game produce the same hash.
	AsciiString input;
	input.format("%s|%s|%u|%s", mapName.str(), gameMode.str(), startTime, sortedPlayerNames.str());

	UnsignedInt hash = 2166136261u; // FNV offset basis
	const char* p = input.str();
	while (*p)
	{
		hash ^= (UnsignedInt)(UnsignedByte)(*p);
		hash *= 16777619u; // FNV prime
		++p;
	}

	AsciiString result;
	result.format("%08X", hash);
	return result;
}

// ============================================================================
// init — set relay URL and start the background network thread
// ============================================================================

void LiveStreamer::init(const AsciiString& relayUrl)
{
	if (m_shouldRun)
		return;

	m_relayUrl = relayUrl;
	m_shouldRun = TRUE;
	m_connected = FALSE;
	m_isStreaming = FALSE;
	m_isBackup = FALSE;

	DEBUG_LOG(("LiveStreamer::init() - connecting to %s", relayUrl.str()));
	liveDebugLog("init() - relay=%s\n", relayUrl.str());

	// Start the background network thread
	m_networkThread = std::thread(&LiveStreamer::networkThreadFunc, this);
}

// ============================================================================
// close — shut down the background thread
// ============================================================================

void LiveStreamer::close()
{
	if (!m_shouldRun)
		return;

	m_shouldRun = FALSE;
	m_isStreaming = FALSE;
	m_isBackup = FALSE;

	if (m_networkThread.joinable())
	{
		m_networkThread.join();
	}

	m_connected = FALSE;
	DEBUG_LOG(("LiveStreamer::close() - shut down"));
	liveDebugLog("close() - shutting down\n");
}

// ============================================================================
// registerForGame — tell the relay server about this game
// ============================================================================

void LiveStreamer::registerForGame(
	const AsciiString& gameHash,
	const AsciiString& playerName,
	const AsciiString& mapName,
	const AsciiString& gameMode,
	Bool canStream)
{
	m_gameHash = gameHash;
	m_playerName = playerName;

	// Build JSON registration message
	AsciiString json;
	json.format(
		"{\"type\":\"register\",\"game_hash\":\"%s\",\"player_name\":\"%s\","
		"\"map_name\":\"%s\",\"gameMode\":\"%s\",\"can_stream\":%s}",
		gameHash.str(),
		playerName.str(),
		mapName.str(),
		gameMode.str(),
		canStream ? "true" : "false");

	// Queue for sending on the network thread
	QueuedMessage msg;
	msg.isBinary = FALSE;
	const char* jsonStr = json.str();
	msg.data.assign(jsonStr, jsonStr + strlen(jsonStr));

	std::lock_guard<std::mutex> lock(m_sendMutex);
	m_outgoingQueue.push(msg);

	DEBUG_LOG(("LiveStreamer::registerForGame() - hash=%s player=%s", gameHash.str(), playerName.str()));
	liveDebugLog("registerForGame() - hash=%s player=%s can_stream=%s\n",
		gameHash.str(), playerName.str(), canStream ? "true" : "false");
}

// ============================================================================
// onRoleAssigned — relay tells us our role
// ============================================================================

void LiveStreamer::onRoleAssigned(const AsciiString& role, const AsciiString& gameId)
{
	m_gameId = gameId;

	if (role == "streamer")
	{
		m_isStreaming = TRUE;
		m_isBackup = FALSE;
		DEBUG_LOG(("LiveStreamer::onRoleAssigned() - STREAMER for game %s", gameId.str()));
		liveDebugLog("onRoleAssigned() - STREAMER for game %s\n", gameId.str());
	}
	else if (role == "backup")
	{
		m_isStreaming = FALSE;
		m_isBackup = TRUE;
		DEBUG_LOG(("LiveStreamer::onRoleAssigned() - BACKUP for game %s", gameId.str()));
		liveDebugLog("onRoleAssigned() - BACKUP for game %s\n", gameId.str());
	}
	else
	{
		m_isStreaming = FALSE;
		m_isBackup = FALSE;
		DEBUG_LOG(("LiveStreamer::onRoleAssigned() - OBSERVER for game %s", gameId.str()));
		liveDebugLog("onRoleAssigned() - OBSERVER for game %s\n", gameId.str());
	}
}

// ============================================================================
// onTakeover — backup becomes the active streamer
// ============================================================================

void LiveStreamer::onTakeover()
{
	m_isStreaming = TRUE;
	m_isBackup = FALSE;
	DEBUG_LOG(("LiveStreamer::onTakeover() - now STREAMER for game %s", m_gameId.str()));
	liveDebugLog("onTakeover() - now STREAMER for game %s\n", m_gameId.str());
}

// ============================================================================
// sendMetadata — send game info to relay server
// ============================================================================

void LiveStreamer::sendMetadata()
{
	if (!m_connected || !m_isStreaming)
		return;

	// Build metadata JSON
	AsciiString mapName;
	AsciiString gameMode;
	if (TheGlobalData)
	{
		mapName = TheGlobalData->m_mapName;
	}

	// Gather player info
	AsciiString playersJson;
	if (ThePlayerList)
	{
		playersJson = "[";
		Bool first = TRUE;
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			Player* p = ThePlayerList->getNthPlayer(i);
			if (p && p->isPlayerActive())
			{
				if (!first)
					playersJson.concat(",");
				first = FALSE;

				UnicodeString displayName = p->getPlayerDisplayName();
				AsciiString nameAscii;
				nameAscii.translate(displayName);

				AsciiString playerEntry;
				playerEntry.format("{\"slot\":%d,\"name\":\"%s\",\"team\":%d}",
					i, nameAscii.str(), p->getTeam());
				playersJson.concat(playerEntry);
			}
		}
		playersJson.concat("]");
	}
	else
	{
		playersJson = "[]";
	}

	UnsignedInt exeCRC = TheGlobalData ? TheGlobalData->m_exeCRC : 0;
	UnsignedInt iniCRC = TheGlobalData ? TheGlobalData->m_iniCRC : 0;
	UnsignedInt currentFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;

	AsciiString json;
	json.format(
		"{\"type\":\"metadata\",\"game_hash\":\"%s\",\"game_id\":\"%s\","
		"\"map_name\":\"%s\",\"players\":%s,"
		"\"exe_crc\":%u,\"ini_crc\":%u,\"current_frame\":%u}",
		m_gameHash.str(),
		m_gameId.str(),
		mapName.str(),
		playersJson.str(),
		exeCRC,
		iniCRC,
		currentFrame);

	QueuedMessage msg;
	msg.isBinary = FALSE;
	const char* jsonStr = json.str();
	msg.data.assign(jsonStr, jsonStr + strlen(jsonStr));

	std::lock_guard<std::mutex> lock(m_sendMutex);
	m_outgoingQueue.push(msg);

	DEBUG_LOG(("LiveStreamer::sendMetadata() - sent metadata for frame %u", currentFrame));
	liveDebugLog("sendMetadata() - map=%s, frame=%u\n", mapName.str(), currentFrame);
}

// ============================================================================
// streamFrame — serialize and queue a frame for sending
// ============================================================================

void LiveStreamer::streamFrame(UnsignedInt frame, GameMessage* cmdList, Int currentFps)
{
	if (!m_connected || !m_isStreaming)
		return;

	// Throttle metadata sends — once every 5 seconds (300 frames at 60fps)
	if (m_lastMetadataFrame == 0 || (frame - m_lastMetadataFrame) > (UnsignedInt)(currentFps * 5))
	{
		sendMetadata();
		m_lastMetadataFrame = frame;
	}

	// Serialize the frame commands into a binary buffer
	std::vector<char> frameBuffer;
	serializeFrame(frame, cmdList, frameBuffer);

	if (frameBuffer.empty())
		return;

	// Base64-encode the binary commands
	AsciiString b64Commands;
	base64Encode(frameBuffer.data(), frameBuffer.size(), b64Commands);

	// Build a JSON frame message
	AsciiString jsonFrame;
	jsonFrame.format(
		"{\"type\":\"frame\",\"frame\":%u,\"fps\":%d,\"commands\":\"%s\"}",
		frame, currentFps, b64Commands.str());

	// Queue as text message
	QueuedMessage msg;
	msg.isBinary = FALSE;
	const char* jsonStr = jsonFrame.str();
	msg.data.assign(jsonStr, jsonStr + strlen(jsonStr));

	std::lock_guard<std::mutex> lock(m_sendMutex);
	m_outgoingQueue.push(msg);
}

// ============================================================================
// serializeFrame — replicate the .rep file writeToFile() format
// ============================================================================

void LiveStreamer::serializeFrame(UnsignedInt frame, GameMessage* cmdList, std::vector<char>& outBuffer)
{
	// Walk the command list and serialize each network message
	// Same format as RecorderClass::writeToFile():
	//   [4-byte frame][2-byte msg type][4-byte player index]
	//   [1-byte numTypes][type entries...][arguments...]

	GameMessage* msg = cmdList;
	while (msg != nullptr)
	{
		if (msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES &&
			msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES)
		{
			// Frame number
			UnsignedInt f = frame;
			outBuffer.insert(outBuffer.end(), (const char*)&f, (const char*)&f + sizeof(f));

			// Message type (2 bytes — uses the Int enum, written as 4 bytes in .rep)
			GameMessage::Type type = msg->getType();
			outBuffer.insert(outBuffer.end(), (const char*)&type, (const char*)&type + sizeof(type));

			// Player index
			Int playerIndex = msg->getPlayerIndex();
			outBuffer.insert(outBuffer.end(), (const char*)&playerIndex, (const char*)&playerIndex + sizeof(playerIndex));

			// Argument type info (via GameMessageParser)
			GameMessageParser* parser = newInstance(GameMessageParser)(msg);
			UnsignedByte numTypes = (UnsignedByte)parser->getNumTypes();
			outBuffer.push_back((char)numTypes);

			GameMessageParserArgumentType* argType = parser->getFirstArgumentType();
			while (argType != nullptr)
			{
				UnsignedByte t = (UnsignedByte)(argType->getType());
				outBuffer.push_back((char)t);

				UnsignedByte argCount = (UnsignedByte)(argType->getArgCount());
				outBuffer.push_back((char)argCount);

				argType = argType->getNext();
			}

			// Arguments
			Int numArgs = msg->getArgumentCount();
			for (Int i = 0; i < numArgs; ++i)
			{
				serializeArgument(msg->getArgumentDataType(i), msg->getArgument(i), outBuffer);
			}

			deleteInstance(parser);
			parser = nullptr;
		}
		msg = msg->next();
	}
}

// ============================================================================
// serializeArgument — replicate writeArgument()
// ============================================================================

void LiveStreamer::serializeArgument(Int argType, const void* argData, std::vector<char>& outBuffer)
{
	switch (argType)
	{
	case ARGUMENTDATATYPE_INTEGER:
		{
			Int val = *((const Int*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_REAL:
		{
			Real val = *((const Real*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_BOOLEAN:
		{
			Bool val = *((const Bool*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_OBJECTID:
		{
			ObjectID val = *((const ObjectID*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_DRAWABLEID:
		{
			DrawableID val = *((const DrawableID*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_TEAMID:
		{
			UnsignedInt val = *((const UnsignedInt*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_LOCATION:
		{
			Coord3D val = *((const Coord3D*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_PIXEL:
		{
			ICoord2D val = *((const ICoord2D*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_PIXELREGION:
		{
			IRegion2D val = *((const IRegion2D*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_TIMESTAMP:
		{
			UnsignedInt val = *((const UnsignedInt*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	case ARGUMENTDATATYPE_WIDECHAR:
		{
			WideChar val = *((const WideChar*)argData);
			outBuffer.insert(outBuffer.end(), (const char*)&val, (const char*)&val + sizeof(val));
		}
		break;
	default:
		DEBUG_LOG(("LiveStreamer::serializeArgument - unknown type %d", argType));
		break;
	}
}

// ============================================================================
// networkThreadFunc — background thread for all network I/O
// ============================================================================

void LiveStreamer::networkThreadFunc()
{
	DEBUG_LOG(("LiveStreamer::networkThreadFunc() - thread started"));
	liveDebugLog("networkThreadFunc() - thread started\n");

	// Initialize CURL
	curl_global_init(CURL_GLOBAL_DEFAULT);

	if (!connectToRelay())
	{
		DEBUG_LOG(("LiveStreamer::networkThreadFunc() - failed to connect to relay"));
		liveDebugLog("networkThreadFunc() - FAILED to connect to relay\n");
		m_shouldRun = FALSE;
		curl_global_cleanup();
		return;
	}

	// Main loop: send queued messages, receive responses
	while (m_shouldRun)
	{
		// --- Send outgoing messages ---
		{
			std::lock_guard<std::mutex> lock(m_sendMutex);
			while (!m_outgoingQueue.empty())
			{
				QueuedMessage& msg = m_outgoingQueue.front();
				if (msg.isBinary)
				{
					wsSend(msg.data.data(), msg.data.size());
				}
				else
				{
					sendJsonMessage(AsciiString(msg.data.data()));
				}
				m_outgoingQueue.pop();
			}
		}

		// --- Receive incoming messages ---
		std::vector<char> recvBuffer;
		if (wsRecv(recvBuffer) && !recvBuffer.empty())
		{
			// Parse incoming JSON messages
			// Expected: {"type":"role","role":"streamer","gameId":"..."}
			AsciiString incoming(recvBuffer.data(), (Int)recvBuffer.size());

			if (findSubstring(incoming, "role") != -1)
			{
				// Extract role and gameId from JSON (simple parsing)
				AsciiString role;
				AsciiString gameId;

				Int rolePos = findSubstring(incoming, "\"role\":\"");
				if (rolePos != -1)
				{
					Int start = rolePos + 8;
					Int end = findSubstring(incoming, "\"", start);
					if (end != -1)
						role = AsciiString(incoming.str() + start, end - start);
				}

				Int idPos = findSubstring(incoming, "\"game_id\":\"");
				if (idPos != -1)
				{
					Int start = idPos + 10;
					Int end = findSubstring(incoming, "\"", start);
					if (end != -1)
						gameId = AsciiString(incoming.str() + start, end - start);
				}

				if (!role.isEmpty())
				{
					onRoleAssigned(role, gameId);
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
	DEBUG_LOG(("LiveStreamer::networkThreadFunc() - thread exiting"));
	liveDebugLog("networkThreadFunc() - thread exiting\n");
}

// ============================================================================
// connectToRelay — establish WebSocket connection (called from network thread)
// ============================================================================

bool LiveStreamer::connectToRelay()
{
	CURL* easy = curl_easy_init();
	if (!easy)
	{
		DEBUG_LOG(("LiveStreamer::connectToRelay() - curl_easy_init failed"));
		liveDebugLog("connectToRelay() - curl_easy_init FAILED\n");
		return false;
	}

	CURLM* multi = curl_multi_init();
	if (!multi)
	{
		DEBUG_LOG(("LiveStreamer::connectToRelay() - curl_multi_init failed"));
		liveDebugLog("connectToRelay() - curl_multi_init FAILED\n");
		curl_easy_cleanup(easy);
		return false;
	}

	// Configure the WebSocket connection
	curl_easy_setopt(easy, CURLOPT_URL, m_relayUrl.str());
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
	CURLMsg* infoMsg = curl_multi_info_read(multi, &runningHandles);
	if (infoMsg)
	{
		CURLcode res = infoMsg->data.result;
		if (res != CURLE_OK)
		{
			DEBUG_LOG(("LiveStreamer::connectToRelay() - connection failed: %s", curl_easy_strerror(res)));
			liveDebugLog("connectToRelay() - FAILED: %s\n", curl_easy_strerror(res));
			curl_multi_remove_handle(multi, easy);
			curl_easy_cleanup(easy);
			curl_multi_cleanup(multi);
			return false;
		}
	}

	m_curlEasy = easy;
	m_curlMulti = multi;
	m_connected = TRUE;

	DEBUG_LOG(("LiveStreamer::connectToRelay() - connected to %s", m_relayUrl.str()));
	liveDebugLog("connectToRelay() - connected to %s\n", m_relayUrl.str());
	return true;
}

// ============================================================================
// wsSend — send data over WebSocket
// ============================================================================

bool LiveStreamer::wsSend(const void* data, size_t len)
{
	if (!m_curlEasy || !m_connected)
		return false;

	size_t sent = 0;
	CURLcode res = curl_ws_send((CURL*)m_curlEasy, data, len, &sent, 0, CURLWS_BINARY);

	if (res == CURLE_OK)
		return true;

	// CURLE_AGAIN means the socket would block — retry later
	if (res == CURLE_AGAIN)
		return true; // not a real error, will retry next tick

	DEBUG_LOG(("LiveStreamer::wsSend() - error: %s", curl_easy_strerror(res)));
	liveDebugLog("wsSend() - ERROR: %s\n", curl_easy_strerror(res));
	m_connected = FALSE;
	return false;
}

// ============================================================================
// wsRecv — receive data from WebSocket (non-blocking)
// ============================================================================

bool LiveStreamer::wsRecv(std::vector<char>& outBuffer)
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
		DEBUG_LOG(("LiveStreamer::wsRecv() - error: %s", curl_easy_strerror(res)));
		m_connected = FALSE;
	}

	return false;
}

// ============================================================================
// sendJsonMessage — convenience wrapper
// ============================================================================

bool LiveStreamer::sendJsonMessage(const AsciiString& jsonMsg)
{
	if (!m_curlEasy || !m_connected)
		return false;
	size_t sent = 0;
	CURLcode res = curl_ws_send((CURL*)m_curlEasy, jsonMsg.str(), strlen(jsonMsg.str()), &sent, 0, CURLWS_TEXT);
	if (res == CURLE_OK)
		return true;
	if (res == CURLE_AGAIN)
		return true;
	DEBUG_LOG(("LiveStreamer::sendJsonMessage() - error: %s", curl_easy_strerror(res)));
	m_connected = FALSE;
	return false;
}

// ============================================================================
// tick — called periodically to process incoming messages
// ============================================================================

void LiveStreamer::tick()
{
	// The background thread handles all network I/O.
	// This method is available for any game-thread-side housekeeping
	// if needed in the future.
}
