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
#include "Common/GlobalData.h"
#include "Common/GameCommon.h"		// LIVE_DELAY_SECONDS_DEFAULT / _MAX
#include "GameClient/ClientInstance.h"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/multi.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/websockets.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <fstream>		// cacert.pem presence check, see connectToRelay
#include <algorithm>

// ============================================================================
// liveStreamLog — write diagnostic messages to live_streamer_debug.log
// ============================================================================
// TheSuperHackers @fix Kept in sync with LIVE_OBSERVER_BUILD_TAG in LiveObserver.cpp — bump
// both together so a log file can be matched to the exact build that produced it.
#define LIVE_OBSERVER_BUILD_TAG "2026-08-03-fix11-signed-char-msglen-corruption"

void liveStreamLog(const char* fmt, ...) {
#if !defined(LIVE_OBSERVER_LOGGING)
    // Compiled out by RTS_DEBUG_LIVE_OBSERVER=OFF — see LiveObserver.h.
    (void)fmt;
#else
    static FILE* logFile = NULL;
    if (!logFile) {
        // TheSuperHackers @fix Give this log a per-instance name — see the matching
        // comment in LiveObserver.cpp::liveObserverLog.
        AsciiString path;
        path.format("live_streamer_debug_Instance%.2u.log", rts::ClientInstance::getInstanceId());
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
// LiveStreamer
// ============================================================================
LiveStreamer* TheLiveStreamer = nullptr;

LiveStreamer::LiveStreamer()
    : m_isStreaming(false)
    , m_isBackup(false)
    , m_connected(false)
    , m_shouldRun(false)
    , m_curlEasy(nullptr)
    , m_curlMulti(nullptr)
    , m_bodySentOffset(0)
{
    m_headerBuffer.reserve(4096);
    m_bodyBuffer.reserve(4096);
}

LiveStreamer::~LiveStreamer()
{
    close();
}

LiveStreamer* createLiveStreamer()
{
    return new LiveStreamer();
}

// ============================================================================
// IReplayStreamSink implementation
// ============================================================================

void LiveStreamer::onHeaderBytes(const void* data, Int size)
{
    if (size <= 0)
        return;

    const char* p = static_cast<const char*>(data);
    m_headerBuffer.insert(m_headerBuffer.end(), p, p + size);
}

void LiveStreamer::onHeaderComplete()
{
    if (m_headerBuffer.empty())
        return;

    queueFrame(LIVE_MSG_HEADER, m_headerBuffer.data(), m_headerBuffer.size());
    m_headerBuffer.clear();
}

void LiveStreamer::onHeaderPatch(Int offset, const void* data, Int size)
{
    if (size <= 0)
        return;

    // Encode: 4 bytes offset (LE) + 4 bytes length (LE) + data
    unsigned char patchBuf[8];
    patchBuf[0] = (unsigned char)(offset & 0xFF);
    patchBuf[1] = (unsigned char)((offset >> 8) & 0xFF);
    patchBuf[2] = (unsigned char)((offset >> 16) & 0xFF);
    patchBuf[3] = (unsigned char)((offset >> 24) & 0xFF);
    patchBuf[4] = (unsigned char)(size & 0xFF);
    patchBuf[5] = (unsigned char)((size >> 8) & 0xFF);
    patchBuf[6] = (unsigned char)((size >> 16) & 0xFF);
    patchBuf[7] = (unsigned char)((size >> 24) & 0xFF);

    std::vector<char> payload;
    payload.reserve(8 + size);
    payload.insert(payload.end(), patchBuf, patchBuf + 8);
    payload.insert(payload.end(), static_cast<const char*>(data), static_cast<const char*>(data) + size);

    queueFrame(LIVE_MSG_PATCH, payload.data(), payload.size());
}

void LiveStreamer::onBodyBytes(const void* data, Int size)
{
    if (size <= 0)
        return;

    const char* p = static_cast<const char*>(data);
    m_bodyBuffer.insert(m_bodyBuffer.end(), p, p + size);
}

void LiveStreamer::onBodyFlush()
{
    if (m_bodyBuffer.empty())
        return;

    // Build framed BODY: [8B offset LE][data]
    uint64_t off = m_bodySentOffset;
    unsigned char offBuf[8];
    offBuf[0] = (unsigned char)(off & 0xFF);
    offBuf[1] = (unsigned char)((off >> 8) & 0xFF);
    offBuf[2] = (unsigned char)((off >> 16) & 0xFF);
    offBuf[3] = (unsigned char)((off >> 24) & 0xFF);
    offBuf[4] = (unsigned char)((off >> 32) & 0xFF);
    offBuf[5] = (unsigned char)((off >> 40) & 0xFF);
    offBuf[6] = (unsigned char)((off >> 48) & 0xFF);
    offBuf[7] = (unsigned char)((off >> 56) & 0xFF);

    std::vector<char> framed;
    framed.reserve(8 + m_bodyBuffer.size());
    framed.insert(framed.end(), offBuf, offBuf + 8);
    framed.insert(framed.end(), m_bodyBuffer.begin(), m_bodyBuffer.end());

    queueFrame(LIVE_MSG_BODY, framed.data(), framed.size());
    m_bodySentOffset += m_bodyBuffer.size();
    m_bodyBuffer.clear();
}

void LiveStreamer::onRecordingEnded()
{
    // Flush any remaining body data
    onBodyFlush();

    // Send END signal
    queueFrame(LIVE_MSG_END, nullptr, 0);
}

// ============================================================================
// Network setup
// ============================================================================

void LiveStreamer::init(const AsciiString& relayUrl)
{
    m_relayUrl = relayUrl;
    m_shouldRun.store(true);

    liveStreamLog("LiveStreamer::init connecting to %s\n", relayUrl.str());

    m_networkThread = std::thread(&LiveStreamer::networkThreadFunc, this);
}

void LiveStreamer::close()
{
    m_shouldRun.store(false);

    if (m_networkThread.joinable())
        m_networkThread.join();

    m_connected.store(false);
    m_isStreaming.store(false);
    m_isBackup.store(false);
}

// ============================================================================
// Registration
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

    liveStreamLog("LiveStreamer::registerForGame hash=%s player=%s canStream=%d\n",
        gameHash.str(), playerName.str(), (int)canStream);

    // TheSuperHackers @feature 03/08/2026 The streamer owns the broadcast delay — it is
    // their spoiler window, so it travels with the registration rather than being an
    // observer-side setting. Sourced from TheGlobalData, which the pre-game lobby writes
    // through to, so a per-game override in the lobby takes effect without a restart.
    Int delaySeconds = TheGlobalData
        ? TheGlobalData->m_liveStreamDelaySeconds
        : (Int)LIVE_DELAY_SECONDS_DEFAULT;

    // Build JSON registration payload
    char regJson[1024];
    snprintf(regJson, sizeof(regJson),
        "{\"type\":\"register\",\"game_hash\":\"%s\",\"player_name\":\"%s\","
        "\"map_name\":\"%s\",\"game_mode\":\"%s\",\"can_stream\":%s,\"delay_seconds\":%d}",
        gameHash.str(), playerName.str(), mapName.str(), gameMode.str(),
        canStream ? "true" : "false", delaySeconds);

    liveStreamLog("LiveStreamer::registerForGame delay_seconds=%d\n", delaySeconds);

    // Queue the REGISTER message — the network thread will send it once connected.
    // (Must NOT call sendBinaryFrame directly here because m_connected may still be false.)
    queueFrame(LIVE_MSG_REGISTER, regJson, strlen(regJson));
}

void LiveStreamer::onRoleAssigned(const AsciiString& role, const AsciiString& gameId, uint64_t bodyOffset)
{
    m_gameId = gameId;
    m_isStreaming.store(role == "streamer");
    m_isBackup.store(role == "backup");

    // Track the body offset reported by the relay. For the primary streamer
    // this is 0. For a backup taking over, this is the current body length.
    m_bodySentOffset = bodyOffset;

    liveStreamLog("LiveStreamer::onRoleAssigned role=%s gameId=%s streaming=%d bodyOff=%llu\n",
        role.str(), gameId.str(), (int)m_isStreaming.load(), (unsigned long long)bodyOffset);
}

void LiveStreamer::onTakeover(uint64_t bodyOffset)
{
    m_isStreaming.store(true);
    m_isBackup.store(false);
    m_bodySentOffset = bodyOffset;
    liveStreamLog("LiveStreamer::onTakeover promoted to streamer, bodyOff=%llu\n",
        (unsigned long long)bodyOffset);
}

// ============================================================================
// Compute game hash
// ============================================================================

AsciiString LiveStreamer::computeGameHash(
    const AsciiString& mapName,
    const AsciiString& gameMode,
    UnsignedInt startTime,
    const AsciiString& sortedPlayerNames)
{
    AsciiString raw;
    raw.format("%s|%s|%u|%s", mapName.str(), gameMode.str(), startTime, sortedPlayerNames.str());

    // Simple FNV-1a hash
    unsigned int hash = 2166136261u;
    for (const char* pc = raw.str(); *pc; ++pc)
    {
        hash ^= (unsigned char)(*pc);
        hash *= 16777619u;
    }

    AsciiString result;
    result.format("%08X", hash);
    return result;
}

// ============================================================================
// Binary frame helpers
// ============================================================================

void LiveStreamer::queueFrame(LiveMsgType type, const void* data, size_t len)
{
    QueuedFrame frame;
    frame.type = (unsigned char)type;
    if (data && len > 0)
    {
        frame.data.assign(static_cast<const char*>(data), static_cast<const char*>(data) + len);
    }
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        m_outgoingQueue.push(std::move(frame));
    }
}

bool LiveStreamer::sendBinaryFrame(LiveMsgType type, const void* payload, size_t payloadLen)
{
    if (!m_connected.load())
        return false;

    // Envelope: 1 byte type + 4 bytes length (LE) + payload
    // Must be sent as ONE WebSocket frame — curl_ws_send writes a frame per call.
    unsigned int len = (unsigned int)payloadLen;
    size_t totalSize = 5 + (payload ? len : 0);
    std::vector<unsigned char> buf(totalSize);
    buf[0] = (unsigned char)type;
    buf[1] = (unsigned char)(len & 0xFF);
    buf[2] = (unsigned char)((len >> 8) & 0xFF);
    buf[3] = (unsigned char)((len >> 16) & 0xFF);
    buf[4] = (unsigned char)((len >> 24) & 0xFF);
    if (payload && len > 0)
        memcpy(buf.data() + 5, payload, len);

    return wsSendBinary(buf.data(), totalSize);
}

bool LiveStreamer::sendBinaryFrame(const QueuedFrame& frame)
{
    return sendBinaryFrame((LiveMsgType)frame.type,
        frame.data.empty() ? nullptr : frame.data.data(),
        frame.data.size());
}

bool LiveStreamer::sendJsonFrame(const char* jsonStr)
{
    return sendBinaryFrame(LIVE_MSG_REGISTER, jsonStr, strlen(jsonStr));
}

// ============================================================================
// WebSocket I/O (libcurl, background thread)
// ============================================================================

bool LiveStreamer::wsSendBinary(const unsigned char* data, size_t len)
{
    if (!m_curlEasy)
        return false;

    size_t sent = 0;
    CURLcode rc = curl_ws_send(m_curlEasy, data, len, &sent, 0, CURLWS_BINARY);
    if (rc != CURLE_OK)
    {
        liveStreamLog("LiveStreamer::wsSendBinary failed: %d\n", (int)rc);
        return false;
    }
    return (sent == len);
}

bool LiveStreamer::wsRecv(std::vector<char>& outBuffer)
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
        liveStreamLog("LiveStreamer::wsRecv error: %d\n", (int)rc);
        outBuffer.clear();
        return false;
    }

    outBuffer.resize(nread);
    return nread > 0;
}

bool LiveStreamer::connectToRelay()
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

    CURL* easy = curl_easy_init();
    if (!easy)
    {
        liveStreamLog("LiveStreamer::connectToRelay curl_easy_init failed\n");
        return false;
    }

    // Append /register if not already in the URL
    AsciiString url = m_relayUrl;
    {
        Int len = (Int)strlen(url.str());
        if (len < 9 || strcmp(url.str() + len - 9, "/register") != 0)
        {
            if (len > 0 && url.str()[len - 1] == '/')
                url.concat("register");
            else
                url.concat("/register");
        }
    }

    curl_easy_setopt(easy, CURLOPT_URL, url.str());
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
            liveStreamLog("LiveStreamer: cacert.pem not found — TLS certificate verification DISABLED\n");
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }
    }

    CURLM* multi = curl_multi_init();
    if (!multi)
    {
        curl_easy_cleanup(easy);
        liveStreamLog("LiveStreamer::connectToRelay curl_multi_init failed\n");
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
        liveStreamLog("LiveStreamer::connectToRelay curl_multi_perform failed: %d\n", (int)mc);
        curl_multi_remove_handle(multi, easy);
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
        return false;
    }

    // Verify the HTTP upgrade (WebSocket handshake) actually succeeded.
    // A 404 or other HTTP error still results in stillRunning==0, so we
    // must explicitly check the transfer result via curl_multi_info_read.
    int infoRunning = 0;
    CURLMsg* infoMsg = curl_multi_info_read(multi, &infoRunning);
    if (!infoMsg || infoMsg->data.result != CURLE_OK)
    {
        int result = infoMsg ? (int)infoMsg->data.result : -1;
        liveStreamLog("LiveStreamer::connectToRelay: handshake failed (result=%d)\n", result);
        curl_multi_remove_handle(multi, easy);
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
        return false;
    }

    m_curlEasy = easy;
    m_curlMulti = multi;
    m_connected.store(true);
    liveStreamLog("LiveStreamer::connectToRelay connected\n");
    return true;
}

// ============================================================================
// Background network thread
// ============================================================================

void LiveStreamer::networkThreadFunc()
{
    liveStreamLog("LiveStreamer::networkThreadFunc started\n");

    if (!connectToRelay())
    {
        liveStreamLog("LiveStreamer::networkThreadFunc connectToRelay failed\n");
        m_shouldRun.store(false);
        return;
    }

    m_connected.store(true);

    while (m_shouldRun.load())
    {
        // Drain outgoing queue
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            while (!m_outgoingQueue.empty() && m_connected.load())
            {
                QueuedFrame& frame = m_outgoingQueue.front();
                if (!sendBinaryFrame(frame))
                {
                    liveStreamLog("LiveStreamer::networkThreadFunc send failed, type=%d\n", (int)frame.type);
                    m_connected.store(false);
                    break;
                }
                if (frame.type == LIVE_MSG_HEADER)
                    liveStreamLog("LiveStreamer: sent HEADER (%zu bytes)\n", frame.data.size());
                else if (frame.type == LIVE_MSG_BODY)
                {
                    // Log only periodically
                    static int bodyCount = 0;
                    if (++bodyCount % 60 == 0)
                        liveStreamLog("LiveStreamer: sent BODY #%d (%zu bytes)\n", bodyCount, frame.data.size());
                }
                else if (frame.type == LIVE_MSG_END)
                    liveStreamLog("LiveStreamer: sent END\n");
                m_outgoingQueue.pop();
            }
        }

        // Receive incoming messages
        std::vector<char> recvBuf;
        while (wsRecv(recvBuf) && m_shouldRun.load())
        {
            if (recvBuf.size() < 5)
                continue;

            // TheSuperHackers @fix Same signed-char sign-extension bug as
            // LiveObserver.cpp::networkThreadFunc — must zero-extend through unsigned char.
            unsigned char msgType = (unsigned char)recvBuf[0];
            unsigned int msgLen = (unsigned int)(unsigned char)recvBuf[1]
                | ((unsigned int)(unsigned char)recvBuf[2] << 8)
                | ((unsigned int)(unsigned char)recvBuf[3] << 16)
                | ((unsigned int)(unsigned char)recvBuf[4] << 24);

            if (msgType == LIVE_MSG_ROLE && msgLen > 0 && recvBuf.size() >= (size_t)(5 + msgLen))
            {
                // Parse JSON role assignment
                std::string json(recvBuf.data() + 5, msgLen);
                liveStreamLog("LiveStreamer: received role: %s\n", json.c_str());

                // Simple JSON parsing for role/action/game_id
                const char* roleStart = strstr(json.c_str(), "\"role\":\"");
                const char* actionStart = strstr(json.c_str(), "\"action\":\"");
                const char* gameIdStart = strstr(json.c_str(), "\"game_id\":\"");
                const char* bodyOffStart = strstr(json.c_str(), "\"body_offset\":");

                AsciiString role("none");
                AsciiString gameId;
                uint64_t bodyOffset = 0;

                if (roleStart)
                {
                    roleStart += 8;
                    const char* roleEnd = strchr(roleStart, '"');
                    if (roleEnd)
                        role.set(roleStart, roleEnd - roleStart);
                }
                if (gameIdStart)
                {
                    gameIdStart += 12;
                    const char* gidEnd = strchr(gameIdStart, '"');
                    if (gidEnd)
                        gameId.set(gameIdStart, gidEnd - gameIdStart);
                }
                if (bodyOffStart)
                {
                    bodyOffStart += 14; // skip "body_offset":
                    bodyOffset = (uint64_t)strtoull(bodyOffStart, nullptr, 10);
                }
                if (actionStart && actionStart + 10)
                {
                    const char* actPtr = actionStart + 10;
                    if (strncmp(actPtr, "takeover", 8) == 0)
                        onTakeover(bodyOffset);
                }

                onRoleAssigned(role, gameId, bodyOffset);
            }
            else if (msgType == LIVE_MSG_ERROR)
            {
                liveStreamLog("LiveStreamer: received ERROR\n");
            }
        }

        // Small sleep to avoid busy-looping.
        // TheSuperHackers @fix curl_multi_poll's out-param is numfds, not "still
        // running" — see the matching comment in LiveObserver.cpp::networkThreadFunc.
        // curl_multi_perform() must run unconditionally or incoming ROLE/ERROR
        // messages can silently stop being received.
        {
            int numfds = 0;
            curl_multi_poll(m_curlMulti, NULL, 0, 10, &numfds);
            int runningHandles = 0;
            curl_multi_perform((CURLM*)m_curlMulti, &runningHandles);
        }
    }

    // Final drain — frames queued after the last loop iteration
    // (e.g. PATCH + END from stopRecording) must still be sent.
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        while (!m_outgoingQueue.empty() && m_connected.load())
        {
            QueuedFrame& frame = m_outgoingQueue.front();
            if (!sendBinaryFrame(frame))
            {
                liveStreamLog("LiveStreamer: final drain send failed, type=%d\n", (int)frame.type);
                break;
            }
            liveStreamLog("LiveStreamer: final drain sent type=%d (%zu bytes)\n", (int)frame.type, frame.data.size());
            m_outgoingQueue.pop();
        }
    }

    // Cleanup
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
    liveStreamLog("LiveStreamer::networkThreadFunc ended\n");
}
