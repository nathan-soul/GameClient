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

#pragma once

#include "Common/GameCommon.h"		// LOGICFRAMES_PER_SECOND, for the broadcast delay
#include "Common/MessageStream.h"
#include "Common/ReplayStreamSink.h"
#include "GameNetwork/GameInfo.h"

class File;

/**
  * The ReplayGameInfo class holds information about the replay game and
	* the contents of its slot list for reconstructing multiplayer games.
	*/
class ReplayGameInfo : public GameInfo
{
private:
	GameSlot m_ReplaySlot[MAX_SLOTS];

public:
	ReplayGameInfo()
	{
		for (Int i = 0; i< MAX_SLOTS; ++i)
			setSlotPointer(i, &m_ReplaySlot[i]);
	}
};

enum RecorderModeType CPP_11(: Int) {
	RECORDERMODETYPE_RECORD,
	RECORDERMODETYPE_PLAYBACK,
	RECORDERMODETYPE_SIMULATION_PLAYBACK, // Play back replay without any graphics
	RECORDERMODETYPE_LIVE_OBSERVER, // Live observer mode — receiving frames from relay server
	RECORDERMODETYPE_NONE // this is a valid state to be in on the shell map, or in saved games
};

#if defined(GENERALS_ONLINE)
// LIVE_DELAY_SECONDS_DEFAULT / LIVE_DELAY_SECONDS_MAX live in Common/GameCommon.h, so that
// OptionPreferences (in Core) can share them.

// TheSuperHackers @feature Shared replay-record scanner.
//
// One replay record on disk is laid out as:
//   [UnsignedInt frame][GameMessage::Type type][Int playerIndex][UnsignedByte numTypes]
//   { [UnsignedByte argType][UnsignedByte numArgs] } x numTypes
//   [argument payload]
//
// Two places need to agree on that layout byte-for-byte: RecorderClass::appendNextCommand()
// (which consumes records during playback) and the LiveObserver network thread (which scans
// arriving bytes to publish the live-edge / safe-read watermarks). They previously had
// separate copies of the sizing logic, and the copy in the old probeLiveEdge() silently
// treated an unrecognised argument type as zero-width — which desynced the parse and made it
// report float payload bytes as frame numbers. Keep this the single source of truth, and note
// that it fails closed: an unparseable record stalls the watermark rather than poisoning it.
enum ScanRecordResult CPP_11(: Int)
{
	SCANRECORD_OK,				///< a complete record is present; outSize/outFrame are valid
	SCANRECORD_INCOMPLETE,		///< the buffer holds a valid prefix — more bytes needed
	SCANRECORD_CORRUPT			///< unparseable (e.g. unknown argument type)
};

/// Scan one replay record from buf[0..len). Never reads past len. outSize/outFrame may be null.
ScanRecordResult scanReplayRecord(const unsigned char* buf, Int len, Int* outSize, UnsignedInt* outFrame);
#endif // GENERALS_ONLINE

class RecorderClass : public SubsystemInterface
{
protected:
	// TheSuperHackers @info helmutbuhler 03/04/2025 CRC overview:
	// Each peer periodically computes a CRC from its local game state and broadcasts it to all peers, including itself,
	// to verify synchronization. CRC messages are received a few frames later in network games to avoid stalling every
	// frame while waiting for all peers. This works because all peers compare the same received CRCs on the same frame.
	//
	// Replays are different: recorded CRC messages appear on the frame they were originally received, so directly
	// comparing them against the current local state would mismatch. To handle this, local CRCs must be queued until the
	// corresponding replay CRC messages arrive. This class implements that queue.
	class CRCInfo
	{
	public:
		CRCInfo();
		CRCInfo(UnsignedInt localPlayer, Bool isMultiplayer);
		void addCRC(UnsignedInt val);
		UnsignedInt readCRC();
		int GetQueueSize() const { return m_data.size(); }
		UnsignedInt getLocalPlayer() const { return m_localPlayer; }
		void setSawCRCMismatch() { m_sawCRCMismatch = TRUE; }
		Bool sawCRCMismatch() const { return m_sawCRCMismatch; }

	protected:
		Bool m_sawCRCMismatch;
		Bool m_skippedOne;
		UnsignedInt m_localPlayer;
		std::list<UnsignedInt> m_data;
	};

public:
	struct ReplayHeader;

	RecorderClass();																	///< Constructor.
	virtual ~RecorderClass() override;													///< Destructor.

	virtual void init() override;																			///< Initialize TheRecorder.
	virtual void reset() override;																			///< Reset the state of TheRecorder.
	virtual void update() override;																		///< General purpose update function.

	// Methods dealing with recording.
	void updateRecord();															///< The update function for recording.

	// Methods dealing with playback.
	void updatePlayback();														///< The update function for playing back a file.
	Bool playbackFile(AsciiString filename);					///< Starts playback of the specified file.
	Bool replayMatchesGameVersion(AsciiString filename); ///< Returns true if the playback is a valid playback file for this version.
	static Bool replayMatchesGameVersion(const ReplayHeader& header); ///< Returns true if the playback is a valid playback file for this version.
	AsciiString getCurrentReplayFilename();			///< valid during playback only
	UnsignedInt getPlaybackFrameCount() const { return m_playbackFrameCount; }			///< valid during playback only
	void stopPlayback();															///< Stops playback.  Its fine to call this even if not playing back a file.
	Bool simulateReplay(AsciiString filename);
	Bool startLiveObserverPlayback(AsciiString filename);
#if defined(RTS_DEBUG)
	Bool analyzeReplay( AsciiString filename );
#endif
	Bool isPlaybackInProgress() const;

public:
	void handleCRCMessage(UnsignedInt newCRC, Int playerIndex, Bool fromPlayback);

	// read in info relating to a replay, conditionally setting up m_file for playback
	struct ReplayHeader
	{
		AsciiString filename;
		Bool forPlayback;
		UnicodeString replayName;
		SYSTEMTIME timeVal;
		UnicodeString versionString;
		UnicodeString versionTimeString;
		UnsignedInt versionNumber;
		UnsignedInt exeCRC;
		UnsignedInt iniCRC;
		time_t startTime;
		time_t endTime;
		UnsignedInt frameCount;
		Bool quitEarly;
		Bool desyncGame;
		Bool playerDiscons[MAX_SLOTS];
		AsciiString gameOptions;
		Int localPlayerIndex;
	};
	Bool readReplayHeader( ReplayHeader& header );

	RecorderModeType getMode();														///< Returns the current operating mode.
	void setMode(RecorderModeType mode) { m_mode = mode; }							///< Sets the current operating mode.
	Bool isPlaybackMode() const { return m_mode == RECORDERMODETYPE_PLAYBACK || m_mode == RECORDERMODETYPE_SIMULATION_PLAYBACK || m_mode == RECORDERMODETYPE_LIVE_OBSERVER; }
	void initControls();															///< Show or Hide the Replay controls

	static AsciiString getReplayDir();								///< Returns the directory that holds the replay files.
	static AsciiString getReplayArchiveDir();					///< Returns the directory that holds the archived replay files.
	static AsciiString getReplayExtention();					///< Returns the file extention for replay files.
	static AsciiString getLastReplayFileName();				///< Returns the filename used for the default replay.

	GameInfo *getGameInfo() { return &m_gameInfo; }	///< Returns the slot list for playback game start

	Bool isMultiplayer();												///< is this a multiplayer game (record OR playback)?

	Int getGameMode() { return m_originalGameMode; }

	void logPlayerDisconnect(UnicodeString player, Int slot);
	void logCRCMismatch();
	Bool sawCRCMismatch() const;
	void cleanUpReplayFile();										///< after a crash, send replay/debug info to a central repository

	void setArchiveEnabled(Bool enable) { m_archiveReplays = enable; } ///< Enable or disable replay archiving.
	void stopRecording();															///< Stop recording and close m_file.

	IReplayStreamSink* getStreamSink() { return m_streamSink; }
	void setStreamSink(IReplayStreamSink* sink) { m_streamSink = sink; }
	void setLiveStream(Bool live) { m_isLiveStream = live; }
	void setStreamEnded(Bool ended) { m_streamEnded = ended; }
	Bool isLiveStream() const { return m_isLiveStream; }
	Bool isLiveWaiting() const { return m_liveWaiting; }
	UnsignedInt getNextFrame() const { return m_nextFrame; }	///< Next frame to execute (used for live gap check).
	/// Highest frame the observer has fully received. Sourced from LiveObserver's
	/// network-thread watermark — O(1) and always fresh. Keeps its old name because the
	/// FF handler and the UI call it; there is no longer any caching or probing involved.
	UnsignedInt getCachedLiveEdge() const;

	// ---- Broadcast delay -------------------------------------------------------------
	// The observer never plays closer than this to the live game. Configured in *seconds*
	// (that is what a streamer thinks in, and it stays correct if the logic tick rate
	// changes — this build runs at 60, not the original 30) with frames derived from it.
	UnsignedInt getLiveDelaySeconds() const { return m_liveDelaySeconds; }
	void setLiveDelaySeconds(UnsignedInt seconds) { m_liveDelaySeconds = seconds; }
	UnsignedInt getLiveDelayFrames() const { return m_liveDelaySeconds * LOGICFRAMES_PER_SECOND; }

	/// Whole seconds until the initial buffer is built; 0 once pre-roll is complete.
	Int getPreRollSecondsRemaining() const;
	Bool isPreRollComplete() const { return m_preRollComplete; }

	// ---- Pause ownership -------------------------------------------------------------
	// The user's intent and the buffering logic's are tracked separately and OR'd together,
	// so neither can silently override the other (see updateLiveStreamPause()).
	Bool isUserPaused() const { return m_userPaused; }
	void setUserPaused(Bool paused) { m_userPaused = paused; }

	/// True only when playback is held *and* the source has genuinely stopped producing —
	/// not during the normal sawtooth of maintaining the delay at the boundary.
	Bool isLiveStalled() const { return m_liveStalled; }

	/// Live-stream housekeeping that must keep running even while GameLogic::UPDATE() is
	/// skipped by the pause — otherwise the pause can never be cleared. Called from
	/// GameEngine::update() outside the halted path.
	void updateLiveStreamPoll();

	/// Release the live replay file and forget the finished session.
	///
	/// Must run before starting another live-observer session. The file is named after the
	/// streamer's game, so rejoining the same game targets the same path — and Windows will
	/// not delete a file this class still holds open, so the new session cannot create it.
	/// Deliberately not stopPlayback(), which also exits the game.
	void endLiveObserverSession();

protected:
	void startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS);					///< Start recording to m_file.
	void writeToFile(GameMessage *msg);								///< Write this GameMessage to m_file.
	void archiveReplay(AsciiString fileName);					///< Move the specified replay file to the archive directory.

	void logGameStart(AsciiString options);
	void logGameEnd();

	AsciiString readAsciiString();										///< Read the next string from m_file using ascii characters.
	UnicodeString readUnicodeString();								///< Read the next string from m_file using unicode characters.
	/// Outcome of trying to read the next record's frame number in a live stream.
	enum ReadFrameResult CPP_11(: Int)
	{
		READFRAME_OK,				///< m_nextFrame updated (or a future frame was peeked and rewound)
		READFRAME_EOF_WAITING,		///< no complete record available yet; m_nextFrame untouched
		READFRAME_STREAM_STOPPED	///< the stream really ended; playback has been stopped
	};

	ReadFrameResult readNextFrame();									///< Read the next frame number to execute a command on.
	void appendNextCommand();													///< Read the next GameMessage and append it to TheCommandList.

	/// Recompute m_liveWaiting / pre-roll and apply the resulting pause state.
	/// Shared by updatePlayback() and updateLiveStreamPoll() so the two cannot drift.
	void updateLiveStreamPause(UnsignedInt curFrame);
	void writeArgument(GameMessageArgumentDataType type, const GameMessageArgumentType arg);
	void readArgument(GameMessageArgumentDataType type, GameMessage *msg);

	struct CullBadCommandsResult
	{
		CullBadCommandsResult() : hasClearGameDataMessage(false) {}
		Bool hasClearGameDataMessage;
	};

	CullBadCommandsResult cullBadCommands(); ///< prevent the user from giving mouse commands that he shouldn't be able to do during playback.

	CRCInfo m_crcInfo;
	File* m_file;
	AsciiString m_fileName;
	Int m_currentFilePosition;
	RecorderModeType m_mode;
	AsciiString m_currentReplayFilename;							///< valid during playback only
	UnsignedInt m_playbackFrameCount;

	ReplayGameInfo m_gameInfo;
	Bool m_wasDesync;

	Bool m_doingAnalysis;
	Bool m_archiveReplays;														///< if true, each replay is archived to the replay archive folder after recording

	Int m_originalGameMode; // valid in replays

	UnsignedInt m_nextFrame;												///< The Frame that the next message is to be executed on.  This can be -1.

	IReplayStreamSink* m_streamSink;
	Bool m_isLiveStream;
	Bool m_streamEnded;
	Bool m_liveWaiting;

	// How long the live edge must sit still before a hold counts as a stall rather than
	// normal delay maintenance. At the boundary m_liveWaiting toggles every few ticks, so
	// the status bar needs this to avoid reading WAITING FOR FRAMES during healthy playback.
	enum { LIVE_STALL_THRESHOLD_MS = 1000 };

	// Let the game actually get on its feet before holding it. GameClient::step() — and so
	// TheDisplay->step() — is only called on ticks where logic runs, so holding immediately
	// at frame 1 leaves a loaded but never-composed scene: a black screen. A couple of
	// seconds of warmup costs a rounding error against a 3600-frame delay.
	enum { LIVE_PREROLL_WARMUP_FRAMES = 120 };

	UnsignedInt m_liveDelaySeconds;
	Bool m_preRollComplete;			///< latches TRUE once the initial buffer is first built
	Bool m_liveStreamAutoPaused;	///< the buffering logic owns the current pause
	Bool m_userPaused;				///< the user pressed P and wants it paused
	Bool m_liveStalled;				///< held, and no new data has arrived for a while
	UnsignedInt m_lastSeenLiveEdge;
	UnsignedInt m_lastLiveEdgeChangeMs;
};

extern RecorderClass *TheRecorder;
RecorderClass *createRecorder();
void recorderLog(const char* fmt, ...);
