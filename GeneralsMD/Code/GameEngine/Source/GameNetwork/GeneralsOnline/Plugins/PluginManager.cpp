#include "GameNetwork/GeneralsOnline/Plugins/PluginManager.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/ThingFactory.h"
#include "Common/Upgrade.h"
#include "Common/Geometry.h"
#include "GameClient/InGameUI.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameNetwork/NetworkDefs.h" // MAX_SLOTS

#include <cctype>
#include <stdio.h>

std::vector<GOPluginManager::LoadedPlugin> GOPluginManager::s_plugins;
std::vector<GOGameplayEventCallbacks> GOPluginManager::s_gameplayEventHooks;
std::vector<GORenderCallbacks> GOPluginManager::s_renderHooks;

// ------------------------------------------------------------------------------------------------
// Host API implementation for the generic plugin framework: loading,
// the function-pointer table handed to each plugin at Initialize(), and the dispatch fan-out. All
// callbacks are gated on IsLocalPlayerObserver() so plugins only run for observers. These free
// functions are what the function-pointer table handed to each plugin at Initialize() points at.
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

	// ---- Player roster queries. Matches playerIndex against Player::getPlayerIndex(), not list
	// position - those aren't guaranteed to be the same thing. ----

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

	// Resolves the real match participants through the engine's own player-slot name keys
	// ("player0".."player7"), the same way the in-engine observer UI used to. Walking
	// ThePlayerList by position instead would include the neutral/civilian player, which is a
	// perfectly ordinary Player - alive, not an observer - and so silently inflates any
	// "how many players are in this match" count by one.
	uint32_t HostAPI_GetActivePlayers(uint32_t* outPlayerIndices, uint32_t maxCount)
	{
		if (outPlayerIndices == nullptr || maxCount == 0)
			return 0;
		if (ThePlayerList == nullptr || TheNameKeyGenerator == nullptr)
			return 0;

		uint32_t count = 0;
		for (Int slot = 0; slot < MAX_SLOTS && count < maxCount; ++slot)
		{
			AsciiString nameKeyStr;
			nameKeyStr.format("player%d", slot);

			Player* p = ThePlayerList->findPlayerWithNameKey(TheNameKeyGenerator->nameToKey(nameKeyStr));
			if (p == nullptr || !p->isPlayerActive() || p->isPlayerObserver())
				continue;

			outPlayerIndices[count++] = (uint32_t)p->getPlayerIndex();
		}
		return count;
	}

	// Mirrors the same check the engine's own observer-only UI uses (e.g.
	// InGameUI's observer stats/notifications gating) - dead also counts as observing, since a dead
	// player becomes spectator-like. Plugins that display information about other players must gate
	// on this.
	uint8_t HostAPI_IsLocalPlayerObserver()
	{
		return GOPluginManager::IsLocalPlayerObserver() ? 1 : 0;
	}

	// General-power roster. Plugin callbacks carry information about
	// other players (queues, powers, buildings), so none of it may reach a plugin while the local
	// client is a match participant - that is an information-advantage vector, not a display bug.
	// Walks the player template's general-power command set the same way the engine's own observer
	// UI does: a power counts as owned when the player has the required science and at least one
	// live object carries its module.

	struct PowerModuleFind
	{
		const SpecialPowerTemplate* powerTemplate;
		SpecialPowerModuleInterface* module;
	};

	static void FindPowerModule(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		PowerModuleFind* find = (PowerModuleFind*)userData;
		if (find->module != nullptr)
			return;
		find->module = obj->getSpecialPowerModule(find->powerTemplate);
	}

	uint32_t HostAPI_GetPlayerGeneralPowers(uint32_t playerIndex, GOGeneralPowerInfo* outPowers, uint32_t maxCount)
	{
		if (outPowers == nullptr || maxCount == 0)
			return 0;
		if (ThePlayerList == nullptr || TheControlBar == nullptr)
			return 0;

		Player* player = FindPlayerByIndex(playerIndex);
		if (player == nullptr)
			return 0;

		const PlayerTemplate* pt = player->getPlayerTemplate();
		if (pt == nullptr)
			return 0;
		AsciiString cmdSetName = pt->getSpecialPowerShortcutCommandSet();
		if (cmdSetName.isEmpty())
			return 0;

		const CommandSet* cmdSet = TheControlBar->findCommandSet(cmdSetName);
		if (cmdSet == nullptr)
			return 0;

		uint32_t count = 0;
		for (Int i = 0; i < MAX_COMMANDS_PER_SET && count < maxCount; ++i)
		{
			const CommandButton* btn = cmdSet->getCommandButton(i);
			if (btn == nullptr)
				continue;
			const SpecialPowerTemplate* sp = btn->getSpecialPowerTemplate();
			if (sp == nullptr)
				continue;

			ScienceType required = sp->getRequiredScience();
			if (required != SCIENCE_INVALID && !player->hasScience(required))
				continue;

			PowerModuleFind find;
			find.powerTemplate = sp;
			find.module = nullptr;
			player->iterateObjects(FindPowerModule, &find);
			if (find.module == nullptr)
				continue;

			GOGeneralPowerInfo& info = outPowers[count];
			info.templateName = sp->getName().str();
			info.rechargeFrames = sp->getReloadTime();
			const UnsignedInt readyFrame = find.module->getReadyFrame();
			const UnsignedInt now = (TheGameLogic != nullptr) ? (UnsignedInt)TheGameLogic->getFrame() : 0;
			info.framesUntilReady = (readyFrame > now) ? (readyFrame - now) : 0;
			++count;
		}
		return count;
	}

	// ---- Icon drawing. Only valid to call from within a GORenderCallbacks::onDrawOverlay
	// callback - same 2D-context requirement as drawText2D/drawRect2D. ----

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

	// Camera control. Same look-at path the engine's own observer actions
	// use, so a plugin can jump the viewport to where a gameplay event happened when the user
	// clicks on it.

	void HostAPI_TeleportViewportTo(float worldX, float worldY, float worldZ)
	{
		if (TheTacticalView == nullptr)
			return;
		Coord3D target;
		target.x = worldX;
		target.y = worldY;
		target.z = worldZ;
		TheTacticalView->userLookAt(&target);
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
	api.getActivePlayers = HostAPI_GetActivePlayers;
	api.isLocalPlayerObserver = HostAPI_IsLocalPlayerObserver;
	api.getPlayerGeneralPowers = HostAPI_GetPlayerGeneralPowers;
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
	api.teleportViewportTo = HostAPI_TeleportViewportTo;
	api.drawRectClock2D = HostAPI_DrawRectClock2D;
	api.drawRemainingRectClock2D = HostAPI_DrawRemainingRectClock2D;
	return api;
}

//-------------------------------------------------------------------------------------------------
// The single host API table handed to every plugin. Function-local static, so it is built once and
// then lives for the rest of the process: plugins retain this pointer indefinitely, so it must not
// be a temporary. Every entry points at a free function, so there is nothing per-plugin about it
// and one shared instance is correct.
//-------------------------------------------------------------------------------------------------
const GOPluginHostAPI& GOPluginManager::GetHostAPI()
{
	static const GOPluginHostAPI s_hostAPI = BuildHostAPI();
	return s_hostAPI;
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

// Scans for a single flat "key": "value" string field rather than
// pulling the full json.hpp parser into this file for two optional log-line fields. Not a general
// JSON parser - no nested objects/arrays, only \" and \\ are unescaped - but the manifest below is
// purely informational, so anything this doesn't understand just yields an empty value.
static std::string ExtractJsonStringField(const std::string& json, const char* key)
{
	const std::string quotedKey = std::string("\"") + key + "\"";
	size_t pos = json.find(quotedKey);
	if (pos == std::string::npos)
		return std::string();

	pos = json.find(':', pos + quotedKey.size());
	if (pos == std::string::npos)
		return std::string();
	++pos;

	while (pos < json.size() && isspace((unsigned char)json[pos]))
		++pos;
	if (pos >= json.size() || json[pos] != '"')
		return std::string();
	++pos;

	std::string value;
	while (pos < json.size() && json[pos] != '"')
	{
		if (json[pos] == '\\' && pos + 1 < json.size())
			++pos;
		value += json[pos];
		++pos;
	}
	return value;
}

// Optional sidecar manifest next to the DLL (foo.goplugin.dll -> foo.goplugin.json) with
// author/website info for the load log line. Purely informational: GOPluginInfo from the plugin's
// own export stays authoritative for ABI version and hook categories, so a missing or malformed
// manifest is never a reason to refuse a plugin. Returns a fragment to append to the load log
// line, or "".
static std::string ReadManifestSummary(const char* dllPath)
{
	std::string path(dllPath);
	const size_t dot = path.rfind(".dll");
	if (dot == std::string::npos)
		return std::string();
	path.replace(dot, 4, ".json");

	FILE* f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return std::string();

	std::string text;
	char buffer[512];
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
		text.append(buffer, got);
	fclose(f);

	const std::string author = ExtractJsonStringField(text, "plugin_author");
	const std::string website = ExtractJsonStringField(text, "website");
	if (author.empty() && website.empty())
		return std::string();

	std::string summary = " [by " + (author.empty() ? std::string("unknown") : author);
	if (!website.empty())
		summary += ", " + website;
	return summary + "]";
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

	// Must outlive this call: every plugin keeps the pointer it is handed and calls through it for
	// the rest of the process (see the lifetime note in PluginABI.h). Building it into a local and
	// passing its address would hand out a pointer into a stack frame that dies on return - the
	// plugin's first call after load would then jump through whatever reused that stack.
	//
	// A plugin may call registerGameplayEventHooks/registerRenderHooks
	// during Initialize() and then still return false. Snapshot both hook vectors' sizes first and
	// roll back on failure - otherwise the callbacks it just registered would keep pointing into
	// the DLL we are about to FreeLibrary() below, and the next dispatch would call through a freed
	// function pointer.
	const size_t gameplayHooksBefore = s_gameplayEventHooks.size();
	const size_t renderHooksBefore = s_renderHooks.size();
	if (!fnInitialize(&GetHostAPI()))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s Initialize() returned failure", dllPath);
		s_gameplayEventHooks.resize(gameplayHooksBefore);
		s_renderHooks.resize(renderHooksBefore);
		FreeLibrary(hModule);
		return false;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] Loaded %s v%s (%s) hooks=0x%X%s",
		loaded.name.c_str(), loaded.version.c_str(), dllPath, info.hookCategories,
		ReadManifestSummary(dllPath).c_str());

	s_plugins.push_back(loaded);
	return true;
}

// Loads every *.goplugin.dll directly inside pluginDir. Returns how many loaded.
static int LoadPluginsInFolder(const std::string& pluginDir)
{
	const std::string searchPattern = pluginDir + "\\*.goplugin.dll";

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
	if (hFind == INVALID_HANDLE_VALUE)
		return 0;

	int loaded = 0;
	do
	{
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		const std::string fullPath = pluginDir + "\\" + findData.cFileName;
		if (GOPluginManager::LoadPlugin(fullPath.c_str()))
			++loaded;
	}
	while (FindNextFileA(hFind, &findData) != 0);

	FindClose(hFind);
	return loaded;
}

void GOPluginManager::LoadPluginsFromDirectory(const char* directoryPath)
{
	if (directoryPath == nullptr)
		return;

	const std::string root(directoryPath);

	// One folder per plugin, holding its DLL plus an optional .json manifest, so each plugin's
	// files (DLL, manifest, any of its own data) stay together and the plugins directory does not
	// become a pile of loose DLLs.
	int loaded = 0;
	int folders = 0;

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA((root + "\\*").c_str(), &findData);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				continue;
			if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0)
				continue;

			++folders;
			loaded += LoadPluginsInFolder(root + "\\" + findData.cFileName);
		}
		while (FindNextFileA(hFind, &findData) != 0);

		FindClose(hFind);
	}

	// A DLL dropped loose in plugins\ is not picked up. Say so loudly rather than silently doing
	// nothing: a plugin that never runs looks exactly like a plugin that runs and draws nothing,
	// and telling those apart cost a full round of testing once already.
	WIN32_FIND_DATAA strayData;
	HANDLE hStray = FindFirstFileA((root + "\\*.goplugin.dll").c_str(), &strayData);
	if (hStray != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (strayData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			NetworkLog(ELogVerbosity::LOG_RELEASE,
				"[Plugin] IGNORED %s\\%s - plugins live in their own folder. Move it to %s\\<name>\\%s",
				root.c_str(), strayData.cFileName, root.c_str(), strayData.cFileName);
		}
		while (FindNextFileA(hStray, &strayData) != 0);

		FindClose(hStray);
	}

	if (loaded == 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] No plugins loaded (%s, %d folder(s) scanned)",
			root.c_str(), folders);
	}
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

// ------------------------------------------------------------------------------------------------
// Observer gate. One check for the whole framework: plugin callbacks carry information about
// other players (their production queues, powers, buildings), so none of it may reach a plugin
// while the local client is a match participant. The same condition the engine's own
// observer-only UI (observer stats/notifications) is gated on; replay playback runs with
// GAME_REPLAY, whose local player is the observer player, so it passes.
// ------------------------------------------------------------------------------------------------
bool GOPluginManager::IsLocalPlayerObserver()
{
	if (ThePlayerList == nullptr)
		return false;
	Player* localPlayer = ThePlayerList->getLocalPlayer();
	return (localPlayer != nullptr && (localPlayer->isPlayerObserver() || localPlayer->isPlayerDead())) ? true : false;
}

void GOPluginManager::Tick()
{
	if (!IsLocalPlayerObserver())
		return;

	for (LoadedPlugin& plugin : s_plugins)
	{
		if (plugin.tick != nullptr)
			plugin.tick();
	}
}

// ---- IGameplayEventHooks dispatch. All no-ops unless IsLocalPlayerObserver() - a match
// participant never receives plugin callbacks. ----
void GOPluginManager::DispatchUnitQueued(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitQueued != nullptr) cb.onUnitQueued(&ev);
}

void GOPluginManager::DispatchUnitCancelled(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitCancelled != nullptr) cb.onUnitCancelled(&ev);
}

void GOPluginManager::DispatchUnitCompleted(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitCompleted != nullptr) cb.onUnitCompleted(&ev);
}

void GOPluginManager::DispatchUpgradeQueued(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeQueued != nullptr) cb.onUpgradeQueued(&ev);
}

void GOPluginManager::DispatchUpgradeCancelled(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeCancelled != nullptr) cb.onUpgradeCancelled(&ev);
}

void GOPluginManager::DispatchUpgradeCompleted(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeCompleted != nullptr) cb.onUpgradeCompleted(&ev);
}

void GOPluginManager::DispatchBuildingDestroyed(const GOBuildingEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onBuildingDestroyed != nullptr) cb.onBuildingDestroyed(&ev);
}

void GOPluginManager::DispatchSpecialPowerTriggered(const GOSpecialPowerEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onSpecialPowerTriggered != nullptr) cb.onSpecialPowerTriggered(&ev);
}

// ---- IRenderHooks dispatch. Same observer gate as the gameplay events. ----

void GOPluginManager::DispatchDrawOverlay()
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onDrawOverlay != nullptr) cb.onDrawOverlay();
}

void GOPluginManager::DispatchRawKeyUp(uint32_t scanCode, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onRawKeyUp != nullptr) cb.onRawKeyUp(scanCode, modifierFlags);
}

void GOPluginManager::DispatchMouseMove(int32_t x, int32_t y)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseMove != nullptr) cb.onMouseMove(x, y);
}

void GOPluginManager::DispatchMouseButtonDown(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseButtonDown != nullptr) cb.onMouseButtonDown(buttonIndex, x, y, modifierFlags);
}

void GOPluginManager::DispatchMouseButtonUp(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseButtonUp != nullptr) cb.onMouseButtonUp(buttonIndex, x, y, modifierFlags);
}
