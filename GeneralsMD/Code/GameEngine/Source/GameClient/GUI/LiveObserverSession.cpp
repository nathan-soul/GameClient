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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LiveObserverSession.cpp
// Description: The live-observer join state machine. Extracted from MainMenu.cpp, which had
// accumulated it because the first entry point was the main menu; it is not main-menu logic.
// Every shell screen that can queue or pump a session calls in here — the Watch Live browser,
// the read-only pre-game lobby view, the Online welcome screen, the main menu and the password
// popup — and none of them owns the state.
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if defined(GENERALS_ONLINE)

#include "GameClient/LiveObserverSession.h"

#include "Common/GlobalData.h"
#include "Common/LiveObserver.h"
#include "Common/Recorder.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GUICallbacks.h"			// liveWatchOpenPasswordPopup
#include "GameClient/Shell.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameNetwork/NetworkDefs.h"		// TheNetwork, for the entry log line

#include <windows.h>						// timeGetTime

// The queued session. Written by StartLiveObserverSession from whichever screen picked the
// game, read by the pump below from whichever screen the player is standing on.
static Bool startLiveObserverGame = FALSE;
static AsciiString m_liveObserverStartLobbyId;
static AsciiString s_liveObserverPassword;		// password for a password-protected stream
static AsciiString s_liveObserverDisplayName;	// lobby name, for the password reprompt title
static Int s_liveObserverDelaySeconds = -1;		// lobby broadcast delay, -1 = unknown

// Declared in LiveObserverSession.h so any screen can hand a chosen game back here.
//
// Only records the intent. doLiveObserverGameStart() blocks waiting for the relay's HEADER
// before starting playback, so it must not run while another screen is still up — the
// LiveObserverStartPendingSession() pump fires it once the shell animation and transition
// have settled.
void StartLiveObserverSession(const AsciiString& lobbyId,
	const AsciiString& password, const AsciiString& displayName, Int delaySeconds)
{
	if (lobbyId.isEmpty())
		return;

	// The observer joins from a live shell screen, so the client must stay in the ordinary
	// menu state. Deliberately NOT setting m_afterIntro here: that flag re-enters the
	// intro/movie machinery, which sets m_breakTheMovie and disables rendering until a menu
	// or load screen clears it again — the whole screen freezes while the logic keeps
	// running. The shell map stays off so nothing competes with the replay that is about to
	// start.
	if (TheWritableGlobalData)
	{
		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_playSizzle = FALSE;
		TheWritableGlobalData->m_shellMapOn = FALSE;
	}

	// Multi-instance support like replay mode
	rts::ClientInstance::setMultiInstance(TRUE);
	rts::ClientInstance::skipPrimaryInstance();

	m_liveObserverStartLobbyId = lobbyId;
	s_liveObserverPassword = password;
	s_liveObserverDisplayName = displayName;
	s_liveObserverDelaySeconds = delaySeconds;
	startLiveObserverGame = TRUE;

	liveObserverLog("StartLiveObserverSession: queued lobby %s\n", lobbyId.str());
}

// The join is split into a non-blocking connect and a playback start, pumped from the shell
// screens via LiveObserverStartPendingSession(). The wait between them — for the relay to
// deliver the header plus enough body to cover the broadcast delay — must not block the main
// loop, or the shell could not keep rendering the countdown that explains it. All of the
// readiness logic lives on LiveObserver (isPlaybackReady); this file only sequences it.
enum ObserverJoinPhase
{
	kObserverJoinIdle,		// nothing pending
	kObserverJoinWaiting,	// connected; waiting for the file to cover the delay
};
static ObserverJoinPhase s_observerJoinPhase = kObserverJoinIdle;
static UnsignedInt s_observerJoinStartedAt = 0;

// Declared in LiveObserverSession.h. Abort a queued or connecting observer session — the
// counterpart to StartLiveObserverSession, needed by the read-only lobby view's LEAVE button
// (plans/lobby-observer.md). Mirrors the timeout path in LiveObserverStartPendingSession.
void CancelLiveObserverPendingSession(void)
{
	liveObserverLog("CancelLiveObserverPendingSession: cancelling queued observer join\n");

	if (s_observerJoinPhase == kObserverJoinWaiting && TheLiveObserver != nullptr)
	{
		liveObserverEndSession();
	}

	s_observerJoinPhase = kObserverJoinIdle;
	startLiveObserverGame = FALSE;
	m_liveObserverStartLobbyId.clear();

	// A cancelled session must not leak its password into the next join.
	s_liveObserverPassword.clear();
	s_liveObserverDisplayName.clear();
	s_liveObserverDelaySeconds = -1;
}

// Declared in LiveObserverSession.h. TRUE while a session is queued or connecting.
Bool LiveObserverPendingSessionActive(void)
{
	return startLiveObserverGame;
}

// Phase 1: end any previous session and start connecting. Non-blocking — the network thread
// does the work and publishes the header and watermarks as it goes.
static Bool doLiveObserverConnect(void)
{
	liveObserverInitLog(m_liveObserverStartLobbyId.str());
	liveObserverLog("=== doLiveObserverGameStart (from menu) ===\n");
	liveObserverLog("Lobby: %s\n", m_liveObserverStartLobbyId.str());
	liveObserverLog("doLiveObserverGameStart: entry — TheNetwork=%p isInMultiplayerGame=%d\n",
		(void*)TheNetwork, TheGameLogic->isInMultiplayerGame() ? 1 : 0);

	// End any previous session outright. This destroys the old LiveObserver and releases the
	// Recorder's read handle on its file — both are needed, because the live file is named
	// after the streamer's game, so rejoining a game we already watched targets the same
	// path, which openLiveFile() cannot delete or recreate while either handle is open.
	liveObserverEndSession();

	TheLiveObserver = createLiveObserver();
	if (!TheLiveObserver)
	{
		liveObserverLog("doLiveObserverConnect: createLiveObserver() returned NULL!\n");
		return FALSE;
	}

	liveObserverLog("doLiveObserverConnect: connecting to relay...\n");
	TheLiveObserver->connect(m_liveObserverStartLobbyId, s_liveObserverPassword.str(),
		s_liveObserverDelaySeconds);
	return TRUE;
}

// Phase 2: the file is playable (LiveObserver::isPlaybackReady). Sanity-check it and start
// playback; returns TRUE only when playback actually started, so the shell screen that pumped
// this can tear itself down and reveal the game.
static Bool doLiveObserverStartPlayback(void)
{
	if (TheLiveObserver == nullptr)
		return FALSE;

	AsciiString filename = TheLiveObserver->getLiveReplayFilename();
	{
		AsciiString filepath = RecorderClass::getReplayDir();
		filepath.concat(filename);
		FILE* fp = fopen(filepath.str(), "rb");
		if (fp)
		{
			fseek(fp, 0, SEEK_END);
			long fsize = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			char magic[7] = {0};
			fread(magic, 1, 6, fp);
			fclose(fp);
			liveObserverLog("doLiveObserverStartPlayback: %s size=%ld magic=%.6s\n", filename.str(), fsize, magic);
		}
		else
		{
			liveObserverLog("doLiveObserverStartPlayback: %s MISSING at %s\n", filename.str(), filepath.str());
		}
	}

	if (!TheRecorder->startLiveObserverPlayback(filename))
	{
		liveObserverLog("doLiveObserverStartPlayback: FAILED — playbackFile returned false\n");
		liveObserverEndSession();
		return FALSE;
	}

	liveObserverLog("doLiveObserverStartPlayback: playback started\n");
	return TRUE;
}

// Declared in LiveObserverSession.h. Fire a queued session once the shell has settled, then
// keep pumping it while the relay builds the file.
//
// Connecting and starting a game must not happen mid-animation, so the first call (which
// connects) waits for the shell to settle. After that the screen pumps this every frame:
// while the file is still buffering the broadcast delay it returns FALSE and the shell keeps
// drawing the countdown; once the file is playable it starts playback and returns TRUE so the
// caller can stand itself down and reveal the game.
Bool LiveObserverStartPendingSession(void)
{
	if (!startLiveObserverGame)
		return FALSE;

	if (s_observerJoinPhase == kObserverJoinIdle)
	{
		if (!TheShell->isAnimFinished() || !TheTransitionHandler->isFinished())
			return FALSE;

		if (!doLiveObserverConnect())
		{
			startLiveObserverGame = FALSE;
			return FALSE;
		}
		s_observerJoinStartedAt = timeGetTime();
		s_observerJoinPhase = kObserverJoinWaiting;
		liveObserverLog("LiveObserverStartPendingSession: connected, waiting for the file to cover the delay (up to %ums)\n",
			TheLiveObserver->getJoinTimeoutMs());
		return FALSE;
	}

	// kObserverJoinWaiting — pump the wait. Every failure path clears the join state.
	if (TheLiveObserver == nullptr)
	{
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return FALSE;
	}

	if (TheLiveObserver->isPlaybackReady())
	{
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return doLiveObserverStartPlayback();
	}

	// A 401 from the watch-ticket request means the stream is password protected and the
	// supplied password was missing or wrong. Stop the join and ask again through the same
	// popup custom games use; OK reopens it for another try (see plans/live-watch-password.md).
	// This is the single reprompt path — it covers a wrong password typed into the browser
	// popup and the pre-game→live handoff of a passworded lobby alike.
	if (TheLiveObserver->isPasswordRejected())
	{
		liveObserverLog("LiveObserverStartPendingSession: password rejected for lobby %s\n",
			m_liveObserverStartLobbyId.str());

		liveObserverEndSession();
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;

		// The queue statics still hold the lobby id and display name — they are cleared by
		// CancelLiveObserverPendingSession or overwritten by the next StartLiveObserverSession.
		GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"),
			TheGameText->fetch("GUI:JoinFailedBadPassword"), []()
			{
				liveWatchOpenPasswordPopup(m_liveObserverStartLobbyId, s_liveObserverDisplayName, FALSE);
			});

		return FALSE;
	}

	if (timeGetTime() > TheLiveObserver->getJoinDeadlineMs())
	{
		liveObserverLog("LiveObserverStartPendingSession: timed out waiting for a playable file — abandoning the join\n");
		liveObserverEndSession();
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return FALSE;
	}

	return FALSE;
}

#endif // GENERALS_ONLINE
