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
#include "Common/Recorder.h"
#include "Common/GlobalData.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "GameClient/ClientInstance.h"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/multi.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/websockets.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <fstream>		// cacert.pem presence check, see connectToRelay

// ============================================================================
// liveObserverLog
// ============================================================================
// TheSuperHackers @fix Bump this string with every debugging change to LiveObserver.cpp/
// LiveStreamer.cpp/GameLogic.cpp's LIVE_OBSERVER_LOG instrumentation, so a log file can be
// matched to the exact build that produced it (avoids debugging a stale binary by mistake).
#define LIVE_OBSERVER_BUILD_TAG "2026-08-03-fix23-browser-standalone-fetch"

void liveObserverLog(const char* fmt, ...) {
    static FILE* logFile = NULL;
    if (!logFile) {
        // TheSuperHackers @fix Give this log a per-instance name, instead of a bare
        // relative filename in the exe's cwd (the install dir). Streamer and observer
        // are the same exe and can share a cwd/install; without this, both processes'
        // calls to liveObserverLog() (this file AND GameLogic.cpp's unconditional
        // LIVE_OBSERVER instrumentation) truncate the same file.
        AsciiString path;
        path.format("live_observer_debug_Instance%.2u.log", rts::ClientInstance::getInstanceId());
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
}

void liveObserverInitLog(const char* watchUrl) {
    liveObserverLog("=== Live Observer Init ===\n");
    liveObserverLog("LiveWatchUrl: %s\n", watchUrl ? watchUrl : "(empty)");
    liveObserverLog("Observer mode activated\n");
}

// ============================================================================
// LiveObserver
// ============================================================================
LiveObserver* TheLiveObserver = nullptr;

LiveObserver::LiveObserver()
    : m_connected(false)
    , m_shouldRun(false)
    , m_headerReceived(false)
    , m_streamEnded(false)
    , m_maxCompleteFrame(0)
    , m_safeReadOffset(0)
    , m_parseAbsOffset(0)
    , m_parseCorrupt(false)
    , m_liveFile(nullptr)
    , m_curlEasy(nullptr)
    , m_curlMulti(nullptr)
{
}

// ============================================================================
// Standalone relay HTTP fetch (live game browser)
// ============================================================================

namespace
{
	std::mutex s_fetchMutex;
	std::atomic<bool> s_fetchInFlight(false);
	std::atomic<bool> s_fetchReady(false);
	std::string s_fetchBody;
	bool s_fetchSuccess = false;
	long s_fetchStatus = 0;

	size_t liveRelayWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		std::string* out = static_cast<std::string*>(userdata);
		out->append(ptr, size * nmemb);
		return size * nmemb;
	}

	void liveRelayFetchThread(std::string url)
	{
		std::string body;
		bool success = false;
		long status = 0;

		CURL* easy = curl_easy_init();
		if (easy)
		{
			curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
			curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, liveRelayWriteCb);
			curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);
			curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
			// Keep this short: it runs behind a menu the user is looking at, and a hung
			// relay must not leave the list saying "Loading" indefinitely.
			curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);
			curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);

			// Same CA handling as connectToRelay — this libcurl is OpenSSL-backed and has
			// no trust anchors of its own, so https:// fails without an explicit bundle.
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
				curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
				curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
			}

			CURLcode res = curl_easy_perform(easy);
			curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
			success = (res == CURLE_OK);
			if (!success)
				liveObserverLog("liveRelayFetch: curl failed (result=%d) for %s\n", (int)res, url.c_str());
			curl_easy_cleanup(easy);
		}

		{
			std::lock_guard<std::mutex> lock(s_fetchMutex);
			s_fetchBody = body;
			s_fetchSuccess = success;
			s_fetchStatus = status;
		}
		s_fetchReady.store(true);
		s_fetchInFlight.store(false);
	}
}

Bool liveRelayBeginFetch(const AsciiString& url)
{
	bool expected = false;
	if (!s_fetchInFlight.compare_exchange_strong(expected, true))
		return FALSE;	// one already running

	s_fetchReady.store(false);
	liveObserverLog("liveRelayFetch: GET %s\n", url.str());

	std::thread(liveRelayFetchThread, std::string(url.str())).detach();
	return TRUE;
}

Bool liveRelayPollFetch(AsciiString& outBody, Bool& outSuccess, Int& outStatusCode)
{
	if (!s_fetchReady.load())
		return FALSE;

	std::lock_guard<std::mutex> lock(s_fetchMutex);
	// Re-check under the lock so two callers in one frame cannot both consume the result.
	if (!s_fetchReady.load())
		return FALSE;

	outBody = s_fetchBody.c_str();
	outSuccess = s_fetchSuccess ? TRUE : FALSE;
	outStatusCode = (Int)s_fetchStatus;
	s_fetchReady.store(false);
	return TRUE;
}

Bool liveRelayFetchInFlight()
{
	return s_fetchInFlight.load() ? TRUE : FALSE;
}

// ============================================================================
// Parse cursor — publishes the live-edge and safe-read watermarks
// ============================================================================

void LiveObserver::resetParseCursor(Int bodyStartOffset)
{
    m_parseTail.clear();
    m_parseAbsOffset = bodyStartOffset;
    m_parseCorrupt = false;
    m_maxCompleteFrame.store(0);
    m_safeReadOffset.store(bodyStartOffset);
}

void LiveObserver::advanceParseCursor(Int chunkOffset, const unsigned char* data, size_t dataLen)
{
    if (m_parseCorrupt || dataLen == 0)
        return;

    // The relay appends body data strictly in order (see server.py apply_body, which only
    // accepts offset == body_len), so chunks normally arrive contiguously. The exception is
    // the observer-join race documented in plans/live-observer-900-frame-broadcast-delay.md
    // §8: a live chunk can be broadcast to a freshly-registered observer before catch-up has
    // sent the earlier chunks, leaving a hole in the file. Parsing across a hole would feed
    // uninitialised bytes to the scanner, so stall the watermark instead. The cursor resumes
    // by itself once the missing bytes are backfilled and a contiguous chunk arrives.
    Int expected = m_parseAbsOffset + (Int)m_parseTail.size();
    if (chunkOffset != expected)
    {
        liveObserverLog("LiveObserver: parse cursor gap — chunkOffset=%d expected=%d, watermark stalled\n",
            chunkOffset, expected);
        return;
    }

    m_parseTail.insert(m_parseTail.end(), data, data + dataLen);

    Int consumed = 0;
    UnsignedInt maxFrame = m_maxCompleteFrame.load();
    const Int tailSize = (Int)m_parseTail.size();

    while (consumed < tailSize)
    {
        Int recSize = 0;
        UnsignedInt recFrame = 0;
        ScanRecordResult r = scanReplayRecord(&m_parseTail[consumed], tailSize - consumed, &recSize, &recFrame);

        if (r == SCANRECORD_INCOMPLETE)
            break;

        if (r == SCANRECORD_CORRUPT)
        {
            // Fail closed. Freezing the watermark stops playback advancing into data we
            // cannot trust, which is recoverable and diagnosable; the old behaviour skipped
            // zero bytes and reported garbage frame numbers indefinitely.
            m_parseCorrupt = true;
            liveObserverLog("LiveObserver: parse cursor CORRUPT at abs offset %d — watermark frozen at frame %u\n",
                m_parseAbsOffset + consumed, maxFrame);
            break;
        }

        consumed += recSize;
        if (recFrame > maxFrame)
            maxFrame = recFrame;
    }

    if (consumed > 0)
    {
        m_parseTail.erase(m_parseTail.begin(), m_parseTail.begin() + consumed);
        m_parseAbsOffset += consumed;
        // Publish the frame before the offset: a reader that sees the new safe offset must
        // never see a stale live edge for the records it is now allowed to read.
        m_maxCompleteFrame.store(maxFrame);
        m_safeReadOffset.store(m_parseAbsOffset);
    }
}

LiveObserver::~LiveObserver()
{
    close();
}

LiveObserver* createLiveObserver()
{
    return new LiveObserver();
}

// ============================================================================
// Network setup
// ============================================================================

void LiveObserver::connect(const AsciiString& watchUrl)
{
    m_relayUrl = watchUrl;
    m_shouldRun.store(true);

    // Extract game ID from the watch URL (everything after "/watch/")
    {
        const char* urlStr = watchUrl.str();
        const char* watchPos = strstr(urlStr, "/watch/");
        if (watchPos)
        {
            const char* gameIdStart = watchPos + 7; // skip "/watch/"
            m_gameId.set(gameIdStart);
            m_liveFilename.format("%s_live.rep", m_gameId.str());
        }
        else
        {
            m_gameId = "unknown";
            m_liveFilename = "_live.rep";
        }
    }

    liveObserverLog("LiveObserver::connect to %s (game=%s, file=%s)\n",
        watchUrl.str(), m_gameId.str(), m_liveFilename.str());

    m_networkThread = std::thread(&LiveObserver::networkThreadFunc, this);
}

void LiveObserver::close()
{
    m_shouldRun.store(false);

    if (m_networkThread.joinable())
        m_networkThread.join();

    if (m_liveFile)
    {
        m_liveFile->close();
        m_liveFile = nullptr;
    }

    m_connected.store(false);
    m_headerReceived.store(false);
    m_streamEnded.store(false);
}

// ============================================================================
// Live file management
// ============================================================================

bool LiveObserver::openLiveFile()
{
    AsciiString filepath = RecorderClass::getReplayDir();
    filepath.concat(m_liveFilename);

    m_liveFilePath = filepath;
    m_liveFile = TheFileSystem->openFile(filepath.str(), File::WRITE | File::BINARY);
    if (!m_liveFile)
    {
        liveObserverLog("LiveObserver::openLiveFile FAILED for %s\n", filepath.str());
        return false;
    }

    liveObserverLog("LiveObserver::openLiveFile opened %s\n", filepath.str());
    return true;
}

// ============================================================================
// Frame handler
// ============================================================================

void LiveObserver::handleFrame(unsigned char type, const char* payload, size_t len)
{
    switch (type)
    {
    case 1: // LIVE_MSG_HEADER
    {
        if (!openLiveFile())
            return;

        // Write header bytes
        if (len > 0)
            m_liveFile->write(payload, len);
        m_liveFile->flush();

        // Close write handle so the Recorder can open the file for reading.
        // Do NOT reopen here — the Recorder needs exclusive access to read
        // the header during playbackFile().  Reopen lazily when the first
        // BODY or PATCH frame arrives.
        m_liveFile->close();
        m_liveFile = nullptr;

        // Body records start immediately after the header, so that is where the parse
        // cursor begins. Reset here rather than only in the constructor, so a second
        // live-observer session in the same process cannot inherit a stale watermark.
        resetParseCursor((Int)len);

		m_headerReceived.store(true);
		liveObserverLog("LiveObserver: HEADER received (%zu bytes), ready for playback\n", len);
        break;
    }

    case 2: // LIVE_MSG_PATCH
    {
        if (len < 8)
            return;

        // Lazy-open the file for read/write (no truncation)
        if (!m_liveFile)
        {
            m_liveFile = TheFileSystem->openFile(m_liveFilePath.str(), File::READWRITE | File::BINARY);
        }
        if (!m_liveFile)
            return;

        // Parse offset and data length from payload
        const unsigned char* p = (const unsigned char*)payload;
        Int offset = (Int)p[0] | ((Int)p[1] << 8) | ((Int)p[2] << 16) | ((Int)p[3] << 24);
        Int dataLen = (Int)p[4] | ((Int)p[5] << 8) | ((Int)p[6] << 16) | ((Int)p[7] << 24);

        if (dataLen <= 0 || (size_t)(8 + dataLen) > len)
            return;

        Int fileSize = (Int)m_liveFile->size();
        Int seekRes = m_liveFile->seek(offset, File::seekMode::START);
        if (seekRes == offset)
        {
            m_liveFile->write(payload + 8, dataLen);
            m_liveFile->seek(fileSize, File::seekMode::START);
        }

		liveObserverLog("LiveObserver: PATCH offset=%d size=%d fileSize=%d\n", offset, dataLen, (int)m_liveFile->size());
        break;
    }

    case 3: // LIVE_MSG_BODY
    {
        // BODY payload: [8B offset uint64 LE][data]
        if (len < 8)
        {
            liveObserverLog("LiveObserver: BODY frame too short (len=%d)\n", (int)len);
            return;
        }

        const unsigned char* p = (const unsigned char*)payload;
        // Read 8-byte absolute file offset (little-endian)
        Int offset = (Int)(p[0] | ((unsigned long long)p[1] << 8)
            | ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24)
            | ((unsigned long long)p[4] << 32) | ((unsigned long long)p[5] << 40)
            | ((unsigned long long)p[6] << 48) | ((unsigned long long)p[7] << 56));
        size_t dataLen = len - 8;
        if (dataLen == 0)
        {
            liveObserverLog("LiveObserver: BODY frame with zero dataLen at offset=%d\n", offset);
            return;
        }

        // Lazy-open the file for read/write (no truncation, file exists from HEADER handler)
        if (!m_liveFile)
        {
            m_liveFile = TheFileSystem->openFile(m_liveFilePath.str(), File::READWRITE | File::BINARY);
        }
        if (!m_liveFile)
        {
            liveObserverLog("LiveObserver: BODY openFile FAILED for %s\n", m_liveFilePath.str());
            return;
        }

        // Seek to the specified offset and write the data
        m_liveFile->seek(offset, File::seekMode::START);
        m_liveFile->write(payload + 8, (Int)dataLen);
        m_liveFile->flush();

        // Scan the bytes we just committed, so the game thread's live edge and safe-read
        // limit are up to date the moment the data is readable.
        advanceParseCursor(offset, (const unsigned char*)(payload + 8), dataLen);

        liveObserverLog("LiveObserver: BODY written offset=%d dataLen=%d fileSize=%d liveEdge=%u safeOffset=%d\n",
            offset, (int)dataLen, (int)m_liveFile->size(), m_maxCompleteFrame.load(), m_safeReadOffset.load());
        break;
    }

    case 5: // LIVE_MSG_ROLE — session config, sent by the relay ahead of the HEADER
    {
        // The broadcast delay originates with the streamer and reaches us here. It must be
        // applied before playback starts, because the pre-roll buffer latches against it and
        // there is no un-latching once a session is running — which is exactly why the relay
        // sends this frame before the HEADER that triggers the game start.
        std::string json(payload, len);
        liveObserverLog("LiveObserver: ROLE received: %s\n", json.c_str());

        const char* delayStart = strstr(json.c_str(), "\"delay_seconds\":");
        if (delayStart && TheRecorder)
        {
            delayStart += 16; // skip "delay_seconds":
            Int delaySeconds = (Int)strtol(delayStart, nullptr, 10);
            if (delaySeconds >= 0 && delaySeconds <= LIVE_DELAY_SECONDS_MAX)
            {
                TheRecorder->setLiveDelaySeconds((UnsignedInt)delaySeconds);
                liveObserverLog("LiveObserver: broadcast delay set to %d seconds\n", delaySeconds);
            }
            else
            {
                liveObserverLog("LiveObserver: ignoring out-of-range delay_seconds=%d, keeping %u\n",
                    delaySeconds, TheRecorder->getLiveDelaySeconds());
            }
        }
        // No delay_seconds (older relay) simply leaves the built-in default in place.
        break;
    }

    case 4: // LIVE_MSG_END
    {
        liveObserverLog("LiveObserver: END received\n");
        m_streamEnded.store(true);

        if (TheRecorder)
            TheRecorder->setStreamEnded(TRUE);

        if (m_liveFile)
        {
            m_liveFile->flush();
            m_liveFile->close();
            m_liveFile = nullptr;
        }
        break;
    }

    case 6: // LIVE_MSG_ERROR
    {
        AsciiString errMsg;
        if (len > 0)
            errMsg.set(payload, len);
        liveObserverLog("LiveObserver: ERROR from relay: %s\n", errMsg.str());
        m_streamEnded.store(true);
        if (TheRecorder)
            TheRecorder->setStreamEnded(TRUE);
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// WebSocket I/O
// ============================================================================

bool LiveObserver::wsSendBinary(const unsigned char* data, size_t len)
{
    if (!m_curlEasy)
        return false;

    size_t sent = 0;
    CURLcode rc = curl_ws_send(m_curlEasy, data, len, &sent, 0, CURLWS_BINARY);
    return (rc == CURLE_OK && sent == len);
}

bool LiveObserver::wsRecv(std::vector<char>& outBuffer)
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
        liveObserverLog("LiveObserver::wsRecv error: %d\n", (int)rc);
        outBuffer.clear();
        return false;
    }

    outBuffer.resize(nread);
    return nread > 0;
}

bool LiveObserver::connectToRelay()
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
        liveObserverLog("LiveObserver::connectToRelay curl_easy_init failed\n");
        return false;
    }

    curl_easy_setopt(easy, CURLOPT_URL, m_relayUrl.str());
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
            liveObserverLog("LiveObserver: cacert.pem not found — TLS certificate verification DISABLED\n");
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }
    }

    CURLM* multi = curl_multi_init();
    if (!multi)
    {
        curl_easy_cleanup(easy);
        liveObserverLog("LiveObserver::connectToRelay curl_multi_init failed\n");
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
        liveObserverLog("LiveObserver::connectToRelay failed: %d\n", (int)mc);
        curl_multi_remove_handle(multi, easy);
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
        return false;
    }

    int infoRunning = 0;
    CURLMsg* infoMsg = curl_multi_info_read(multi, &infoRunning);
    if (!infoMsg || infoMsg->data.result != CURLE_OK)
    {
        int result = infoMsg ? (int)infoMsg->data.result : -1;
        liveObserverLog("LiveObserver::connectToRelay: handshake failed (result=%d)\n", result);
        curl_multi_remove_handle(multi, easy);
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
        return false;
    }

    m_curlEasy = easy;
    m_curlMulti = multi;
    m_connected.store(true);
    liveObserverLog("LiveObserver::connectToRelay connected to %s\n", m_relayUrl.str());
    return true;
}

// ============================================================================
// Background network thread
// ============================================================================

void LiveObserver::networkThreadFunc()
{
    liveObserverLog("LiveObserver::networkThreadFunc started\n");

    if (!connectToRelay())
    {
        liveObserverLog("LiveObserver::networkThreadFunc connectToRelay failed\n");
        m_shouldRun.store(false);
        return;
    }

    m_connected.store(true);

    // Persistent buffer for incoming data — multiple frames may arrive in
    // one wsRecv call, or a frame may be split across multiple calls.
    std::vector<char> buf;
    size_t totalBytesReceived = 0;
    size_t totalFramesProcessed = 0;

    while (m_shouldRun.load() && m_connected.load())
    {
        {
            // TheSuperHackers @fix curl_multi_poll's out-param is numfds (fds with
            // activity during this poll), NOT "transfers still running" — it can be 0
            // on a timeout even though curl_multi_perform() would still have work to
            // do. curl_multi_perform() must be called unconditionally every iteration,
            // or received bytes can sit unprocessed forever and curl_ws_recv() keeps
            // returning CURLE_AGAIN even though data already arrived on the socket.
            int numfds = 0;
            CURLMcode mpoll = curl_multi_poll(m_curlMulti, NULL, 0, 50, &numfds);
            if (mpoll != CURLM_OK)
            {
                liveObserverLog("LiveObserver: curl_multi_poll failed (%d), connection lost\n", (int)mpoll);
                m_connected.store(false);
                break;
            }
            int runningHandles = 0;
            curl_multi_perform((CURLM*)m_curlMulti, &runningHandles);
        }

        // Append any newly received data to our persistent buffer
        {
            std::vector<char> tmp;
            if (wsRecv(tmp) && !tmp.empty())
            {
                totalBytesReceived += tmp.size();
                if (totalBytesReceived % 10240 < tmp.size() || totalBytesReceived < 1024)
                    liveObserverLog("LiveObserver: received %zu bytes from relay (total=%zu, queued=%zu)\n",
                        tmp.size(), totalBytesReceived, buf.size() + tmp.size());
                buf.insert(buf.end(), tmp.begin(), tmp.end());
            }
        }

        // Process as many complete frames as possible from the buffer
        while (buf.size() >= 5)
        {
            // TheSuperHackers @fix `buf` is std::vector<char>, and char is signed on MSVC.
            // Casting a byte >= 0x80 straight to unsigned int sign-extends it (e.g. 0x87 ->
            // 0xFFFFFF87) instead of zero-extending, corrupting the length for roughly half of
            // all possible values. Must cast through (unsigned char) first to zero-extend.
            unsigned char msgType = (unsigned char)buf[0];
            unsigned int msgLen = (unsigned int)(unsigned char)buf[1]
                | ((unsigned int)(unsigned char)buf[2] << 8)
                | ((unsigned int)(unsigned char)buf[3] << 16)
                | ((unsigned int)(unsigned char)buf[4] << 24);

            if (buf.size() < (size_t)(5 + msgLen))
            {
                // TheSuperHackers @fix Log once per distinct incomplete-frame situation
                // (not every ~50ms poll) so we can tell a legitimately large still-arriving
                // frame apart from a corrupted/garbage length that would wait forever.
                static unsigned int s_lastWaitMsgLen = 0xFFFFFFFFu;
                static unsigned char s_lastWaitMsgType = 0xFF;
                if (msgLen != s_lastWaitMsgLen || msgType != s_lastWaitMsgType)
                {
                    liveObserverLog("LiveObserver: waiting for frame type=%d len=%u — have %zu of %zu bytes\n",
                        (int)msgType, msgLen, buf.size(), (size_t)(5 + msgLen));
                    s_lastWaitMsgLen = msgLen;
                    s_lastWaitMsgType = msgType;
                }
                break; // Partial frame — wait for more data
            }

            const char* payload = (msgLen > 0) ? buf.data() + 5 : nullptr;
            handleFrame(msgType, payload, msgLen);
            ++totalFramesProcessed;

            // Remove the processed frame from the buffer
            buf.erase(buf.begin(), buf.begin() + 5 + msgLen);
        }
    }

    // Cleanup
    if (m_liveFile)
    {
        m_liveFile->close();
        m_liveFile = nullptr;
    }
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
    liveObserverLog("LiveObserver::networkThreadFunc ended — totalBytes=%zu totalFrames=%zu\n", totalBytesReceived, totalFramesProcessed);
}

#endif // GENERALS_ONLINE
