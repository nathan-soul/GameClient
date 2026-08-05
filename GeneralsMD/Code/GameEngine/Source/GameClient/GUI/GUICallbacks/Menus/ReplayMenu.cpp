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

// FILE: ReplayMenu.cpp /////////////////////////////////////////////////////////////////////
// Author: Chris The masta Huybregts, December 2001
// Description: Replay Menus
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine


#include "Lib/BaseType.h"
#include "Common/FileSystem.h"
#include "Common/GameCommon.h"		// LIVE_DEFAULT_RELAY_URL, LIVE_DELAY_SECONDS_DEFAULT
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"		// m_liveStreamRelayUrl
#include "Common/LiveObserver.h"	// liveRelayBeginFetch / PollFetch / FetchInFlight
#include "Common/Recorder.h"
#include "Common/version.h"
#include "GameNetwork/GeneralsOnline/json.hpp"	// parses the relay's /games reply
#include <vector>
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/MessageBox.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Mouse.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowTransitions.h"

typedef UnicodeString ReplayName;
typedef UnicodeString TooltipString;
typedef std::map<ReplayName, TooltipString> ReplayTooltipMap;

static ReplayTooltipMap replayTooltipCache;

// window ids -------------------------------------------------------------------------------------
static NameKeyType parentReplayMenuID = NAMEKEY_INVALID;
static NameKeyType buttonLoadID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType listboxReplayFilesID = NAMEKEY_INVALID;
static NameKeyType buttonDeleteID = NAMEKEY_INVALID;
static NameKeyType buttonCopyID = NAMEKEY_INVALID;

static Bool isShuttingDown = false;

// window pointers --------------------------------------------------------------------------------
static GameWindow *parentReplayMenu = nullptr;
static GameWindow *buttonLoad = nullptr;
static GameWindow *buttonBack = nullptr;
static GameWindow *listboxReplayFiles = nullptr;
static GameWindow *buttonDelete = nullptr;
static GameWindow *buttonCopy = nullptr;
static Int	initialGadgetDelay = 2;
static Bool justEntered = FALSE;

#if defined(GENERALS_ONLINE)
// TheSuperHackers @feature 03/08/2026 Live games browser.
//
// This screen is reused rather than duplicated. A separate .wnd layout would be the tidier
// separation, but the layouts live inside an archive we cannot read to copy one from, and
// building the list out of code-created gadgets meant hand-matching the theme by eye — the
// listbox especially, since no styled listbox exists on the main menu to copy from.
// Borrowing this layout gives the real frame, listbox, scrollbar and hover states for free.
//
// The cost is that the two modes share a control set: four buttons and one list, which
// happens to be exactly what a browser needs. Everything below is guarded by s_liveGamesMode
// so the replay behaviour is untouched when it is off.
static Bool s_liveGamesMode = FALSE;
static std::vector<AsciiString> s_liveGameIds;	///< game id per listbox row
static UnsignedInt s_lastLiveFetchMs = 0;
static GameWindow *s_liveTitleWindow = nullptr;
static UnicodeString s_savedTitleText;
static UnicodeString s_savedLoadText;
static UnicodeString s_savedDeleteText;

enum { LIVE_GAMES_REFRESH_INTERVAL_MS = 5000 };

void ReplayMenuEnterLiveGamesMode(void) { s_liveGamesMode = TRUE; }
Bool ReplayMenuIsLiveGamesMode(void) { return s_liveGamesMode; }

static void liveGamesRequestList(void);
static void liveGamesApplyResponse(Bool success, Int statusCode, const AsciiString& body);

/// Depth-first search for the first static-text window carrying any text.
///
/// The heading is not necessarily a direct child of ParentReplayMenu — the layout nests controls
/// under GadgetParent — which is exactly why the previous, single-level version of this never
/// found it and the screen stayed titled LOAD REPLAY.
static GameWindow* findFirstStaticTextWithText(GameWindow* parent)
{
	if (parent == nullptr)
		return nullptr;

	for (GameWindow* child = parent->winGetChild(); child != nullptr; child = child->winGetNext())
	{
		WinInstanceData* data = child->winGetInstanceData();
		if (data != nullptr &&
			(data->m_style & GWS_STATIC_TEXT) != 0 &&
			!child->winGetText().isEmpty())
		{
			return child;
		}

		GameWindow* nested = findFirstStaticTextWithText(child);
		if (nested != nullptr)
			return nested;
	}

	return nullptr;
}

/// Find the screen's heading.
///
/// The layout lives in an archive we cannot read, so the control's name cannot be confirmed from
/// here — but every other menu in this codebase calls its heading StaticTextTitle
/// (WOLWelcomeMenu, WOLQuickMatchMenu, EstablishConnectionsWindow), so try that first and only
/// fall back to searching the subtree. A name lookup is worth preferring because the fallback can
/// only ever guess: "first static text carrying text" is the heading on this layout by luck, not
/// by rule.
///
/// Logs what it settled on. If the fallback is what fires, that one line names the real control,
/// which is the only way to turn this guess into a lookup without being able to open the .wnd.
static GameWindow* findTitleWindow(GameWindow* parent)
{
	static const char* const TITLE_CONTROL_NAMES[] = {
		"ReplayMenu.wnd:StaticTextTitle",
		"ReplayMenu.wnd:StaticTextHeader",
		nullptr
	};

	for (Int i = 0; TITLE_CONTROL_NAMES[i] != nullptr; ++i)
	{
		GameWindow* win = TheWindowManager->winGetWindowFromId(
			parent, (Int)TheNameKeyGenerator->nameToKey(TITLE_CONTROL_NAMES[i]));
		if (win != nullptr)
		{
			liveObserverLog("ReplayMenu: title control found by name '%s'\n", TITLE_CONTROL_NAMES[i]);
			return win;
		}
	}

	GameWindow* fallback = findFirstStaticTextWithText(parent);
	if (fallback != nullptr)
	{
		AsciiString text;
		text.translate(fallback->winGetText());
		liveObserverLog("ReplayMenu: title control not found by name; fell back to id='%s' text='%s'\n",
			KEYNAME((NameKeyType)fallback->winGetWindowId()).str(), text.str());
	}
	else
	{
		liveObserverLog("ReplayMenu: no title control found at all — heading will not be retitled\n");
	}

	return fallback;
}

/// Turn the relay's WebSocket URL into its HTTP origin: wss://host/ -> https://host
/// Derived rather than configured separately so the two cannot drift apart.
static AsciiString liveGamesHttpBase(void)
{
	AsciiString relay = TheGlobalData ? TheGlobalData->m_liveStreamRelayUrl : AsciiString::TheEmptyString;
	if (relay.isEmpty())
		relay = LIVE_DEFAULT_RELAY_URL;

	AsciiString scheme, remainder;
	const char* str = relay.str();
	const char* sep = strstr(str, "://");
	if (sep)
	{
		AsciiString rawScheme(str, (Int)(sep - str));
		remainder = sep + 3;
		// wss is TLS and ws is not, so they cannot share a mapping.
		scheme = (stricmp(rawScheme.str(), "wss") == 0 || stricmp(rawScheme.str(), "https") == 0)
			? "https" : "http";
	}
	else
	{
		scheme = "http";
		remainder = str;
	}

	const char* slash = strchr(remainder.str(), '/');
	if (slash)
		remainder = AsciiString(remainder.str(), (Int)(slash - remainder.str()));

	AsciiString base;
	base.format("%s://%s", scheme.str(), remainder.str());
	return base;
}

static void liveGamesRequestList(void)
{
	if (liveRelayFetchInFlight())
		return;

	AsciiString uri;
	uri.format("%s/games", liveGamesHttpBase().str());
	s_lastLiveFetchMs = timeGetTime();
	liveRelayBeginFetch(uri);
}

static void liveGamesApplyResponse(Bool success, Int statusCode, const AsciiString& body)
{
	if (listboxReplayFiles == nullptr || !s_liveGamesMode)
		return;

	// Repopulating clears the selection, so remember it and restore by game id afterwards —
	// by id and not row, since a game ending shifts every row beneath it.
	AsciiString previouslySelected;
	{
		Int wasSelected = -1;
		GadgetListBoxGetSelected(listboxReplayFiles, &wasSelected);
		if (wasSelected >= 0 && wasSelected < (Int)s_liveGameIds.size())
			previouslySelected = s_liveGameIds[wasSelected];
	}

	GadgetListBoxReset(listboxReplayFiles);
	s_liveGameIds.clear();

	if (!success || statusCode != 200)
	{
		GadgetListBoxAddEntryText(listboxReplayFiles,
			UnicodeString(L"Could not reach the relay server"),
			GameMakeColor(255, 120, 120, 255), -1);
		return;
	}

	try
	{
		nlohmann::json games = nlohmann::json::parse(body.str());
		if (!games.is_array() || games.empty())
		{
			GadgetListBoxAddEntryText(listboxReplayFiles,
				UnicodeString(L"No live games right now"),
				GameMakeColor(200, 200, 200, 255), -1);
			return;
		}

		for (const auto& game : games)
		{
			AsciiString gameId = game.value("lobbyid", std::string("")).c_str();
			if (gameId.isEmpty())
				continue;

			// A relay row is GO's own lobby shape: "mapname" is already a display name, not
			// the raw path the old field carried, so it needs no leaf/extension stripping.
			// Missing metadata means the streamer registered without a lobby — still perfectly
			// watchable, so fall back rather than dropping the row.
			std::string mapName = game.value("mapname", std::string(""));
			if (mapName.empty())
				mapName = "(unknown map)";

			// members[] mirrors GO exactly, empty slots (userid -1) included. Skip those.
			std::string playerList;
			if (game.contains("members") && game["members"].is_array())
			{
				for (const auto& member : game["members"])
				{
					if (!member.is_object() || member.value("userid", -1) == -1)
						continue;

					std::string name = member.value("displayname", std::string(""));
					if (name.empty())
						continue;

					if (!playerList.empty())
						playerList += ", ";
					playerList += name;
				}
			}
			if (playerList.empty())
				playerList = "?";

			Int viewers = game.value("viewers", 0);
			Int delaySeconds = game.value("delay_seconds", (Int)LIVE_DELAY_SECONDS_DEFAULT);
			Int ageSeconds = game.value("age_seconds", 0);

			const Color rowColor = GameMakeColor(255, 255, 255, 255);
			UnicodeString text;
			AsciiString tmp;

			// Four columns, laid out for the replay list. Reuse them as
			// map / running-for / delay / players. Append column 0 and use the row index it
			// returns for the rest, exactly as PopulateReplayFileListbox does — passing a
			// precomputed row instead is what merged these cells together.
			text.translate(AsciiString(mapName.c_str()));
			const Int row = GadgetListBoxAddEntryText(listboxReplayFiles, text, rowColor, -1, 0);
			if (row < 0)
				continue;

			tmp.format("%dm in", ageSeconds / 60);
			text.translate(tmp);
			GadgetListBoxAddEntryText(listboxReplayFiles, text, rowColor, row, 1);

			tmp.format("%ds delay", delaySeconds);
			text.translate(tmp);
			GadgetListBoxAddEntryText(listboxReplayFiles, text, rowColor, row, 2);

			tmp.format("%s (%d watching)", playerList.c_str(), viewers);
			text.translate(tmp);
			GadgetListBoxAddEntryText(listboxReplayFiles, text, rowColor, row, 3);

			// Index by the row the listbox actually used, so a lookup on selection cannot
			// drift out of step with the rows if one is ever skipped.
			if ((Int)s_liveGameIds.size() <= row)
				s_liveGameIds.resize(row + 1);
			s_liveGameIds[row] = gameId;
		}

		if (!previouslySelected.isEmpty())
		{
			for (Int i = 0; i < (Int)s_liveGameIds.size(); ++i)
			{
				if (s_liveGameIds[i] == previouslySelected)
				{
					GadgetListBoxSetSelected(listboxReplayFiles, i);
					break;
				}
			}
		}
	}
	catch (...)
	{
		// Malformed JSON degrades to an explanatory row; it must never take the menu down.
		GadgetListBoxReset(listboxReplayFiles);
		s_liveGameIds.clear();
		GadgetListBoxAddEntryText(listboxReplayFiles,
			UnicodeString(L"Unexpected reply from relay server"),
			GameMakeColor(255, 120, 120, 255), -1);
	}
}

/// Connect to the selected game. Returns TRUE if a connection was started.
static Bool liveGamesConnectSelected(void)
{
	Int selected = -1;
	GadgetListBoxGetSelected(listboxReplayFiles, &selected);
	if (selected < 0 || selected >= (Int)s_liveGameIds.size())
		return FALSE;

	AsciiString base = TheGlobalData ? TheGlobalData->m_liveStreamRelayUrl : AsciiString::TheEmptyString;
	if (base.isEmpty())
		base = LIVE_DEFAULT_RELAY_URL;

	// Strip any path so /watch/<id> hangs off the origin.
	{
		const char* str = base.str();
		const char* sep = strstr(str, "://");
		if (sep)
		{
			const char* slash = strchr(sep + 3, '/');
			if (slash)
				base = AsciiString(str, (Int)(slash - str));
		}
	}

	AsciiString watchUrl;
	watchUrl.format("%s/watch/%s", base.str(), s_liveGameIds[selected].str());

	// Record the intent, then return to the main menu, which performs the connection once it
	// is the active screen again. Connecting from here would run while this layout is being
	// torn down, and the start path expects a settled shell.
	StartLiveObserverSession(watchUrl);
	TheShell->pop();
	return TRUE;
}
#endif


#if defined(RTS_DEBUG)
static GameWindow *buttonAnalyzeReplay = nullptr;
#endif

void deleteReplay();
void copyReplay();
static Bool callCopy = FALSE;
static Bool callDelete = FALSE;
void deleteReplayFlag() { callDelete = TRUE;}
void copyReplayFlag() { callCopy = TRUE;}

UnicodeString GetReplayFilenameFromListbox(GameWindow *listbox, Int index)
{
	UnicodeString fname = GadgetListBoxGetText(listbox, index);

	if (fname == TheGameText->fetch("GUI:LastReplay"))
	{
		fname.translate(TheRecorder->getLastReplayFileName());
	}

	UnicodeString ext;
	ext.translate(TheRecorder->getReplayExtention());
	fname.concat(ext);

	return fname;
}

//-------------------------------------------------------------------------------------------------

static Bool readReplayMapInfo(const AsciiString& filename, RecorderClass::ReplayHeader &header, ReplayGameInfo &info, const MapMetaData *&mapData)
{
	header.forPlayback = FALSE;
	header.filename = filename;

	if (TheRecorder != nullptr && TheRecorder->readReplayHeader(header))
	{
		if (ParseAsciiStringToGameInfo(&info, header.gameOptions))
		{
			if (TheMapCache != nullptr)
				mapData = TheMapCache->findMap(info.getMap());
			else
				mapData = nullptr;

			return true;
		}
	}
	return false;
}

//-------------------------------------------------------------------------------------------------

static void removeReplayExtension(UnicodeString& replayName)
{
	const Int extensionLength = TheRecorder->getReplayExtention().getLength();
	replayName.truncateBy(extensionLength);
}

//-------------------------------------------------------------------------------------------------

static UnicodeString createReplayName(const AsciiString& filename)
{
	AsciiString lastReplayFName = TheRecorder->getLastReplayFileName();
	lastReplayFName.concat(TheRecorder->getReplayExtention());
	UnicodeString replayName;

	if (lastReplayFName.compareNoCase(filename) == 0)
	{
		replayName = TheGameText->fetch("GUI:LastReplay");
	}
	else
	{
		replayName.translate(filename);
		removeReplayExtension(replayName);
	}
	return replayName;
}

//-------------------------------------------------------------------------------------------------

static UnicodeString createMapName(const AsciiString& filename, const ReplayGameInfo& info, const MapMetaData *mapData)
{
	UnicodeString mapName;
	if (!mapData)
	{
		// TheSuperHackers @bugfix helmutbuhler 08/03/2025 Just use the filename.
		// Displaying a long map path string would break the map list gui.
		const char* filename = info.getMap().reverseFind('\\');
		mapName.translate(filename ? filename + 1 : info.getMap());
	}
	else
	{
		mapName = mapData->m_displayName;
	}
	return mapName;
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @feature Stubbjax 21/10/2025 Show extra info tooltip when hovering over a replay.

static void showReplayTooltip(GameWindow* window, WinInstanceData* instData, UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
		return;
	}

	UnicodeString replayFileName = GetReplayFilenameFromListbox(window, row);

	ReplayTooltipMap::const_iterator it = replayTooltipCache.find(replayFileName);
	if (it != replayTooltipCache.end())
		TheMouse->setCursorTooltip(it->second, -1, nullptr, 1.5f);
	else
		TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
}

static UnicodeString buildReplayTooltip(RecorderClass::ReplayHeader header, ReplayGameInfo info)
{
	UnicodeString tooltipStr;

	if (header.endTime < header.startTime)
		header.startTime = header.endTime;

	time_t totalSeconds = header.endTime - header.startTime;
	UnsignedInt hours = totalSeconds / 3600;
	UnsignedInt mins = (totalSeconds % 3600) / 60;
	UnsignedInt secs = totalSeconds % 60;
	Real fps = totalSeconds > 0 ? header.frameCount / totalSeconds : 0;
	tooltipStr.format(L"%02u:%02u:%02u (%g fps)", hours, mins, secs, fps);

	if (header.localPlayerIndex >= 0)
	{
		// MP game
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const GameSlot* slot = info.getConstSlot(i);
			if (slot && slot->isHuman())
			{
				tooltipStr.concat(L"\n");
				tooltipStr.concat(info.getConstSlot(i)->getName());
			}
		}
	}

	return tooltipStr;
}

//-------------------------------------------------------------------------------------------------
/** Populate the listbox with the names of the available replay files */
//-------------------------------------------------------------------------------------------------
void PopulateReplayFileListbox(GameWindow *listbox)
{
	replayTooltipCache.clear();

	if (!TheMapCache)
		return;

	GadgetListBoxReset(listbox);
	const Int listboxLength = GadgetListBoxGetListLength(listbox);
	const Int columns = GadgetListBoxGetNumColumns(listbox);

	// TheSuperHackers @tweak xezon 08/06/2025 Now shows missing maps in red color.
	enum {
		COLOR_SP = 0,
		COLOR_SP_CRC_MISMATCH,
		COLOR_MP,
		COLOR_MP_CRC_MISMATCH,
		COLOR_MISSING_MAP,
		COLOR_MISSING_MAP_CRC_MISMATCH,
		COLOR_MAX
	};
	Color colors[] = {
		GameMakeColor( 255, 255, 255, 255 ),
		GameMakeColor( 128, 128, 128, 255 ),
		GameMakeColor( 255, 255, 255, 255 ),
		GameMakeColor( 128, 128, 128, 255 ),
		GameMakeColor( 243,  24,  24, 255 ),
		GameMakeColor( 128,  32,  32, 255 )
	};
	static_assert(ARRAY_SIZE(colors) == COLOR_MAX, "Mismatch between colors array size and COLOR_MAX");

	AsciiString asciistr;
	AsciiString asciisearch;
	asciisearch = "*";
	asciisearch.concat(TheRecorder->getReplayExtention());

	FilenameList replayFilenames;
	FilenameListIter it;

	TheFileSystem->getFileListInDirectory(TheRecorder->getReplayDir(), asciisearch, replayFilenames, FALSE);

	TheMapCache->updateCache();

	for (it = replayFilenames.begin(); it != replayFilenames.end(); ++it)
	{
		// just want the filename
		asciistr.set((*it).reverseFind('\\') + 1);

		RecorderClass::ReplayHeader header;
		ReplayGameInfo info;
		const MapMetaData *mapData;

		if (readReplayMapInfo(asciistr, header, info, mapData))
		{
			// columns are: name, date, version, map, extra

			// name
			UnicodeString replayNameToShow = createReplayName(asciistr);

			// TheSuperHackers @tweak Caball009 07/02/2026 Display both time and date instead of only time.
			const UnicodeString displayTimeBuffer = getUnicodeTimeBuffer(header.timeVal);
			const UnicodeString displayDateBuffer = getUnicodeDateBuffer(header.timeVal);

			// version (no-op)

			// map
			UnicodeString mapStr = createMapName(asciistr, info, mapData);

			// tooltip
			UnicodeString tooltipStr = buildReplayTooltip(header, info);

			UnicodeString key;
			key.translate(asciistr);
			replayTooltipCache[key] = tooltipStr;

			// pick a color
			Color color;
			Color mapColor;

			const Bool hasMap = mapData != nullptr;

			const Bool isCrcCompatible = RecorderClass::replayMatchesGameVersion(header);

			if (isCrcCompatible)
			{
				if (header.localPlayerIndex >= 0)
				{
					// MP
					color = colors[COLOR_MP];
				}
				else
				{
					// SP
					color = colors[COLOR_SP];
				}

				if (hasMap)
					mapColor = color;
				else
					mapColor = colors[COLOR_MISSING_MAP];
			}
			else
			{
				if (header.localPlayerIndex >= 0)
				{
					// MP
					color = colors[COLOR_MP_CRC_MISMATCH];
				}
				else
				{
					// SP
					color = colors[COLOR_SP_CRC_MISMATCH];
				}

				if (hasMap)
					mapColor = color;
				else
					mapColor = colors[COLOR_MISSING_MAP_CRC_MISMATCH];
			}

			const Int insertionIndex = GadgetListBoxAddEntryText(listbox, replayNameToShow, color, -1, 0);
			DEBUG_ASSERTCRASH(insertionIndex >= 0, ("Expects valid index"));

			// TheSuperHackers @info Caball009 09/02/2026 Original replay menu has 4 columns; the code now supports a future 5-column layout.
			// If there aren't two columns for time and date, concatenate them for a single column.
			if (columns == 4)
			{
				UnicodeString displayDateTimeBuffer;
				displayDateTimeBuffer.format(L"%s %s", displayTimeBuffer.str(), displayDateBuffer.str());

				GadgetListBoxAddEntryText(listbox, displayDateTimeBuffer, color, insertionIndex, 1);
				GadgetListBoxAddEntryText(listbox, header.versionString, color, insertionIndex, 2);
				GadgetListBoxAddEntryText(listbox, mapStr, mapColor, insertionIndex, 3);
			}
			else if (columns == 5)
			{
				GadgetListBoxAddEntryText(listbox, displayTimeBuffer, color, insertionIndex, 1);
				GadgetListBoxAddEntryText(listbox, displayDateBuffer, color, insertionIndex, 2);
				GadgetListBoxAddEntryText(listbox, header.versionString, color, insertionIndex, 3);
				GadgetListBoxAddEntryText(listbox, mapStr, mapColor, insertionIndex, 4);
			}
			else
			{
				DEBUG_CRASH(("Replay menu uses %d columns; expected either 4 or 5", columns));
			}

			// TheSuperHackers @performance Now stops processing when the list is full.
			if (insertionIndex == listboxLength - 1)
				break;
		}
	}
	GadgetListBoxSetSelected(listbox, 0);
}

//-------------------------------------------------------------------------------------------------
/** Initialize the single player menu */
//-------------------------------------------------------------------------------------------------
void ReplayMenuInit( WindowLayout *layout, void *userData )
{
	TheShell->showShellMap(TRUE);

	// get ids for our children controls
	parentReplayMenuID = TheNameKeyGenerator->nameToKey( "ReplayMenu.wnd:ParentReplayMenu" );
	buttonLoadID = TheNameKeyGenerator->nameToKey( "ReplayMenu.wnd:ButtonLoadReplay" );
	buttonBackID = TheNameKeyGenerator->nameToKey( "ReplayMenu.wnd:ButtonBack" );
	listboxReplayFilesID = TheNameKeyGenerator->nameToKey( "ReplayMenu.wnd:ListboxReplayFiles" );
	buttonDeleteID = TheNameKeyGenerator->nameToKey( "ReplayMenu.wnd:ButtonDeleteReplay" );
	buttonCopyID = TheNameKeyGenerator->nameToKey( "ReplayMenu.wnd:ButtonCopyReplay" );

	parentReplayMenu = TheWindowManager->winGetWindowFromId( nullptr, parentReplayMenuID );
	buttonLoad = TheWindowManager->winGetWindowFromId( parentReplayMenu, buttonLoadID );
	buttonBack = TheWindowManager->winGetWindowFromId( parentReplayMenu, buttonBackID );
	listboxReplayFiles = TheWindowManager->winGetWindowFromId( parentReplayMenu, listboxReplayFilesID );
	listboxReplayFiles->winSetTooltipFunc(showReplayTooltip);
	buttonDelete = TheWindowManager->winGetWindowFromId( parentReplayMenu, buttonDeleteID );
	buttonCopy = TheWindowManager->winGetWindowFromId( parentReplayMenu, buttonCopyID );

#if defined(GENERALS_ONLINE)
	if (s_liveGamesMode)
	{
		// Retitle and repurpose the action buttons. Copy is hidden rather than relabelled:
		// a browser has no sensible third action, and a dead button is worse than a gap.
		s_liveTitleWindow = findTitleWindow(parentReplayMenu);
		if (s_liveTitleWindow)
		{
			s_savedTitleText = s_liveTitleWindow->winGetText();
			s_liveTitleWindow->winSetText(UnicodeString(L"LIVE GAMES"));
		}
		if (buttonLoad)
		{
			s_savedLoadText = buttonLoad->winGetText();
			buttonLoad->winSetText(UnicodeString(L"CONNECT"));
		}
		if (buttonDelete)
		{
			s_savedDeleteText = buttonDelete->winGetText();
			buttonDelete->winSetText(UnicodeString(L"REFRESH"));
		}
		if (buttonCopy)
			buttonCopy->winHide(TRUE);

		// The replay tooltip reads the hovered file off disk; these rows are not files.
		listboxReplayFiles->winSetTooltipFunc(nullptr);
	}
#endif

#if ENABLE_GUI_HACKS
	// TheSuperHackers @tweak Caball009 07/02/2026 The version column is wider than the time / date column.
	// Switch them so that there's enough space to show both time and date without a line break.
	ListboxData* list = static_cast<ListboxData*>(listboxReplayFiles->winGetUserData());

	if (list->columns == 4 && list->columnWidth[1] < list->columnWidth[2])
		std::swap(list->columnWidth[1], list->columnWidth[2]);
#endif

	//Load the listbox shiznit
	GadgetListBoxReset(listboxReplayFiles);
#if defined(GENERALS_ONLINE)
	if (s_liveGamesMode)
	{
		GadgetListBoxAddEntryText(listboxReplayFiles,
			UnicodeString(L"Loading live games..."), GameMakeColor(200, 200, 200, 255), -1);
		liveGamesRequestList();
	}
	else
#endif
	PopulateReplayFileListbox(listboxReplayFiles);

#if defined(RTS_DEBUG)
	WinInstanceData instData;
	instData.init();
	BitSet( instData.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK );
	instData.m_textLabelString = "Debug: Analyze Replay";
	instData.setTooltipText(L"Only Used in Debug and Internal!");
	buttonAnalyzeReplay = TheWindowManager->gogoGadgetPushButton( parentReplayMenu,
																									 WIN_STATUS_ENABLED | WIN_STATUS_IMAGE,
																									 4, 4,
																									 180, 26,
																									 &instData, nullptr, TRUE );
#endif

	// show menu
	layout->hide( FALSE );

	// set keyboard focus to main parent
	TheWindowManager->winSetFocus( parentReplayMenu );
	justEntered = TRUE;
	initialGadgetDelay = 2;
	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ReplayMenu.wnd:GadgetParent"));
	if(win)
		win->winHide(TRUE);
	isShuttingDown = FALSE;

}

//-------------------------------------------------------------------------------------------------
/** single player menu shutdown method */
//-------------------------------------------------------------------------------------------------
void ReplayMenuShutdown( WindowLayout *layout, void *userData )
{
#if defined(GENERALS_ONLINE)
	// Leave the screen as we found it. The controls belong to the shared layout, so a
	// retitled heading or a hidden Copy button would otherwise persist into the next visit
	// to the real replay menu.
	if (s_liveGamesMode)
	{
		if (s_liveTitleWindow)
			s_liveTitleWindow->winSetText(s_savedTitleText);
		if (buttonLoad && !s_savedLoadText.isEmpty())
			buttonLoad->winSetText(s_savedLoadText);
		if (buttonDelete && !s_savedDeleteText.isEmpty())
			buttonDelete->winSetText(s_savedDeleteText);
		if (buttonCopy)
			buttonCopy->winHide(FALSE);
		s_liveTitleWindow = nullptr;
		s_liveGameIds.clear();
		s_liveGamesMode = FALSE;
	}
#endif

	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		layout->hide( TRUE );
		TheShell->shutdownComplete( layout );
		return;

	}

	// our shutdown is complete
	TheTransitionHandler->reverse("ReplayMenuFade");
	isShuttingDown = TRUE;
}

//-------------------------------------------------------------------------------------------------
/** single player menu update method */
//-------------------------------------------------------------------------------------------------
void ReplayMenuUpdate( WindowLayout *layout, void *userData )
{
#if defined(GENERALS_ONLINE)
	if (s_liveGamesMode)
	{
		// The relay fetch runs on its own thread, so collect its result here rather than via
		// a callback — nothing off the main thread may touch gadget state.
		AsciiString body;
		Bool fetchOk = FALSE;
		Int statusCode = 0;
		if (liveRelayPollFetch(body, fetchOk, statusCode))
			liveGamesApplyResponse(fetchOk, statusCode, body);

		// Keep the list current while it is open, so games starting and ending appear on
		// their own without the user thinking about refreshing.
		if (!liveRelayFetchInFlight() &&
			timeGetTime() - s_lastLiveFetchMs >= LIVE_GAMES_REFRESH_INTERVAL_MS)
		{
			liveGamesRequestList();
		}
	}
#endif

	if(justEntered)
	{
		if(initialGadgetDelay == 1)
		{
			TheTransitionHandler->remove("MainMenuDefaultMenuLogoFade");
			TheTransitionHandler->setGroup("ReplayMenuFade");
			initialGadgetDelay = 2;
			justEntered = FALSE;
		}
		else
			initialGadgetDelay--;
	}

	if(callCopy)
		copyReplay();
	if(callDelete)
		deleteReplay();
		// We'll only be successful if we've requested to
	if(isShuttingDown && TheShell->isAnimFinished()&& TheTransitionHandler->isFinished())
		TheShell->shutdownComplete( layout );

}

//-------------------------------------------------------------------------------------------------
/** Replay menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType ReplayMenuInput( GameWindow *window, UnsignedInt msg,
																						WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{

					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)buttonBack, buttonBackID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;

}

void reallyLoadReplay()
{
	UnicodeString filename;
	Int selected;
	GadgetListBoxGetSelected( listboxReplayFiles,  &selected );
	if(selected < 0)
	{
		MessageBoxOk(TheGameText->fetch("GUI:NoFileSelected"),TheGameText->fetch("GUI:PleaseSelectAFile"), nullptr);
		return;
	}

	filename = GetReplayFilenameFromListbox(listboxReplayFiles, selected);

	AsciiString asciiFilename;
	asciiFilename.translate(filename);

	TheRecorder->playbackFile(asciiFilename);

	if(parentReplayMenu != nullptr)
	{
		parentReplayMenu->winHide(TRUE);
	}
}

static void loadReplay(UnicodeString filename)
{
	AsciiString asciiFilename;
	asciiFilename.translate(filename);

	RecorderClass::ReplayHeader header;
	ReplayGameInfo info;
	const MapMetaData *mapData;

	if(!readReplayMapInfo(asciiFilename, header, info, mapData))
	{
		// TheSuperHackers @bugfix Prompts a message box when the replay was deleted by the user while the Replay Menu was opened.

		UnicodeString title = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayFileNotFoundTitle", L"REPLAY NOT FOUND");
		UnicodeString body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayFileNotFound", L"This replay cannot be loaded because the file no longer exists on this device.");

		MessageBoxOk(title, body, nullptr);
	}
	else if(mapData == nullptr)
	{
		// TheSuperHackers @bugfix Prompts a message box when the map used by the replay was not found.

		UnicodeString title = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayMapNotFoundTitle", L"MAP NOT FOUND");
		UnicodeString body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayMapNotFound", L"This replay cannot be loaded because the map was not found on this device.");

		MessageBoxOk(title, body, nullptr);
	}
	else if(!TheRecorder->replayMatchesGameVersion(header))
	{
		// Pressing OK loads the replay.

		MessageBoxOkCancel(TheGameText->fetch("GUI:OlderReplayVersionTitle"), TheGameText->fetch("GUI:OlderReplayVersion"), reallyLoadReplay, nullptr);
	}
	else
	{
		TheRecorder->playbackFile(asciiFilename);

		if(parentReplayMenu != nullptr)
		{
			parentReplayMenu->winHide(TRUE);
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** single player menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType ReplayMenuSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{


			break;

		}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{

			break;

		}

		// --------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{

			// if we're given the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			return MSG_HANDLED;

		}
		//---------------------------------------------------------------------------------------------
		case GLM_DOUBLE_CLICKED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxReplayFilesID )
				{
					int rowSelected = mData2;

					if (rowSelected >= 0)
					{
#if defined(GENERALS_ONLINE)
						if (s_liveGamesMode)
						{
							liveGamesConnectSelected();
							break;
						}
#endif
						UnicodeString filename = GetReplayFilenameFromListbox(listboxReplayFiles, rowSelected);
						loadReplay(filename);
					}
				}
				break;
			}
		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			UnicodeString filename;
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

#if defined(RTS_DEBUG)
			if( controlID == buttonAnalyzeReplay->winGetWindowId() )
			{
				if(listboxReplayFiles)
				{
					Int selected;
					GadgetListBoxGetSelected( listboxReplayFiles,  &selected );
					if(selected < 0)
					{
						MessageBoxOk(L"Blah Blah",L"Please select something munkee boy", nullptr);
						break;
					}

					filename = GetReplayFilenameFromListbox(listboxReplayFiles, selected);

					AsciiString asciiFilename;
					asciiFilename.translate(filename);
					if (TheRecorder->analyzeReplay(asciiFilename))
					{
						do
						{
							TheRecorder->update();
						} while (TheRecorder->isPlaybackInProgress());
					}
				}
			}
			else
#endif
			if( controlID == buttonLoadID )
			{
#if defined(GENERALS_ONLINE)
				if (s_liveGamesMode)
				{
					if (!liveGamesConnectSelected())
						MessageBoxOk(UnicodeString(L"No game selected"),
							UnicodeString(L"Please select a live game to watch."), nullptr);
					break;
				}
#endif
				if(listboxReplayFiles)
				{
					Int selected;
					GadgetListBoxGetSelected( listboxReplayFiles,  &selected );
					if(selected < 0)
					{
						MessageBoxOk(TheGameText->fetch("GUI:NoFileSelected"),TheGameText->fetch("GUI:PleaseSelectAFile"), nullptr);
						break;
					}

					filename = GetReplayFilenameFromListbox(listboxReplayFiles, selected);
					loadReplay(filename);
				}
			}
			else if( controlID == buttonBackID )
			{

				// thou art directed to return to thy known solar system immediately!
				TheShell->pop();

			}
			else if( controlID == buttonDeleteID )
			{
#if defined(GENERALS_ONLINE)
				if (s_liveGamesMode)
				{
					liveGamesRequestList();		// this button is REFRESH here
					break;
				}
#endif
				Int selected;
				GadgetListBoxGetSelected( listboxReplayFiles,  &selected );
				if(selected < 0)
				{
					MessageBoxOk(TheGameText->fetch("GUI:NoFileSelected"),TheGameText->fetch("GUI:PleaseSelectAFile"), nullptr);
					break;
				}
				filename = GetReplayFilenameFromListbox(listboxReplayFiles, selected);
				MessageBoxYesNo(TheGameText->fetch("GUI:DeleteFile"), TheGameText->fetch("GUI:AreYouSureDelete"), deleteReplayFlag, nullptr);
			}
			else if( controlID == buttonCopyID )
			{
#if defined(GENERALS_ONLINE)
				if (s_liveGamesMode)
					break;		// hidden in this mode; nothing to copy
#endif
				Int selected;
				GadgetListBoxGetSelected( listboxReplayFiles,  &selected );
				if(selected < 0)
				{
					MessageBoxOk(TheGameText->fetch("GUI:NoFileSelected"),TheGameText->fetch("GUI:PleaseSelectAFile"), nullptr);
					break;
				}
				filename = GetReplayFilenameFromListbox(listboxReplayFiles, selected);
				MessageBoxYesNo(TheGameText->fetch("GUI:CopyReplay"), TheGameText->fetch("GUI:AreYouSureCopy"), copyReplayFlag, nullptr);
			}
			break;
		}

		default:
			return MSG_IGNORED;
	}

	return MSG_HANDLED;
}

void deleteReplay()
{
	callDelete = FALSE;
	Int selected;
	GadgetListBoxGetSelected( listboxReplayFiles,  &selected );
	if(selected < 0)
	{
		MessageBoxOk(TheGameText->fetch("GUI:NoFileSelected"),TheGameText->fetch("GUI:PleaseSelectAFile"), nullptr);
		return;
	}
	AsciiString filename, translate;
	filename = TheRecorder->getReplayDir();
	translate.translate(GetReplayFilenameFromListbox(listboxReplayFiles, selected));
	filename.concat(translate);
	if(DeleteFile(filename.str()) == 0)
	{
		char buffer[1024];
		FormatMessage ( FORMAT_MESSAGE_FROM_SYSTEM, nullptr, GetLastError(), 0, buffer, sizeof(buffer), nullptr);
		UnicodeString errorStr;
		translate.set(buffer);
		errorStr.translate(translate);
		MessageBoxOk(TheGameText->fetch("GUI:Error"),errorStr, nullptr);
	}
	//Load the listbox shiznit
	GadgetListBoxReset(listboxReplayFiles);
	PopulateReplayFileListbox(listboxReplayFiles);
}


void copyReplay()
{
	callCopy = FALSE;
	Int selected;
	GadgetListBoxGetSelected( listboxReplayFiles,  &selected );
	if(selected < 0)
	{
		MessageBoxOk(TheGameText->fetch("GUI:NoFileSelected"),TheGameText->fetch("GUI:PleaseSelectAFile"), nullptr);
		return;
	}
	AsciiString filename, translate;
	filename = TheRecorder->getReplayDir();
	translate.translate(GetReplayFilenameFromListbox(listboxReplayFiles, selected));
	filename.concat(translate);

	char path[1024];
	LPITEMIDLIST pidl;
	SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOPDIRECTORY, &pidl);
	SHGetPathFromIDList(pidl,path);
	AsciiString newFilename;
	newFilename.set(path);
	newFilename.concat("\\");
	newFilename.concat(translate);
	if(CopyFile(filename.str(),newFilename.str(), FALSE) == 0)
	{
		wchar_t buffer[1024];
		FormatMessageW( FORMAT_MESSAGE_FROM_SYSTEM, nullptr, GetLastError(), 0, buffer, ARRAY_SIZE(buffer), nullptr);
		UnicodeString errorStr;
		errorStr.set(buffer);
		errorStr.trim();
		MessageBoxOk(TheGameText->fetch("GUI:Error"),errorStr, nullptr);
	}

}

