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
#include "Common/LiveObserver.h"	// LIVE_OBSERVER_LOGGING gate + LIVE_OBSERVER_BUILD_TAG
#include "Common/GlobalData.h"
#include "Common/GameCommon.h"		// LIVE_DELAY_SECONDS_DEFAULT / _MAX
#include "GameClient/ClientInstance.h"

#include "GameNetwork/GeneralsOnline/json.hpp"	// parses GO's register reply

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
// LIVE_OBSERVER_BUILD_TAG and the LIVE_OBSERVER_LOGGING gate both come from LiveObserver.h,
// included above. Without that include this file kept its own copy of the tag (which drifted
// out of sync) and never saw the DEFAULT logging resolution, so streamer logging stayed off
// in builds where observer logging was on.

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
    , m_queuedBytes(0)
    , m_queueOverflowed(false)
    , m_isHost(FALSE)
    , m_delaySeconds(-1)
    , m_shouldRun(false)
    , m_curlEasy(nullptr)
    , m_curlMulti(nullptr)
    , m_bodySentOffset(0)
{
    m_headerBuffer.reserve(4096);
    m_bodyBuffer.reserve(64 * 1024);
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
// Pending registration — the pre-game lobby → Recorder handover
// ============================================================================

// Only ever touched from the main thread: the lobby fills it in from a UI callback, the Recorder
// consumes it from MSG_NEW_GAME. The network thread never sees it — by the time any of this
// reaches the wire it has been copied into the REGISTER payload.
static LiveStreamRegistration s_pendingRegistration;

void liveStreamSetPendingRegistration(const LiveStreamRegistration& registration)
{
    s_pendingRegistration = registration;
    liveStreamLog("liveStreamSetPendingRegistration lobbyId=%s player='%s' canStream=%d lobbyJsonLen=%u\n",
        registration.lobbyId.str(), registration.playerName.str(),
        (int)registration.canStream, (unsigned int)registration.lobbyJson.length());
}

void liveStreamClearPendingRegistration()
{
    if (s_pendingRegistration.isValid())
        liveStreamLog("liveStreamClearPendingRegistration dropping lobbyId=%s\n",
            s_pendingRegistration.lobbyId.str());

    s_pendingRegistration = LiveStreamRegistration();
}

Bool liveStreamHasPendingRegistration()
{
    return s_pendingRegistration.isValid();
}

// Ceiling on replay bytes held while waiting for a relay connection. Roughly a few minutes of
// a busy match: enough that a slow registration costs nothing, small enough that a refused one
// cannot grow without bound for the rest of the game.
static const size_t LIVE_STREAM_MAX_QUEUED_BYTES = 8u * 1024u * 1024u;

LiveStreamer* liveStreamStartPendingSession()
{
    if (TheGlobalData == nullptr || !TheGlobalData->m_liveStreamEnabled)
        return nullptr;

    if (!s_pendingRegistration.isValid())
    {
        // Normal for skirmish, replays and LAN — there is no lobby to have registered one.
        liveStreamLog("liveStreamStartPendingSession: nothing pending, not streaming this game\n");
        return nullptr;
    }

    if (TheLiveStreamer == nullptr)
        TheLiveStreamer = createLiveStreamer();

    if (TheLiveStreamer == nullptr)
        return nullptr;

    // Register first, then start the thread. registerForGame only fills in fields and queues
    // the REGISTER frame, and the network thread needs those fields to ask GO for a token --
    // doing it the other way round raced the connect.
    //
    // No relay address is chosen here. GO registers the livestream, mints this player's
    // single-use stream token and returns the connect URL, so the relay's location is GO's
    // to decide (see LiveStreamer::requestStreamUrl).
    TheLiveStreamer->registerForGame(s_pendingRegistration);
    TheLiveStreamer->init();

    // Consumed. A second recording without a fresh lobby visit must not re-register this one
    // under the same lobby id — that would merge two unrelated matches into one relay session.
    liveStreamClearPendingRegistration();

    return TheLiveStreamer;
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

    // Demoted (backup) streamers do not send the header — the relay already has the
    // session's canonical one. Defensive: the header is normally sent at match start,
    // before any demotion can occur, so this only matters for an edge-case ordering.
    if (m_isBackup.load())
    {
        m_headerBuffer.clear();
        return;
    }

    queueFrame(LIVE_MSG_HEADER, m_headerBuffer.data(), m_headerBuffer.size());
    m_headerBuffer.clear();
}

void LiveStreamer::onHeaderPatch(Int offset, const void* data, Int size)
{
    if (size <= 0)
        return;

    // Demoted: header mutations are not sent (see onHeaderComplete).
    if (m_isBackup.load())
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

    // One buffer, both roles: while streaming it is flushed to the wire by onBodyFlush;
    // while backup (demoted) onBodyFlush does nothing, so the same buffer accumulates the
    // body from the demotion point onward — which is exactly the backfill source a later
    // takeover needs. Its first byte sits at absolute offset m_bodySentOffset (frozen while
    // backup), so takeover offsets stay absolute-correct. Guarded by m_sendMutex because
    // onTakeover (network thread) reads it while this (game thread) appends.
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_bodyBuffer.size() + size > BODY_BUFFER_MAX)
    {
        // Drop the oldest bytes to stay under the cap and advance the buffer's start
        // offset so the m_bodySentOffset invariant holds. Only a long backup session can
        // grow this large (streaming flushes every 4KB), and real matches are tens of KB.
        size_t drop = m_bodyBuffer.size() + size - BODY_BUFFER_MAX;
        m_bodyBuffer.erase(m_bodyBuffer.begin(), m_bodyBuffer.begin() + drop);
        m_bodySentOffset += drop;
    }
    m_bodyBuffer.insert(m_bodyBuffer.end(), p, p + size);
}

void LiveStreamer::onBodyFlush()
{
    // Backup: keep the bytes. The relay told us to stop pushing, so nothing is sent; the
    // buffer keeps growing here and becomes the backfill source on takeover. END is still
    // sent via onRecordingEnded so the relay knows this player's recording is over.
    if (m_isBackup.load())
        return;

    std::vector<char> framed;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
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

        framed.reserve(8 + m_bodyBuffer.size());
        framed.insert(framed.end(), offBuf, offBuf + 8);
        framed.insert(framed.end(), m_bodyBuffer.begin(), m_bodyBuffer.end());

        m_bodySentOffset += m_bodyBuffer.size();
        m_bodyBuffer.clear();
    }

    queueFrame(LIVE_MSG_BODY, framed.data(), framed.size());
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

void LiveStreamer::init()
{
    m_shouldRun.store(true);

    liveStreamLog("LiveStreamer::init lobby=%s, asking GO for a stream URL\n", m_lobbyId.str());

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

    // Release the body buffer (the streaming/backup accumulation).
    std::lock_guard<std::mutex> lock(m_sendMutex);
    m_bodyBuffer.clear();
    m_bodyBuffer.shrink_to_fit();
    m_bodySentOffset = 0;
}

// ============================================================================
// Registration
// ============================================================================

std::string liveStreamJsonEscape(const char* str)
{
    std::string out;
    if (str == nullptr)
        return out;

    for (const unsigned char* pc = (const unsigned char*)str; *pc; ++pc)
    {
        switch (*pc)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (*pc < 0x20)
                {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned int)*pc);
                    out += esc;
                }
                else
                {
                    // Includes every byte >= 0x80: a UTF-8 sequence is already legal JSON.
                    out += (char)*pc;
                }
                break;
        }
    }

    return out;
}

void LiveStreamer::registerForGame(const LiveStreamRegistration& registration)
{
    m_lobbyId = registration.lobbyId;
    m_playerName = registration.playerName;
    m_isHost = registration.isHost;
    m_delaySeconds = registration.delaySeconds;

    liveStreamLog("LiveStreamer::registerForGame lobbyId=%s player='%s' isHost=%d canStream=%d lobbyJsonLen=%u\n",
        registration.lobbyId.str(), registration.playerName.str(), (int)registration.isHost,
        (int)registration.canStream, (unsigned int)registration.lobbyJson.length());

    // Built into a std::string rather than a fixed buffer: the GO-shaped lobby block carries a
    // lobby name, two map paths and up to eight members, which comfortably outgrew the 2KB
    // char array this used to use — and a truncated payload is unparseable, not merely lossy.
    char scratch[64];
    std::string regJson = "{\"type\":\"register\"";

    regJson += ",\"lobbyid\":\"" + liveStreamJsonEscape(registration.lobbyId.str()) + "\"";
    regJson += ",\"player_name\":\"" + liveStreamJsonEscape(registration.playerName.str()) + "\"";
    regJson += registration.canStream ? ",\"can_stream\":true" : ",\"can_stream\":false";
    // is_host is sent for the relay's logs only. It no longer grants anything: the relay
    // compares our stream token's user against the owner GO recorded for the session, so a
    // client cannot claim host authority by asserting it here.
    regJson += registration.isHost ? ",\"is_host\":true" : ",\"is_host\":false";

    // Both host-only. The relay ignores them from anyone else, but not sending them at all from
    // a non-host keeps the payload honest about who is claiming to describe the game.
    if (registration.isHost)
    {
        if (registration.delaySeconds >= 0)
        {
            snprintf(scratch, sizeof(scratch), ",\"delay_seconds\":%d", registration.delaySeconds);
            regJson += scratch;
        }

        if (!registration.lobbyJson.empty())
        {
            regJson += ",\"lobby\":";
            regJson += registration.lobbyJson;
        }
    }

    regJson += "}";

    liveStreamLog("LiveStreamer::registerForGame payload=%s\n", regJson.c_str());

    // Queue the REGISTER message — the network thread will send it once connected.
    // (Must NOT call sendBinaryFrame directly here because m_connected may still be false.)
    queueFrame(LIVE_MSG_REGISTER, regJson.c_str(), regJson.length());
}

void LiveStreamer::onRoleAssigned(const AsciiString& role, const AsciiString& lobbyId, uint64_t bodyOffset)
{
    m_lobbyId = lobbyId;

    // Only the initial streamer ROLE (fresh connect or mid-match reconnect) establishes the
    // send offset from the relay's value. A demote (backup) ROLE must not: while backup,
    // m_bodySentOffset is frozen at the absolute offset of the accumulating body buffer (see
    // onBodyBytes/onBodyFlush), and the relay's current body length is ahead of it —
    // clobbering it would mislabel the buffered bytes. A takeover ROLE (streamer + action)
    // must not either: onTakeover runs right after and computes the backfill slice against
    // that same frozen offset, then advances m_bodySentOffset itself.
    Bool wasBackup = m_isBackup.load();
    m_isStreaming.store(role == "streamer");
    m_isBackup.store(role == "backup");
    if (!m_isBackup.load() && !wasBackup)
        m_bodySentOffset = bodyOffset;

    liveStreamLog("LiveStreamer::onRoleAssigned role=%s lobbyId=%s streaming=%d bodyOff=%llu\n",
        role.str(), lobbyId.str(), (int)m_isStreaming.load(), (unsigned long long)bodyOffset);
}

void LiveStreamer::onTakeover(uint64_t bodyOffset)
{
    liveStreamLog("LiveStreamer::onTakeover promoted to streamer, bodyOff=%llu\n",
        (unsigned long long)bodyOffset);

    // Resume live buffering FIRST: any body bytes that arrive while the backfill below is
    // being sent must land in m_bodyBuffer (framed at m_bodySentOffset after the snapshot),
    // not be silently dropped by the backup gate.
    m_isStreaming.store(true);
    m_isBackup.store(false);

    // Backfill the relay's gap from the same body buffer that accumulated while backup.
    //
    // While demoted, onBodyFlush was a no-op, so m_bodyBuffer holds every body byte from
    // the demotion point onward, with m_bodySentOffset frozen at the absolute offset of
    // buffer[0]. The relay is missing [bodyOffset..buffer_end]; live data continues from
    // buffer_end. Snapshot atomically with respect to onBodyBytes (game thread).
    uint64_t backfillStart = bodyOffset;
    std::vector<char> backfill;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (bodyOffset < m_bodySentOffset)
        {
            // The cap trimmed past the requested offset — we no longer hold those bytes, so
            // the hole cannot be filled. Degrade to skip-forward; the relay logs a gap, as
            // it does for any missing chunk.
            liveStreamLog("LiveStreamer::onTakeover cannot backfill from %llu "
                "(buffer starts at %llu) — skipping forward\n",
                (unsigned long long)bodyOffset, (unsigned long long)m_bodySentOffset);
            backfillStart = m_bodySentOffset;
        }
        size_t rel = (size_t)(backfillStart - m_bodySentOffset);
        if (rel < m_bodyBuffer.size())
        {
            backfill.assign(m_bodyBuffer.begin() + rel, m_bodyBuffer.end());
        }
        // Live data continues from the end of what we just sent; onBodyFlush frames the
        // next flush at this offset.
        m_bodySentOffset = m_bodySentOffset + m_bodyBuffer.size();
        m_bodyBuffer.clear();
    }

    // Send the backfill in bounded chunks (network thread — same thread that drains the
    // send queue, so a direct send here cannot interleave with it).
    if (!backfill.empty())
    {
        const size_t BACKFILL_CHUNK = 64 * 1024;
        uint64_t absOff = backfillStart;
        for (size_t i = 0; i < backfill.size(); i += BACKFILL_CHUNK)
        {
            size_t n = backfill.size() - i;
            if (n > BACKFILL_CHUNK)
                n = BACKFILL_CHUNK;

            std::vector<char> framed;
            framed.reserve(8 + n);
            unsigned char offBuf[8];
            offBuf[0] = (unsigned char)(absOff & 0xFF);
            offBuf[1] = (unsigned char)((absOff >> 8) & 0xFF);
            offBuf[2] = (unsigned char)((absOff >> 16) & 0xFF);
            offBuf[3] = (unsigned char)((absOff >> 24) & 0xFF);
            offBuf[4] = (unsigned char)((absOff >> 32) & 0xFF);
            offBuf[5] = (unsigned char)((absOff >> 40) & 0xFF);
            offBuf[6] = (unsigned char)((absOff >> 48) & 0xFF);
            offBuf[7] = (unsigned char)((absOff >> 56) & 0xFF);
            framed.insert(framed.end(), offBuf, offBuf + 8);
            framed.insert(framed.end(), backfill.data() + i, backfill.data() + i + n);

            if (!sendBinaryFrame(LIVE_MSG_BODY, framed.data(), framed.size()))
            {
                liveStreamLog("LiveStreamer::onTakeover backfill send failed at offset %llu\n",
                    (unsigned long long)absOff);
                break;
            }
            absOff += n;
        }
        liveStreamLog("LiveStreamer::onTakeover backfilled %zu bytes from offset %llu\n",
            backfill.size(), (unsigned long long)backfillStart);
    }
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

        // Everything queued before the connection exists is held in memory. That is deliberate
        // -- it is what lets the replay sink attach at match start and stream the header the
        // moment the relay accepts us -- but it has to be bounded, because a registration GO
        // refuses means nothing ever drains this. REGISTER itself is always kept: dropping it
        // would make a connection that does succeed useless.
        if (type != LIVE_MSG_REGISTER &&
            m_queuedBytes + frame.data.size() > LIVE_STREAM_MAX_QUEUED_BYTES)
        {
            if (!m_queueOverflowed)
            {
                m_queueOverflowed = true;
                liveStreamLog("LiveStreamer::queueFrame queue exceeded %u bytes with no relay "
                    "connection - dropping stream data from here on\n",
                    (unsigned int)LIVE_STREAM_MAX_QUEUED_BYTES);
            }
            return;
        }

        m_queuedBytes += frame.data.size();
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

    // TheSuperHackers @fix Only binary payloads belong in the envelope reassembly buffer. meta was
    // being ignored entirely, so a PING/PONG/TEXT/CLOSE payload surfaced by libcurl would be
    // appended straight into the byte stream and misparse everything after it - and the relay's
    // server sends keepalive pings on a timer, which is exactly the kind of thing that shows up
    // minutes into an otherwise healthy session and never before.
    if (meta != nullptr && (meta->flags & CURLWS_BINARY) == 0)
    {
        liveStreamLog("LiveStreamer::wsRecv: dropped a non-binary websocket frame (flags=0x%X, %d bytes)\n",
            (unsigned)meta->flags, (int)nread);
        outBuffer.clear();
        return false;
    }

    outBuffer.resize(nread);
    return nread > 0;
}

bool LiveStreamer::requestStreamUrl(AsciiString& outUrl)
{
    // The host reports the broadcast delay here rather than in the REGISTER frame. GO records
    // it and forwards it to the relay when the session is created, which is before any source
    // connects -- so every observer of this game is held behind the same number, settled
    // before the first byte of replay data exists. A non-host sends no delay at all, so a
    // player who is merely a second source cannot redefine the host's spoiler window.
    std::string postBody = "{}";
    if (m_isHost && m_delaySeconds >= 0)
    {
        char scratch[64];
        snprintf(scratch, sizeof(scratch), "{\"delay_seconds\":%d}", m_delaySeconds);
        postBody = scratch;
    }

    AsciiString url;
    url.format("%s/register", liveServicesEndpoint("Livestreams").str());

    AsciiString body;
    Int statusCode = 0;
    if (!liveServicesRequest(url, TRUE, postBody.c_str(), body, statusCode))
    {
        liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s failed (request not sent)\n",
            m_lobbyId.str());
        return false;
    }

    if (statusCode != 200)
    {
        // 404 means GO does not think we are in an in-progress match, 503 that the deployment
        // has no relay configured. Neither is retryable from here: the match simply records
        // locally, as it would with streaming switched off.
        liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s refused (status=%d) %s\n",
            m_lobbyId.str(), statusCode, body.str());
        return false;
    }

    try
    {
        nlohmann::json response = nlohmann::json::parse(body.str());
        if (response.is_object() && response.contains("url") && response["url"].is_string())
        {
            const std::string streamUrl = response["url"].get<std::string>();
            if (!streamUrl.empty())
            {
                outUrl = streamUrl.c_str();
                liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s got a stream URL\n",
                    m_lobbyId.str());
                return true;
            }
        }
    }
    catch (const nlohmann::json::exception&)
    {
    }

    liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s failed (no url in reply)\n",
        m_lobbyId.str());
    return false;
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

    // The relay no longer accepts an unauthenticated /register. GO registers the livestream,
    // mints a single-use stream token for this player, and hands back the complete connect
    // URL -- so the relay's address is GO's to decide, not ours to assemble.
    AsciiString url;
    if (!requestStreamUrl(url))
    {
        curl_easy_cleanup(easy);
        return false;
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
                m_queuedBytes -= frame.data.size();
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

                // Simple JSON parsing for role/action/lobbyid.
                //
                // Each key advances by the literal's own strlen rather than a hand-counted
                // constant. The old hand-counted skip for the id key was one too many, quietly
                // chopping the first character off every id it read — harmless only because
                // nothing but a log line ever consumed it.
                static const char ROLE_KEY[]     = "\"role\":\"";
                static const char ACTION_KEY[]   = "\"action\":\"";
                static const char LOBBY_ID_KEY[] = "\"lobbyid\":\"";
                static const char BODY_OFF_KEY[] = "\"body_offset\":";

                const char* roleStart = strstr(json.c_str(), ROLE_KEY);
                const char* actionStart = strstr(json.c_str(), ACTION_KEY);
                const char* lobbyIdStart = strstr(json.c_str(), LOBBY_ID_KEY);
                const char* bodyOffStart = strstr(json.c_str(), BODY_OFF_KEY);

                AsciiString role("none");
                AsciiString lobbyId;
                uint64_t bodyOffset = 0;

                if (roleStart)
                {
                    roleStart += strlen(ROLE_KEY);
                    const char* roleEnd = strchr(roleStart, '"');
                    if (roleEnd)
                        role.set(roleStart, roleEnd - roleStart);
                }
                if (lobbyIdStart)
                {
                    lobbyIdStart += strlen(LOBBY_ID_KEY);
                    const char* idEnd = strchr(lobbyIdStart, '"');
                    if (idEnd)
                        lobbyId.set(lobbyIdStart, idEnd - lobbyIdStart);
                }
                if (bodyOffStart)
                {
                    bodyOffStart += strlen(BODY_OFF_KEY);
                    bodyOffset = (uint64_t)strtoull(bodyOffStart, nullptr, 10);
                }
                // onRoleAssigned runs first deliberately: it overwrites m_bodySentOffset with
                // the ROLE's body_offset, which would clobber the backfill position onTakeover
                // establishes. So the flags/offset are applied, then the backfill overrides.
                onRoleAssigned(role, lobbyId, bodyOffset);

                if (actionStart)
                {
                    const char* actPtr = actionStart + strlen(ACTION_KEY);
                    if (strncmp(actPtr, "takeover", 8) == 0)
                        onTakeover(bodyOffset);
                }
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
            m_queuedBytes -= frame.data.size();
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
