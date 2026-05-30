#include "SkyboxChanger.h"
#include "metamod_oslink.h"
#include "include/menus.h"

#include <iserver.h>
#include <convar.h>
#include <cstring>
#include <string>
#include <vector>

SkyboxChanger g_SkyboxChanger;
PLUGIN_EXPOSE(SkyboxChanger, g_SkyboxChanger);

IVEngineServer2* engine = nullptr;
IUtilsApi* g_pUtils = nullptr;
CGlobalVars* gpGlobals = nullptr;

namespace
{
  std::string g_DefaultSkyName;
  std::string g_CurrentSkyName;
  bool g_StartupInitialized = false;

  std::string Trim(std::string value)
  {
    while (!value.empty() && (value.front() == ' ' || value.front() == '"' || value.front() == '\''))
      value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '"' || value.back() == '\''))
      value.pop_back();
    return value;
  }

  std::string GetCurrentMapName()
  {
    if (!gpGlobals)
      return std::string();

    char buffer[64] = {};
    g_SMAPI->Format(buffer, sizeof(buffer), "%s", gpGlobals->mapname);
    return std::string(buffer);
  }

  std::string NormalizeSkyName(const std::string& raw)
  {
    std::string value = Trim(raw);
    for (char& ch : value)
    {
      if (ch == '\\')
        ch = '/';
    }

    size_t slash = value.find_last_of('/');
    if (slash != std::string::npos)
      value = value.substr(slash + 1);

    size_t dot = value.find(".vmat");
    if (dot != std::string::npos)
      value = value.substr(0, dot);

    if (value.size() > 2 && value.compare(value.size() - 2, 2, "_c") == 0)
      value.resize(value.size() - 2);

    return Trim(value);
  }

  void PrintResult(int slot, const char* message)
  {
    if (!g_pUtils)
      return;

    if (slot >= 0)
      g_pUtils->PrintToChat(slot, message);
    else
      META_CONPRINT(message);
  }

  bool ExecuteSkyChange(const std::string& requested, int slot, bool reloadMap)
  {
    const std::string normalized = NormalizeSkyName(requested);
    if (normalized.empty())
    {
      PrintResult(slot, "[SkyboxChangerCpp] Invalid sky name.\n");
      return true;
    }

    char command[512] = {};
    g_SMAPI->Format(command, sizeof(command), "sv_skyname \"%s\"", normalized.c_str());
    engine->ServerCommand(command);
    g_CurrentSkyName = normalized;

    if (reloadMap)
    {
      const std::string map = GetCurrentMapName();
      if (!map.empty())
      {
        char reloadCommand[256] = {};
        g_SMAPI->Format(reloadCommand, sizeof(reloadCommand), "changelevel %s", map.c_str());
        engine->ServerCommand(reloadCommand);
      }
    }

    char result[512] = {};
    g_SMAPI->Format(result, sizeof(result),
      "[SkyboxChangerCpp] Sky set to '%s'%s",
      normalized.c_str(),
      reloadMap ? " and map reload requested.\n" : ". Reload the map if the sky does not refresh immediately.\n");
    PrintResult(slot, result);
    return true;
  }

  bool HandleSkyCommandCommon(int slot, const char* content, bool reloadMap)
  {
    CCommand args;
    args.Tokenize(content);
    if (args.ArgC() < 2)
    {
      PrintResult(slot, "[SkyboxChangerCpp] Usage: sky_set <skyname-or-path>\n");
      return true;
    }

    return ExecuteSkyChange(args.Arg(1), slot, reloadMap);
  }

  void StartupServer()
  {
    gpGlobals = g_pUtils->GetCGlobalVars();

    if (!g_StartupInitialized)
    {
      g_DefaultSkyName.clear();
      g_CurrentSkyName.clear();
      g_StartupInitialized = true;
    }
  }
}

bool SkyboxChanger::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
  PLUGIN_SAVEVARS();

  GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
  GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);

  g_SMAPI->AddListener(this, this);
  ConVar_Register(FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE | FCVAR_CLIENT_CAN_EXECUTE | FCVAR_NOTIFY);
  return true;
}

bool SkyboxChanger::Unload(char* error, size_t maxlen)
{
  if (g_pUtils)
    g_pUtils->ClearAllHooks(g_PLID);

  ConVar_Unregister();
  return true;
}

void SkyboxChanger::AllPluginsLoaded()
{
  int ret = 0;
  g_pUtils = static_cast<IUtilsApi*>(g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr));
  if (ret == META_IFACE_FAILED || !g_pUtils)
  {
    META_CONPRINTF("[%s] Missing Utils system plugin\n", GetLogTag());
    engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
    return;
  }

  g_pUtils->StartupServer(g_PLID, StartupServer);

  g_pUtils->RegCommand(g_PLID, {"sky_set"}, {"!skyset"}, [](int slot, const char* content) {
    return HandleSkyCommandCommon(slot, content, false);
  });

  g_pUtils->RegCommand(g_PLID, {"sky_reload"}, {"!skyreload"}, [](int slot, const char* content) {
    const std::string current = g_CurrentSkyName;
    if (current.empty())
    {
      PrintResult(slot, "[SkyboxChangerCpp] No current sky is stored yet.\n");
      return true;
    }
    return ExecuteSkyChange(current, slot, true);
  });

  g_pUtils->RegCommand(g_PLID, {"sky_reset"}, {"!skyreset"}, [](int slot, const char* content) {
    if (g_DefaultSkyName.empty())
    {
      PrintResult(slot, "[SkyboxChangerCpp] Default sky is unknown for this session.\n");
      return true;
    }
    return ExecuteSkyChange(g_DefaultSkyName, slot, false);
  });

  g_pUtils->RegCommand(g_PLID, {"sky_status"}, {"!skystatus"}, [](int slot, const char* content) {
    char buffer[512] = {};
    g_SMAPI->Format(buffer, sizeof(buffer),
      "[SkyboxChangerCpp] default='%s' current='%s'\n",
      g_DefaultSkyName.c_str(),
      g_CurrentSkyName.c_str());
    PrintResult(slot, buffer);
    return true;
  });
}

const char* SkyboxChanger::GetLicense() { return "GPL"; }
const char* SkyboxChanger::GetVersion() { return "0.1.0"; }
const char* SkyboxChanger::GetDate() { return __DATE__; }
const char* SkyboxChanger::GetLogTag() { return "SkyboxChangerCpp"; }
const char* SkyboxChanger::GetAuthor() { return "OpenAI Codex"; }
const char* SkyboxChanger::GetDescription() { return "Native CS2 skybox changer baseline"; }
const char* SkyboxChanger::GetName() { return "SkyboxChangerCpp"; }
const char* SkyboxChanger::GetURL() { return "https://openai.com"; }
