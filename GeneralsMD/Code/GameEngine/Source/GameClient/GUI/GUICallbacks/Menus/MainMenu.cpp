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

// FILE: MainMenu.cpp /////////////////////////////////////////////////////////////////////////////
// Author: Colin Day, October 2001
// Description: Main menu window callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "gamespy/ghttp/ghttp.h"

#include "Lib/BaseType.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/RandomValue.h"
#include "Common/OptionPreferences.h"
#include "Common/version.h"
#include "Common/GameLOD.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/ExtendedMessageBox.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Display.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameText.h"
#include "GameClient/HeaderTemplate.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Shell.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/Mouse.h"
#include "GameClient/WindowVideoManager.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/HotKey.h"
#include "GameClient/GameClient.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/ChallengeGenerals.h"

#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpy/BuddyThread.h"

#include "GameNetwork/DownloadManager.h"
#include "GameNetwork/GameSpy/MainMenuUtils.h"

#include "GameClient/InGameUI.h"
#include "../OnlineServices_Init.h"

#if defined(GENERALS_ONLINE)
#include "Common/LiveObserver.h"
#include "Common/Recorder.h"
#include "Common/GameCommon.h"					// LIVE_DEFAULT_RELAY_URL, LIVE_DELAY_SECONDS_DEFAULT
#include "Common/MessageStream.h"
#include "GameClient/GadgetListBox.h"			// live game browser list
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/json.hpp"	// already vendored; used to parse /games
#include <vector>
#include "Common/PlayerTemplate.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameLogic/GameLogic.h"
#endif


// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////

enum
{
	DROPDOWN_NONE = 0,
	DROPDOWN_SINGLE,
	DROPDOWN_MULTIPLAYER,
	DROPDOWN_MAIN,
	DROPDOWN_LOADREPLAY,
	DROPDOWN_DIFFICULTY,

	DROPDOWN_COUNT
};

static Bool raiseMessageBoxes = TRUE;
static Bool campaignSelected = FALSE;
#if defined(RTS_DEBUG) || defined RTS_PROFILE_LEGACY
static NameKeyType campaignID = NAMEKEY_INVALID;
static GameWindow *buttonCampaign = nullptr;
#ifdef TEST_COMPRESSION
static GameWindow *buttonCompressTest = nullptr;
void DoCompressTest();
#endif // TEST_COMPRESSION
#endif


// window ids -------------------------------------------------------------------------------------
static NameKeyType mainMenuID = NAMEKEY_INVALID;
static NameKeyType skirmishID = NAMEKEY_INVALID;
static NameKeyType onlineID = NAMEKEY_INVALID;
static NameKeyType networkID = NAMEKEY_INVALID;
static NameKeyType optionsID = NAMEKEY_INVALID;
static NameKeyType exitID = NAMEKEY_INVALID;
static NameKeyType motdID = NAMEKEY_INVALID;
static NameKeyType worldBuilderID = NAMEKEY_INVALID;
static NameKeyType getUpdateID = NAMEKEY_INVALID;
static NameKeyType buttonTRAININGID = NAMEKEY_INVALID;
static NameKeyType buttonChallengeID = NAMEKEY_INVALID;
static NameKeyType buttonUSAID = NAMEKEY_INVALID;
static NameKeyType buttonGLAID = NAMEKEY_INVALID;
static NameKeyType buttonChinaID = NAMEKEY_INVALID;
static NameKeyType buttonUSARecentSaveID = NAMEKEY_INVALID;
static NameKeyType buttonUSALoadGameID = NAMEKEY_INVALID;
static NameKeyType buttonGLARecentSaveID = NAMEKEY_INVALID;
static NameKeyType buttonGLALoadGameID = NAMEKEY_INVALID;
static NameKeyType buttonChinaRecentSaveID = NAMEKEY_INVALID;
static NameKeyType buttonChinaLoadGameID = NAMEKEY_INVALID;
static NameKeyType buttonSinglePlayerID = NAMEKEY_INVALID;
static NameKeyType buttonMultiPlayerID = NAMEKEY_INVALID;
static NameKeyType buttonMultiBackID = NAMEKEY_INVALID;
static NameKeyType buttonSingleBackID = NAMEKEY_INVALID;
static NameKeyType buttonLoadReplayBackID = NAMEKEY_INVALID;
static NameKeyType buttonReplayID = NAMEKEY_INVALID;
static NameKeyType buttonLoadReplayID = NAMEKEY_INVALID;
static NameKeyType buttonLoadID = NAMEKEY_INVALID;
static NameKeyType buttonCreditsID = NAMEKEY_INVALID;
static NameKeyType buttonEasyID = NAMEKEY_INVALID;
static NameKeyType buttonMediumID = NAMEKEY_INVALID;
static NameKeyType buttonHardID = NAMEKEY_INVALID;
static NameKeyType buttonDiffBackID = NAMEKEY_INVALID;

#if defined(GENERALS_ONLINE)
// Live Observer button and dialog controls
static NameKeyType buttonLiveObserverID = NAMEKEY_INVALID;
static GameWindow *buttonLiveObserver = nullptr;
static GameWindow *liveObserverDialogPanel = nullptr;
static Bool startLiveObserverGame = FALSE;
static AsciiString m_liveObserverStartLobbyId;

// TheSuperHackers @feature 03/08/2026 Live game browser. Replaces typing a game ID by hand,
// which offered no way to discover a game and no feedback on a typo.

// Game IDs indexed by listbox row. Kept alongside the listbox rather than stuffed into
// GadgetListBoxSetItemData, so nothing depends on the lifetime of a void* we hand the gadget.

// Forward declaration
static void doLiveObserverGameStart(const AsciiString& lobbyId);

#endif

// window pointers --------------------------------------------------------------------------------
static GameWindow *parentMainMenu = nullptr;
static GameWindow *buttonSinglePlayer = nullptr;
static GameWindow *buttonMultiPlayer = nullptr;
static GameWindow *buttonSkirmish = nullptr;
static GameWindow *buttonOnline = nullptr;
static GameWindow *buttonNetwork = nullptr;
static GameWindow *buttonOptions = nullptr;
static GameWindow *buttonExit = nullptr;
static GameWindow *buttonMOTD = nullptr;
static GameWindow *buttonWorldBuilder = nullptr;
static GameWindow *mainMenuMovie = nullptr;
static GameWindow *getUpdate = nullptr;
static GameWindow *buttonTRAINING = nullptr;
static GameWindow *buttonChallenge = nullptr;
static GameWindow *buttonUSA = nullptr;
static GameWindow *buttonGLA = nullptr;
static GameWindow *buttonChina = nullptr;
static GameWindow *buttonUSARecentSave = nullptr;
static GameWindow *buttonUSALoadGame = nullptr;
static GameWindow *buttonGLARecentSave = nullptr;
static GameWindow *buttonGLALoadGame = nullptr;
static GameWindow *buttonChinaRecentSave = nullptr;
static GameWindow *buttonChinaLoadGame = nullptr;
static GameWindow *buttonReplay = nullptr;
static GameWindow *buttonLoadReplay = nullptr;
static GameWindow *buttonLoad = nullptr;
static GameWindow *buttonCredits = nullptr;
static GameWindow *buttonEasy = nullptr;
static GameWindow *buttonMedium = nullptr;
static GameWindow *buttonHard = nullptr;
static GameWindow *buttonDiffBack = nullptr;
static GameWindow *dropDownWindows[DROPDOWN_COUNT];

static Bool buttonPushed = FALSE;
static Bool isShuttingDown = FALSE;
static Bool startGame = FALSE;
static Int	initialGadgetDelay = 210;

enum
{
	SHOW_NONE = 0,
	SHOW_TRAINING,
	SHOW_USA,
	SHOW_GLA,
	SHOW_CHINA,
	SHOW_SKIRMISH,
	SHOW_FRAMES_LIMIT = 20
};

static Int showFade = FALSE;
static Int dropDown = DROPDOWN_NONE;
static Int pendingDropDown = DROPDOWN_NONE;
static AnimateWindowManager *localAnimateWindowManager = nullptr;
static Bool notShown = TRUE;
static Bool FirstTimeRunningTheGame = TRUE;

static Bool showLogo = FALSE;
static Int  showFrames = 0;
static Int  showSide = SHOW_NONE;
static Bool logoIsShown = FALSE;
static Bool justEntered = FALSE;
static Bool launchChallengeMenu = FALSE;

static Bool dontAllowTransitions = FALSE;

const Int /*TIME_OUT = 15,*/ CORNER = 10;
void AcceptResolution();
void DeclineResolution();
GameWindow *resAcceptMenu = nullptr;
extern DisplaySettings oldDispSettings, newDispSettings;
extern Bool dispChanged;
//static time_t timeStarted = 0, currentTime = 0;

void diffReverseSide();
void HandleCanceledDownload( Bool resetDropDown )
{
	NGMP_OnlineServicesManager::GetInstance()->CancelUpdate();

	buttonPushed = FALSE;
	if (resetDropDown)
	{
		dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
		TheTransitionHandler->setGroup("MainMenuDefaultMenuLogoFade");
	}
}

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------

static void showSelectiveButtons( Int show )
{
	buttonUSARecentSave->winHide(!(show == SHOW_USA ));
	buttonUSALoadGame->winHide(!(show == SHOW_USA ));
	buttonGLARecentSave->winHide(!(show == SHOW_GLA ));
	buttonGLALoadGame->winHide(!(show == SHOW_GLA ));
	buttonChinaRecentSave->winHide(!(show == SHOW_CHINA ));
	buttonChinaLoadGame->winHide(!(show == SHOW_CHINA ));
}

static void quitCallback()
{
	buttonPushed = TRUE;
	TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_EXIT_SELECTED]);
	TheShell->pop();
	TheGameEngine->setQuitting( TRUE );



	//if (!TheGameLODManager->didMemPass())
	{	//GIANT CRAPTACULAR HACK ALERT!!!!  On sytems with little memory, we skip all normal exit code
//		//and let Windows clean up the mess.  This reduces exit times from minutes to seconds.
//		//8-19-03. MW
//		delete TheGameClient;
//		_exit(EXIT_SUCCESS);

//  THE CRAP IS NOW EVEN MORE TACULAR
//  NOW WE PERSUADE THE MEMORYPOOLMANAGER TO RETURN STUPID FROM ITS FREE()
//    if (TheMemoryPoolFactory) TheMemoryPoolFactory->prepareForMinSpecShutDown();

	}
	if (TheGameLogic->isInGame())
		TheMessageStream->appendMessage( GameMessage::MSG_CLEAR_GAME_DATA );
}


void setupGameStart(AsciiString mapName, GameDifficulty diff)
{
	TheCampaignManager->setGameDifficulty(diff);

	if (launchChallengeMenu)
	{
		if (TheChallengeGenerals)
			TheChallengeGenerals->setCurrentDifficulty(diff);

		campaignSelected = TRUE;
		TheShell->push( "Menus/ChallengeMenu.wnd" );
		TheTransitionHandler->reverse("MainMenuDifficultyMenuTraining");
	}
	else
	{
		startGame = TRUE;
		TheWritableGlobalData->m_pendingFile = mapName;
		TheShell->reverseAnimatewindow();
		TheTransitionHandler->setGroup("FadeWholeScreen");
	}
}

void prepareCampaignGame(GameDifficulty diff)
{
	dontAllowTransitions = TRUE;
	OptionPreferences pref;
	pref.setCampaignDifficulty(diff);
	pref.write();
	TheScriptEngine->setGlobalDifficulty(diff);

	buttonPushed = FALSE;
	TheTransitionHandler->reverse("MainMenuDifficultyMenuBack");
	setupGameStart(TheCampaignManager->getCurrentMap(), diff );
}

static void doGameStart()
{
	startGame = FALSE;

	if (TheGameLogic->isInGame())
		TheGameLogic->clearGameData();

#if defined(GENERALS_ONLINE)
	// If live streaming, playbackFile already sent MSG_NEW_GAME with GAME_REPLAY.
	// Don't send a second one or we'd start a duplicate game.
	if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_LIVE_OBSERVER)
	{
		liveObserverLog("doGameStart: skipping MSG_NEW_GAME (already sent by live playback)\n");
		isShuttingDown = TRUE;
		return;
	}
#endif

	// send a message to the logic for a new game
	GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
	msg->appendIntegerArgument(GAME_SINGLE_PLAYER);
	msg->appendIntegerArgument(TheCampaignManager->getGameDifficulty());
	msg->appendIntegerArgument(TheCampaignManager->getRankPoints());
	InitRandom(0);

	isShuttingDown = TRUE;
}

static void shutdownComplete( WindowLayout *layout )
{
	isShuttingDown = FALSE;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout );

}



/*
static void TimetToFileTime( time_t t, LPFILETIME pft )
{
	LONGLONG ll = Int32x32To64(t, 10000000) + 116444736000000000;
	pft->dwLowDateTime = (DWORD) ll;
	pft->dwHighDateTime = ll >>32;
}
*/

void initialHide()
{
GameWindow *win = nullptr;
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLA"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChina"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUS"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinGrowMarker"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTraining"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTrainingSmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTrainingMedium"));
	if(win)
		win->winHide(TRUE);

	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionSkirmish"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionSkirmishSmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionSkirmishMedium"));
	if(win)
		win->winHide(TRUE);

	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUS"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUSSmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUSMedium"));
	if(win)
		win->winHide(TRUE);

	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLA"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLASmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLAMedium"));
	if(win)
		win->winHide(TRUE);

	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChina"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChinaSmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChinaMedium"));
	if(win)
		win->winHide(TRUE);

}

// TheSuperHackers @tweak Now prints version information in an optional version label.
// Originally this label does not exist in the Main Menu. It can be copied from the Options Menu.
static void initLabelVersion()
{
	NameKeyType versionID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:LabelVersion" );
	GameWindow *labelVersion = TheWindowManager->winGetWindowFromId( nullptr, versionID );

	if (labelVersion)
	{
		if (TheVersion && TheGlobalData)
		{
			UnicodeString text = TheVersion->getUnicodeProductVersionHashString();
			GadgetStaticTextSetText( labelVersion, text );
		}
		else
		{
			labelVersion->winHide( TRUE );
		}
	}
}

#if defined(GENERALS_ONLINE)
//-------------------------------------------------------------------------------------------------
/** Find a window with the given id among this parent's descendants only.
 *
 * Deliberately not GameWindowManager::winGetWindowFromId(), which also walks the given window's
 * *siblings* — with a main-menu layout still pending destruction it could hand back the button
 * belonging to the outgoing layout, which is the same class of bug this lookup exists to avoid.
 * The question being asked is strictly "does THIS parent already own one?".
 */
//-------------------------------------------------------------------------------------------------
static GameWindow* findDescendantById( GameWindow *parent, Int id )
{
	if( parent == nullptr )
		return nullptr;

	for( GameWindow *child = parent->winGetChild(); child != nullptr; child = child->winGetNext() )
	{
		if( child->winGetWindowId() == id )
			return child;

		GameWindow *nested = findDescendantById( child, id );
		if( nested != nullptr )
			return nested;
	}

	return nullptr;
}
#endif

//-------------------------------------------------------------------------------------------------
/** Initialize the main menu */
//-------------------------------------------------------------------------------------------------
void MainMenuInit( WindowLayout *layout, void *userData )
{
	TheWritableGlobalData->m_breakTheMovie = FALSE;

	TheShell->showShellMap(TRUE);
	TheMouse->setVisibility(TRUE);
	//winVidManager = NEW WindowVideoManager;
	buttonPushed = FALSE;
	isShuttingDown = FALSE;
	startGame = FALSE;
	dropDown = DROPDOWN_NONE;
	pendingDropDown = DROPDOWN_NONE;
	Int i = 0;
	for(; i < DROPDOWN_COUNT; ++i)
		dropDownWindows[i] = nullptr;

	// get ids for our windows
	mainMenuID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MainMenuParent" );
//	campaignID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonCampaign" );
	skirmishID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonSkirmish" );
	onlineID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonOnline" );
	networkID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonNetwork" );
	optionsID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonOptions" );
	exitID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonExit" );
	motdID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonMOTD" );
	worldBuilderID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonWorldBuilder" );
	getUpdateID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonGetUpdate" );
//	buttonTRAININGID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonTRAINING" );
	buttonChallengeID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonChallenge" );
	buttonUSAID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonUSA" );
	buttonGLAID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonGLA" );
	buttonChinaID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonChina" );
	buttonUSARecentSaveID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonUSARecentSave" );
	buttonUSALoadGameID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonUSALoadGame" );
	buttonGLARecentSaveID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonGLARecentSave" );
	buttonGLALoadGameID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonGLALoadGame" );
	buttonChinaRecentSaveID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonChinaRecentSave" );
	buttonChinaLoadGameID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonChinaLoadGame" );
	buttonSinglePlayerID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonSinglePlayer" );
	buttonMultiPlayerID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonMultiplayer" );
	buttonMultiBackID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonMultiBack" );
	buttonSingleBackID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonSingleBack" );
	buttonLoadReplayBackID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonLoadReplayBack" );
	buttonReplayID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonReplay" );
	buttonLoadReplayID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonLoadReplay" );
	buttonLoadID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonLoadGame" );
	buttonCreditsID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonCredits" );

	buttonEasyID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonEasy" );
	buttonMediumID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonMedium" );
	buttonHardID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonHard" );
	buttonDiffBackID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonDiffBack" );

	// get pointers to the window buttons
	parentMainMenu = TheWindowManager->winGetWindowFromId( nullptr, mainMenuID );
	//buttonCampaign = TheWindowManager->winGetWindowFromId( parentMainMenu, campaignID );
	buttonSinglePlayer = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonSinglePlayerID );
	buttonMultiPlayer = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonMultiPlayerID );
	buttonSkirmish = TheWindowManager->winGetWindowFromId( parentMainMenu, skirmishID );
	buttonOnline = TheWindowManager->winGetWindowFromId( parentMainMenu, onlineID );
	buttonNetwork = TheWindowManager->winGetWindowFromId( parentMainMenu, networkID );
	buttonOptions = TheWindowManager->winGetWindowFromId( parentMainMenu, optionsID );
	buttonExit = TheWindowManager->winGetWindowFromId( parentMainMenu, exitID );
	buttonMOTD = TheWindowManager->winGetWindowFromId( parentMainMenu, motdID );
	buttonWorldBuilder = TheWindowManager->winGetWindowFromId( parentMainMenu, worldBuilderID );
	buttonReplay = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonReplayID );
	buttonLoadReplay = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonLoadReplayID );
	buttonLoad = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonLoadID );
	buttonCredits = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonCreditsID );

	buttonEasy = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonEasyID );
	buttonMedium = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonMediumID );
	buttonHard = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonHardID );
	buttonDiffBack = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonDiffBackID );

	getUpdate = TheWindowManager->winGetWindowFromId( parentMainMenu, getUpdateID );
//	buttonTRAINING = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonTRAININGID );
	buttonChallenge = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonChallengeID );
	buttonUSA = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonUSAID );
	buttonGLA = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonGLAID );
	buttonChina = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonChinaID );
	buttonUSARecentSave = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonUSARecentSaveID );
	buttonUSALoadGame = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonUSALoadGameID );
	buttonGLARecentSave = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonGLARecentSaveID );
	buttonGLALoadGame = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonGLALoadGameID );
	buttonChinaRecentSave = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonChinaRecentSaveID );
	buttonChinaLoadGame = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonChinaLoadGameID );

	dropDownWindows[DROPDOWN_SINGLE] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder" ));
	dropDownWindows[DROPDOWN_MULTIPLAYER] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder1" ) );
	dropDownWindows[DROPDOWN_MAIN] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder2" ) );
	dropDownWindows[DROPDOWN_LOADREPLAY] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder3" ) );
	dropDownWindows[DROPDOWN_DIFFICULTY] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder4" ) );
	for(i = 1; i < DROPDOWN_COUNT; ++i)
		dropDownWindows[i]->winHide(TRUE);

	initialHide();

	showSelectiveButtons(SHOW_NONE);
	// Set up the version number
#if defined(RTS_DEBUG) || defined RTS_PROFILE_LEGACY
	WinInstanceData instData;
#ifdef TEST_COMPRESSION
	instData.init();
	BitSet( instData.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK );
	instData.m_textLabelString = "Debug: Compress/Decompress Maps";
	instData.setTooltipText(L"Only Used in Debug and Internal!");
	buttonCompressTest = TheWindowManager->gogoGadgetPushButton( parentMainMenu,
																									 WIN_STATUS_ENABLED | WIN_STATUS_IMAGE,
																									 25, 175,
																									 400, 400,
																									 &instData, nullptr, TRUE );
#endif // TEST_COMPRESSION

	instData.init();
	BitSet( instData.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK );
	instData.m_textLabelString = "Debug: Load Map";

	instData.setTooltipText(L"Only Used in Debug and Internal!");
	buttonCampaign = TheWindowManager->gogoGadgetPushButton( parentMainMenu,
																									 WIN_STATUS_ENABLED,
																									 25, 54,
																									 180, 26,
																									 &instData, nullptr, TRUE );
#endif

#if defined(GENERALS_ONLINE)
	// Live Observer button — only show if relay URL is configured
	if (!TheGlobalData->m_liveStreamRelayUrl.isEmpty())
	{
		// Register control IDs
		buttonLiveObserverID = TheNameKeyGenerator->nameToKey("MainMenu.wnd:ButtonLiveObserver");

		// Create the "Live Observer" button
		WinInstanceData instDataLive;
		instDataLive.init();
		BitSet(instDataLive.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK);
		instDataLive.m_textLabelString = "Watch Live";
		instDataLive.setTooltipText(L"Watch a live game via the relay server");

		// TheSuperHackers @feature 03/08/2026 Sit with Online and Network in the multiplayer
		// submenu, which is where someone looking for a game to watch would go. Position is
		// taken from the Network button at runtime rather than hardcoded: this is a .wnd
		// layout we cannot see from here, and one row below a known button is stable in a way
		// that absolute coordinates are not.
		Int liveX = 25, liveY = 240, liveW = 180, liveH = 26;
		if (buttonNetwork)
		{
			buttonNetwork->winGetPosition(&liveX, &liveY);
			buttonNetwork->winGetSize(&liveW, &liveH);
			liveY += liveH + 4;
		}

		// TheSuperHackers @fix 05/08/2026 Ask the window system whether this parent already has
		// the button, instead of inferring it from a static pointer.
		//
		// The pointer cannot answer the question. MainMenuShutdown() nulls it without destroying
		// anything (see the comment there — destroying is genuinely unsafe during layout
		// teardown), so "null" means only "shutdown ran", not "the button is gone". When the
		// shell keeps the layout alive across a menu round-trip, the button survives while the
		// pointer does not, and init duly created a second one on top of the first: a dead
		// leftover button, which is the symptom this was supposed to have fixed.
		//
		// Giving the button its own window id makes the question answerable. A lookup that finds
		// nothing means this parent really has no button, whether the layout was rebuilt or a
		// fresh pointer happens to land on a freed one's address.
		buttonLiveObserver = findDescendantById(parentMainMenu, (Int)buttonLiveObserverID);

		if (buttonLiveObserver == nullptr)
		{
			buttonLiveObserver = TheWindowManager->gogoGadgetPushButton(parentMainMenu,
				WIN_STATUS_ENABLED | WIN_STATUS_IMAGE,
				liveX, liveY, liveW, liveH,
				&instDataLive, nullptr, TRUE);

			// gogoGadget* leaves a code-created gadget with no id of its own, which is what made
			// it unfindable above. Set it before anything else can look for it.
			if (buttonLiveObserver)
			{
				buttonLiveObserver->winSetWindowId((Int)buttonLiveObserverID);

				// Adopt the real menu's look instead of the placeholder red/yellow that
				// defaultVisual leaves behind. Guarded: gogoGadget* can fail, and the very next
				// use of this pointer was already null-checked while this one was not.
				buttonLiveObserver->winCopyVisualsFrom(buttonOnline);
			}
		}

		// The submenu is animated by a transition group defined in MainMenu.wnd, which a
		// code-created window cannot join. So it is shown and hidden explicitly alongside the
		// dropdown instead — see the buttonMultiPlayerID / buttonMultiBackID handlers.
		if (buttonLiveObserver)
			buttonLiveObserver->winHide(TRUE);
	}
#endif

	initLabelVersion();

	//TheShell->registerWithAnimateManager(buttonCampaign, WIN_ANIMATION_SLIDE_LEFT, TRUE, 800);
	//TheShell->registerWithAnimateManager(buttonSkirmish, WIN_ANIMATION_SLIDE_LEFT, TRUE, 600);
//	TheShell->registerWithAnimateManager(buttonSinglePlayer, WIN_ANIMATION_SLIDE_LEFT, TRUE, 400);
//	TheShell->registerWithAnimateManager(buttonMultiPlayer, WIN_ANIMATION_SLIDE_LEFT, TRUE, 200);
//	TheShell->registerWithAnimateManager(buttonOptions, WIN_ANIMATION_SLIDE_LEFT, TRUE, 1);
//	TheShell->registerWithAnimateManager(buttonExit, WIN_ANIMATION_SLIDE_RIGHT, TRUE, 1);
//
	layout->hide( FALSE );

	/*
	if (!checkedForUpdate)
	{
		DWORD state = 0;
		Bool isConnected = InternetGetConnectedState(&state, 0);
		if (isConnected && !(state & INTERNET_CONNECTION_MODEM_BUSY))
		{
			// wohoo - we're connected!  fire off a check for updates
			checkedForUpdate = TRUE;
			DEBUG_LOG(("Looking for a patch for productID=%d, versionStr=%s, distribution=%d",
				gameProductID, gameVersionUniqueIDStr, gameDistributionID));
			ptCheckForPatch( gameProductID, gameVersionUniqueIDStr, gameDistributionID, patchAvailableCallback, PTFalse, nullptr );
			//ptCheckForPatch( productID, versionUniqueIDStr, distributionID, mapPackAvailableCallback, PTFalse, nullptr );
		}
	}
	if (getUpdate != nullptr)
	{
		getUpdate->winHide( TRUE );
		//getUpdate->winEnable( FALSE );
	}
	/**/

	if (TheGameSpyPeerMessageQueue && !TheGameSpyPeerMessageQueue->isConnected())
	{
		DEBUG_LOG(("Tearing down GameSpy from MainMenuInit()"));
		TearDownGameSpy();
	}
	if (TheMapCache)
		TheMapCache->updateCache();

	/*
	if (MOTDBuffer && buttonMOTD)
	{
		buttonMOTD->winHide(FALSE);
	}
	*/

	TheShell->loadScheme("MainMenu");
	raiseMessageBoxes = TRUE;

//	if(!localAnimateWindowManager)
//		localAnimateWindowManager = NEW AnimateWindowManager;

	//pendingDropDown =DROPDOWN_MAIN;


	GameWindow *rule = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MainMenuRuler" ) );
	if(rule)
		rule->winHide(TRUE);
	campaignSelected = FALSE;
//	dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
	if(FirstTimeRunningTheGame)
	{
		TheMouse->setVisibility(FALSE);

		TheTransitionHandler->reverse("FadeWholeScreen");
		FirstTimeRunningTheGame  = FALSE;
	}
	else
	{
		showFade = TRUE;
		justEntered = TRUE;
		initialGadgetDelay = 2;
		if(rule)
		rule->winHide(FALSE);
	}

	layout->bringForward();
	// set keyboard focus to main parent
	TheWindowManager->winSetFocus( parentMainMenu );


}

//-------------------------------------------------------------------------------------------------
/** Main menu shutdown method */
//-------------------------------------------------------------------------------------------------
void MainMenuShutdown( WindowLayout *layout, void *userData )
{
	if (!startGame)
		isShuttingDown = TRUE;

	CancelPatchCheckCallback();

#if defined(GENERALS_ONLINE)
	// Drop the reference without destroying anything — the layout owns this window and tears it
	// down with the rest of its children.
	//
	// Deliberately not winDestroy(): that is deferred through processDestroyList while the
	// layout is tearing the same children down, and a crash on returning to the menu is what
	// came of assuming ownership here.
	//
	// Nulling here is only hygiene against a dangling pointer between screens; it is NOT how
	// MainMenuInit() decides whether to build a button. It cannot be — the layout sometimes
	// survives this call and takes its button with it. Init looks the button up by id on the
	// actual parent instead.
	buttonLiveObserver = nullptr;
#endif

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;

//	if(winVidManager)
	//		delete winVidManager;
	//	winVidManager = nullptr;


	if( popImmediate )
	{
//		if(localAnimateWindowManager)
//		{
//			delete localAnimateWindowManager;
//			localAnimateWindowManager = nullptr;
//		}
		shutdownComplete( layout );
		return;

	}

	if (!startGame)
		TheShell->reverseAnimatewindow();
	//TheShell->reverseAnimatewindow();
//	if(localAnimateWindowManager && dropDown != DROPDOWN_NONE)
//		localAnimateWindowManager->reverseAnimateWindow();
}

extern Bool DontShowMainMenu;

////////////////////////////////////////////////////////////////////////////
//Allows the user to confirm the change, goes back to the previous mode
//if the time to change expires.
////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
// Accept Resolution callback method
//-------------------------------------------------------------------------------------------------
void AcceptResolution()
{
	//Keep new settings and bail with setting the display changed flag
	//set to off
	oldDispSettings = newDispSettings;
	dispChanged = FALSE;
}

//-------------------------------------------------------------------------------------------------
// Decline Resolution callback method
//-------------------------------------------------------------------------------------------------
void DeclineResolution()
{
	//Revert back to old resolution and reset all necessary
	//parts of the shell

	if (TheDisplay->setDisplayMode(oldDispSettings.xRes, oldDispSettings.yRes,
										oldDispSettings.bitDepth, oldDispSettings.windowed))
	{
		dispChanged = FALSE;
		newDispSettings = oldDispSettings;

		TheWritableGlobalData->m_xResolution = newDispSettings.xRes;
		TheWritableGlobalData->m_yResolution = newDispSettings.yRes;

		TheHeaderTemplateManager->onResolutionChanged();
		TheMouse->onResolutionChanged();

		AsciiString prefString;
		prefString.format("%d %d", newDispSettings.xRes, newDispSettings.yRes);

		OptionPreferences optionPref;
		optionPref["Resolution"] = prefString;
		optionPref.write();

		TheShell->recreateWindowLayouts();

		TheInGameUI->recreateControlBar();
	}
}

//-------------------------------------------------------------------------------------------------
// Accept/Decline Resolution dialog method
//-------------------------------------------------------------------------------------------------
void DoResolutionDialog()
{
	//Bring up a dialog to accept the resolution chosen in the options menu
	UnicodeString resolutionNew;

	UnicodeString resTimerString = TheGameText->fetch("GUI:Resolution");

	resolutionNew.format(L": %dx%d\n", newDispSettings.xRes , newDispSettings.yRes);

	resTimerString.concat(resolutionNew);


	resAcceptMenu = TheWindowManager->gogoMessageBox( CORNER, CORNER, -1, -1,MSG_BOX_OK | MSG_BOX_CANCEL ,
																									 TheGameText->fetch("GUI:Resolution"),
																									 resTimerString, nullptr, nullptr, AcceptResolution,
																									 DeclineResolution);
}

/* This function is not being currently used because we do not need a timer on the
// dialog box.
//-------------------------------------------------------------------------------------------------
//ResolutionDialogUpdate() - if resolution dialog box is shown, this must count 10 seconds for
//	accepting resolution changes otherwise we go back to previous display settings
//-------------------------------------------------------------------------------------------------
void ResolutionDialogUpdate()
{
	if (timeStarted == 0 && currentTime == 0)
	{
		timeStarted = currentTime = time(nullptr);
	}
	else
	{
		currentTime = time(nullptr);
	}

	if ( ( currentTime - timeStarted ) >= TIME_OUT)
	{
		currentTime = timeStarted = 0;
		DeclineResolution();
	}
	//------------------------------------------------------------------------------------------------------
	// Used for debugging purposes
	//------------------------------------------------------------------------------------------------------
	DEBUG_LOG(("Resolution Timer :  started at %d,  current time at %d, frameTicker is %d", timeStarted,
							time(nullptr) , currentTime));
}
*/

//-------------------------------------------------------------------------------------------------
/** Main menu update method */
//-------------------------------------------------------------------------------------------------
void DownloadMenuUpdate( WindowLayout *layout, void *userData );
void MainMenuUpdate( WindowLayout *layout, void *userData )
{
	if( TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() )
	{
		return;
	}
	if(DontShowMainMenu && justEntered)
		justEntered = FALSE;

	if (TheDownloadManager && !TheDownloadManager->isDone())
	{
		TheDownloadManager->update();
		DownloadMenuUpdate(layout, userData);
	}

	/* This is also commented for the same reason as the top
	if (dispChanged)
	{
		ResolutionDialogUpdate();
		return;
	}
	*/

	if(justEntered)
	{
		if(initialGadgetDelay == 1)
		{
			TheTransitionHandler->setGroup("MainMenuDefaultMenuLogoFade");
			TheWindowManager->winSetFocus( parentMainMenu );
			initialGadgetDelay = 2;
			justEntered = FALSE;
		}
		else
			initialGadgetDelay--;
	}

	if(dontAllowTransitions && TheTransitionHandler->isFinished())
		dontAllowTransitions = FALSE;

	if(showLogo && dontAllowTransitions == FALSE)
	{
//		if(showFrames == SHOW_FRAMES_LIMIT)
//		{
//			TheTransitionHandler->remove("MainMenuSinglePlayerMenu");
			switch (showSide) {
			case SHOW_TRAINING:
				TheTransitionHandler->setGroup("MainMenuFactionTraining");
				break;
			case SHOW_CHINA:
				TheTransitionHandler->setGroup("MainMenuFactionChina");
				break;
			case SHOW_GLA:
				TheTransitionHandler->setGroup("MainMenuFactionGLA");
				break;
			case SHOW_USA:
				TheTransitionHandler->setGroup("MainMenuFactionUS");
				break;
			case SHOW_SKIRMISH:
				TheTransitionHandler->setGroup("MainMenuFactionSkirmish");
				break;
			}
			showLogo = FALSE;
//			showFrames = 0;
//			logoIsShown = TRUE;
//		}
//		else
//			showFrames++;
	}

//	if(showFade)
//	{
//		showFade = FALSE;
//		TheTransitionHandler->reverse("FadeWholeScreen");
//	}
////
//	if (notShown)
//	{
//		if(initialGadgetDelay == 1)
//		{
//			dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
//			TheTransitionHandler->setGroup("MainMenuFade", TRUE);
//			TheTransitionHandler->setGroup("MainMenuDefaultMenu");
//			TheMouse->setVisibility(TRUE);
//			initialGadgetDelay = 2;
//			notShown = FALSE;
//		}
//		else
//			initialGadgetDelay--;
//	}

	if (raiseMessageBoxes)
	{
		RaiseGSMessageBox();
		raiseMessageBoxes = FALSE;
	}

	HTTPThinkWrapper();
	GameSpyUpdateOverlays();
//	if(localAnimateWindowManager)
//		localAnimateWindowManager->update();
//	if(localAnimateWindowManager && pendingDropDown != DROPDOWN_NONE && localAnimateWindowManager->isFinished())
//	{
//		localAnimateWindowManager->reset();
//		if(dropDown != DROPDOWN_NONE)
//			dropDownWindows[dropDown]->winHide(TRUE);
//		dropDown = pendingDropDown;
//		dropDownWindows[dropDown]->winHide(FALSE);
//		localAnimateWindowManager->registerGameWindow(dropDownWindows[dropDown],WIN_ANIMATION_SLIDE_TOP_FAST,TRUE,1,1);
//		//buttonPushed = FALSE;
//		pendingDropDown = DROPDOWN_NONE;
//	}
//	else if(localAnimateWindowManager && dropDown == DROPDOWN_NONE && pendingDropDown == DROPDOWN_NONE && localAnimateWindowManager->isReversed() && localAnimateWindowManager->isFinished())
//	{
//		localAnimateWindowManager->reset();
//		for(Int i = 1; i < DROPDOWN_COUNT; ++i)
//			dropDownWindows[i]->winHide(TRUE);
//	}





#if defined(GENERALS_ONLINE)
	if (startLiveObserverGame && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
	{
		startLiveObserverGame = FALSE;
		doLiveObserverGameStart(m_liveObserverStartLobbyId);
	}

#endif

	if (startGame && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
	{
		doGameStart();
	}

	// We'll only be successful if we've requested to
	if(isShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
	{
		shutdownComplete(layout);
	}


	// We'll only be successful if we've requested to
//	if(TheShell->isAnimReversed() && TheShell->isAnimFinished())
//		shutdownComplete( layout );

//	if(winVidManager)
//		winVidManager->update();

}

//-------------------------------------------------------------------------------------------------
/** Main menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType MainMenuInput( GameWindow *window, UnsignedInt msg,
																		WindowMsgData mData1, WindowMsgData mData2 )
{
	if(!notShown)
		return MSG_IGNORED;

	switch( msg )
	{
		// --------------------------------------------------------------------------------------------
		case GWM_MOUSE_POS:
		{
			// TheSuperHackers @tweak 26/02/2026 Show mouse and menu immediately when shellmap is disabled.
			Bool doShow = !TheGlobalData->m_shellMapOn;

			if (!doShow)
			{
				ICoord2D mouse;
				mouse.x = mData1 & 0xFFFF;
				mouse.y = mData1 >> 16;

				static Int mousePosX = mouse.x;
				static Int mousePosY = mouse.y;

				doShow = abs(mouse.x - mousePosX) > 20 || abs(mouse.y - mousePosY) > 20;
			}

			if (doShow)
			{
				initialGadgetDelay = 1;
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->setGroup("MainMenuFade", TRUE);
				TheTransitionHandler->setGroup("MainMenuDefaultMenu");
				TheMouse->setVisibility(TRUE);
				notShown = FALSE;
				return MSG_HANDLED;
			}

			break;
		}

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			initialGadgetDelay = 1;
			dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
			TheTransitionHandler->setGroup("MainMenuFade", TRUE);
			TheTransitionHandler->setGroup("MainMenuDefaultMenu");
			TheMouse->setVisibility(TRUE);
			notShown = FALSE;
			return MSG_HANDLED;
		}
	}

	return MSG_IGNORED;
}

void PrintOffsetsFromControlBarParent();
#if defined(GENERALS_ONLINE)




// Declared in LiveObserver.h so the replay-menu browser can hand a chosen game back here.
//
// Only records the intent. doLiveObserverGameStart() blocks waiting for the relay's HEADER
// before starting playback, so it must not run while another screen is still up — the
// MainMenuUpdate hook below fires it once the shell animation and transition have settled.
void StartLiveObserverSession(const AsciiString& lobbyId)
{
	if (lobbyId.isEmpty())
		return;

	// Match the environment doLiveObserverGameStart() expects, exactly as the main menu's
	// own Connect path sets it up.
	if (TheWritableGlobalData)
	{
		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_afterIntro = TRUE;
		TheWritableGlobalData->m_playSizzle = FALSE;
		TheWritableGlobalData->m_shellMapOn = FALSE;
	}

	// Multi-instance support like replay mode
	rts::ClientInstance::setMultiInstance(TRUE);
	rts::ClientInstance::skipPrimaryInstance();

	m_liveObserverStartLobbyId = lobbyId;
	startLiveObserverGame = TRUE;

	liveObserverLog("StartLiveObserverSession: queued lobby %s
", lobbyId.str());
}

// Initialize the live observer, connect to relay, wait for HEADER,
// then start live playback via Recorder::playbackFile on the shared file.
static void doLiveObserverGameStart(const AsciiString& lobbyId)
{
	liveObserverInitLog(lobbyId.str());
	liveObserverLog("=== doLiveObserverGameStart (from menu) ===\n");
	liveObserverLog("Lobby: %s
", lobbyId.str());
	liveObserverLog("doLiveObserverGameStart: entry — TheNetwork=%p isInMultiplayerGame=%d\n",
		(void*)TheNetwork, TheGameLogic->isInMultiplayerGame() ? 1 : 0);

	// Clean up any previous LiveObserver that might still be running
	if (TheLiveObserver)
	{
		TheLiveObserver->close();
		delete TheLiveObserver;
		TheLiveObserver = nullptr;
	}

	// Release the Recorder's read handle on the previous session's file too. LiveObserver
	// only closes its own write handle, and the live file is named after the streamer's
	// game — so rejoining a game we already watched targets the same path, which
	// openLiveFile() cannot delete or recreate while this handle is still open.
	if (TheRecorder)
		TheRecorder->endLiveObserverSession();

	TheLiveObserver = createLiveObserver();
	if (!TheLiveObserver)
	{
		liveObserverLog("doLiveObserverGameStart: createLiveObserver() returned NULL!\n");
		return;
	}

	liveObserverLog("doLiveObserverGameStart: connecting to relay...\n");
	TheLiveObserver->connect(lobbyId);

	// Block until the HEADER arrives and is written to _live.rep.
	// readReplayHeader + playbackFile will parse everything we need
	// (slot list, map, game options) from the header bytes.
	liveObserverLog("doLiveObserverGameStart: waiting for HEADER (blocking, up to 20s)...\n");
	Int waited = 0;
	while (!TheLiveObserver->isReady() && waited < 20000)
	{
		Sleep(100);
		waited += 100;
	}
	if (!TheLiveObserver->isReady())
	{
		liveObserverLog("doLiveObserverGameStart: FAILED — HEADER not received (timeout)\n");
		TheLiveObserver->close();
		delete TheLiveObserver;
		TheLiveObserver = nullptr;
		return;
	}

	liveObserverLog("doLiveObserverGameStart: HEADER received! Starting live playback...\n");

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
			liveObserverLog("doLiveObserverGameStart: %s size=%ld magic=%.6s\n", filename.str(), fsize, magic);
		}
		else
		{
			liveObserverLog("doLiveObserverGameStart: %s MISSING at %s\n", filename.str(), filepath.str());
		}
	}

	if (!TheRecorder->startLiveObserverPlayback(filename))
	{
		liveObserverLog("doLiveObserverGameStart: FAILED — playbackFile returned false\n");
		TheLiveObserver->close();
		delete TheLiveObserver;
		TheLiveObserver = nullptr;
		return;
	}

	liveObserverLog("doLiveObserverGameStart: playback started, set startGame=TRUE\n");
	startGame = TRUE;
}
#endif

//-------------------------------------------------------------------------------------------------
/** Main menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType MainMenuSystem( GameWindow *window, UnsignedInt msg,
										 WindowMsgData mData1, WindowMsgData mData2 )
{
	static Bool triedToInitWOLAPI = FALSE;
	static Bool canInitWOLAPI = FALSE;

	switch( msg )
	{

		//---------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{
			ghttpStartup();
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			ghttpCleanup();
			DEBUG_LOG(("Tearing down GameSpy from MainMenuSystem(GWM_DESTROY)"));
			TearDownGameSpy();
			StopAsyncDNSCheck(); // kill off the async DNS check thread in case it is still running
			break;

		}

		// --------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			break;

		}
		//---------------------------------------------------------------------------------------------
		case GBM_MOUSE_ENTERING:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();
			if(controlID == onlineID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_ONLINE_HIGHLIGHTED]);
			}
			else if(controlID == networkID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_NETWORK_HIGHLIGHTED]);
			}
			else if(controlID == optionsID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_OPTIONS_HIGHLIGHTED]);
			}
			else if(controlID == exitID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_EXIT_HIGHLIGHTED]);
			}
			else if(controlID == buttonChallengeID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_TRAINING;
				}

				if(campaignSelected || dontAllowTransitions)
					break;

				TheTransitionHandler->setGroup("MainMenuFactionTraining");
			}
/*			else if(controlID == buttonTRAININGID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_TRAINING;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");

				TheTransitionHandler->setGroup("MainMenuFactionTraining");

				//showSelectiveButtons(SHOW_NONE);
			}
*/			else if(controlID == skirmishID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_SKIRMISH;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");

				TheTransitionHandler->setGroup("MainMenuFactionSkirmish");
				//showSelectiveButtons(SHOW_NONE);
			}

			else if(controlID == buttonUSAID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_USA;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");

				TheTransitionHandler->setGroup("MainMenuFactionUS");
//				showLogo = TRUE;
//				showFrames = 0;
//				showSide = SHOW_USA;

			}
			else if(controlID == buttonGLAID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_GLA;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");
				TheTransitionHandler->setGroup("MainMenuFactionGLA");
//				showLogo = TRUE;
//				showFrames = 0;
//				showSide = SHOW_GLA;
			}
			else if(controlID == buttonChinaID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_CHINA;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");
				TheTransitionHandler->setGroup("MainMenuFactionChina");
//				showLogo = TRUE;
//				showFrames = 0;
//				showSide = SHOW_CHINA;
			}

		break;
		}
		//---------------------------------------------------------------------------------------------
		case GBM_MOUSE_LEAVING:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if(controlID == onlineID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_ONLINE_UNHIGHLIGHTED]);
			}
			else if(controlID == networkID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_NETWORK_UNHIGHLIGHTED]);
			}
			else if(controlID == optionsID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_OPTIONS_UNHIGHLIGHTED]);
			}
			else if(controlID == exitID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_EXIT_UNHIGHLIGHTED]);
			}
			else if(controlID == buttonChallengeID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}

				if(campaignSelected || dontAllowTransitions)
					break;

				// we'll just use the training logo anim for now
				TheTransitionHandler->reverse("MainMenuFactionTraining");
			}
/*			else if(controlID == buttonTRAININGID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionTraining");

				//showSelectiveButtons(SHOW_NONE);
			}
*/			else if(controlID == skirmishID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionSkirmish");
				//showSelectiveButtons(SHOW_NONE);
			}
			else if(controlID == buttonUSAID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionUS");

				//showSelectiveButtons(SHOW_NONE);
			}
			else if(controlID == buttonGLAID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionGLA");
				//showSelectiveButtons(SHOW_NONE);
			}
			else if(controlID == buttonChinaID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionChina");
				//showSelectiveButtons(SHOW_NONE);
			}
		break;
		}
		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{

			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if(buttonPushed)
				break;
#if defined(RTS_DEBUG) || defined RTS_PROFILE_LEGACY
			if( control == buttonCampaign )
			{
				buttonPushed = TRUE;
				TheShell->push("Menus/MapSelectMenu.wnd");
				// As soon as we have a campaign, add it in here!;
			}
#ifdef TEST_COMPRESSION
			else if( control == buttonCompressTest )
			{
				DoCompressTest();
			}
#endif // TEST_COMPRESSION
			else
#endif

			// don't allow mouse click slop that occurs during transitions to unset this flag
			if (TheTransitionHandler->isFinished()
				&& controlID != buttonEasyID && controlID != buttonMediumID && controlID != buttonHardID)
			{
				// this toggle must only be reset if one of these buttons have not been pressed
				// ...the difficulty selection behavior must have a chance to act upon this toggle
				launchChallengeMenu = FALSE;
			}


			if( controlID == buttonSinglePlayerID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				//buttonPushed = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_SINGLE]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuDefaultMenu");
				TheTransitionHandler->reverse("MainMenuDefaultMenuBack");
				TheTransitionHandler->setGroup("MainMenuSinglePlayerMenu");
			}
			else if( controlID == buttonSingleBackID )
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuSinglePlayerMenu");
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBack");
				TheTransitionHandler->setGroup("MainMenuDefaultMenu");
				dontAllowTransitions = TRUE;
			}
			else if( controlID == buttonMultiBackID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuMultiPlayerMenu");
				TheTransitionHandler->reverse("MainMenuMultiPlayerMenuReverse");
				TheTransitionHandler->setGroup("MainMenuDefaultMenu");
#if defined(GENERALS_ONLINE)
				if (buttonLiveObserver)
					buttonLiveObserver->winHide(TRUE);
#endif
			}
			else if( controlID == buttonLoadReplayBackID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuLoadReplayMenu");
				TheTransitionHandler->reverse("MainMenuLoadReplayMenuBack");
				TheTransitionHandler->setGroup("MainMenuDefaultMenu");
			}

			else if( control == buttonCredits )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				TheShell->push("Menus/CreditsMenu.wnd" );
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuDefaultMenu");
			}
			else if( controlID == buttonMultiPlayerID)
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				//buttonPushed = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_MULTIPLAYER]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuDefaultMenu");
				TheTransitionHandler->reverse("MainMenuDefaultMenuBack");
				TheTransitionHandler->setGroup("MainMenuMultiPlayerMenu");
#if defined(GENERALS_ONLINE)
				if (buttonLiveObserver)
					buttonLiveObserver->winHide(FALSE);
#endif
			}
			else if( controlID == buttonLoadReplayID)
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				//buttonPushed = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_LOADREPLAY]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuDefaultMenu");
				TheTransitionHandler->reverse("MainMenuDefaultMenuBack");
				TheTransitionHandler->setGroup("MainMenuLoadReplayMenu");
			}
			else if( controlID == buttonLoadID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
//				SaveLoadLayoutType layoutType = SLLT_LOAD_ONLY;
//        WindowLayout *saveLoadMenuLayout = TheShell->getSaveLoadMenuLayout();
//				DEBUG_ASSERTCRASH( saveLoadMenuLayout, ("Unable to get save load menu layout.") );
//				saveLoadMenuLayout->runInit( &layoutType );
//				saveLoadMenuLayout->hide( FALSE );
//				saveLoadMenuLayout->bringForward();
				buttonPushed = TRUE;
				dropDownWindows[DROPDOWN_LOADREPLAY]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuLoadReplayMenuBackTransition");
				TheShell->push("Menus/SaveLoad.wnd");

			}
			else if( controlID == buttonReplayID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				dropDownWindows[DROPDOWN_LOADREPLAY]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuLoadReplayMenuBackTransition");
				TheShell->push("Menus/ReplayMenu.wnd");
			}
			else if( controlID == skirmishID )
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				buttonPushed = TRUE;
				campaignSelected = TRUE;
				dropDownWindows[DROPDOWN_SINGLE]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuFactionSkirmish");

				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackSkirmish");
#ifdef _CAMPEA_DEMO
				TheCampaignManager->setCampaign( "MD_CAMPEA_DEMO" );
/*
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuUS");
				logoIsShown = FALSE;
				showLogo = FALSE;
				showSide = SHOW_USA;
*/
				prepareCampaignGame(DIFFICULTY_NORMAL);
				break;
#endif
				TheShell->push( "Menus/SkirmishGameOptionsMenu.wnd" );
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_SKIRMISH_SELECTED]);
			}
			else if( controlID == onlineID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				dropDownWindows[DROPDOWN_MULTIPLAYER]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuMultiPlayerMenuTransitionToNext");
#if defined(GENERALS_ONLINE)
				if (buttonLiveObserver)
					buttonLiveObserver->winHide(TRUE);
#endif

				StartPatchCheck();
//				localAnimateWindowManager->reverseAnimateWindow();
				dropDown = DROPDOWN_NONE;

			}
			else if( controlID == networkID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				dropDownWindows[DROPDOWN_MULTIPLAYER]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuMultiPlayerMenuTransitionToNext");
#if defined(GENERALS_ONLINE)
				if (buttonLiveObserver)
					buttonLiveObserver->winHide(TRUE);
#endif
				TheShell->push( "Menus/LanLobbyMenu.wnd" );

				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_NETWORK_SELECTED]);
			}
			else if( controlID == optionsID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				//buttonPushed = TRUE;
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_OPTIONS_SELECTED]);

				// load the options menu
				WindowLayout *optLayout = TheShell->getOptionsLayout(TRUE);
				DEBUG_ASSERTCRASH(optLayout != nullptr, ("unable to get options menu layout"));
				optLayout->runInit();
				optLayout->hide(FALSE);
				optLayout->bringForward();
			}
			else if( controlID == worldBuilderID )
			{
#if defined RTS_DEBUG
				if(_spawnl(_P_NOWAIT,"WorldBuilderD.exe","WorldBuilderD.exe", nullptr) < 0)
					MessageBoxOk(TheGameText->fetch("GUI:WorldBuilder"), TheGameText->fetch("GUI:WorldBuilderLoadFailed"),nullptr);
#else
				if(_spawnl(_P_NOWAIT,"WorldBuilder.exe","WorldBuilder.exe", nullptr) < 0)
					MessageBoxOk(TheGameText->fetch("GUI:WorldBuilder"), TheGameText->fetch("GUI:WorldBuilderLoadFailed"),nullptr);
#endif
			}
			else if( controlID == getUpdateID )
			{
				StartDownloadingPatches();
			}
			else if( controlID == exitID )
			{
				// If we ever want to add a dialog before we exit out of the game, uncomment this line and kill the quitCallback() line below.
//#if defined(RTS_DEBUG)
				if (TheGlobalData->m_windowed)
				{
					quitCallback();
//#else
				}
				else
				{
					QuitMessageBoxYesNo(TheGameText->fetch("GUI:QuitPopupTitle"), TheGameText->fetch("GUI:QuitPopupMessage"),quitCallback,nullptr);
				}
//#endif

			}
			else if(controlID == buttonChallengeID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;

				// set up for the difficulty select into challenge menu
				TheTransitionHandler->setGroup("MainMenuFactionTraining");
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTraining"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackTraining");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuTraining");
				campaignSelected = TRUE;
				showLogo = FALSE;
				showSide = SHOW_TRAINING;
				launchChallengeMenu = TRUE;
			}


// This button has been removed for the mission disk -June 2003
/*			else if(controlID == buttonTRAININGID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				TheCampaignManager->setCampaign( "TRAINING" );
				TheTransitionHandler->setGroup("MainMenuFactionTraining");
				TheTransitionHandler->remove("MainMenuFactionTraining", TRUE);
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTraining"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackTraining");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuTraining");
				campaignSelected = TRUE;
				showLogo = FALSE;
				showSide = SHOW_TRAINING;

//				setupGameStart(TheCampaignManager->getCurrentMap());
			}
*/			else if(controlID == buttonUSAID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				TheCampaignManager->setCampaign( "USA" );
#ifdef _CAMPEA_DEMO
				TheCampaignManager->setCampaign( "MD_USA_1_DEMO" );
#endif
				TheTransitionHandler->setGroup("MainMenuFactionUS");
				TheTransitionHandler->remove("MainMenuFactionUS", TRUE);
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUS"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackUS");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuUS");
				campaignSelected = TRUE;
				logoIsShown = FALSE;
				showLogo = FALSE;
				showSide = SHOW_USA;
//				launchChallengeMenu = FALSE;
//				WindowLayout *layout = nullptr;
//				layout = TheWindowManager->winCreateLayout( "Menus/DifficultySelect.wnd" );
//				layout->runInit();
//				layout->hide( FALSE );
//				layout->bringForward();

//				setupGameStart(TheCampaignManager->getCurrentMap());
			}
			else if(controlID == buttonGLAID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				TheCampaignManager->setCampaign( "GLA" );
#ifdef _CAMPEA_DEMO
				TheCampaignManager->setCampaign( "MD_USA_2_DEMO" );
#endif
				TheTransitionHandler->setGroup("MainMenuFactionGLA");
				TheTransitionHandler->remove("MainMenuFactionGLA", TRUE);
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLA"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackGLA");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuGLA");
				campaignSelected = TRUE;
				logoIsShown = FALSE;
				showLogo = FALSE;
				showSide = SHOW_GLA;
//				launchChallengeMenu = FALSE;
//				WindowLayout *layout = nullptr;
//				layout = TheWindowManager->winCreateLayout( "Menus/DifficultySelect.wnd" );
//				layout->runInit();
//				layout->hide( FALSE );
//				layout->bringForward();

//				setupGameStart(TheCampaignManager->getCurrentMap());
			}
			else if(controlID == buttonChinaID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				TheCampaignManager->setCampaign( "China" );
#ifdef _CAMPEA_DEMO
				TheCampaignManager->setCampaign( "MD_GLA_3_DEMO" );
#endif
				TheTransitionHandler->setGroup("MainMenuFactionChina");
				TheTransitionHandler->remove("MainMenuFactionChina", TRUE);
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChina"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackChina");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuChina");
				campaignSelected = TRUE;
				logoIsShown = FALSE;
				showLogo = FALSE;
				showSide = SHOW_CHINA;
//				launchChallengeMenu = FALSE;
//				WindowLayout *layout = nullptr;
//				layout = TheWindowManager->winCreateLayout( "Menus/DifficultySelect.wnd" );
//				layout->runInit();
//				layout->hide( FALSE );
//				layout->bringForward();

//				setupGameStart(TheCampaignManager->getCurrentMap());
			}
			else if(controlID == buttonEasyID)
			{
				if(dontAllowTransitions)
					break;

				prepareCampaignGame(DIFFICULTY_EASY);
			}
			else if(controlID == buttonMediumID)
			{
				if(dontAllowTransitions)
					break;

				prepareCampaignGame(DIFFICULTY_NORMAL);
			}
			else if(controlID == buttonHardID)
			{
				if(dontAllowTransitions)
					break;

				prepareCampaignGame(DIFFICULTY_HARD);
			}
			else if(controlID == buttonDiffBackID)
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				TheCampaignManager->setCampaign( AsciiString::TheEmptyString );
				diffReverseSide();
				campaignSelected = FALSE;
			}
			#if defined(GENERALS_ONLINE)
			else if(control == buttonLiveObserver)
			{
				// The browser reuses the replay menu's layout, so it gets the real frame, listbox
				// and hover states instead of the placeholder look a code-built dialog had.
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				ReplayMenuEnterLiveGamesMode();
				TheShell->push("Menus/ReplayMenu.wnd");
			}
			#endif


			break;

		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

void diffReverseSide()
{
	switch (showSide) {
	case SHOW_TRAINING:
		TheTransitionHandler->reverse("MainMenuDifficultyMenuTrainingBack");
		TheTransitionHandler->setGroup("MainMenuSinglePlayerTrainingMenuFromDiff");
		break;
	case SHOW_USA:
		TheTransitionHandler->reverse("MainMenuDifficultyMenuUSBack");
		TheTransitionHandler->setGroup("MainMenuSinglePlayerUSAMenuFromDiff");
		break;
	case SHOW_GLA:
		TheTransitionHandler->reverse("MainMenuDifficultyMenuGLABack");
		TheTransitionHandler->setGroup("MainMenuSinglePlayerGLAMenuFromDiff");
		break;
	case SHOW_CHINA:
		TheTransitionHandler->reverse("MainMenuDifficultyMenuChinaBack");
		TheTransitionHandler->setGroup("MainMenuSinglePlayerChinaMenuFromDiff");

		break;
	}
}
