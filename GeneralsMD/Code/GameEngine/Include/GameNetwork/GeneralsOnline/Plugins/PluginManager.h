#pragma once

// ------------------------------------------------------------------------------------------------
// GOPluginManager — host-side loader/dispatcher for the generic plugin ABI (see PluginABI.h).
//
// Generalizes AnticheatPlugInterface's single-DLL LoadLibrary+GetProcAddress pattern to support any
// number of loaded plugin DLLs, each opting into one or more hook categories. Engine call sites
// (ProductionUpdate/SpecialPowerModule, InGameUI, CommandXlat/WindowXlat, GameEngine) call the
// Dispatch* functions below; this class fans each call out to every plugin that registered for
// that category.
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
	static void DispatchRawKeyUp(uint32_t virtualKeyCode, uint32_t modifierFlags);
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
};
