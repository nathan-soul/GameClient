#pragma once

// ------------------------------------------------------------------------------------------------
// NGMP: Generic plugin-hook framework: the host loads any
// number of out-of-tree plugin DLLs, each opting into gameplay-event or render hook categories,
// so features like the observer overlay can live outside the engine.
//
// GeneralsOnline generic plugin ABI.
//
// This header is the contract between the game client (host) and any plugin DLL. It must stay
// pure C (POD structs, function pointers, primitives only — no STL, no engine types) so plugin
// projects can compile against it without any engine headers, and both sides always agree on the
// layout of everything they exchange.
//
// The host loads any number of plugin DLLs; each plugin opts into the hook categories it supports
// at Initialize() time via the host API function table it is handed.
// ------------------------------------------------------------------------------------------------

#include <stdint.h>

#ifdef _WIN32
#define GO_PLUGIN_API extern "C" __declspec(dllexport)
#define GO_PLUGIN_IMPORT extern "C" __declspec(dllimport)
#else
#define GO_PLUGIN_API extern "C"
#define GO_PLUGIN_IMPORT extern "C"
#endif

// Bump when the layout of any struct below changes. GOPluginInfo::abiVersion and
// GOPluginHostAPI::abiVersion are checked against this on load; a mismatch fails the plugin load
// rather than risk silently misreading a function-pointer table with a different layout.
#define GO_PLUGIN_ABI_VERSION 1

// ------------------------------------------------------------------------------------------------
// Hook categories a plugin can use. A plugin advertises which categories it *might* use via
// GOPluginInfo::hookCategories (informational, used for logging/diagnostics only). Actual use of
// GAMEPLAY_EVENTS/RENDER requires calling the matching GOPluginHostAPI::register* function during
// GO_Plugin_Initialize.
//
// Only the categories current plugins consume are exposed, so the ABI surface stays as small as
// the engine has to guarantee. Replay/stream capture, for instance, lives in core engine code
// rather than behind a plugin hook, because it needs recording and native-UI integration across
// engine subsystems that a plugin cannot reach.
// ------------------------------------------------------------------------------------------------
enum EGOPluginHookCategory : uint32_t
{
	GO_HOOK_NONE                 = 0,
	GO_HOOK_GAMEPLAY_EVENTS      = 1 << 0, // unit/upgrade/power/building gameplay events
	GO_HOOK_RENDER               = 1 << 1, // per-frame overlay draw + raw hotkey passthrough
};

// ------------------------------------------------------------------------------------------------
// IGameplayEventHooks — production/power/building events. Payloads are plain data (template names
// as strings, object ids as uint32) rather than engine pointers, since these cross a DLL boundary
// and must not depend on the host's C++ ABI/class layout.
// ------------------------------------------------------------------------------------------------
struct GOUnitEvent
{
	uint32_t playerIndex;
	const char* templateName;     // ThingTemplate name, e.g. "AmericaVehicleCrusader"
	uint32_t producerObjectId;    // producing building's ObjectID (0 if unavailable)
	float percentComplete;        // 0..100, valid for queued/completed; ignored for cancelled
	int32_t productionID;         // engine ProductionID, stable per queue entry until completion/cancel
	float producerPositionX;      // producing building's world position at the moment of this event
	float producerPositionY;
	float producerPositionZ;
};

struct GOUpgradeEvent
{
	uint32_t playerIndex;
	const char* templateName;     // UpgradeTemplate name
	uint32_t producerObjectId;
	float percentComplete;
	float producerPositionX;
	float producerPositionY;
	float producerPositionZ;
};

struct GOBuildingEvent
{
	uint32_t objectId;             // the destroyed/sold building's ObjectID
	uint32_t playerIndex;
	float positionX;               // world position at the moment of destruction
	float positionY;
	float positionZ;
};

struct GOSpecialPowerEvent
{
	uint32_t playerIndex;
	const char* powerTemplateName;
	float locationX;
	float locationY;
	float locationZ;
	float rechargeTimeSeconds;     // static reload duration for this power template (0 if unknown)
};

struct GOGameplayEventCallbacks
{
	void (*onUnitQueued)(const GOUnitEvent* ev);
	void (*onUnitCancelled)(const GOUnitEvent* ev);
	void (*onUnitCompleted)(const GOUnitEvent* ev);
	void (*onUpgradeQueued)(const GOUpgradeEvent* ev);
	void (*onUpgradeCancelled)(const GOUpgradeEvent* ev);
	void (*onUpgradeCompleted)(const GOUpgradeEvent* ev);
	void (*onBuildingDestroyed)(const GOBuildingEvent* ev);
	void (*onSpecialPowerTriggered)(const GOSpecialPowerEvent* ev);
};

// One entry in a player's general-power roster (see getPlayerGeneralPowers). templateName points
// into engine-owned static storage and is valid for the duration of the call.
struct GOGeneralPowerInfo
{
	const char* templateName;    // SpecialPowerTemplate name, e.g. "AmericaSuperweaponParticleCannon"
	uint32_t rechargeFrames;     // full reload duration in logic frames (0 = recharges instantly)
	uint32_t framesUntilReady;   // logic frames until it can be triggered again (0 = ready now)
};

// ------------------------------------------------------------------------------------------------
// IRenderHooks — per-frame overlay draw + raw hotkey passthrough. onDrawOverlay is called once per
// frame from InGameUI's draw path, in 2D screen-space with the HUD's ortho projection already
// active, so a plugin can issue its own 2D draw calls (text/quads via the host's drawing primitives
// exposed through GOPluginHostAPI, see below) without touching 3D state.
// ------------------------------------------------------------------------------------------------
struct GORenderCallbacks
{
	void (*onDrawOverlay)();
	// scanCode is a DirectInput scan code (DIK_*, e.g. DIK_F9 == 0x43), which is what the engine
	// itself keys off - NOT a Windows virtual-key code. Comparing against VK_* silently never
	// matches. modifierFlags: bit0 = CTRL, bit1 = SHIFT, bit2 = ALT.
	void (*onRawKeyUp)(uint32_t scanCode, uint32_t modifierFlags);

	// Mouse passthrough with the same "fires for every event, engine behavior unaffected" discipline
	// as onRawKeyUp — a side-channel notification, never gates or consumes the underlying input.
	// onMouseMove fires far more often than any other hook in this ABI (every raw mouse-move
	// message, easily dozens of times a second while the cursor moves) — keep handlers cheap.
	// Coordinates are screen-space pixels, same space as drawText2D/drawRect2D.
	void (*onMouseMove)(int32_t x, int32_t y);
	// Same side-channel discipline as onMouseMove. buttonIndex: 0 = left, 1 = middle, 2 = right.
	// modifierFlags: bit0 = CTRL, bit1 = SHIFT, bit2 = ALT. The mouse-button cases exist so a
	// plugin can act on clicks on its own drawn UI (e.g. jump the viewport to what was clicked).
	// NGMP: Mouse-button passthrough for clickable plugin UI.
	void (*onMouseButtonDown)(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags);
	void (*onMouseButtonUp)(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags);
};

// ------------------------------------------------------------------------------------------------
// Host API — handed to the plugin at GO_Plugin_Initialize time. Registration functions may be
// called multiple times by the same plugin (e.g. to register both gameplay-event and render
// hooks); each is independent and optional.
//
// LIFETIME: the pointer passed to GO_Plugin_Initialize has process lifetime and stays valid until
// the plugin is unloaded, so a plugin may retain it. Copying the struct by value is still the safer
// habit - it costs nothing and does not depend on the host getting this right.
// ------------------------------------------------------------------------------------------------
struct GOPluginHostAPI
{
	uint32_t abiVersion;

	void (*log)(const char* msg);

	void (*registerGameplayEventHooks)(const GOGameplayEventCallbacks* cb);
	void (*registerRenderHooks)(const GORenderCallbacks* cb);

	// --- Player roster queries. Pure queries, no side effects. playerIndex matches the
	// GOUnitEvent/GOUpgradeEvent/GOBuildingEvent/GOSpecialPowerEvent field of the same name. Return
	// 0 / empty string / false if playerIndex doesn't currently resolve to an active player (no
	// match currently loaded, index out of range, observer slot, etc). ---
	uint32_t (*getPlayerColor)(uint32_t playerIndex);      // colorARGB, same packing as drawText2D/drawRect2D

	// Writes the playerIndex of each active, non-observer match participant into outPlayerIndices
	// and returns how many were written (never more than maxCount). This is the roster query to
	// build UI from: it walks the engine's real player slots, so the neutral/civilian player is not
	// in it, and the values returned are the same playerIndex the gameplay-event structs carry -
	// no second numbering scheme to reconcile. Returns 0 when no match is loaded.
	uint32_t (*getActivePlayers)(uint32_t* outPlayerIndices, uint32_t maxCount);

		// NGMP: Observer-only delivery gate: while this returns
		// false the host delivers no callbacks and never ticks a plugin, so a live match
		// participant cannot use plugin-provided information.
		// TRUE if the local client is observing/spectating the match rather than playing in it (dead
		// counts as observing, matching the engine's own observer-UI gating). A render/tick plugin
		// that shows information about other players (queues, cooldowns, etc.) MUST gate on this
		// before drawing/acting - without it, that information is visible to a live match participant,
		// which is a gameplay-advantage/cheat vector, not just a display bug.
		//
		// The host additionally enforces this: while it returns false, no gameplay-event or render
		// callbacks are delivered and GO_Plugin_Tick is not called, so a plugin cannot observe a
		// live match at all. Replay playback always returns true.
		uint8_t (*isLocalPlayerObserver)();

	// NGMP: General-power roster query: the gameplay events only
	// fire when a power is used, so owned-but-unused powers (e.g. a ready superweapon) are
	// otherwise invisible to a plugin.
	// Enumerates the general powers the player currently owns, walked from the player template's
	// general-power command set and gated on the required science plus a live object carrying the
	// power's module (a superweapon only counts once its building exists). The gameplay-event
	// hooks only fire when a power is used, so this is the only way to show owned-but-unused
	// powers (e.g. a ready superweapon). Cooldown state comes from the module's own clock, so it
	// stays correct across pause/delay/fast-forward. Returns the number written (never more than
	// maxCount), or 0 when no game is running or the player has no general powers.
	uint32_t (*getPlayerGeneralPowers)(uint32_t playerIndex, GOGeneralPowerInfo* outPowers, uint32_t maxCount);

	// --- Icon drawing. Looks up the named template server-side (where the engine's ThingTemplate/
	// UpgradeTemplate/CommandButton data safely lives) and draws its button art; the plugin never
	// needs the Image/ThingTemplate/CommandButton types themselves. Does nothing if
	// the name doesn't resolve. ---

	// Draws a unit or player-upgrade's button icon. Tries a ThingTemplate lookup by templateName
	// first, then an UpgradeTemplate lookup if that fails — matches GOUnitEvent::templateName or
	// GOUpgradeEvent::templateName respectively, so you don't need to track which kind it was.
	void (*drawTemplateIcon2D)(const char* templateName, int32_t x, int32_t y, int32_t width, int32_t height);

	// Draws a special power's button icon for the given player (a power's icon is defined on the
	// CommandButton that exposes it, which is per-faction — hence needing playerIndex, unlike
	// drawTemplateIcon2D). Matches GOSpecialPowerEvent::powerTemplateName.
	void (*drawPowerIcon2D)(uint32_t playerIndex, const char* powerTemplateName, int32_t x, int32_t y, int32_t width, int32_t height);

	// Minimal 2D draw primitives for render-hook plugins, screen-space pixels, top-left origin.
	void (*drawText2D)(int32_t x, int32_t y, const char* utf8Text, uint32_t colorARGB);
	void (*drawRect2D)(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colorARGB, uint8_t filled);

	// Current render target size in pixels, same coordinate space as drawText2D/drawRect2D/
	// drawTemplateIcon2D/drawPowerIcon2D/onMouseMove. Lets a render-hook plugin anchor drawn UI to
	// a screen edge (bottom/right) instead of only a fixed top-left-relative offset. Safe to call
	// from onDrawOverlay every frame.
	void (*getScreenSize)(int32_t* outWidth, int32_t* outHeight);

	// --- Simulation clock. The gameplay-event hooks are edge notifications with no timing
	// information of their own, so a plugin that wants to age anything it recorded (a cooldown, a
	// "just happened" flash) previously had no choice but to use wall-clock time, which drifts
	// against a paused, delayed or fast-forwarded simulation. These give the simulation's own
	// clock instead. Return 0 when no game is running. ---
	uint32_t (*getLogicFrame)();
	uint32_t (*getLogicFramesPerSecond)();

	// --- Live production progress. GOUnitEvent/GOUpgradeEvent carry percentComplete only as
	// of the moment they fire, i.e. essentially always 0 for a queued event, and there is no
	// periodic "progress changed" hook — so anything drawn from the event value alone is frozen.
	// These read the engine's own ProductionEntry state on demand, which is exact and needs no
	// build-time arithmetic on the plugin side. Return 0..100, or -1 if that queue entry is not
	// (or no longer) in the producer's queue. ---
	float (*getUnitProductionProgress)(uint32_t producerObjectId, int32_t productionID);
	float (*getUpgradeProductionProgress)(uint32_t producerObjectId, const char* upgradeTemplateName);

	// --- World-space anchoring. GOUnitEvent/GOUpgradeEvent/GOBuildingEvent carry world
	// positions, but there was no way to turn one into a screen position, so world-anchored UI was
	// unreachable from a plugin. worldToScreen returns 0 (and leaves the outputs untouched) when
	// the point is outside the view frustum. ---
	uint8_t (*worldToScreen)(float worldX, float worldY, float worldZ, int32_t* outX, int32_t* outY);

	// Screen-space bounding box of a live object, derived from its world position and bounding
	// radius — for sizing/placing UI relative to the thing it belongs to, rather than guessing a
	// scale from the screen resolution. Returns 0 if the object no longer exists or is off screen.
	uint8_t (*getObjectScreenBounds)(uint32_t objectId, int32_t* outX, int32_t* outY, int32_t* outWidth, int32_t* outHeight);

	// NGMP: Camera control for plugins: lets a plugin jump the
	// viewport to where a gameplay event happened, e.g. when the user clicks on drawn UI.
	// --- Camera control. Moves the observer's viewport to a world position — for jumping to
	// where a gameplay event happened (GOUnitEvent::producerPosition*, GOBuildingEvent::position*,
	// GOSpecialPowerEvent::location*) when the user clicks on drawn UI for that event. Uses the
	// same camera path as the engine's own observer look-at actions. ---
	void (*teleportViewportTo)(float worldX, float worldY, float worldZ);

	// --- Clock-wedge draw primitives. The engine's own radial "pie" fill, as used by the
	// command bar for build progress and power recharge; a plugin could previously only
	// approximate it with a rectangular bar. percent is 0..100. drawRectClock2D fills the elapsed
	// wedge, drawRemainingRectClock2D fills the remaining one. ---
	void (*drawRectClock2D)(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB);
	void (*drawRemainingRectClock2D)(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB);
};

struct GOPluginInfo
{
	uint32_t abiVersion;
	const char* name;
	const char* version;
	uint32_t hookCategories; // bitmask of EGOPluginHookCategory, informational only
};

// ------------------------------------------------------------------------------------------------
// Fixed export names every plugin DLL must implement, resolved via GetProcAddress so the DLL
// interface is plain C — no link-time coupling between host and plugin.
// ------------------------------------------------------------------------------------------------
typedef void (*GOPluginGetInfoFunc)(GOPluginInfo* outInfo);
typedef bool (*GOPluginInitializeFunc)(const GOPluginHostAPI* hostAPI);
typedef void (*GOPluginShutdownFunc)();
typedef void (*GOPluginTickFunc)(); // optional; may be null

#define GO_PLUGIN_EXPORT_GETINFO_NAME "GO_Plugin_GetInfo"
#define GO_PLUGIN_EXPORT_INITIALIZE_NAME "GO_Plugin_Initialize"
#define GO_PLUGIN_EXPORT_SHUTDOWN_NAME "GO_Plugin_Shutdown"
#define GO_PLUGIN_EXPORT_TICK_NAME "GO_Plugin_Tick"
