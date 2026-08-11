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
// FILE: LobbyObserverMenu.h
// Description: Read-only pre-game lobby view (see plans/lobby-observer.md). Rendered
// by the observer-mode branch of WOLGameSetupMenu, which owns the
// GameSpyGameOptionsMenu.wnd layout this screen reuses; implemented here so the
// read-only mode is self-contained and never touches lobby/mesh/NGMP state.
///////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "GameClient/GameWindow.h"	// WindowMsgHandledType / WindowMsgData

/// Arm observer mode for the next push of GameSpyGameOptionsMenu.wnd (ReplayMenu.cpp).
void SetLobbyObserverMode(const char* lobbyId);

/// Same, for a password-protected lobby: the password is sent with the watch-ticket request
/// when the stream goes live (pre-game watch is gated too, plans/live-watch-password.md).
void SetLobbyObserverModeWithPassword(const char* lobbyId, const char* password);

/// TRUE while the setup menu is running in observer mode.
Bool LobbyObserverModeActive(void);

void LobbyObserverInit(WindowLayout* layout, void* userData);
void LobbyObserverUpdate(WindowLayout* layout, void* userData);
void LobbyObserverShutdown(WindowLayout* layout, void* userData);
WindowMsgHandledType LobbyObserverInput(GameWindow* window, UnsignedInt msg,
	WindowMsgData mData1, WindowMsgData mData2);
