#pragma once

#include <functional>
#include <string>
#include <vector>

class CGlobalVars;
class CGameEntitySystem;
class CBaseEntity;
class CEntityInstance;
class CEntityKeyValues;

#define Utils_INTERFACE "IUtilsApi"

typedef std::function<bool(int iSlot, const char* szContent)> CommandCallback;
typedef std::function<void()> StartupCallback;

class IUtilsApi
{
public:
  virtual void PrintToChat(int iSlot, const char* msg, ...) = 0;
  virtual void PrintToChatAll(const char* msg, ...) = 0;
  virtual CGameEntitySystem* GetCGameEntitySystem() = 0;
  virtual CGlobalVars* GetCGlobalVars() = 0;
  virtual const char* GetLanguage() = 0;
  virtual void StartupServer(SourceMM::PluginId id, StartupCallback fn) = 0;
  virtual void RegCommand(SourceMM::PluginId id, const std::vector<std::string>& console, const std::vector<std::string>& chat, const CommandCallback& callback) = 0;
  virtual void DispatchSpawn(CEntityInstance* entity, CEntityKeyValues* keyValues) = 0;
  virtual CBaseEntity* CreateEntityByName(const char* className, CEntityIndex forceEdictIndex) = 0;
  virtual void RemoveEntity(CEntityInstance* entity) = 0;
  virtual void SetStateChanged(CBaseEntity* entity, const char* className, const char* fieldName, int extraOffset = 0) = 0;
  virtual void ClearAllHooks(SourceMM::PluginId id) = 0;
  virtual void PrintToConsole(int iSlot, const char* msg, ...) = 0;
  virtual void PrintToConsoleAll(const char* msg, ...) = 0;
  virtual void ErrorLog(const char* msg, ...) = 0;
  virtual const char* GetVersion() = 0;
};
