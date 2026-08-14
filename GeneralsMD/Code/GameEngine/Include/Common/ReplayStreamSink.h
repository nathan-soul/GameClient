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

#pragma once

#include "Common/GameCommon.h"
#include "Common/UnicodeString.h"

class IReplayStreamSink
{
public:
	virtual void onHeaderBytes(const void* data, Int size) = 0;
	virtual void onHeaderComplete() = 0;
	virtual void onHeaderPatch(Int offset, const void* data, Int size) = 0;
	virtual void onBodyBytes(const void* data, Int size) = 0;
	virtual void onBodyFlush() = 0;
	virtual void onRecordingEnded() = 0;

	/// Player chat line that this client displayed, for live-stream capture (MSG_CHAT).
	/// frame = the streamer's game frame at capture (the observer frame-gates on it);
	/// text is the already-formatted "[name] message"; colorArgb is the sender's player
	/// color as displayed. Non-pure with an empty default so other implementors are
	/// unaffected. See plans/relay/live-observer-chat.md.
	virtual void onChat(UnsignedInt frame, const UnicodeString& text, UnsignedInt colorArgb) {}

	/// Frame heartbeat: the recording client's current logic frame, published so a live
	/// observer can know where the game is without waiting for the next record to appear in
	/// the body. Called immediately after onBodyFlush() for the same frame, which is what
	/// lets a receiver treat it as "every record up to this frame has been sent" rather than
	/// as a guess. Non-pure with an empty default so other implementors are unaffected.
	virtual void onTick(UnsignedInt frame) {}
};
