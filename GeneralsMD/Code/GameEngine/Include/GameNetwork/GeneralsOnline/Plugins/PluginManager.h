#pragma once

// ------------------------------------------------------------------------------------------------
// GOPluginManager - host-side loader/dispatcher for the generic plugin
// ABI (see PluginABI.h).
//
// Loads any number of plugin DLLs, each opting into one or more hook categories. Engine call
// sites (ProductionUpdate/SpecialPowerModule, InGameUI, CommandXlat/WindowXlat, GameEngine) call
// the Dispatch* functions below; this class fans each call out to every plugin that registered
// for that category, so call sites stay plugin-agnostic.
// ------------------------------------------------------------------------------------------------

#include "GameNetwork/GeneralsOnline/Plugins/PluginABI.h"
#include <vector>
#include <string>
#include <windows.h>

class GOPluginManager
{
public:
	// Loads every *.goplugin.dll found (non-recursive) in directoryPath. Safe to call once at
	// startup; failures to load an individual plugin are logged and skipped, not fatal.
	static void LoadPluginsFromDirectory(const char* directoryPath);

	static bool LoadPlugin(const char* dllPath);
	static void UnloadAll();

	// Calls GO_Plugin_Tick on every plugin that exported it. Call once per frame from
	// GameEngine::update(), unconditionally (not gated behind GameLogic pause state).
	static void Tick();

	// TRUE when the local client is spectating rather than playing: the local player is an
	// observer or dead, which is also true during replay playback
	// (which runs with GAME_REPLAY, whose local player is the observer player). Every Dispatch*
	// function and Tick() are gated on this, so a plugin never receives gameplay data or
	// draw/input callbacks during a live match - that data is an information-advantage vector
	// for a match participant.
	static bool IsLocalPlayerObserver();

	// ---- IGameplayEventHooks dispatch (called from ProductionUpdate/SpecialPowerModule/etc) ----
	static bool HasGameplayEventHooks() { return !s_gameplayEventHooks.empty(); }
	static void DispatchUnitQueued(const GOUnitEvent& ev);
	static void DispatchUnitCancelled(const GOUnitEvent& ev);
	static void DispatchUnitCompleted(const GOUnitEvent& ev);
	static void DispatchUpgradeQueued(const GOUpgradeEvent& ev);
	static void DispatchUpgradeCancelled(const GOUpgradeEvent& ev);
	static void DispatchUpgradeCompleted(const GOUpgradeEvent& ev);
	static void DispatchBuildingDestroyed(const GOBuildingEvent& ev);
	static void DispatchSpecialPowerTriggered(const GOSpecialPowerEvent& ev);

	// ---- IRenderHooks dispatch (called from InGameUI / CommandXlat / WindowXlat) ----
	static bool HasRenderHooks() { return !s_renderHooks.empty(); }
	static void DispatchDrawOverlay();
	static void DispatchRawKeyUp(uint32_t scanCode, uint32_t modifierFlags);
	static void DispatchMouseMove(int32_t x, int32_t y);
	static void DispatchMouseButtonDown(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags);
	static void DispatchMouseButtonUp(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags);

	// Registration callbacks, invoked from GOPluginHostAPI function pointers during plugin
	// Initialize. Public because the free functions backing the host API function-pointer table
	// (in PluginManager.cpp) call them; not intended to be called directly by engine code.
	static void RegisterGameplayEventHooks(const GOGameplayEventCallbacks* cb);
	static void RegisterRenderHooks(const GORenderCallbacks* cb);
	static void Log(const char* msg);

private:
	struct LoadedPlugin
	{
		HMODULE module;
		std::string path;
		GOPluginInfo info;
		std::string name;    // owns GOPluginInfo::name's lifetime on the host side
		std::string version;
		GOPluginShutdownFunc shutdown;
		GOPluginTickFunc tick;
	};

	static std::vector<LoadedPlugin> s_plugins;
	static std::vector<GOGameplayEventCallbacks> s_gameplayEventHooks;
	static std::vector<GORenderCallbacks> s_renderHooks;

	static GOPluginHostAPI BuildHostAPI();

	// The one table every plugin is handed. Process lifetime - see the definition.
	static const GOPluginHostAPI& GetHostAPI();
};
