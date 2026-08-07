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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/Recorder.h"
#include "Common/ReplayStreamSink.h"
#include "Common/LiveStreamer.h"
#include "Common/LiveObserver.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Shell.h"
#include "GameClient/GameText.h"

#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/GameMessageParser.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"
#include "Common/RandomValue.h"
#include "Common/CRCDebug.h"
#include "Common/OptionPreferences.h"
#include "Common/version.h"
#include "../NGMPGame.h"
#include "../OnlineServices_Init.h"

extern NGMPGame* TheNGMPGame;

constexpr const char s_genrep[] = "GENREP";
constexpr const UnsignedInt replayBufferBytes = 8192;

Int REPLAY_CRC_INTERVAL = 100;

const char* replayExtention = ".rep";
const char* lastReplayFileName = "00000000";	// a name the user is unlikely to ever type, but won't cause panic & confusion

// TheSuperHackers @tweak helmutbuhler 25/04/2025
// The replay header contains two time fields; startTime and endTime of type time_t.
// time_t is 32 bit wide on VC6, but on newer compilers it is 64 bit wide.
// In order to remain compatible we need to load and save time values with 32 bits.
// Note that this will overflow on January 18, 2038. @todo Upgrade to 64 bits when we break compatibility.
typedef int32_t replay_time_t;

static time_t startTime;
static const UnsignedInt startTimeOffset = 6;
static const UnsignedInt endTimeOffset = startTimeOffset + sizeof(replay_time_t);
static const UnsignedInt frameCountOffset = endTimeOffset + sizeof(replay_time_t);
static const UnsignedInt desyncOffset = frameCountOffset + sizeof(UnsignedInt);
static const UnsignedInt quitEarlyOffset = desyncOffset + sizeof(Bool);
static const UnsignedInt disconOffset = quitEarlyOffset + sizeof(Bool);

static void writeAtOffset(File* file, Int offset, const void* data, Int dataSize)
{
	UnsignedInt fileSize = file->size();
	DEBUG_ASSERTCRASH((UnsignedInt)(offset + dataSize) <= fileSize, ("writeAtOffset would exceed file size!"));
	if (file->seek(offset, File::seekMode::START) == offset)
	{
		file->write(data, dataSize);
	}
	MAYBE_UNUSED Int res = file->seek(fileSize, File::seekMode::START);
	(void)res;
	DEBUG_ASSERTCRASH(res == fileSize, ("Could not seek to end of file!"));

	if (TheRecorder && TheRecorder->getStreamSink())
		TheRecorder->getStreamSink()->onHeaderPatch(offset, data, dataSize);
}

static void writeBodyBytes(File* file, const void* data, Int size)
{
	file->write(data, size);
	if (TheRecorder && TheRecorder->getStreamSink())
		TheRecorder->getStreamSink()->onBodyBytes(data, size);
}


#if defined(RTS_DEBUG)
static FILE* openStatsLogFile()
{
	unsigned long bufSize = MAX_COMPUTERNAME_LENGTH + 1;
	char computerName[MAX_COMPUTERNAME_LENGTH + 1];
	if (!GetComputerName(computerName, &bufSize))
	{
		strcpy(computerName, "unknown");
	}
	AsciiString statsFile = TheGlobalData->m_baseStatsDir;
	statsFile.concat(computerName);
	statsFile.concat(".txt");
	return fopen(statsFile.str(), "a+");
}
#endif

RecorderClass::CRCInfo::CRCInfo() :
	m_sawCRCMismatch(FALSE),
	m_skippedOne(FALSE),
	m_localPlayer(0)
{}

RecorderClass::CRCInfo::CRCInfo(UnsignedInt localPlayer, Bool isMultiplayer)
{
	m_sawCRCMismatch = FALSE;
	m_skippedOne = !isMultiplayer;
	m_localPlayer = localPlayer;
}

void RecorderClass::CRCInfo::addCRC(UnsignedInt val)
{
	// TheSuperHackers @fix helmutbuhler 03/04/2025
	// In Multiplayer, the first MSG_LOGIC_CRC message somehow doesn't make it through the network.
	// Perhaps this happens because the network is not yet set up on frame 0.
	// So we also don't queue up the first local crc message, otherwise the crc
	// messages wouldn't match up anymore and we'd desync immediately during playback.
	if (!m_skippedOne)
	{
		m_skippedOne = TRUE;
		return;
	}

	m_data.push_back(val);
	//DEBUG_LOG(("CRCInfo::addCRC() - crc %8.8X pushes list to %d entries (full=%d)", val, m_data.size(), !m_data.empty()));
}

UnsignedInt RecorderClass::CRCInfo::readCRC()
{
	if (m_data.empty())
	{
		DEBUG_LOG(("CRCInfo::readCRC() - bailing, full=0, size=%d", m_data.size()));
		return 0;
	}

	UnsignedInt val = m_data.front();
	m_data.pop_front();
	//DEBUG_LOG(("CRCInfo::readCRC() - returning %8.8X, full=%d, size=%d", val, !m_data.empty(), m_data.size()));
	return val;
}

void RecorderClass::logGameStart(AsciiString options)
{
	if (!m_file)
		return;

	time(&startTime);
	replay_time_t tmp = (replay_time_t)startTime;
	writeAtOffset(m_file, startTimeOffset, &tmp, sizeof(tmp));

#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		TheFileSystem->createDirectory(TheGlobalData->m_baseStatsDir);
		FILE *logFP = openStatsLogFile();
		if (!logFP)
		{
			TheWritableGlobalData->m_baseStatsDir = TheGlobalData->getPath_UserData();
			logFP = openStatsLogFile();
		}
		if (logFP)
		{
			struct tm *t2 = localtime(&startTime);
			fprintf(logFP, "\nGame start at %s\tOptions are %s\n", asctime(t2), options.str());
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logPlayerDisconnect(UnicodeString player, Int slot)
{
	if (!m_file)
		return;

	DEBUG_ASSERTCRASH((slot >= 0) && (slot < MAX_SLOTS), ("Attempting to disconnect an invalid slot number"));
	if ((slot < 0) || (slot >= (MAX_SLOTS)))
	{
		return;
	}
	Bool flag = TRUE;
	Int playerSlotDisconOffset = disconOffset + slot * sizeof(Bool);
	writeAtOffset(m_file, playerSlotDisconOffset, &flag, sizeof(flag));

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			time_t t;
			time(&t);
			struct tm *t2 = localtime(&t);
			fprintf(logFP, "\tPlayer %ls dropped at %s", player.str(), asctime(t2));
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logCRCMismatch()
{
	if (!m_file)
		return;

	Bool flag = TRUE;
	writeAtOffset(m_file, desyncOffset, &flag, sizeof(flag));

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		m_wasDesync = TRUE;
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			time_t t;
			time(&t);
			struct tm *t2 = localtime(&t);
			fprintf(logFP, "\tCRC mismatch at %s", asctime(t2));
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logGameEnd()
{
	if (!m_file)
		return;

	time_t t;
	time(&t);
	UnsignedInt frameCount = TheGameLogic->getFrame();
	replay_time_t tmp = (replay_time_t)t;
	writeAtOffset(m_file, endTimeOffset, &tmp, sizeof(tmp));
	writeAtOffset(m_file, frameCountOffset, &frameCount, sizeof(frameCount));

#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			struct tm *t2 = localtime(&t);
			time_t duration = t - startTime;
			Int minutes = duration/60;
			Int seconds = duration%60;
			fprintf(logFP, "Game end at   %s(%d:%2.2d elapsed time)\n", asctime(t2), minutes, seconds);
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::cleanUpReplayFile()
{
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		char fname[_MAX_PATH + 1];
		strlcpy(fname, TheGlobalData->m_baseStatsDir.str(), ARRAY_SIZE(fname));
		strlcat(fname, m_fileName.str(), ARRAY_SIZE(fname));
		DEBUG_LOG(("Saving replay to %s", fname));
		AsciiString oldFname;
		oldFname.format("%s%s", getReplayDir().str(), m_fileName.str());
		CopyFile(oldFname.str(), fname, TRUE);

#ifdef DEBUG_LOGGING
		const char* logFileName = DebugGetLogFileName();
		if (logFileName[0] == '\0')
			return;

		AsciiString debugFname = fname;
		debugFname.truncateBy(3);
		debugFname.concat("txt");
		UnsignedInt fileSize = 0;
		FILE* fp = fopen(logFileName, "rb");
		if (fp)
		{
			fseek(fp, 0, SEEK_END);
			fileSize = ftell(fp);
			fclose(fp);
			fp = nullptr;
			DEBUG_LOG(("Log file size was %d", fileSize));
		}

		const int MAX_DEBUG_SIZE = 65536;
		if (fileSize <= MAX_DEBUG_SIZE || TheGlobalData->m_saveAllStats)
		{
			DEBUG_LOG(("Using CopyFile to copy %s", logFileName));
			CopyFile(logFileName, debugFname.str(), TRUE);
		}
		else
		{
			DEBUG_LOG(("manual copy of %s", logFileName));
			FILE* ifp = fopen(logFileName, "rb");
			FILE* ofp = fopen(debugFname.str(), "wb");
			if (ifp && ofp)
			{
				fseek(ifp, fileSize - MAX_DEBUG_SIZE, SEEK_SET);
				char buf[4096];
				Int len;
				while ((len = fread(buf, 1, 4096, ifp)) > 0)
				{
					fwrite(buf, 1, len, ofp);
				}
				fclose(ofp);
				fclose(ifp);
				ifp = nullptr;
				ofp = nullptr;
			}
			else
			{
				if (ifp) fclose(ifp);
				if (ofp) fclose(ofp);
				ifp = nullptr;
				ofp = nullptr;
			}
		}
#endif // DEBUG_LOGGING
	}
#endif
}

/**
 * The recorder object.
 */
RecorderClass *TheRecorder = nullptr;

/**
 * Constructor
 */
RecorderClass::RecorderClass()
{
	m_originalGameMode = GAME_NONE;
	m_mode = RECORDERMODETYPE_RECORD;
	m_file = nullptr;
	m_fileName.clear();
	m_currentFilePosition = 0;
	m_doingAnalysis = FALSE;
	m_archiveReplays = FALSE;
	m_nextFrame = 0;
	m_wasDesync = FALSE;
	m_streamSink = nullptr;
	m_isLiveStream = FALSE;
	m_userPaused = FALSE;
	init(); // just for the heck of it.
}

/**
 * Destructor
 */
RecorderClass::~RecorderClass() {
}

/**
 * Initialization
 * The recorder will record by default since every game will be recorded.
 * Obviously a game that is being played back will not be recorded.
 * Since the playback is done through a special interface, that interface
 * will set the recorder mode to RECORDERMODETYPE_PLAYBACK.
 */
void RecorderClass::init() {
	// This pair is deliberately ungated, unlike every other live-observer log site: they exist to
	// catch m_mode being silently reset to RECORDERMODETYPE_NONE, so gating them on the mode still
	// being LIVE_OBSERVER would hide exactly the failure they were added to detect.
	liveObserverLog("RecorderClass::init: enter — m_mode=%d m_isLiveStream=%d\n", m_mode, m_isLiveStream ? 1 : 0);
	m_originalGameMode = GAME_NONE;
	if (m_mode != RECORDERMODETYPE_LIVE_OBSERVER)
		m_mode = RECORDERMODETYPE_NONE;
	m_file = nullptr;
	m_fileName.clear();
	m_currentFilePosition = 0;
	m_gameInfo.clearSlotList();
	m_gameInfo.reset();
	if (TheGlobalData->m_pendingFile.isEmpty())
		m_gameInfo.setMap(TheGlobalData->m_mapName);
	else
		m_gameInfo.setMap(TheGlobalData->m_pendingFile);
	m_gameInfo.setSeed(GetGameLogicRandomSeed());
	m_wasDesync = FALSE;
	m_doingAnalysis = FALSE;
	m_playbackFrameCount = 0;
	m_streamSink = nullptr;
	if (m_mode != RECORDERMODETYPE_LIVE_OBSERVER)
		m_isLiveStream = FALSE;
	m_userPaused = FALSE;

	OptionPreferences optionPref;
	m_archiveReplays = optionPref.getArchiveReplaysEnabled();

	liveObserverLog("RecorderClass::init: exit — m_mode=%d m_isLiveStream=%d\n", m_mode, m_isLiveStream ? 1 : 0);
}

/**
 * Reset the recorder to the "initialized state."
 */
void RecorderClass::reset() {
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();

	init();
}

/**
 * update
 * Do the update for this frame.
 */
void RecorderClass::update() {
	if (m_mode == RECORDERMODETYPE_RECORD || m_mode == RECORDERMODETYPE_NONE) {
		updateRecord();
	}
	else if (isPlaybackMode()) {
		updatePlayback();
	}
}

/**
 * Do the update for the next frame of this playback.
 */
void RecorderClass::updatePlayback() {
	// Remove any bad commands that have been inserted by the local user that shouldn't be
	// executed during playback.
	CullBadCommandsResult result = cullBadCommands();

	if (result.hasClearGameDataMessage) {
		LIVE_OBSERVER_LOG("updatePlayback: MSG_CLEAR_GAME_DATA detected, stopping command processing. curFrame=%d nextFrame=%d\n",
			TheGameLogic->getFrame(), m_nextFrame);
		// TheSuperHackers @bugfix Stop appending more commands if the replay playback is about to end.
		// Previously this would be able to append more commands, which could have unintended consequences,
		// such as crashing the game when a MSG_PLACE_BEACON is appended after MSG_CLEAR_GAME_DATA.
		// MSG_CLEAR_GAME_DATA is supposed to be processed later this frame, which will then stop this playback.
		return;
	}

	if (m_nextFrame == -1) {
		// This is reached if there are no more commands to be executed.
		return;
	}
	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (m_doingAnalysis)
		curFrame = m_nextFrame;

	// While there are commands to be queued up for this frame or a past frame (live observer may be behind), process them.
	while (m_nextFrame != (UnsignedInt)-1 && m_nextFrame <= curFrame) {
		if (m_isLiveStream) {
			// In live mode: consume the frame number first. May rewind if the frame is in
			// the future, or report that no complete record is available yet.
			ReadFrameResult r = readNextFrame();
			if (r == READFRAME_EOF_WAITING) {
				// Nothing more to read this tick. Breaking out here is what stops the old
				// unbounded spin: previously m_nextFrame was forced to curFrame, which
				// satisfied no break condition, so this loop re-read EOF forever.
				break;
			}
			if (r == READFRAME_STREAM_STOPPED)
				break;
		}
		if (m_nextFrame > curFrame)
			break;				// readNextFrame saw a future frame — wait for game to catch up
		appendNextCommand();	// append the next command to TheCommandQueue
		if (!m_isLiveStream)
			readNextFrame();	// Read the next command's frame number for playback.
	}

	// Whether the observer may keep running is the session's call, not ours: it depends on the
	// broadcast delay and the live edge, both of which belong to LiveObserver. All we supply
	// is the playback state it cannot see.
	if (m_isLiveStream && TheLiveObserver)
		TheLiveObserver->updatePlaybackGate(curFrame, m_userPaused);
}

/**
 * Live-stream housekeeping that must run even when GameLogic::UPDATE() is being skipped.
 *
 * The buffering pause genuinely halts logic (see GameEngine::isGameHalted). updatePlayback()
 * runs from inside that halted update, so it cannot be the thing that lifts its own pause —
 * that was a self-deadlock, previously worked around by making the pause a no-op for
 * observers entirely. Calling this from GameEngine::update() breaks the cycle: the gate is
 * re-evaluated against fresh network data and clears the pause when the buffer is ready. It
 * deliberately does not append commands; that stays in updatePlayback().
 */
void RecorderClass::updateLiveStreamPoll() {
	if (!m_isLiveStream || m_mode != RECORDERMODETYPE_LIVE_OBSERVER || TheLiveObserver == nullptr)
		return;
	if (m_nextFrame == (UnsignedInt)-1 || TheGameLogic == nullptr)
		return;

	TheLiveObserver->updatePlaybackGate(TheGameLogic->getFrame(), m_userPaused);
}

Bool RecorderClass::liveStreamEnded() const {
	return TheLiveObserver ? TheLiveObserver->isStreamEnded() : TRUE;
}

/**
 * Stop the currently running playback. This is probably due either to the user exiting out of the playback or
 * reaching the end of the playback file.
 */
void RecorderClass::stopPlayback() {
	LIVE_OBSERVER_LOG("stopPlayback: isLiveStream=%d streamEnded=%d mode=%d nextFrame=%d curFrame=%d\n",
		m_isLiveStream, liveStreamEnded(), (int)m_mode, m_nextFrame, TheGameLogic->getFrame());
	Bool wasLiveObserver = (m_mode == RECORDERMODETYPE_LIVE_OBSERVER);
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();

	if (wasLiveObserver)
	{
		// The same teardown the menu path uses, deliberately: a live session has exactly one
		// way to end, so there is no second list of fields here to keep in step with it.
		// What remains below is the shell state that only this exit path has to put back.
		endLiveObserverSession();
		if (TheWritableGlobalData)
			TheWritableGlobalData->m_shellMapOn = TRUE;
		liveObserverLog("stopPlayback: live session ended - mode=%d isLiveStream=%d shellMapOn=%d\n",
			(int)m_mode, m_isLiveStream ? 1 : 0,
			TheWritableGlobalData ? (TheWritableGlobalData->m_shellMapOn ? 1 : 0) : -1);
	}

	if (!m_doingAnalysis)
	{
		TheGameLogic->exitGame();
	}
}

/**
 * Update function for recording a game. Basically all the pertinent logic commands for this frame are written out
 * to a file.
 */
void RecorderClass::updateRecord()
{
	Bool needFlush = FALSE;
	static Int lastFrame = -1;
	GameMessage* msg = TheCommandList->getFirstMessage();

	// DEBUG: log message count every 300 frames
	{
		static UnsignedInt s_lastCountLog = 0;
		UnsignedInt curFrame = TheGameLogic->getFrame();
		Int msgCount = 0;
		Int netMsgCount = 0;
		GameMessage* tmp = msg;
		while (tmp) {
			msgCount++;
			GameMessage::Type t = tmp->getType();
			if (t > GameMessage::MSG_BEGIN_NETWORK_MESSAGES && t < GameMessage::MSG_END_NETWORK_MESSAGES)
				netMsgCount++;
			tmp = tmp->next();
		}
		if (curFrame - s_lastCountLog >= 300)
		{
			char buf[256];
			sprintf(buf, "Recorder::updateRecord frame=%u total=%d net=%d\n", curFrame, msgCount, netMsgCount);
			OutputDebugStringA(buf);
			liveStreamLog("%s", buf);
			s_lastCountLog = curFrame;
		}
	}

	while (msg != nullptr) {
		if (msg->getType() == GameMessage::MSG_NEW_GAME &&
			msg->getArgument(0)->integer != GAME_SHELL &&
			msg->getArgument(0)->integer != GAME_SINGLE_PLAYER && // Due to the massive amount of scripts that use <local player> in GC and single player, replays have been cut for them.
			msg->getArgument(0)->integer != GAME_NONE)
		{
			m_originalGameMode = msg->getArgument(0)->integer;
			DEBUG_LOG(("RecorderClass::updateRecord() - original game is mode %d", m_originalGameMode));
			lastFrame = 0;
			GameDifficulty diff = DIFFICULTY_NORMAL;
			if (msg->getArgumentCount() >= 2)
				diff = (GameDifficulty)msg->getArgument(1)->integer;
			Int rankPoints = 0;
			if (msg->getArgumentCount() >= 3)
				rankPoints = msg->getArgument(2)->integer;
			Int maxFPS = 0;
			if (msg->getArgumentCount() >= 4)
				maxFPS = msg->getArgument(3)->integer;

			startRecording(diff, m_originalGameMode, rankPoints, maxFPS);
		}
		else if (msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA) {
			if (m_file != nullptr) {
				lastFrame = -1;
				writeToFile(msg);
				stopRecording();
				needFlush = FALSE;
			}
			m_fileName.clear();
		}
		else {
			// Write network messages to .rep file (if recording is active)
			if (m_file != nullptr) {
				if ((msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
					(msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES)) {
					// Only write the important messages to the file.
					writeToFile(msg);
					needFlush = TRUE;
				}
			}
		}
		msg = msg->next();
	}

	if (needFlush) {
		DEBUG_ASSERTCRASH(m_file != nullptr, ("RecorderClass::updateRecord() - unexpected call to fflush(m_file)"));
		m_file->flush();
	}

	if (m_streamSink)
		m_streamSink->onBodyFlush();
}

/**
 * Start a new file for recording. This will always overwrite the "LastReplay.rep" file with the new one.
 * So don't call this unless you really mean it.
 */
void RecorderClass::startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS) {
	DEBUG_ASSERTCRASH(m_file == nullptr, ("Starting to record game while game is in progress."));

	reset();

	m_mode = RECORDERMODETYPE_RECORD;

	AsciiString filepath = getReplayDir();

	// We have to make sure the replay dir exists.
	TheFileSystem->createDirectory(filepath);

	m_fileName = getLastReplayFileName();
	m_fileName.concat(getReplayExtention());
	filepath.concat(m_fileName);
	m_file = TheFileSystem->openFile(filepath.str(), File::READWRITE | File::BINARY | File::CREATE);
	if (m_file == nullptr) {
		DEBUG_ASSERTCRASH(m_file != nullptr, ("Failed to create replay file"));
		return;
	}
	// TheSuperHackers @info the null terminator needs to be ignored to maintain retail replay file layout
	m_file->writeFormat("%s", s_genrep);

	//
	// save space for stats to be filled in.
	//
	// **** if this changes, change the LAN code above ****
	//
	replay_time_t time = 0;
	m_file->write(&time, sizeof(time));	// reserve space for start time
	m_file->write(&time, sizeof(time));	// reserve space for end time

	UnsignedInt frames = 0;
	m_file->write(&frames, sizeof(frames));	// reserve space for duration in frames

	Bool flag = FALSE;
	m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if we desync)
	m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if we quit early)
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if player i disconnects)
	}

	// Print out the name of the replay.
	UnicodeString replayName;
	replayName = TheGameText->fetch("GUI:LastReplay");
	m_file->writeFormat(L"%s", replayName.str());
	m_file->writeChar(L"\0");

	// Date and Time
	SYSTEMTIME systemTime;
	GetLocalTime(&systemTime);
	m_file->write(&systemTime, sizeof(systemTime));

	// write out version info
	UnicodeString versionString = TheVersion->getUnicodeVersion();
	UnicodeString versionTimeString = TheVersion->getUnicodeBuildTime();
	UnsignedInt versionNumber = TheVersion->getVersionNumber();
	m_file->writeFormat(L"%s", versionString.str());
	m_file->writeChar(L"\0");
	m_file->writeFormat(L"%s", versionTimeString.str());
	m_file->writeChar(L"\0");
	m_file->write(&versionNumber, sizeof(versionNumber));
	m_file->write(&(TheGlobalData->m_exeCRC), sizeof(TheGlobalData->m_exeCRC));
	m_file->write(&(TheGlobalData->m_iniCRC), sizeof(TheGlobalData->m_iniCRC));

	// Number of players
	/*
	Int numPlayers = ThePlayerList->getPlayerCount();
	fwrite(&numPlayers, sizeof(numPlayers), 1, m_file);
	*/

	// Write the slot list.
	AsciiString theSlotList;
	Int localIndex = -1;
	if (TheNetwork)
	{
		if (TheLAN)
		{
			GameInfo* game = TheLAN->GetMyGame();
			DEBUG_ASSERTCRASH(game, ("Starting a LAN game with no LANGameInfo object!"));
			theSlotList = GameInfoToAsciiString(game);

			for (Int i = 0; i < MAX_SLOTS; ++i)
			{
				if (game->getLocalIP() == game->getSlot(i)->getIP())
				{
					localIndex = i;
					break;
				}
			}
		}
		else
		{
#if defined(GENERALS_ONLINE)
			theSlotList = GameInfoToAsciiString(TheNGMPGame);
			localIndex = TheNGMPGame->getLocalSlotNum();
#else
			theSlotList = GameInfoToAsciiString(TheGameSpyGame);
			localIndex = TheGameSpyGame->getLocalSlotNum();
#endif
		}
	}
	else
	{
		if (TheSkirmishGameInfo)
		{
			TheSkirmishGameInfo->setCRCInterval(REPLAY_CRC_INTERVAL);
			theSlotList = GameInfoToAsciiString(TheSkirmishGameInfo);
			DEBUG_LOG(("GameInfo String: %s", theSlotList.str()));
			localIndex = 0;
		}
		else
		{
			// single player.  format the generic (empty) slotlist
			m_gameInfo.setCRCInterval(REPLAY_CRC_INTERVAL);
			theSlotList = GameInfoToAsciiString(&m_gameInfo);
		}
	}
	logGameStart(theSlotList);
	DEBUG_LOG(("RecorderClass::startRecording - theSlotList = %s", theSlotList.str()));

	// write slot list (starting spots, color, alliances, etc
	m_file->writeFormat("%s", theSlotList.str());
	m_file->writeChar("\0");

	m_file->writeFormat("%d", localIndex);
	m_file->writeChar("\0");

	/*
	/// @todo fix this to use starting spots and player alliances when those are put in the game.
	for (Int i = 0; i < numPlayers; ++i) {
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr) {
			continue;
		}
		UnicodeString name = player->getPlayerDisplayName();
		fwprintf(m_file, L"%s", name.str());
		fputwc(0, m_file);
		UnicodeString faction = player->getFaction()->getFactionDisplayName();
		fwprintf(m_file, L"%s", faction.str());
		fputwc(0, m_file);
		Int color = player->getColor()->getAsInt();
		fwrite(&color, sizeof(color), 1, m_file);
		Int team = 0;
		Int startingSpot = 0;
		fwrite(&startingSpot, sizeof(Int), 1, m_file);
		fwrite(&team, sizeof(Int), 1, m_file);
	}
	*/

	// Write the game difficulty.
	m_file->write(&diff, sizeof(diff));

	// Write original game mode
	m_file->write(&originalGameMode, sizeof(originalGameMode));

	// Write rank points to add at game start
	m_file->write(&rankPoints, sizeof(rankPoints));

	// Write maxFPS chosen
	m_file->write(&maxFPS, sizeof(maxFPS));

	DEBUG_LOG(("RecorderClass::startRecording() - diff=%d, mode=%d, FPS=%d", diff, originalGameMode, maxFPS));

	// --- Always log live stream config at game start (works without GENERALS_ONLINE) ---
	liveStreamerInitLog();
	if (TheGlobalData)
	{
		liveStreamLog("=== Live Stream Config (Recorder) ===\n");
		liveStreamLog("LiveStreamEnabled: %s\n", TheGlobalData->m_liveStreamEnabled ? "true" : "false");
		liveStreamLog("LiveStreamRelayUrl: %s\n", TheGlobalData->m_liveStreamRelayUrl.str());
		liveStreamLog("LiveStreamCanStream: %s\n", TheGlobalData->m_liveStreamCanStream ? "true" : "false");
	}
	// --- End live stream config logging ---

	/*
	// Write the map name.
	fprintf(m_file, "%s", (TheGlobalData->m_mapName).str());
	fputc(0, m_file);
	*/

	/// @todo Need to write game options when there are some to be written.

#if defined(GENERALS_ONLINE)
	// Live streaming — initialize and register with relay server
	try
	{
	// The pre-game lobby already assembled the registration — it is the only place that can see
	// a GeneralsOnline lobby in full, and doing it there is what keeps this file free of any GO
	// includes. All that is left here, at the moment a match actually begins, is to open the
	// session and hook the streamer in as a replay sink.
	LiveStreamer* streamer = liveStreamStartPendingSession();
	if (streamer)
	{
		// From now on the streamer receives raw header/body/patch bytes.
		m_streamSink = streamer;
		DEBUG_LOG(("RecorderClass::startRecording() - Live stream registered, lobbyId=%s",
			streamer->getLobbyId().str()));
	}
	}
	catch (...)
	{
		// Live streamer init failed — game continues without streaming
		DEBUG_LOG(("RecorderClass::startRecording() - Live streamer init failed, continuing without streaming"));
		liveStreamLog("RecorderClass::startRecording() - Live streamer init EXCEPTION, continuing without streaming\n");
	}
#endif

	// Header complete — snapshot it for the stream sink (now that m_streamSink may be set above).
	// This captures ALL header bytes (GENREP magic, binary fields, format writes)
	// as a single byte-for-byte identical blob that an observer can write to disk.
	if (m_streamSink)
	{
		m_file->flush();
		UnsignedInt headerSize = m_file->size();
		if (headerSize > 0)
		{
			char* headerBuf = new char[headerSize];
			Int seekRes = m_file->seek(0, File::seekMode::START);
			if (seekRes == 0)
			{
				Int bytesRead = m_file->read(headerBuf, headerSize);
				if (bytesRead == (Int)headerSize)
					m_streamSink->onHeaderBytes(headerBuf, headerSize);
			}
			m_file->seek(headerSize, File::seekMode::START);
			delete[] headerBuf;
		}
		m_streamSink->onHeaderComplete();
	}
}

/**
 * This will stop the current recording session and close the file. This should always be called at the end of
 * every game.
 */
void RecorderClass::stopRecording() {
	logGameEnd();

	if (m_streamSink)
		m_streamSink->onRecordingEnded();

	if (TheNetwork)
	{
		//if (TheLAN)
		{
			if (m_wasDesync)
				cleanUpReplayFile();
			m_wasDesync = FALSE;
		}
	}
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;

		if (m_archiveReplays)
			archiveReplay(m_fileName);
		// upload
#if defined(GENERALS_ONLINE)
		if (TheNGMPGame != nullptr)
		{
			NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
			if (pOnlineServicesMgr != nullptr)
			{
				AsciiString absoluteReplayPath = getReplayDir();
				absoluteReplayPath.concat(m_fileName);

				pOnlineServicesMgr->CommitReplay(absoluteReplayPath);
			}
		}
#endif
	}
	m_fileName.clear();

#if defined(GENERALS_ONLINE)
	// Live streaming — shut down the streamer when recording stops
	if (TheLiveStreamer)
	{
		TheLiveStreamer->close();
		delete TheLiveStreamer;
		TheLiveStreamer = nullptr;
		m_streamSink = nullptr;
	}
#endif
}

/**
 * TheSuperHackers @feature Stubbjax 17/10/2025 Copy the replay file to the archive directory and rename it using the current timestamp.
 */
void RecorderClass::archiveReplay(AsciiString fileName)
{
	SYSTEMTIME st;
	GetLocalTime(&st);

	AsciiString archiveFileName;
	// Use a standard YYYYMMDD_HHMMSS format for simplicity and to avoid conflicts.
	archiveFileName.format("%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	AsciiString extension = getReplayExtention();
	AsciiString sourcePath = getReplayDir();
	sourcePath.concat(fileName);

	if (!sourcePath.endsWith(extension))
		sourcePath.concat(extension);

	AsciiString destPath = getReplayArchiveDir();
	TheFileSystem->createDirectory(destPath.str());

	destPath.concat(archiveFileName);
	destPath.concat(extension);

	if (!CopyFile(sourcePath.str(), destPath.str(), FALSE))
		DEBUG_LOG(("RecorderClass::archiveReplay: Failed to copy %s to %s", sourcePath.str(), destPath.str()));
}

/**
 * Write this game message to the record file. This also writes the game message's execution frame.
 */
void RecorderClass::writeToFile(GameMessage* msg) {
	// Write the frame number for this command.
	UnsignedInt frame = TheGameLogic->getFrame();
	writeBodyBytes(m_file, &frame, sizeof(frame));

	// Write the command type
	GameMessage::Type type = msg->getType();
	writeBodyBytes(m_file, &type, sizeof(type));

	// Write the player index
	Int playerIndex = msg->getPlayerIndex();
	writeBodyBytes(m_file, &playerIndex, sizeof(playerIndex));

#ifdef DEBUG_LOGGING
	AsciiString commandName = msg->getCommandAsString();
	if (type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (Non-Network message!)");
	}
	else if (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		AsciiString tmp;
		tmp.format(" (CRC 0x%8.8X)", msg->getArgument(0)->integer);
		commandName.concat(tmp);
	}

	//DEBUG_LOG(("RecorderClass::writeToFile - Adding %s command from player %d to TheCommandList on frame %d",
		//commandName.str(), msg->getPlayerIndex(), TheGameLogic->getFrame()));
#endif // DEBUG_LOGGING

	GameMessageParser* parser = newInstance(GameMessageParser)(msg);
	UnsignedByte numTypes = parser->getNumTypes();
	writeBodyBytes(m_file, &numTypes, sizeof(numTypes));

	GameMessageParserArgumentType* argType = parser->getFirstArgumentType();
	while (argType != nullptr) {
		UnsignedByte type = (UnsignedByte)(argType->getType());
		writeBodyBytes(m_file, &type, sizeof(type));

		UnsignedByte argTypeCount = (UnsignedByte)(argType->getArgCount());
		writeBodyBytes(m_file, &argTypeCount, sizeof(argTypeCount));

		argType = argType->getNext();
	}

	//	UnsignedByte lasttype = (UnsignedByte)ARGUMENTDATATYPE_UNKNOWN;
	Int numArgs = msg->getArgumentCount();
	for (Int i = 0; i < numArgs; ++i) {
		//		UnsignedByte type = (UnsignedByte)(msg->getArgumentDataType(i));
		//		if (lasttype != type) {
		//			fwrite(&type, sizeof(type), 1, m_file);
		//			lasttype = type;
		//		}
		writeArgument(msg->getArgumentDataType(i), *(msg->getArgument(i)));
	}

	deleteInstance(parser);
	parser = nullptr;

}

void RecorderClass::writeArgument(GameMessageArgumentDataType type, const GameMessageArgumentType arg) {

	switch (type) {

	case ARGUMENTDATATYPE_INTEGER:
		writeBodyBytes(m_file, &(arg.integer), sizeof(arg.integer));
		break;
	case ARGUMENTDATATYPE_REAL:
		writeBodyBytes(m_file, &(arg.real), sizeof(arg.real));
		break;
	case ARGUMENTDATATYPE_BOOLEAN:
		writeBodyBytes(m_file, &(arg.boolean), sizeof(arg.boolean));
		break;
	case ARGUMENTDATATYPE_OBJECTID:
		writeBodyBytes(m_file, &(arg.objectID), sizeof(arg.objectID));
		break;
	case ARGUMENTDATATYPE_DRAWABLEID:
		writeBodyBytes(m_file, &(arg.drawableID), sizeof(arg.drawableID));
		break;
	case ARGUMENTDATATYPE_TEAMID:
		writeBodyBytes(m_file, &(arg.teamID), sizeof(arg.teamID));
		break;
	case ARGUMENTDATATYPE_LOCATION:
		writeBodyBytes(m_file, &(arg.location), sizeof(arg.location));
		break;
	case ARGUMENTDATATYPE_PIXEL:
		writeBodyBytes(m_file, &(arg.pixel), sizeof(arg.pixel));
		break;
	case ARGUMENTDATATYPE_PIXELREGION:
		writeBodyBytes(m_file, &(arg.pixelRegion), sizeof(arg.pixelRegion));
		break;
	case ARGUMENTDATATYPE_TIMESTAMP:
		writeBodyBytes(m_file, &(arg.timestamp), sizeof(arg.timestamp));
		break;
	case ARGUMENTDATATYPE_WIDECHAR:
		writeBodyBytes(m_file, &(arg.wChar), sizeof(arg.wChar));
		break;
	default:
		DEBUG_LOG(("Unknown GameMessageArgumentDataType in RecorderClass::writeArgument"));
		break;
	}
}

/**
 * Read in a replay header, for (1) populating a replay listbox or (2) starting playback.  In
 * case (2), set FILE *m_file.
 */
Bool RecorderClass::readReplayHeader(ReplayHeader& header)
{
	AsciiString filepath = getReplayDir();
	filepath.concat(header.filename.str());

	// TheSuperHackers @performance More buffered data reduces disk overhead and will improve fast forward playback
	const UnsignedInt buffersize = header.forPlayback ? replayBufferBytes : File::BUFFERSIZE;
	m_file = TheFileSystem->openFile(filepath.str(), File::READ | File::BINARY, buffersize);

	if (m_file == nullptr)
	{
		DEBUG_LOG(("Can't open %s (%s)", filepath.str(), header.filename.str()));
		return FALSE;
	}

	// Read the GENREP header.
	char genrep[sizeof(s_genrep) - 1] = {0};
	m_file->read( &genrep, sizeof(s_genrep) - 1 );
	if ( strncmp(genrep, s_genrep, sizeof(s_genrep) - 1 ) != 0 ) {
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay file did not have GENREP at the start."));
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}

	// read in some stats
	replay_time_t tmp;
	m_file->read(&tmp, sizeof(tmp));
	header.startTime = tmp;
	m_file->read(&tmp, sizeof(tmp));
	header.endTime = tmp;

	m_file->read(&header.frameCount, sizeof(header.frameCount));

	m_file->read(&header.desyncGame, sizeof(header.desyncGame));
	m_file->read(&header.quitEarly, sizeof(header.quitEarly));
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_file->read(&(header.playerDiscons[i]), sizeof(Bool));
	}

	// Read the Replay Name.  We don't actually do anything with it.  Oh well.
	header.replayName = readUnicodeString();

	// Read the date and time.  We don't really do anything with this either. Oh well.
	m_file->read(&header.timeVal, sizeof(header.timeVal));

	// Read in the Version info
	header.versionString = readUnicodeString();
	header.versionTimeString = readUnicodeString();
	m_file->read(&header.versionNumber, sizeof(header.versionNumber));
	m_file->read(&header.exeCRC, sizeof(header.exeCRC));
	m_file->read(&header.iniCRC, sizeof(header.iniCRC));

	// Read in the GameInfo
	header.gameOptions = readAsciiString();
	m_gameInfo.reset();
	m_gameInfo.enterGame();
	DEBUG_LOG(("RecorderClass::readReplayHeader - GameInfo = %s", header.gameOptions.str()));
	if (!ParseAsciiStringToGameInfo(&m_gameInfo, header.gameOptions))
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay file did not have a valid GameInfo string."));
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}
	m_gameInfo.startGame(0);

	AsciiString playerIndex = readAsciiString();
	header.localPlayerIndex = atoi(playerIndex.str());
	if (header.localPlayerIndex < -1 || header.localPlayerIndex >= MAX_SLOTS)
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - invalid local slot number."));
		m_gameInfo.endGame();
		m_gameInfo.reset();
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}
	if (header.localPlayerIndex >= 0)
	{
		Int localIP = m_gameInfo.getSlot(header.localPlayerIndex)->getIP();
		m_gameInfo.setLocalIP(localIP);
	}

	if (!header.forPlayback)
	{
		m_gameInfo.endGame();
		m_gameInfo.reset();
		m_file->close();
		m_file = nullptr;
	}

	return TRUE;
}

Bool RecorderClass::simulateReplay(AsciiString filename)
{
	Bool success = playbackFile(filename);
	if (success)
		m_mode = RECORDERMODETYPE_SIMULATION_PLAYBACK;
	return success;
}

void RecorderClass::endLiveObserverSession() {
	liveObserverLog("endLiveObserverSession: closing playback file (was %s)\n",
		m_fileName.isEmpty() ? "(none)" : m_fileName.str());

	// Destroying the observer *is* the cleanup. Everything the live session knew — the
	// broadcast delay, the pre-roll latch, the pause claim, the live edge, the desync frame —
	// lives on that object, so it all goes at once and there is no list here that a future
	// field could quietly fall off. This used to be a dozen assignments duplicated between
	// here and stopPlayback(), and missing one leaked straight into the next session.
	if (TheLiveObserver)
	{
		liveObserverLog("endLiveObserverSession: destroying the live observer\n");
		TheLiveObserver->close();
		delete TheLiveObserver;
		TheLiveObserver = nullptr;
	}

	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();

	// What is left is the Recorder's own: the mode, the playback cursor, and the game the
	// replay was describing.
	m_mode = RECORDERMODETYPE_NONE;
	m_isLiveStream = FALSE;
	m_userPaused = FALSE;
	m_streamSink = nullptr;
	m_currentReplayFilename.clear();
	m_playbackFrameCount = 0;
	m_originalGameMode = GAME_NONE;
	m_crcInfo = CRCInfo();
	m_gameInfo.clearSlotList();
	m_gameInfo.reset();
	m_nextFrame = 0;
}

Bool RecorderClass::startLiveObserverPlayback(AsciiString filename)
{
	// Nothing to clear: the caller has already ended any previous session, and a session's
	// state now lives on the LiveObserver it was handed — which is a fresh one. This used to
	// re-clear eight fields here as well, because a stale stream-ended flag from the last
	// session made the very first read stop playback outright.
	m_mode = RECORDERMODETYPE_LIVE_OBSERVER;
	m_isLiveStream = TRUE;
	liveObserverLog("startLiveObserverPlayback: mode=LIVE_OBSERVER isLiveStream=1 filename=%s\n", filename.str());

	Bool success = playbackFile(filename);
	if (!success)
	{
		m_mode = RECORDERMODETYPE_NONE;
		m_isLiveStream = FALSE;
		liveObserverLog("startLiveObserverPlayback: FAILED, mode=NONE isLiveStream=0\n");
	}
	else
	{
		m_mode = RECORDERMODETYPE_LIVE_OBSERVER;
		m_isLiveStream = TRUE;
		m_nextFrame = 0;
		liveObserverLog("startLiveObserverPlayback: OK, mode restored to LIVE_OBSERVER, isLiveStream=1, nextFrame=0\n");
	}
	return success;
}

#if defined(RTS_DEBUG)
Bool RecorderClass::analyzeReplay(AsciiString filename)
{
	m_doingAnalysis = TRUE;
	return playbackFile(filename);
}



#endif

Bool RecorderClass::isPlaybackInProgress() const
{
	return isPlaybackMode() && m_nextFrame != -1;
}

AsciiString RecorderClass::getCurrentReplayFilename()
{
	if (isPlaybackMode())
	{
		return m_currentReplayFilename;
	}
	return AsciiString::TheEmptyString;
}

Bool RecorderClass::sawCRCMismatch() const
{
	return m_crcInfo.sawCRCMismatch();
}

void RecorderClass::handleCRCMessage(UnsignedInt newCRC, Int playerIndex, Bool fromPlayback)
{
	if (fromPlayback)
	{
		//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Adding CRC of %X from %d to m_crcInfo", newCRC, playerIndex));
		m_crcInfo.addCRC(newCRC);
		return;
	}

	Int localPlayerIndex = m_crcInfo.getLocalPlayer();
	Bool samePlayer = FALSE;
	AsciiString playerName;
	playerName.format("player%d", localPlayerIndex);
	const Player* p = ThePlayerList->getNthPlayer(playerIndex);
	if (!p || (p->getPlayerNameKey() == NAMEKEY(playerName)))
		samePlayer = TRUE;
	if (samePlayer || (localPlayerIndex < 0))
	{
		UnsignedInt playbackCRC = m_crcInfo.readCRC();
		//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Comparing CRCs of InGame:%8.8X Replay:%8.8X Frame:%d from Player %d",
		//	playbackCRC, newCRC, TheGameLogic->getFrame()-m_crcInfo.GetQueueSize()-1, playerIndex));
		if (TheGameLogic->getFrame() > 0 && newCRC != playbackCRC && !m_crcInfo.sawCRCMismatch())
		{
			//Kris: Patch 1.01 November 10, 2003 (integrated changes from Matt Campbell)
			// Since we don't seem to have any *visible* desyncs when replaying games, but get this warning
			// virtually every replay, the assumption is our CRC checking is faulty.  Since we're at the
			// tail end of patch season, let's just disable the message, and hope the users believe the
			// problem is fixed. -MDC 3/20/2003
			//
			// TheSuperHackers @tweak helmutbuhler 03/04/2025
			// More than 20 years later, but finally fixed and re-enabled!
			TheInGameUI->message("GUI:CRCMismatch");

			// TheSuperHackers @info helmutbuhler 03/04/2025
			// Note: We subtract the queue size from the frame number. This way we calculate the correct frame
			// the mismatch first happened in case the NetCRCInterval is set to 1 during the game.
			const UnsignedInt mismatchFrame = TheGameLogic->getFrame() - m_crcInfo.GetQueueSize() - 1;

			// Now also prints a UI message for it.
			const UnicodeString mismatchDetailsStr = TheGameText->FETCH_OR_SUBSTITUTE("GUI:CRCMismatchDetails", L"InGame:%8.8X Replay:%8.8X Frame:%d");
			TheInGameUI->message(mismatchDetailsStr, playbackCRC, newCRC, mismatchFrame);

			DEBUG_LOG(("Replay has gone out of sync!\nInGame:%8.8X Replay:%8.8X\nFrame:%d",
				playbackCRC, newCRC, mismatchFrame));

			// Print Mismatch in case we are simulating replays from console.
			printf("CRC Mismatch in Frame %d\n", mismatchFrame);

			// TheSuperHackers @fix Record the divergence for a live-observer session, where the
			// stream keeps arriving and playback keeps running: without this the observer has no
			// way of knowing its view stopped being the real game. Logged with the live-edge state
			// too, since the interesting question is always whether we had run out of data.
			if (m_mode == RECORDERMODETYPE_LIVE_OBSERVER && TheLiveObserver)
			{
				TheLiveObserver->noteDesync(mismatchFrame);
				liveObserverLog("DESYNC: observer diverged from the stream. InGame:%8.8X Replay:%8.8X frame=%d "
					"curFrame=%d nextFrame=%d liveEdge=%d delayFrames=%d holdPlayback=%d stalled=%d\n",
					playbackCRC, newCRC, mismatchFrame, TheGameLogic->getFrame(), m_nextFrame,
					TheLiveObserver->getMaxCompleteFrame(), TheLiveObserver->getDelayFrames(),
					TheLiveObserver->shouldHoldPlayback() ? 1 : 0, TheLiveObserver->isStalled() ? 1 : 0);

				// Report once, then stop comparing - a desynced simulation diverges further every
				// frame, so everything after the first mismatch is noise.
				m_crcInfo.setSawCRCMismatch();
				return;
			}

			// TheSuperHackers @tweak Pause the game on mismatch.
			// But not when a window with focus is opened, because that can make resuming difficult.
			if (TheWindowManager->winGetFocus() == nullptr)
			{
				Bool pause = TRUE;
				Bool pauseMusic = FALSE;
				Bool pauseInput = FALSE;
				TheGameLogic->setGamePaused(pause, pauseMusic, pauseInput);

				// Mark this mismatch as seen when we had the chance to pause once.
				m_crcInfo.setSawCRCMismatch();
			}
		}
		return;
	}

	//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Skipping CRC of %8.8X from %d (our index is %d)", newCRC, playerIndex, localPlayerIndex));
}

/**
 * Returns true if this version of the file is the same as our version of the game
 */
Bool RecorderClass::replayMatchesGameVersion(AsciiString filename)
{
	ReplayHeader header;
	header.forPlayback = TRUE;
	header.filename = filename;
	if (readReplayHeader(header))
	{
		return replayMatchesGameVersion(header);
	}
	return FALSE;
}

Bool RecorderClass::replayMatchesGameVersion(const ReplayHeader& header)
{
	// TheSuperHackers @fix No longer checks the build time here to prevent incorrect Replay playback incompatibility messages when the Replay playback would actually be technically compatible.
	if (header.versionString != TheVersion->getUnicodeVersion())
		return false;
	if (header.versionNumber != TheVersion->getVersionNumber())
		return false;
	if (header.exeCRC != TheGlobalData->m_exeCRC)
		return false;
	if (header.iniCRC != TheGlobalData->m_iniCRC)
		return false;
	return true;
}

/**
 * Start playback of the file. Return true or false depending on if the file is
 * a valid replay file or not.
 */
Bool RecorderClass::playbackFile(AsciiString filename)
{
	if (!m_doingAnalysis)
	{
		if (TheGameLogic->isInGame())
		{
			TheGameLogic->clearGameData();
		}
	}

	m_mode = RECORDERMODETYPE_PLAYBACK;

	ReplayHeader header;
	header.forPlayback = TRUE;
	header.filename = filename;
	Bool success = readReplayHeader(header);
	if (!success)
	{
		return FALSE;
	}

#ifdef DEBUG_CRASHING
	Bool versionStringDiff = header.versionString != TheVersion->getUnicodeVersion();
	Bool versionTimeStringDiff = header.versionTimeString != TheVersion->getUnicodeBuildTime();
	Bool versionNumberDiff = header.versionNumber != TheVersion->getVersionNumber();
	Bool exeCRCDiff = header.exeCRC != TheGlobalData->m_exeCRC;
	Bool exeDifferent = versionStringDiff || versionTimeStringDiff || versionNumberDiff || exeCRCDiff;
	Bool iniDifferent = header.iniCRC != TheGlobalData->m_iniCRC;

	AsciiString debugString;
	AsciiString tempStr;
	if (exeDifferent)
	{
		// TheSuperHackers @fix helmutbuhler 05/05/2025 No longer attempts to print unicode as ascii
		// via a call to AsciiString::format with %ls format. It does not work with non-ascii characters.
		UnicodeString tempStrWide;
		debugString = "EXE is different:\n";
		if (versionStringDiff)
		{
			tempStrWide.format(L"   Version [%s] vs [%s]\n", TheVersion->getUnicodeVersion().str(), header.versionString.str());
			tempStr.translate(tempStrWide);
			debugString.concat(tempStr);
		}
		if (versionTimeStringDiff)
		{
			tempStrWide.format(L"   Build Time [%s] vs [%s]\n", TheVersion->getUnicodeBuildTime().str(), header.versionTimeString.str());
			tempStr.translate(tempStrWide);
			debugString.concat(tempStr);
		}
		if (versionNumberDiff)
		{
			tempStr.format("   Version Number %8.8X vs %8.8X\n", TheVersion->getVersionNumber(), header.versionNumber);
			debugString.concat(tempStr);
		}
		if (exeCRCDiff)
		{
			tempStr.format("   CRC %8.8X vs %8.8X\n", TheGlobalData->m_exeCRC, header.exeCRC);
			debugString.concat(tempStr);
		}
	}
	if (iniDifferent)
	{
		debugString.concat("INIs are different:\n");
		tempStr.format("   CRC %8.8X vs %8.8X\n", TheGlobalData->m_iniCRC, header.iniCRC);
		debugString.concat(tempStr);
	}
	DEBUG_ASSERTCRASH(!exeDifferent && !iniDifferent, (debugString.str()));
#endif

	TheWritableGlobalData->m_pendingFile = m_gameInfo.getMap();

#ifdef DEBUG_LOGGING
	if (header.localPlayerIndex >= 0)
	{
		DEBUG_LOG(("Local player is %ls (slot %d, IP %8.8X)",
			m_gameInfo.getSlot(header.localPlayerIndex)->getName().str(), header.localPlayerIndex, m_gameInfo.getSlot(header.localPlayerIndex)->getIP()));
	}
#endif

    REPLAY_CRC_INTERVAL = m_gameInfo.getCRCInterval();

    Int difficulty = 0;
    m_file->read(&difficulty, sizeof(difficulty));

    m_file->read(&m_originalGameMode, sizeof(m_originalGameMode));

    Int rankPoints = 0;
    m_file->read(&rankPoints, sizeof(rankPoints));

    Int maxFPS = 0;
    m_file->read(&maxFPS, sizeof(maxFPS));

    Bool isMultiplayer = (m_originalGameMode == GAME_INTERNET || m_originalGameMode == GAME_LAN);
    m_crcInfo = CRCInfo(header.localPlayerIndex, isMultiplayer);
    DEBUG_LOG(("Player index is %d, replay CRC interval is %d, isMultiplayer is %d", m_crcInfo.getLocalPlayer(), REPLAY_CRC_INTERVAL, isMultiplayer));

    DEBUG_LOG(("RecorderClass::playbackFile() - original game was mode %d", m_originalGameMode));

    // TheSuperHackers @fix helmutbuhler 03/04/2025
    // In case we restart a replay, we need to clear the command list.
    // Otherwise a crc message remains and messes up the crc calculation on the restarted replay.
    TheCommandList->reset();

	readNextFrame();

	// send a message to the logic for a new game
	if (!m_doingAnalysis)
	{
		// TheSuperHackers @info helmutbuhler 13/04/2025
		// We send the New Game message here directly to the command list and bypass the TheMessageStream.
		// That's ok because Multiplayer is disabled during replay playback and is actually required
		// during replay simulation because we don't update TheMessageStream during simulation.
		GameMessage* msg = newInstance(GameMessage)(GameMessage::MSG_NEW_GAME);
		msg->appendIntegerArgument(GAME_REPLAY);
		msg->appendIntegerArgument(difficulty);
		msg->appendIntegerArgument(rankPoints);
		if (maxFPS != 0)
			msg->appendIntegerArgument(maxFPS);
		TheCommandList->appendMessage(msg);
		InitRandom(m_gameInfo.getSeed());
	}

	m_currentReplayFilename = filename;
	m_playbackFrameCount = header.frameCount;
	return TRUE;
}

/**
 * Read a unicode string from the current file position. The string is assumed to be 0-terminated.
 */
UnicodeString RecorderClass::readUnicodeString() {
	WideChar str[1024] = L"";
	Int index = 0;

	Int c = m_file->readWideChar();
	if (c == EOF) {
		str[index] = 0;
	}
	str[index] = c;

	while (index < 1024 && str[index] != 0) {
		++index;
		Int c = m_file->readWideChar();
		if (c == EOF) {
			str[index] = 0;
			break;
		}
		str[index] = c;
	}
	str[1023] = L'\0';

	UnicodeString retval(str);
	return retval;
}

/**
 * Read an ascii string from the current file position. The string is assumed to be 0-terminated.
 */
AsciiString RecorderClass::readAsciiString() {
	char str[1024] = "";
	Int index = 0;

	Int c = m_file->readChar();
	if (c == EOF) {
		str[index] = 0;
	}
	str[index] = c;

	while (index < 1024 && str[index] != 0) {
		++index;
		Int c = m_file->readChar();
		if (c == EOF) {
			str[index] = 0;
			break;
		}
		str[index] = c;
	}
	str[1023] = '\0';

	AsciiString retval(str);
	return retval;
}

/**
 * Read the frame number for the next command in the playback file. If the end of the file is reached, the playback
 * is stopped and the next frame is said to be -1.
 */
RecorderClass::ReadFrameResult RecorderClass::readNextFrame() {
	DEBUG_LOG(("RecorderClass::readNextFrame - isLiveStream=%d streamEnded=%d mode=%d nextFrame=%d curFrame=%d",
		m_isLiveStream, liveStreamEnded(), (int)m_mode, m_nextFrame, TheGameLogic->getFrame()));
	if (m_isLiveStream) {
		Int savedPos = m_file->seek(0, File::CURRENT);
		const Bool streamEnded = liveStreamEnded();

		// Never read past the last complete record. The network thread appends at arbitrary
		// byte offsets, so without this bound a read at the growing tail can return a
		// partial record — which both yields a garbage frame number and leaves the file
		// position mid-record, permanently misaligning everything after it.
		if (TheLiveObserver) {
			Int safeOffset = TheLiveObserver->getSafeReadOffset();
			if (savedPos + (Int)sizeof(m_nextFrame) > safeOffset) {
				if (!streamEnded)
					return READFRAME_EOF_WAITING;

				// Stream ended and every complete record has been consumed — this is the end
				// of the replay. Do NOT fall through to the read: the live file is opened
				// without truncation, so a session reusing a longer previous session's file
				// still has that session's bytes sitting here. Reading them succeeds and
				// yields a garbage frame number at precisely the moment playback should stop.
				LIVE_OBSERVER_LOG("readNextFrame: end of stream at offset %d (safe=%d) — stopping playback\n",
					savedPos, safeOffset);
				m_nextFrame = -1;
				stopPlayback();
				return READFRAME_STREAM_STOPPED;
			}
		}

		Int bytesRead = m_file->read(&m_nextFrame, sizeof(m_nextFrame));
		if (bytesRead != sizeof(m_nextFrame)) {
			if (!streamEnded) {
				// Leave m_nextFrame alone and rewind, so the next tick retries cleanly.
				m_file->seek(savedPos, File::START);
				return READFRAME_EOF_WAITING;
			}
			LIVE_OBSERVER_LOG("readNextFrame: read FAILED (bytes=%d streamEnded=%d) — stopping playback, curFrame=%d\n",
				bytesRead, streamEnded, TheGameLogic->getFrame());
			DEBUG_LOG(("RecorderClass::readNextFrame - read failed on frame %d", TheGameLogic->getFrame()));
			m_nextFrame = -1;
			stopPlayback();
			return READFRAME_STREAM_STOPPED;
		}

		if (m_nextFrame > TheGameLogic->getFrame()) {
			// Future frame — don't consume the bytes yet. Restore position
			// so appendNextCommand doesn't see the data prematurely.
			m_file->seek(savedPos, File::START);
		}
		else if (m_nextFrame % 300 == 0 || m_nextFrame <= 3) {
			LIVE_OBSERVER_LOG("readNextFrame: OK nextFrame=%d curFrame=%d fileSize=%d filePos=%d\n",
				m_nextFrame, TheGameLogic->getFrame(), m_file->size(), savedPos);
		}
		return READFRAME_OK;
	}

	Int bytesRead = m_file->read(&m_nextFrame, sizeof(m_nextFrame));
	if (bytesRead != sizeof(m_nextFrame)) {
		LIVE_OBSERVER_LOG("readNextFrame: NON-LIVE read FAILED (bytes=%d) — stopping playback, curFrame=%d\n",
			bytesRead, TheGameLogic->getFrame());
		DEBUG_LOG(("RecorderClass::readNextFrame - read failed on frame %d", TheGameLogic->getFrame()));
		m_nextFrame = -1;
		stopPlayback();
		return READFRAME_STREAM_STOPPED;
	}
	return READFRAME_OK;
}

/**
 * This reads the next command from the replay file and appends it to TheCommandList.
 */
void RecorderClass::appendNextCommand() {
	Int savedPos = 0;
	if (m_isLiveStream)
		savedPos = m_file->seek(0, File::CURRENT);

	GameMessage::Type type;
	Int bytesRead = m_file->read(&type, sizeof(type));
	if (bytesRead != sizeof(type)) {
		if (m_isLiveStream && !liveStreamEnded()) {
			// TheSuperHackers @fix Rewind to the record boundary before giving up. savedPos was
			// captured and then never used, so a partial read here left the cursor stranded in the
			// middle of a record - and since nothing ever re-derives the boundary, every record
			// after it was misparsed for the rest of the session, feeding garbage message types and
			// object IDs into TheCommandList while playback carried on regardless. A one-way door
			// straight to a silent desync.
			m_file->seek(savedPos, File::START);
			LIVE_OBSERVER_LOG("appendNextCommand: short read of the type field (bytes=%d) at offset %d, rewound\n",
				bytesRead, savedPos);
			return;
		}
		LIVE_OBSERVER_LOG("appendNextCommand: read FAILED (bytes=%d isLiveStream=%d streamEnded=%d) — abandoning, curFrame=%d nextFrame=%d\n",
			bytesRead, m_isLiveStream, liveStreamEnded(), TheGameLogic->getFrame(), m_nextFrame);
		DEBUG_LOG(("RecorderClass::appendNextCommand - read failed on frame %d", m_nextFrame/*TheGameLogic->getFrame()*/));
		return;
	}

	GameMessage* msg = newInstance(GameMessage)(type);

#ifdef DEBUG_LOGGING
	AsciiString commandName = msg->getCommandAsString();
	if (type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (Non-Network message!)");
	}
	else if (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		commandName.concat(" (CRC message!)");
	}
#endif // DEBUG_LOGGING

	Int playerIndex = -1;
	m_file->read(&playerIndex, sizeof(playerIndex));
	msg->friend_setPlayerIndex(playerIndex);

	// don't debug log this if we're debugging sync errors, as it will cause diff problems between a game and it's replay...
#ifdef DEBUG_LOGGING
	Bool logCommand = true;
#ifdef DEBUG_CRC
	if (!m_doingAnalysis)
		logCommand = false;
#endif
	if (logCommand)
	{
		DEBUG_LOG(("RecorderClass::appendNextCommand - Adding %s command from player %d to TheCommandList on frame %d",
			commandName.str(), (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES) ? 0 : msg->getPlayerIndex(), m_nextFrame/*TheGameLogic->getFrame()*/));
	}
#endif

	UnsignedByte numTypes = 0;
	Int totalArgs = 0;
	m_file->read(&numTypes, sizeof(numTypes));

	GameMessageParser* parser = newInstance(GameMessageParser)();
	for (UnsignedByte i = 0; i < numTypes; ++i) {
		UnsignedByte type = (UnsignedByte)ARGUMENTDATATYPE_UNKNOWN;
		m_file->read(&type, sizeof(type));
		UnsignedByte numArgs = 0;
		m_file->read(&numArgs, sizeof(numArgs));
		parser->addArgType((GameMessageArgumentDataType)type, numArgs);
		totalArgs += numArgs;
	}

	GameMessageParserArgumentType* parserArgType = parser->getFirstArgumentType();
	GameMessageArgumentDataType lasttype = ARGUMENTDATATYPE_UNKNOWN;
	Int argsLeftForType = 0;
	if (parserArgType != nullptr) {
		lasttype = parserArgType->getType();
		argsLeftForType = parserArgType->getArgCount();
	}
	for (Int j = 0; j < totalArgs; ++j) {
		readArgument(lasttype, msg);

		--argsLeftForType;
		if (argsLeftForType == 0) {
			DEBUG_ASSERTCRASH(parserArgType != nullptr, ("parserArgType was null when it shouldn't have been."));
			if (parserArgType == nullptr) {
				// Same one-way door as the short read above: bailing here leaves the cursor mid
				// record (and leaks the parser). Rewind so the next attempt starts on a boundary.
				if (m_isLiveStream) {
					m_file->seek(savedPos, File::START);
					LIVE_OBSERVER_LOG("appendNextCommand: ran out of argument types mid-record at offset %d, rewound\n",
						savedPos);
				}
				deleteInstance(parser);
				deleteInstance(msg);
				return;
			}

			parserArgType = parserArgType->getNext();
			// parserArgType is allowed to be null here, this is the case if there are no more arguments.
			if (parserArgType != nullptr) {
				argsLeftForType = parserArgType->getArgCount();
				lasttype = parserArgType->getType();
			}
		}
	}

	if (type != GameMessage::MSG_BEGIN_NETWORK_MESSAGES && type != GameMessage::MSG_CLEAR_GAME_DATA && !m_doingAnalysis)
	{
		// The live hold state is computed entirely in LiveObserver::updatePlaybackGate() now.
		// Clearing it here meant the status only ever read "not waiting" on ticks that
		// happened to carry a real player command — which is a small minority of ticks, so
		// WAITING FOR FRAMES was on ~98% of the time regardless of whether the observer was
		// actually blocked.
		TheCommandList->appendMessage(msg);
	}
	else
	{
		deleteInstance(msg);
		msg = nullptr;
	}

	deleteInstance(parser);
	parser = nullptr;
}

#if defined(GENERALS_ONLINE)
// TheSuperHackers @feature Size of one replay argument on disk. Must match readArgument()
// exactly — that function is the authority on what gets read for each type.
// Returns -1 for anything unrecognised so callers can fail closed instead of skipping zero
// bytes and desyncing the rest of the parse.
static Int replayArgumentSize(UnsignedByte argType)
{
	switch ((GameMessageArgumentDataType)argType) {
		case ARGUMENTDATATYPE_INTEGER:      return sizeof(Int);
		case ARGUMENTDATATYPE_REAL:         return sizeof(Real);
		case ARGUMENTDATATYPE_BOOLEAN:      return sizeof(Bool);
		case ARGUMENTDATATYPE_OBJECTID:     return sizeof(ObjectID);
		case ARGUMENTDATATYPE_DRAWABLEID:   return sizeof(DrawableID);
		case ARGUMENTDATATYPE_TEAMID:       return sizeof(UnsignedInt);
		case ARGUMENTDATATYPE_LOCATION:     return sizeof(Coord3D);
		case ARGUMENTDATATYPE_PIXEL:        return sizeof(ICoord2D);
		case ARGUMENTDATATYPE_PIXELREGION:  return sizeof(IRegion2D);
		case ARGUMENTDATATYPE_TIMESTAMP:    return sizeof(UnsignedInt);
		case ARGUMENTDATATYPE_WIDECHAR:     return sizeof(WideChar);
		default:                            return -1;
	}
}

ScanRecordResult scanReplayRecord(const unsigned char* buf, Int len, Int* outSize, UnsignedInt* outFrame)
{
	// numTypes and numArgs are single bytes, so a well-formed record cannot exceed
	// 9 + 255*2 + 255*255*sizeof(IRegion2D) bytes. Anything claiming more than this is
	// misparsed data rather than a record we are merely waiting on — say so, instead of
	// stalling forever waiting for bytes that will never make it complete.
	const Int MAX_SANE_RECORD_SIZE = 2 * 1024 * 1024;

	const Int fixedSize = sizeof(UnsignedInt) + sizeof(GameMessage::Type) + sizeof(Int) + sizeof(UnsignedByte);
	if (len < fixedSize)
		return SCANRECORD_INCOMPLETE;

	UnsignedInt frame;
	memcpy(&frame, buf, sizeof(frame));

	Int pos = sizeof(UnsignedInt) + sizeof(GameMessage::Type) + sizeof(Int);
	UnsignedByte numTypes = buf[pos];
	pos += sizeof(UnsignedByte);

	// All (argType, numArgs) pairs are written consecutively, and only then the argument
	// payload for every type in order — see appendNextCommand(), which reads the full pair
	// list into the parser before its readArgument() loop. Accumulate the payload size and
	// add it once, after the pair list. (The old probeLiveEdge() skipped each type's payload
	// inside this loop instead, which is correct only when numTypes == 1 and desynced the
	// parse for every message carrying two or more argument types.)
	Int payloadSize = 0;
	for (UnsignedByte i = 0; i < numTypes; ++i) {
		if (pos + 2 > len)
			return SCANRECORD_INCOMPLETE;

		UnsignedByte argType = buf[pos];
		UnsignedByte numArgs = buf[pos + 1];
		pos += 2;

		Int argSize = replayArgumentSize(argType);
		if (argSize < 0)
			return SCANRECORD_CORRUPT;

		payloadSize += argSize * (Int)numArgs;
		if (payloadSize > MAX_SANE_RECORD_SIZE)
			return SCANRECORD_CORRUPT;
	}

	pos += payloadSize;
	if (pos > len)
		return SCANRECORD_INCOMPLETE;

	if (outSize)
		*outSize = pos;
	if (outFrame)
		*outFrame = frame;
	return SCANRECORD_OK;
}
#endif // GENERALS_ONLINE

void RecorderClass::readArgument(GameMessageArgumentDataType type, GameMessage *msg) {
	switch (type) {
		case ARGUMENTDATATYPE_INTEGER: {
			Int theint;
			m_file->read(&theint, sizeof(theint));
			msg->appendIntegerArgument(theint);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Integer argument: %d (%8.8X)", theint, theint));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_REAL: {
			Real thereal;
			m_file->read(&thereal, sizeof(thereal));
			msg->appendRealArgument(thereal);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Real argument: %g (%8.8X)", thereal, *(int *)&thereal));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_BOOLEAN: {
			Bool thebool;
			m_file->read(&thebool, sizeof(thebool));
			msg->appendBooleanArgument(thebool);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Bool argument: %d", thebool));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_OBJECTID: {
			ObjectID theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendObjectIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Object ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_DRAWABLEID: {
			DrawableID theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendDrawableIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Drawable ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_TEAMID: {
			UnsignedInt theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendTeamIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Team ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_LOCATION: {
			Coord3D loc;
			m_file->read(&loc, sizeof(loc));
			msg->appendLocationArgument(loc);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Coord3D argument: %g %g %g (%8.8X %8.8X %8.8X)", loc.x, loc.y, loc.z,
					*(int *)&loc.x, *(int *)&loc.y, *(int *)&loc.z));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_PIXEL: {
			ICoord2D pixel;
			m_file->read(&pixel, sizeof(pixel));
			msg->appendPixelArgument(pixel);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Pixel argument: %d,%d", pixel.x, pixel.y));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_PIXELREGION: {
			IRegion2D reg;
			m_file->read(&reg, sizeof(reg));
			msg->appendPixelRegionArgument(reg);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Pixel Region argument: %d,%d -> %d,%d", reg.lo.x, reg.lo.y, reg.hi.x, reg.hi.y));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_TIMESTAMP: {  // Not to be confused with Terrance Stamp... Kneel before Zod!!!
			UnsignedInt stamp;
			m_file->read(&stamp, sizeof(stamp));
			msg->appendTimestampArgument(stamp);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Timestamp argument: %d", stamp));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_WIDECHAR: {
			WideChar theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendWideCharArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("WideChar argument: %d (%lc)", theid, theid));
			}
#endif
			break;
		}
		default:
			break;
	}
}

/**
 * This needs to be called for every frame during playback. Basically it prevents the user from inserting.
 */
RecorderClass::CullBadCommandsResult RecorderClass::cullBadCommands() {
    CullBadCommandsResult result;

    if (m_doingAnalysis)
        return result;

    GameMessage *msg = TheCommandList->getFirstMessage();
    GameMessage *next = nullptr;

    while (msg != nullptr) {
        next = msg->next();
        if ((msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
            (msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES) &&
            (msg->getType() != GameMessage::MSG_LOGIC_CRC)) {
            deleteInstance(msg);
        }
        // TheSuperHackers @fix These two sit outside the network-message range, so the filter above
        // let them through - but unlike every other message that survives culling, they mutate
        // simulation state: BEGIN latches a static that makes doMoveTo() queue waypoints instead of
        // issuing moves, for *every* subsequent move order including the ones arriving from the
        // stream, and END then executes the accumulated queue. One stray local keypress and the
        // observer's simulation stops resembling the recording, permanently.
        else if (msg->getType() == GameMessage::MSG_META_BEGIN_PATH_BUILD ||
                 msg->getType() == GameMessage::MSG_META_END_PATH_BUILD) {
            deleteInstance(msg);
        }
        else if (msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA)
        {
            result.hasClearGameDataMessage = true;
        }
        msg = next;
    }

    return result;
}

/**
 * returns the directory that holds the replay files.
 */
AsciiString RecorderClass::getReplayDir()
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	tmp.concat("Replays\\");
	return tmp;
}

/**
 * returns the directory that holds the archived replay files.
 */
AsciiString RecorderClass::getReplayArchiveDir()
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	tmp.concat("ArchivedReplays\\");
	return tmp;
}

/**
 * returns the file extension for the replay files.
 */
AsciiString RecorderClass::getReplayExtention() {
	return AsciiString(replayExtention);
}

/**
 * returns the file name used for the replay file that is recorded to.
 */
AsciiString RecorderClass::getLastReplayFileName()
{
#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		GameInfo *game = nullptr;
		if (TheLAN)
			game = TheLAN->GetMyGame();
#if defined(GENERALS_ONLINE)
		else if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
			game = TheNGMPGame;
#else
		else if (TheGameSpyInfo)
			game = TheGameSpyGame;
#endif
		if (game)
		{
			AsciiString players;
			AsciiString full;
			AsciiString fullPlusNum;
			AsciiString mapName = game->getMap();
			const char* fname = mapName.reverseFind('\\');
			if (fname)
				mapName = fname + 1;
			for (Int i = 0; i < MAX_SLOTS; ++i)
			{
				GameSlot* slot = game->getSlot(i);
				if (slot && slot->isHuman())
				{
					AsciiString player;
					player.format("%ls_", slot->getName().str());
					players.concat(player);
				}
			}
			full.format("%s%s_%d_%d", players.str(), mapName.str(), game->getSeed(), game->getLocalSlotNum());
			AsciiString testString;
			testString.format("%s%s%s", getReplayDir().str(), full.str(), replayExtention);

			FILE* fp;
			fp = fopen(testString.str(), "rb");
			if (fp)
			{
				fclose(fp);
			}
			else
			{
				return full;
			}
			Int test = 1;
			while (test < 20)
			{
				fullPlusNum.format("%s_%d", full.str(), test);
				testString.format("%s%s%s", getReplayDir().str(), fullPlusNum.str(), replayExtention);
				fp = fopen(testString.str(), "rb");
				if (fp)
				{
					fclose(fp);
					++test;
				}
				else
				{
					return fullPlusNum;
				}
			}
			return fullPlusNum;
		}
	}
#endif

	AsciiString filename;
	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		filename.format("%s_Instance%.2u", lastReplayFileName, rts::ClientInstance::getInstanceId());
	}
	else
	{
		filename = lastReplayFileName;
	}
	return filename;
}

/**
 * return the current operating mode of TheRecorder.
 */
RecorderModeType RecorderClass::getMode() {
	return m_mode;
}

///< Show or Hide the Replay controls
void RecorderClass::initControls()
{
	NameKeyType parentReplayControlID = TheNameKeyGenerator->nameToKey( "ReplayControl.wnd:ParentReplayControl" );
	GameWindow *parentReplayControl = TheWindowManager->winGetWindowFromId( nullptr, parentReplayControlID );

	Bool show = (getMode() != RECORDERMODETYPE_PLAYBACK);
	if (parentReplayControl)
	{
		parentReplayControl->winHide(show);	// show the replay control window.
	}
}

///< is this a multiplayer game (record OR playback)?
Bool RecorderClass::isMultiplayer()
{

	if (isPlaybackMode())
	{
		GameSlot* slot;
		for (int i = 0; i < MAX_SLOTS; ++i)
		{
			slot = m_gameInfo.getSlot(i);
			if (slot && slot->isOccupied())	///< slots default to closed for non-networked games
				return true;
		}
	}
	if (TheGameLogic->getGameMode() == GAME_SINGLE_PLAYER) {
		return false; // single player isn't multiplayer.
	}
	if (TheGameLogic->getGameMode() == GAME_SHELL) {
		return false; // shell isn't multiplayer.
	}
	if (TheNetwork || TheSkirmishGameInfo)
		return true;

	return false;
}

/**
 * Create a new recorder object.
 */
RecorderClass* createRecorder() {
	return NEW RecorderClass;
}
