#include "GameNetwork/GeneralsOnline/Plugins/PluginManager.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/ThingTemplate.h"
#include "Common/ThingFactory.h"
#include "Common/Upgrade.h"
#include "Common/SpecialPower.h"
#include "Common/Geometry.h"
#include "GameClient/InGameUI.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/ProductionUpdate.h"

std::vector<GOPluginManager::LoadedPlugin> GOPluginManager::s_plugins;
std::vector<GOGameplayEventCallbacks> GOPluginManager::s_gameplayEventHooks;
std::vector<GORenderCallbacks> GOPluginManager::s_renderHooks;

// ------------------------------------------------------------------------------------------------
// Host API implementation. These free functions are what the function-pointer table handed to each
// plugin at Initialize() actually points at.
// ------------------------------------------------------------------------------------------------
namespace
{
	void HostAPI_Log(const char* msg)
	{
		GOPluginManager::Log(msg);
	}

	void HostAPI_RegisterGameplayEventHooks(const GOGameplayEventCallbacks* cb)
	{
		GOPluginManager::RegisterGameplayEventHooks(cb);
	}

	void HostAPI_RegisterRenderHooks(const GORenderCallbacks* cb)
	{
		GOPluginManager::RegisterRenderHooks(cb);
	}

	// Bounded copy that never overruns outBuf, used by the player-roster host API functions below.
	void CopyToBuffer(const AsciiString& src, char* outBuf, int32_t bufSize)
	{
		if (outBuf == nullptr || bufSize <= 0)
			return;
		const char* s = src.str();
		int32_t i = 0;
		for (; i < bufSize - 1 && s != nullptr && s[i] != '\0'; ++i)
			outBuf[i] = s[i];
		outBuf[i] = '\0';
	}

	// ---- Player roster queries. Matches playerIndex against Player::getPlayerIndex(), not list
	// position — those aren't guaranteed to be the same thing. ----

	Player* FindPlayerByIndex(uint32_t playerIndex)
	{
		if (ThePlayerList == nullptr)
			return nullptr;
		Int count = ThePlayerList->getPlayerCount();
		for (Int i = 0; i < count; ++i)
		{
			Player* p = ThePlayerList->getNthPlayer(i);
			if (p != nullptr && (uint32_t)p->getPlayerIndex() == playerIndex)
				return p;
		}
		return nullptr;
	}

	uint32_t HostAPI_GetPlayerColor(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		return (p != nullptr) ? (uint32_t)p->getPlayerColor() : 0;
	}

	void HostAPI_GetPlayerDisplayName(uint32_t playerIndex, char* outBuf, int32_t bufSize)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
		{
			CopyToBuffer(AsciiString(), outBuf, bufSize);
			return;
		}
		AsciiString ascii;
		ascii.translate(p->getPlayerDisplayName());
		CopyToBuffer(ascii, outBuf, bufSize);
	}

	uint8_t HostAPI_IsPlayerActive(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		return (p != nullptr && p->isPlayerActive()) ? 1 : 0;
	}

	// ---- Icon drawing. Only valid to call from within a GORenderCallbacks::onDrawOverlay
	// callback — same 2D-context requirement as drawText2D/drawRect2D. ----

	void HostAPI_DrawTemplateIcon2D(const char* templateName, int32_t x, int32_t y, int32_t width, int32_t height)
	{
		if (templateName == nullptr || TheDisplay == nullptr)
			return;

		const Image* img = nullptr;
		if (TheThingFactory != nullptr)
		{
			const ThingTemplate* tmpl = TheThingFactory->findTemplate(AsciiString(templateName), FALSE);
			if (tmpl != nullptr)
				img = tmpl->getButtonImage();
		}
		if (img == nullptr && TheUpgradeCenter != nullptr)
		{
			const UpgradeTemplate* upgrade = TheUpgradeCenter->findUpgrade(templateName);
			if (upgrade != nullptr)
				img = upgrade->getButtonImage();
		}
		if (img != nullptr)
			TheDisplay->drawImage(img, x, y, x + width, y + height);
	}

	void HostAPI_DrawPowerIcon2D(uint32_t playerIndex, const char* powerTemplateName, int32_t x, int32_t y, int32_t width, int32_t height)
	{
		if (powerTemplateName == nullptr || TheDisplay == nullptr || TheControlBar == nullptr)
			return;

		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return;

		const PlayerTemplate* pt = p->getPlayerTemplate();
		if (pt == nullptr)
			return;

		AsciiString cmdSetName = pt->getSpecialPowerShortcutCommandSet();
		if (cmdSetName.isEmpty())
			return;

		const CommandSet* cmdSet = TheControlBar->findCommandSet(cmdSetName);
		if (cmdSet == nullptr)
			return;

		for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton* btn = cmdSet->getCommandButton(i);
			if (btn == nullptr || btn->getSpecialPowerTemplate() == nullptr)
				continue;
			if (strcmp(btn->getSpecialPowerTemplate()->getName().str(), powerTemplateName) != 0)
				continue;

			const Image* img = btn->getButtonImage();
			if (img != nullptr)
				TheDisplay->drawImage(img, x, y, x + width, y + height);
			return;
		}
	}

	void HostAPI_DrawText2D(int32_t x, int32_t y, const char* utf8Text, uint32_t colorARGB)
	{
		if (TheInGameUI != nullptr)
			TheInGameUI->drawPluginText2D((Int)x, (Int)y, utf8Text, (Color)colorARGB);
	}

	void HostAPI_DrawRect2D(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colorARGB, uint8_t filled)
	{
		if (TheInGameUI != nullptr)
			TheInGameUI->drawPluginRect2D((Int)x, (Int)y, (Int)width, (Int)height, (Color)colorARGB, filled != 0);
	}

	void HostAPI_GetScreenSize(int32_t* outWidth, int32_t* outHeight)
	{
		UnsignedInt w = (TheDisplay != nullptr) ? TheDisplay->getWidth() : 0;
		UnsignedInt h = (TheDisplay != nullptr) ? TheDisplay->getHeight() : 0;
		if (outWidth != nullptr)
			*outWidth = (int32_t)w;
		if (outHeight != nullptr)
			*outHeight = (int32_t)h;
	}

	// ---- Simulation clock. ----

	uint32_t HostAPI_GetLogicFrame()
	{
		return (TheGameLogic != nullptr) ? (uint32_t)TheGameLogic->getFrame() : 0;
	}

	uint32_t HostAPI_GetLogicFramesPerSecond()
	{
		return (uint32_t)LOGICFRAMES_PER_SECOND;
	}

	// ---- Live production progress. Reads the producer's own queue, so the value is whatever the
	// simulation currently believes rather than a plugin-side reconstruction. ----

	ProductionUpdateInterface* FindProducer(uint32_t producerObjectId)
	{
		if (TheGameLogic == nullptr || producerObjectId == 0)
			return nullptr;
		Object* producer = TheGameLogic->findObjectByID((ObjectID)producerObjectId);
		return (producer != nullptr) ? producer->getProductionUpdateInterface() : nullptr;
	}

	float HostAPI_GetUnitProductionProgress(uint32_t producerObjectId, int32_t productionID)
	{
		ProductionUpdateInterface* production = FindProducer(producerObjectId);
		if (production == nullptr)
			return -1.0f;

		for (const ProductionEntry* entry = production->firstProduction(); entry != nullptr; entry = production->nextProduction(entry))
		{
			if (entry->getProductionType() == PRODUCTION_UNIT && (int32_t)entry->getProductionID() == productionID)
				return (float)entry->getPercentComplete();
		}
		return -1.0f;
	}

	float HostAPI_GetUpgradeProductionProgress(uint32_t producerObjectId, const char* upgradeTemplateName)
	{
		if (upgradeTemplateName == nullptr)
			return -1.0f;

		ProductionUpdateInterface* production = FindProducer(producerObjectId);
		if (production == nullptr)
			return -1.0f;

		for (const ProductionEntry* entry = production->firstProduction(); entry != nullptr; entry = production->nextProduction(entry))
		{
			if (entry->getProductionType() != PRODUCTION_UPGRADE)
				continue;
			const UpgradeTemplate* upgrade = entry->getProductionUpgrade();
			if (upgrade != nullptr && strcmp(upgrade->getUpgradeName().str(), upgradeTemplateName) == 0)
				return (float)entry->getPercentComplete();
		}
		return -1.0f;
	}

	// ---- World-space anchoring. ----

	uint8_t HostAPI_WorldToScreen(float worldX, float worldY, float worldZ, int32_t* outX, int32_t* outY)
	{
		if (TheTacticalView == nullptr)
			return 0;

		Coord3D world;
		world.x = worldX;
		world.y = worldY;
		world.z = worldZ;

		ICoord2D screen;
		if (!TheTacticalView->worldToScreen(&world, &screen))
			return 0;

		if (outX != nullptr)
			*outX = (int32_t)screen.x;
		if (outY != nullptr)
			*outY = (int32_t)screen.y;
		return 1;
	}

	uint8_t HostAPI_GetObjectScreenBounds(uint32_t objectId, int32_t* outX, int32_t* outY, int32_t* outWidth, int32_t* outHeight)
	{
		if (TheGameLogic == nullptr || TheTacticalView == nullptr || objectId == 0)
			return 0;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;

		const Coord3D* pos = obj->getPosition();
		if (pos == nullptr)
			return 0;

		ICoord2D centre;
		if (!TheTacticalView->worldToScreen(pos, &centre))
			return 0;

		// Project a point one bounding radius to the side of the object as well; the horizontal
		// distance between the two projections is the object's on-screen size, which already
		// accounts for camera zoom and pitch without exposing any camera state to the plugin.
		const Real radius = obj->getGeometryInfo().getBoundingCircleRadius();
		Coord3D edgeWorld = *pos;
		edgeWorld.x += radius;

		ICoord2D edge;
		Int halfWidth = 0;
		if (TheTacticalView->worldToScreen(&edgeWorld, &edge))
			halfWidth = abs(edge.x - centre.x);
		if (halfWidth <= 0)
			halfWidth = 1;

		if (outX != nullptr)
			*outX = (int32_t)(centre.x - halfWidth);
		if (outY != nullptr)
			*outY = (int32_t)(centre.y - halfWidth);
		if (outWidth != nullptr)
			*outWidth = (int32_t)(halfWidth * 2);
		if (outHeight != nullptr)
			*outHeight = (int32_t)(halfWidth * 2);
		return 1;
	}

	// ---- Clock-wedge draw primitives. Same 2D-context requirement as drawText2D/drawRect2D. ----

	void HostAPI_DrawRectClock2D(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB)
	{
		if (TheDisplay != nullptr)
			TheDisplay->drawRectClock((Int)x, (Int)y, (Int)width, (Int)height, (Int)percent, (UnsignedInt)colorARGB);
	}

	void HostAPI_DrawRemainingRectClock2D(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB)
	{
		if (TheDisplay != nullptr)
			TheDisplay->drawRemainingRectClock((Int)x, (Int)y, (Int)width, (Int)height, (Int)percent, (UnsignedInt)colorARGB);
	}
} // anonymous namespace

// ------------------------------------------------------------------------------------------------

void GOPluginManager::Log(const char* msg)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s", msg);
}

GOPluginHostAPI GOPluginManager::BuildHostAPI()
{
	GOPluginHostAPI api = {};
	api.abiVersion = GO_PLUGIN_ABI_VERSION;
	api.log = HostAPI_Log;
	api.registerGameplayEventHooks = HostAPI_RegisterGameplayEventHooks;
	api.registerRenderHooks = HostAPI_RegisterRenderHooks;
	api.getPlayerColor = HostAPI_GetPlayerColor;
	api.getPlayerDisplayName = HostAPI_GetPlayerDisplayName;
	api.isPlayerActive = HostAPI_IsPlayerActive;
	api.drawTemplateIcon2D = HostAPI_DrawTemplateIcon2D;
	api.drawPowerIcon2D = HostAPI_DrawPowerIcon2D;
	api.drawText2D = HostAPI_DrawText2D;
	api.drawRect2D = HostAPI_DrawRect2D;
	api.getScreenSize = HostAPI_GetScreenSize;
	api.getLogicFrame = HostAPI_GetLogicFrame;
	api.getLogicFramesPerSecond = HostAPI_GetLogicFramesPerSecond;
	api.getUnitProductionProgress = HostAPI_GetUnitProductionProgress;
	api.getUpgradeProductionProgress = HostAPI_GetUpgradeProductionProgress;
	api.worldToScreen = HostAPI_WorldToScreen;
	api.getObjectScreenBounds = HostAPI_GetObjectScreenBounds;
	api.drawRectClock2D = HostAPI_DrawRectClock2D;
	api.drawRemainingRectClock2D = HostAPI_DrawRemainingRectClock2D;
	return api;
}

void GOPluginManager::RegisterGameplayEventHooks(const GOGameplayEventCallbacks* cb)
{
	if (cb != nullptr)
		s_gameplayEventHooks.push_back(*cb);
}

void GOPluginManager::RegisterRenderHooks(const GORenderCallbacks* cb)
{
	if (cb != nullptr)
		s_renderHooks.push_back(*cb);
}

bool GOPluginManager::LoadPlugin(const char* dllPath)
{
	if (dllPath == nullptr)
		return false;

	HMODULE hModule = LoadLibraryA(dllPath);
	if (hModule == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] Failed to load %s (err=%u)", dllPath, GetLastError());
		return false;
	}

	GOPluginGetInfoFunc fnGetInfo = (GOPluginGetInfoFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_GETINFO_NAME);
	GOPluginInitializeFunc fnInitialize = (GOPluginInitializeFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_INITIALIZE_NAME);
	GOPluginShutdownFunc fnShutdown = (GOPluginShutdownFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_SHUTDOWN_NAME);
	GOPluginTickFunc fnTick = (GOPluginTickFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_TICK_NAME); // optional

	if (fnGetInfo == nullptr || fnInitialize == nullptr || fnShutdown == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s is missing required exports (GetInfo/Initialize/Shutdown)", dllPath);
		FreeLibrary(hModule);
		return false;
	}

	GOPluginInfo info = {};
	fnGetInfo(&info);

	if (info.abiVersion != GO_PLUGIN_ABI_VERSION)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s ABI version mismatch (plugin=%u, host=%u)",
			dllPath, info.abiVersion, (uint32_t)GO_PLUGIN_ABI_VERSION);
		FreeLibrary(hModule);
		return false;
	}

	LoadedPlugin loaded;
	loaded.module = hModule;
	loaded.path = dllPath;
	loaded.name = (info.name != nullptr) ? info.name : "(unnamed)";
	loaded.version = (info.version != nullptr) ? info.version : "(unknown)";
	loaded.shutdown = fnShutdown;
	loaded.tick = fnTick;
	loaded.info = info;

	GOPluginHostAPI hostAPI = BuildHostAPI();
	if (!fnInitialize(&hostAPI))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s Initialize() returned failure", dllPath);
		FreeLibrary(hModule);
		return false;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] Loaded %s v%s (%s) hooks=0x%X",
		loaded.name.c_str(), loaded.version.c_str(), dllPath, info.hookCategories);

	s_plugins.push_back(loaded);
	return true;
}

void GOPluginManager::LoadPluginsFromDirectory(const char* directoryPath)
{
	if (directoryPath == nullptr)
		return;

	std::string searchPattern = std::string(directoryPath) + "\\*.goplugin.dll";

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
	if (hFind == INVALID_HANDLE_VALUE)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] No plugins found in %s", directoryPath);
		return;
	}

	do
	{
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		std::string fullPath = std::string(directoryPath) + "\\" + findData.cFileName;
		LoadPlugin(fullPath.c_str());
	}
	while (FindNextFileA(hFind, &findData) != 0);

	FindClose(hFind);
}

void GOPluginManager::UnloadAll()
{
	for (auto it = s_plugins.rbegin(); it != s_plugins.rend(); ++it)
	{
		if (it->shutdown != nullptr)
			it->shutdown();
		if (it->module != nullptr)
			FreeLibrary(it->module);
	}

	s_plugins.clear();
	s_gameplayEventHooks.clear();
	s_renderHooks.clear();
}

void GOPluginManager::Tick()
{
	for (LoadedPlugin& plugin : s_plugins)
	{
		if (plugin.tick != nullptr)
			plugin.tick();
	}
}

// ---- IGameplayEventHooks dispatch ----

void GOPluginManager::DispatchUnitQueued(const GOUnitEvent& ev)
{
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitQueued != nullptr) cb.onUnitQueued(&ev);
}

void GOPluginManager::DispatchUnitCancelled(const GOUnitEvent& ev)
{
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitCancelled != nullptr) cb.onUnitCancelled(&ev);
}

void GOPluginManager::DispatchUnitCompleted(const GOUnitEvent& ev)
{
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitCompleted != nullptr) cb.onUnitCompleted(&ev);
}

void GOPluginManager::DispatchUpgradeQueued(const GOUpgradeEvent& ev)
{
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeQueued != nullptr) cb.onUpgradeQueued(&ev);
}

void GOPluginManager::DispatchUpgradeCancelled(const GOUpgradeEvent& ev)
{
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeCancelled != nullptr) cb.onUpgradeCancelled(&ev);
}

void GOPluginManager::DispatchUpgradeCompleted(const GOUpgradeEvent& ev)
{
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeCompleted != nullptr) cb.onUpgradeCompleted(&ev);
}

void GOPluginManager::DispatchBuildingDestroyed(const GOBuildingEvent& ev)
{
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onBuildingDestroyed != nullptr) cb.onBuildingDestroyed(&ev);
}

void GOPluginManager::DispatchSpecialPowerTriggered(const GOSpecialPowerEvent& ev)
{
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onSpecialPowerTriggered != nullptr) cb.onSpecialPowerTriggered(&ev);
}

// ---- IRenderHooks dispatch ----

void GOPluginManager::DispatchDrawOverlay()
{
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onDrawOverlay != nullptr) cb.onDrawOverlay();
}

void GOPluginManager::DispatchRawKeyUp(uint32_t virtualKeyCode, uint32_t modifierFlags)
{
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onRawKeyUp != nullptr) cb.onRawKeyUp(virtualKeyCode, modifierFlags);
}

void GOPluginManager::DispatchMouseMove(int32_t x, int32_t y)
{
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseMove != nullptr) cb.onMouseMove(x, y);
}

void GOPluginManager::DispatchMouseButtonDown(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags)
{
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseButtonDown != nullptr) cb.onMouseButtonDown(buttonIndex, x, y, modifierFlags);
}

void GOPluginManager::DispatchMouseButtonUp(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags)
{
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseButtonUp != nullptr) cb.onMouseButtonUp(buttonIndex, x, y, modifierFlags);
}
