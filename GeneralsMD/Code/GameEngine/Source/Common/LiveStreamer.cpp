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
#include "Common/Recorder.h"

#include "GameNetwork/GameMessageParser.h"
#include "GameNetwork/GameInfo.h"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/multi.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/websockets.h"

#include <algorithm>
#include <cstdio>
#include <cstdarg>

extern GameLogic* TheGameLogic;
extern CommandList* TheCommandList;

// ============================================================================
// liveStreamLog — write diagnostic messages to live_streamer_debug.log
// ============================================================================
void liveStreamLog(const char* fmt, ...) {
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
// liveStreamerInitLog — write initial config header to live_streamer_debug.log
// Called at game start BEFORE the streaming decision, so the log is ALWAYS
// created even if LiveStreamEnabled is false.
// ============================================================================
void liveStreamerInitLog() {
    liveStreamLog("=== Live Stream Init ===\n");
    if (TheGlobalData) {
        liveStreamLog("LiveStreamEnabled: %s\n", TheGlobalData->m_liveStreamEnabled ? "true" : "false");
        liveStreamLog("LiveStreamRelayUrl: %s\n", TheGlobalData->m_liveStreamRelayUrl.str());
        liveStreamLog("LiveStreamCanStream: %s\n", TheGlobalData->m_liveStreamCanStream ? "true" : "false");
    } else {
        liveStreamLog("TheGlobalData is NULL — cannot read config\n");
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
// jsonEscape — escape special characters for safe JSON string values
// ============================================================================

AsciiString LiveStreamer::jsonEscape(const AsciiString& raw)
{
	AsciiString result;
	const char* src = raw.str();
	
	while (*src)
	{
		switch (*src)
		{
			case '\\': result.concat('\\'); result.concat('\\'); break;
			case '"':  result.concat('\\'); result.concat('"');  break;
			case '\n': result.concat('\\'); result.concat('n');  break;
			case '\r': result.concat('\\'); result.concat('r');  break;
			case '	': result.concat('\\'); result.concat('t');  break;
			default:   result.concat(*src); break;
		}
		src++;
	}
	
	return result;
}

// ============================================================================
// init — set relay URL and start the background network thread
// ============================================================================

void LiveStreamer::init(const AsciiString& relayUrl)
{
	if (m_shouldRun)
	{
		liveStreamLog("LiveStreamer::init: already running, skipping\n");
		return;
	}

	m_relayUrl = relayUrl;
	m_shouldRun = TRUE;
	m_connected = FALSE;
	m_isStreaming = FALSE;
	m_isBackup = FALSE;

	DEBUG_LOG(("LiveStreamer::init() - connecting to %s", relayUrl.str()));
	liveStreamLog("LiveStreamer::init: relayUrl=%s\n", relayUrl.str());
	liveStreamLog("LiveStreamer::init: m_shouldRun=%d, m_connected=%d\n", m_shouldRun.load(), m_connected.load());

	// Start the background network thread
	m_networkThread = std::thread(&LiveStreamer::networkThreadFunc, this);
	liveStreamLog("LiveStreamer::init: network thread launched (id=%lu)\n",
		(unsigned long)m_networkThread.native_handle());
}

// ============================================================================
// close — shut down the background thread
// ============================================================================

void LiveStreamer::close()
{
	if (!m_shouldRun)
	{
		liveStreamLog("LiveStreamer::close: not running, skipping\n");
		return;
	}

	liveStreamLog("LiveStreamer::close: shutting down (connected=%d, isStreaming=%d, isBackup=%d)\n",
		m_connected.load(), m_isStreaming.load(), m_isBackup.load());
	m_shouldRun = FALSE;
	m_isStreaming = FALSE;
	m_isBackup = FALSE;

	if (m_networkThread.joinable())
	{
		liveStreamLog("LiveStreamer::close: joining network thread...\n");
		m_networkThread.join();
		liveStreamLog("LiveStreamer::close: network thread joined\n");
	}

	m_connected = FALSE;
	DEBUG_LOG(("LiveStreamer::close() - shut down"));
	liveStreamLog("LiveStreamer::close: done\n");
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
		jsonEscape(gameHash).str(),
		jsonEscape(playerName).str(),
		jsonEscape(mapName).str(),
		jsonEscape(gameMode).str(),
		canStream ? "true" : "false");

	// Log registration details
	liveStreamLog("LiveStreamer::registerForGame: game_hash=%.200s\n", gameHash.str());
	liveStreamLog("LiveStreamer::registerForGame: player=%.200s can_stream=%d\n", playerName.str(), canStream);
	liveStreamLog("LiveStreamer::registerForGame: registration JSON=%.200s\n", json.str());

	// Queue for sending on the network thread
	QueuedMessage msg;
	msg.isBinary = FALSE;
	const char* jsonStr = json.str();
	msg.data.assign(jsonStr, jsonStr + strlen(jsonStr) + 1);  // +1 for null terminator

	std::lock_guard<std::mutex> lock(m_sendMutex);
	m_outgoingQueue.push(msg);

	liveStreamLog("LiveStreamer::registerForGame: queued %d bytes\n", (int)msg.data.size());
	DEBUG_LOG(("LiveStreamer::registerForGame() - hash=%s player=%s", gameHash.str(), playerName.str()));
}

// ============================================================================
// onRoleAssigned — relay tells us our role
// ============================================================================

void LiveStreamer::onRoleAssigned(const AsciiString& role, const AsciiString& gameId)
{
	m_gameId = gameId;
	liveStreamLog("LiveStreamer::onRoleAssigned: role=%.100s gameId=%.100s\n", role.str(), gameId.str());

	if (role == "streamer")
	{
		m_isStreaming = TRUE;
		m_isBackup = FALSE;
		DEBUG_LOG(("LiveStreamer::onRoleAssigned() - STREAMER for game %s", gameId.str()));
		liveStreamLog("onRoleAssigned() - STREAMER for game %s\n", gameId.str());
	}
	else if (role == "backup")
	{
		m_isStreaming = FALSE;
		m_isBackup = TRUE;
		DEBUG_LOG(("LiveStreamer::onRoleAssigned() - BACKUP for game %s", gameId.str()));
		liveStreamLog("onRoleAssigned() - BACKUP for game %s\n", gameId.str());
	}
	else
	{
		m_isStreaming = FALSE;
		m_isBackup = FALSE;
		DEBUG_LOG(("LiveStreamer::onRoleAssigned() - OBSERVER for game %s", gameId.str()));
		liveStreamLog("onRoleAssigned() - OBSERVER for game %s\n", gameId.str());
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
	liveStreamLog("onTakeover() - now STREAMER for game %s\n", m_gameId.str());
}

// ============================================================================
// sendMetadata — send game info to relay server
// ============================================================================

void LiveStreamer::sendMetadata()
{
	if (!m_connected || !m_isStreaming)
	{
		liveStreamLog("LiveStreamer::sendMetadata: skipped (connected=%d, isStreaming=%d)\n",
			m_connected.load(), m_isStreaming.load());
		return;
	}

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
					i, jsonEscape(nameAscii).str(), p->getPlayerIndex());
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

	// Get the full game options string so observers can reconstruct the game state.
	// This includes player slots, teams, map, seed — everything needed to call
	// ParseAsciiStringToGameInfo() on the observer side.
	// During live play, the active game info is in TheGameInfo (set by network layer).
	// TheRecorder->getGameInfo() is only populated during replay playback.
	AsciiString gameOptions;
	if (TheGameInfo)
	{
		gameOptions = GameInfoToAsciiString(TheGameInfo);
	}
	else if (TheRecorder)
	{
		gameOptions = GameInfoToAsciiString(TheRecorder->getGameInfo());
	}

	AsciiString json;
	json.format(
		"{\"type\":\"metadata\",\"game_hash\":\"%s\",\"game_id\":\"%s\","
		"\"map_name\":\"%s\",\"players\":%s,"
		"\"game_options\":\"%s\","
		"\"exe_crc\":%u,\"ini_crc\":%u,\"current_frame\":%u}",
		jsonEscape(m_gameHash).str(),
		jsonEscape(m_gameId).str(),
		jsonEscape(mapName).str(),
		playersJson.str(),
		jsonEscape(gameOptions).str(),
		exeCRC,
		iniCRC,
		currentFrame);

	QueuedMessage msg;
	msg.isBinary = FALSE;
	const char* jsonStr = json.str();
	msg.data.assign(jsonStr, jsonStr + strlen(jsonStr) + 1);  // +1 for null terminator

	std::lock_guard<std::mutex> lock(m_sendMutex);
	m_outgoingQueue.push(msg);

	DEBUG_LOG(("LiveStreamer::sendMetadata() - sent metadata for frame %u", currentFrame));
	liveStreamLog("LiveStreamer::sendMetadata: map=%.100s frame=%u json_size=%d\n",
		mapName.str(), currentFrame, (int)json.getLength());
}

// ============================================================================
// streamFrame — serialize and queue a frame for sending
// ============================================================================

void LiveStreamer::streamFrame(UnsignedInt frame, GameMessage* cmdList, Int currentFps)
{
	if (!m_connected || !m_isStreaming)
	{
		liveStreamLog("LiveStreamer::streamFrame: skipped (connected=%d, isStreaming=%d)\n",
			m_connected.load(), m_isStreaming.load());
		return;
	}

	// Throttle metadata sends — once every 5 seconds (300 frames at 60fps)
	if (m_lastMetadataFrame == 0 || (frame - m_lastMetadataFrame) > (UnsignedInt)(currentFps * 5))
	{
		sendMetadata();
		m_lastMetadataFrame = frame;
	}

	// Serialize the frame commands into a binary buffer
	std::vector<char> frameBuffer;
	serializeFrame(frame, cmdList, frameBuffer);

	// Build a JSON frame message — always send, even if empty.
	// Empty frames are essential for lockstep simulation: the observer needs
	// every frame number so waitForFrame() doesn't stall.
	AsciiString jsonFrame;
	if (frameBuffer.empty())
	{
		// Placeholder for a frame with no commands
		jsonFrame.format(
			"{\"type\":\"frame\",\"frame\":%u,\"fps\":%d,\"commands\":\"\"}",
			frame, currentFps);
	}
	else
	{
		// Base64-encode the binary commands
		AsciiString b64Commands;
		base64Encode(frameBuffer.data(), frameBuffer.size(), b64Commands);

		jsonFrame.format(
			"{\"type\":\"frame\",\"frame\":%u,\"fps\":%d,\"commands\":\"%s\"}",
			frame, currentFps, b64Commands.str());
	}

	// Queue as text message
	QueuedMessage msg;
	msg.isBinary = FALSE;
	const char* jsonStr = jsonFrame.str();
	msg.data.assign(jsonStr, jsonStr + strlen(jsonStr) + 1);  // +1 for null terminator

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
		liveStreamLog("LiveStreamer::serializeArgument: unknown type %d\n", argType);
		break;
	}
}

// ============================================================================
// networkThreadFunc — background thread for all network I/O
// ============================================================================

void LiveStreamer::networkThreadFunc()
{
	DEBUG_LOG(("LiveStreamer::networkThreadFunc() - thread started"));
	liveStreamLog("LiveStreamer: network thread started\n");

	// Initialize CURL
	liveStreamLog("LiveStreamer: curl_global_init(CURL_GLOBAL_DEFAULT)\n");
	curl_global_init(CURL_GLOBAL_DEFAULT);

	if (!connectToRelay())
	{
		DEBUG_LOG(("LiveStreamer::networkThreadFunc() - failed to connect to relay"));
		liveStreamLog("LiveStreamer: network thread FAILED to connect to relay, exiting\n");
		m_shouldRun = FALSE;
		curl_global_cleanup();
		return;
	}
	liveStreamLog("LiveStreamer: connected to relay, entering main loop\n");

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
					sendJsonMessage(AsciiString(msg.data.data(), (Int)msg.data.size()));
				}
				m_outgoingQueue.pop();
			}
		}

		// --- Receive incoming messages ---
		std::vector<char> recvBuffer;
		liveStreamLog("LiveStreamer: wsRecv called, buffer state: size=%d\n", (int)recvBuffer.size());
		if (wsRecv(recvBuffer) && !recvBuffer.empty())
		{
			liveStreamLog("LiveStreamer: received %d bytes from relay\n", (int)recvBuffer.size());
			// Parse incoming JSON messages
			// Expected: {"type":"role","role":"streamer","gameId":"..."}
			AsciiString incoming(recvBuffer.data(), (Int)recvBuffer.size());

			if (findSubstring(incoming, "role") != -1)
			{
				liveStreamLog("LiveStreamer: incoming message contains 'role'\n");
				// Extract role and gameId from JSON (simple parsing)
				AsciiString role;
				AsciiString gameId;

				Int rolePos = findSubstring(incoming, "\"role\":\"");
				if (rolePos != -1)
				{
					Int start = rolePos + 8;
					Int end = findSubstring(incoming, "\"", start);
					if (end != -1)
					{
						role = AsciiString(incoming.str() + start, end - start);
						liveStreamLog("LiveStreamer: parsed role=%.50s\n", role.str());
					}
				}

				Int idPos = findSubstring(incoming, "\"game_id\":\"");
				if (idPos != -1)
				{
					Int start = idPos + 10;
					Int end = findSubstring(incoming, "\"", start);
					if (end != -1)
					{
						gameId = AsciiString(incoming.str() + start, end - start);
						liveStreamLog("LiveStreamer: parsed game_id=%.50s\n", gameId.str());
					}
				}

				if (!role.isEmpty())
				{
					liveStreamLog("LiveStreamer: calling onRoleAssigned(%.50s, %.50s)\n", role.str(), gameId.str());
					onRoleAssigned(role, gameId);
				}
				else
				{
					liveStreamLog("LiveStreamer: role string empty, skipping onRoleAssigned\n");
				}
			}
			else
			{
				liveStreamLog("LiveStreamer: incoming message does not contain 'role', ignoring\n");
			}
		}

		// Small sleep to avoid busy-waiting (1ms)
		Sleep(1);
	}

	// Cleanup
	liveStreamLog("LiveStreamer: cleaning up CURL handles\n");
	if (m_curlEasy)
	{
		liveStreamLog("LiveStreamer: curl_easy_cleanup\n");
		curl_easy_cleanup((CURL*)m_curlEasy);
		m_curlEasy = nullptr;
	}
	if (m_curlMulti)
	{
		liveStreamLog("LiveStreamer: curl_multi_cleanup\n");
		curl_multi_cleanup((CURLM*)m_curlMulti);
		m_curlMulti = nullptr;
	}

	liveStreamLog("LiveStreamer: curl_global_cleanup\n");
	curl_global_cleanup();

	m_connected = FALSE;
	DEBUG_LOG(("LiveStreamer::networkThreadFunc() - thread exiting"));
	liveStreamLog("LiveStreamer: network thread exiting\n");
}

// ============================================================================
// connectToRelay — establish WebSocket connection (called from network thread)
// ============================================================================

bool LiveStreamer::connectToRelay()
{
	liveStreamLog("LiveStreamer::connectToRelay: connecting to %.200s...\n", m_relayUrl.str());
	CURL* easy = curl_easy_init();
	if (!easy)
	{
		DEBUG_LOG(("LiveStreamer::connectToRelay() - curl_easy_init failed"));
		liveStreamLog("LiveStreamer::connectToRelay: curl_easy_init FAILED\n");
		return false;
	}
	liveStreamLog("LiveStreamer::connectToRelay: curl_easy_init=%p (success)\n", (void*)easy);

	CURLM* multi = curl_multi_init();
	if (!multi)
	{
		DEBUG_LOG(("LiveStreamer::connectToRelay() - curl_multi_init failed"));
		liveStreamLog("LiveStreamer::connectToRelay: curl_multi_init FAILED\n");
		curl_easy_cleanup(easy);
		return false;
	}
	liveStreamLog("LiveStreamer::connectToRelay: curl_multi_init=%p (success)\n", (void*)multi);

	// Ensure the URL always ends with /register — the relay server only
	// accepts WebSocket connections on that path.  If the user supplied a
	// bare host:port (e.g. "ws://192.168.2.108:8765") we append "/register".
	{
		AsciiString effectiveUrl = m_relayUrl;
		const char* urlStr = m_relayUrl.str();
		Int schemeEnd = findSubstring(m_relayUrl, "://");
		if (schemeEnd != -1)
		{
			Int pathStart = findSubstring(m_relayUrl, "/", schemeEnd + 3);
			AsciiString pathPart;
			if (pathStart != -1)
			{
				pathPart = AsciiString(urlStr + pathStart);
			}

			if (pathPart != "/register")
			{
				// Rebuild: scheme://host[:port]/register
				effectiveUrl.clear();
				effectiveUrl.concat(AsciiString(urlStr, pathStart != -1 ? pathStart : (Int)strlen(urlStr)));
				effectiveUrl.concat("/register");
				liveStreamLog("LiveStreamer::connectToRelay: URL had no /register path, rewritten to %.200s\n",
					effectiveUrl.str());
			}
		}
		else
		{
			// No :// found — malformed but try appending anyway
			effectiveUrl.concat("/register");
			liveStreamLog("LiveStreamer::connectToRelay: no scheme found, appended /register: %.200s\n",
				effectiveUrl.str());
		}

		liveStreamLog("LiveStreamer::connectToRelay: curl_easy_setopt CURLOPT_URL=%.200s\n", effectiveUrl.str());
		curl_easy_setopt(easy, CURLOPT_URL, effectiveUrl.str());
	}
	liveStreamLog("LiveStreamer::connectToRelay: curl_easy_setopt CURLOPT_CONNECT_ONLY=2L\n");
	curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L); // 2 = use WebSocket protocol
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
	liveStreamLog("LiveStreamer::connectToRelay: SSL verification disabled\n");

	// Add to multi handle
	liveStreamLog("LiveStreamer::connectToRelay: curl_multi_add_handle\n");
	curl_multi_add_handle(multi, easy);

	// Perform the connection — loop until the WebSocket handshake completes
	// or we time out.  The correct curl_multi pattern requires repeated
	// perform→wait cycles; a single cycle is not sufficient because the
	// WebSocket upgrade may not finish within the first 1 s wait.
	int runningHandles = 0;
	liveStreamLog("LiveStreamer::connectToRelay: curl_multi_perform (loop)\n");
	CURLMcode mc = curl_multi_perform(multi, &runningHandles);
	liveStreamLog("LiveStreamer::connectToRelay: curl_multi_perform returned mc=%d, runningHandles=%d\n", (int)mc, runningHandles);

	int numfds = 0;
	const int kConnectTimeoutMs = 10000; // 10 second total connect timeout
	int elapsedMs = 0;
	const int kPollIntervalMs = 250;

	while (runningHandles > 0 && mc == CURLM_OK && elapsedMs < kConnectTimeoutMs)
	{
		mc = curl_multi_wait(multi, nullptr, 0, kPollIntervalMs, &numfds);
		liveStreamLog("LiveStreamer::connectToRelay: curl_multi_wait returned mc=%d, numfds=%d (elapsed %dms)\n",
			(int)mc, numfds, elapsedMs);
		elapsedMs += kPollIntervalMs;
		if (mc != CURLM_OK)
			break;
		mc = curl_multi_perform(multi, &runningHandles);
		liveStreamLog("LiveStreamer::connectToRelay: curl_multi_perform returned mc=%d, runningHandles=%d\n",
			(int)mc, runningHandles);
	}

	// Check if connected
	CURLMsg* infoMsg = curl_multi_info_read(multi, &runningHandles);
	if (infoMsg)
	{
		CURLcode res = infoMsg->data.result;
		if (res != CURLE_OK)
		{
			DEBUG_LOG(("LiveStreamer::connectToRelay() - connection failed: %s", curl_easy_strerror(res)));
			liveStreamLog("LiveStreamer::connectToRelay: FAILED: %s (curl code %d)\n",
				curl_easy_strerror(res), (int)res);
			curl_multi_remove_handle(multi, easy);
			curl_easy_cleanup(easy);
			curl_multi_cleanup(multi);
			return false;
		}
	}
	else
	{
		// curl_multi_info_read returned NULL — the connection did NOT complete.
		// Do NOT set m_connected = TRUE; treat this as a timeout / failure.
		liveStreamLog("LiveStreamer::connectToRelay: connection timed out (no info message after %dms, runningHandles=%d)\n",
			elapsedMs, runningHandles);
		curl_multi_remove_handle(multi, easy);
		curl_easy_cleanup(easy);
		curl_multi_cleanup(multi);
		return false;
	}

	m_curlEasy = easy;
	m_curlMulti = multi;
	m_connected = TRUE;

	DEBUG_LOG(("LiveStreamer::connectToRelay() - connected to %s", m_relayUrl.str()));
	liveStreamLog("LiveStreamer::connectToRelay: connected! easy=%p multi=%p\n", (void*)easy, (void*)multi);
	return true;
}

// ============================================================================
// wsSend — send data over WebSocket
// ============================================================================

bool LiveStreamer::wsSend(const void* data, size_t len)
{
	if (!m_curlEasy || !m_connected)
	{
		liveStreamLog("LiveStreamer::wsSend: skipped (curlEasy=%p, connected=%d)\n",
			m_curlEasy, m_connected.load());
		return false;
	}

	liveStreamLog("LiveStreamer::wsSend: sending %d bytes (binary)\n", (int)len);
	size_t sent = 0;
	CURLcode res = curl_ws_send((CURL*)m_curlEasy, data, len, &sent, 0, CURLWS_BINARY);
	liveStreamLog("LiveStreamer::wsSend: curl_ws_send returned %d, sent=%d\n", (int)res, (int)sent);

	if (res == CURLE_OK)
		return true;

	// CURLE_AGAIN means the socket would block — retry later
	if (res == CURLE_AGAIN)
	{
		liveStreamLog("LiveStreamer::wsSend: CURLE_AGAIN, will retry next tick\n");
		return true; // not a real error, will retry next tick
	}

	DEBUG_LOG(("LiveStreamer::wsSend() - error: %s", curl_easy_strerror(res)));
	liveStreamLog("LiveStreamer::wsSend: ERROR: %s (curl code %d)\n", curl_easy_strerror(res), (int)res);
	m_connected = FALSE;
	return false;
}

// ============================================================================
// wsRecv — receive data from WebSocket (non-blocking)
// ============================================================================

bool LiveStreamer::wsRecv(std::vector<char>& outBuffer)
{
	if (!m_curlEasy || !m_connected)
	{
		liveStreamLog("LiveStreamer::wsRecv: skipped (curlEasy=%p, connected=%d)\n",
			m_curlEasy, m_connected.load());
		return false;
	}

	liveStreamLog("LiveStreamer::wsRecv called, buffer state: size=%d\n", (int)outBuffer.size());
	char buf[4096];
	size_t nread = 0;
	const struct curl_ws_frame* meta = nullptr;

	CURLcode res = curl_ws_recv((CURL*)m_curlEasy, buf, sizeof(buf), &nread, &meta);
	liveStreamLog("LiveStreamer::wsRecv: curl_ws_recv returned %d, nread=%d\n", (int)res, (int)nread);

	if (res == CURLE_OK && nread > 0)
	{
		liveStreamLog("LiveStreamer::wsRecv: received %d bytes, first 100 chars: %.100s\n",
			(int)nread, buf);
		outBuffer.assign(buf, buf + nread);
		return true;
	}

	if (res == CURLE_AGAIN)
	{
		liveStreamLog("LiveStreamer::wsRecv: CURLE_AGAIN (no data available)\n");
		return false; // no data available
	}

	if (res != CURLE_OK)
	{
		DEBUG_LOG(("LiveStreamer::wsRecv() - error: %s", curl_easy_strerror(res)));
		liveStreamLog("LiveStreamer::wsRecv: ERROR: %s (curl code %d)\n", curl_easy_strerror(res), (int)res);
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
	{
		liveStreamLog("LiveStreamer::sendJsonMessage: skipped (curlEasy=%p, connected=%d)\n",
			m_curlEasy, m_connected.load());
		return false;
	}
	liveStreamLog("LiveStreamer::sendJsonMessage: sending %d bytes: %.200s\n",
		(int)strlen(jsonMsg.str()), jsonMsg.str());
	size_t sent = 0;
	CURLcode res = curl_ws_send((CURL*)m_curlEasy, jsonMsg.str(), strlen(jsonMsg.str()), &sent, 0, CURLWS_TEXT);
	liveStreamLog("LiveStreamer::sendJsonMessage: curl_ws_send result=%d, sent=%d\n", (int)res, (int)sent);
	if (res == CURLE_OK)
		return true;
	if (res == CURLE_AGAIN)
	{
		liveStreamLog("LiveStreamer::sendJsonMessage: CURLE_AGAIN, will retry\n");
		return true;
	}
	DEBUG_LOG(("LiveStreamer::sendJsonMessage() - error: %s", curl_easy_strerror(res)));
	liveStreamLog("LiveStreamer::sendJsonMessage: ERROR: %s (curl code %d)\n", curl_easy_strerror(res), (int)res);
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
