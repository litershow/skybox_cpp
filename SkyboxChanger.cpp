#include "SkyboxChanger.h"
#include "metamod_oslink.h"
#include "include/menus.h"

#include <iserver.h>
#include <convar.h>
#include <entity2/entitysystem.h>
#include <entity2/entitykeyvalues.h>
#include <ehandle.h>
#include <vector.h>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <string>
#include <unordered_set>
#include <vector>

SkyboxChanger g_SkyboxChanger;
PLUGIN_EXPOSE(SkyboxChanger, g_SkyboxChanger);

IVEngineServer2* engine = nullptr;
IUtilsApi* g_pUtils = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;
void* g_pMaterialSystem = nullptr;

CGameEntitySystem* GameEntitySystem()
{
  return g_pGameEntitySystem;
}

namespace
{
  constexpr std::ptrdiff_t kEnvSkyMaterialOffset = 0x768;
  constexpr std::ptrdiff_t kEnvSkyMaterialLightingOnlyOffset = 0x770;
  constexpr std::ptrdiff_t kEnvSkyStartDisabledOffset = 0x778;
  constexpr std::ptrdiff_t kEnvSkyBrightnessOffset = 0x784;
  constexpr std::ptrdiff_t kEnvSkyEnabledOffset = 0x79C;
  constexpr int kFindOrCreateMaterialVtableIndex = 14;
  constexpr int kCreateLoadingSpawnGroupVtableIndex = 3;
  constexpr int kSpawnEntitiesVtableIndex = 10;
  constexpr const char* kInitAllSystemsFirstLinuxSig = "4C 8B 35 ? ? ? ? 4D 85 F6 75";

  std::string g_DefaultSkyName;
  std::string g_CurrentSkyName;
  std::string g_CurrentSkyMaterial;
  std::string g_LastReplaceError;
  bool g_StartupInitialized = false;

  void* AcquireMaterialSystem()
  {
    if (g_pMaterialSystem)
      return g_pMaterialSystem;

    using RawCreateInterfaceFn = void* (*)(const char*, int*);
    constexpr const char* kInterfaceName = "VMaterialSystem2_001";
    constexpr const char* kLinuxMaterialModule = "libmaterialsystem2.so";

    void* handle = dlopen(kLinuxMaterialModule, RTLD_NOW | RTLD_NOLOAD);
    if (!handle)
      return nullptr;

    void* createInterfaceRaw = dlsym(handle, "CreateInterface");
    if (!createInterfaceRaw)
      return nullptr;

    auto createInterface = reinterpret_cast<RawCreateInterfaceFn>(createInterfaceRaw);
    g_pMaterialSystem = createInterface(kInterfaceName, nullptr);
    return g_pMaterialSystem;
  }

  struct ModuleInfo
  {
    const char* name;
    std::uintptr_t base;
    std::size_t size;
  };

  bool MatchPattern(const std::uint8_t* address, const std::vector<int>& pattern)
  {
    for (std::size_t i = 0; i < pattern.size(); ++i)
    {
      if (pattern[i] != -1 && address[i] != static_cast<std::uint8_t>(pattern[i]))
        return false;
    }

    return true;
  }

  std::vector<int> ParsePattern(const char* signature)
  {
    std::vector<int> bytes;
    std::string token;
    for (const char* p = signature; ; ++p)
    {
      if (*p == ' ' || *p == '\0')
      {
        if (!token.empty())
        {
          if (token == "?")
            bytes.push_back(-1);
          else
            bytes.push_back(static_cast<int>(std::strtoul(token.c_str(), nullptr, 16)));
          token.clear();
        }

        if (*p == '\0')
          break;

        continue;
      }

      token.push_back(*p);
    }

    return bytes;
  }

  bool GetServerModuleInfo(ModuleInfo& outInfo)
  {
    struct SearchState
    {
      ModuleInfo* out;
      bool found = false;
    } state { &outInfo, false };

    dl_iterate_phdr(
      [](struct dl_phdr_info* info, size_t, void* data)
      {
        auto* state = reinterpret_cast<SearchState*>(data);
        if (!info->dlpi_name || !*info->dlpi_name)
          return 0;

        std::string name(info->dlpi_name);
        if (name.find("server") == std::string::npos)
          return 0;

        std::uintptr_t base = static_cast<std::uintptr_t>(info->dlpi_addr);
        std::size_t size = 0;
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
        {
          const auto& phdr = info->dlpi_phdr[i];
          if (phdr.p_type != PT_LOAD)
            continue;

          std::size_t end = static_cast<std::size_t>(phdr.p_vaddr + phdr.p_memsz);
          if (end > size)
            size = end;
        }

        state->out->name = info->dlpi_name;
        state->out->base = base;
        state->out->size = size;
        state->found = true;
        return 1;
      },
      &state);

    return state.found;
  }

  std::uintptr_t FindPatternInServer(const char* signature)
  {
    ModuleInfo serverInfo {};
    if (!GetServerModuleInfo(serverInfo) || !serverInfo.base || !serverInfo.size)
      return 0;

    const std::vector<int> pattern = ParsePattern(signature);
    if (pattern.empty() || serverInfo.size < pattern.size())
      return 0;

    auto* start = reinterpret_cast<const std::uint8_t*>(serverInfo.base);
    const std::size_t last = serverInfo.size - pattern.size();
    for (std::size_t offset = 0; offset <= last; ++offset)
    {
      if (MatchPattern(start + offset, pattern))
        return serverInfo.base + offset;
    }

    return 0;
  }

  void* FindSpawnGroupManager()
  {
    std::uintptr_t match = FindPatternInServer(kInitAllSystemsFirstLinuxSig);
    if (!match)
      return nullptr;

    auto displacement = *reinterpret_cast<std::int32_t*>(match + 3);
    std::uintptr_t firstFactoryPtrAddr = match + 7 + displacement;
    auto factoryList = *reinterpret_cast<std::uintptr_t*>(firstFactoryPtrAddr);

    while (factoryList && factoryList != static_cast<std::uintptr_t>(-1))
    {
      const char* name = *reinterpret_cast<const char**>(factoryList + 0x10);
      if (name && std::strcmp(name, "SpawnGroupManagerGameSystem") == 0)
      {
        return **reinterpret_cast<void***>(factoryList + 3 * sizeof(void*));
      }

      factoryList = *reinterpret_cast<std::uintptr_t*>(factoryList + 0x8);
    }

    return nullptr;
  }

  CUtlVector<CEntityKeyValues*>* MakeSpawnGroupKeyValues(const std::string& materialPath)
  {
    auto* keyValues = new CEntityKeyValues();
    keyValues->SetString("targetname", "skyboxchanger_cpp");
    keyValues->SetString("vscripts", "skyboxchanger_cpp");
    keyValues->SetString("skyname", materialPath.c_str());
    keyValues->SetString("classname", "env_sky");
    keyValues->SetString("origin", "0.0 0.0 0.0");
    keyValues->SetString("angles", "0.0 0.0 0.0");
    keyValues->SetString("scales", "1.0 1.0 1.0");
    keyValues->SetBool("useLocalOffset", false);
    keyValues->SetBool("StartDisabled", false);
    keyValues->SetFloat("brightnessscale", 1.0f);

    Vector4D tint;
    tint.x = 255.0f;
    tint.y = 255.0f;
    tint.z = 255.0f;
    tint.w = 255.0f;
    keyValues->SetVector4D("tint_color", tint);

    auto* list = new CUtlVector<CEntityKeyValues*>();
    list->AddToTail(keyValues);
    return list;
  }

  bool SpawnSkyViaSpawnGroup(const std::string& materialPath)
  {
    void* spawnGroupManager = FindSpawnGroupManager();
    if (!spawnGroupManager)
    {
      g_LastReplaceError = "SpawnGroupManagerGameSystem not found";
      return false;
    }

    auto* keyValuesList = MakeSpawnGroupKeyValues(materialPath);
    if (!keyValuesList)
    {
      g_LastReplaceError = "failed to allocate spawn-group keyvalues";
      return false;
    }

    using CreateLoadingSpawnGroupFn = void* (*)(void*, std::uint32_t, std::uint8_t, std::uint8_t, void*);
    using SpawnEntitiesFn = void (*)(void*);

    auto** managerVtable = *reinterpret_cast<void***>(spawnGroupManager);
    auto createLoadingSpawnGroup = reinterpret_cast<CreateLoadingSpawnGroupFn>(managerVtable[kCreateLoadingSpawnGroupVtableIndex]);
    if (!createLoadingSpawnGroup)
    {
      g_LastReplaceError = "CreateLoadingSpawnGroup vfunc missing";
      return false;
    }

    void* loadingSpawnGroup = createLoadingSpawnGroup(spawnGroupManager, 0, 1, 1, keyValuesList);
    if (!loadingSpawnGroup)
    {
      g_LastReplaceError = "CreateLoadingSpawnGroup returned null";
      return false;
    }

    auto** loadingGroupVtable = *reinterpret_cast<void***>(loadingSpawnGroup);
    auto spawnEntities = reinterpret_cast<SpawnEntitiesFn>(loadingGroupVtable[kSpawnEntitiesVtableIndex]);
    if (!spawnEntities)
    {
      g_LastReplaceError = "SpawnEntities vfunc missing";
      return false;
    }

    spawnEntities(loadingSpawnGroup);
    g_LastReplaceError = "spawn-group path used";
    return true;
  }

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

  std::string NormalizeSkyMaterialPath(const std::string& raw)
  {
    std::string value = Trim(raw);
    for (char& ch : value)
    {
      if (ch == '\\')
        ch = '/';
    }

    if (value.rfind("materials/", 0) != 0)
    {
      if (value.rfind("skybox/", 0) == 0)
        value = "materials/" + value;
      else
        value = "materials/skybox/" + value;
    }

    if (value.find(".vmat") == std::string::npos)
      value += ".vmat";

    return value;
  }

  void* ExtractMaterialHandle(void* materialPtrPtr)
  {
    if (!materialPtrPtr)
      return nullptr;

    return *reinterpret_cast<void**>(materialPtrPtr);
  }

  void* FindMaterialByPath(const std::string& materialPath)
  {
    if (materialPath.empty())
      return nullptr;

    if (!AcquireMaterialSystem())
      return nullptr;

    auto** vtable = *reinterpret_cast<void***>(g_pMaterialSystem);
    void* fnRaw = vtable[kFindOrCreateMaterialVtableIndex];
    if (!fnRaw)
      return nullptr;

    using LinuxThunkFn = void* (*)(void*, void*, const char*);
    using ThisCallLikeFn = void* (*)(void*, void*, const char*);
    auto linuxThunk = reinterpret_cast<LinuxThunkFn>(fnRaw);
    auto thisCallLike = reinterpret_cast<ThisCallLikeFn>(fnRaw);

    std::vector<std::string> candidates;
    candidates.push_back(materialPath);

    if (materialPath.size() > 2 && materialPath.compare(materialPath.size() - 2, 2, "_c") == 0)
      candidates.push_back(materialPath.substr(0, materialPath.size() - 2));
    else
      candidates.push_back(materialPath + "_c");

    if (materialPath.rfind("materials/", 0) == 0)
      candidates.push_back(materialPath.substr(10));

    if (materialPath.find(".vmat") != std::string::npos)
      candidates.push_back(materialPath.substr(0, materialPath.find(".vmat")));
    else
      candidates.push_back(materialPath + ".vmat");

    if (materialPath.rfind("materials/skybox/", 0) == 0)
      candidates.push_back(materialPath.substr(std::strlen("materials/")));

    std::unordered_set<std::string> seen;
    for (const std::string& candidateRaw : candidates)
    {
      std::string candidate = candidateRaw;
      if (candidate.empty() || !seen.insert(candidate).second)
        continue;

      void* outMaterial = nullptr;
      if (void* material = ExtractMaterialHandle(linuxThunk(&outMaterial, nullptr, candidate.c_str())))
        return material;

      outMaterial = nullptr;
      if (void* material = ExtractMaterialHandle(thisCallLike(g_pMaterialSystem, &outMaterial, candidate.c_str())))
        return material;
    }

    return nullptr;
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

  bool ReplaceMapSkyEntity(const std::string& materialPath)
  {
    g_LastReplaceError.clear();

    if (SpawnSkyViaSpawnGroup(materialPath))
      return true;

    if (!g_pUtils)
    {
      g_LastReplaceError = "utils api missing";
      return false;
    }

    CBaseEntity* created = g_pUtils->CreateEntityByName("env_sky", CEntityIndex(-1));
    if (!created)
    {
      g_LastReplaceError = "CreateEntityByName returned null";
      return false;
    }

    CEntityInstance* entity = reinterpret_cast<CEntityInstance*>(created);
    auto* keyValues = new CEntityKeyValues();
    keyValues->SetString("classname", "env_sky");
    keyValues->SetString("skyname", materialPath.c_str());
    keyValues->SetString("origin", "0.0 0.0 0.0");
    keyValues->SetString("angles", "0.0 0.0 0.0");
    keyValues->SetString("scales", "1.0 1.0 1.0");
    keyValues->SetBool("useLocalOffset", false);
    keyValues->SetBool("StartDisabled", false);
    keyValues->SetFloat("brightnessscale", 1.0f);

    Vector4D tint;
    tint.x = 255.0f;
    tint.y = 255.0f;
    tint.z = 255.0f;
    tint.w = 255.0f;
    keyValues->SetVector4D("tint_color", tint);

    g_pUtils->DispatchSpawn(entity, keyValues);

    void* material = FindMaterialByPath(materialPath);
    if (!material)
    {
      g_LastReplaceError = AcquireMaterialSystem()
        ? "FindMaterialByPath returned null"
        : "VMaterialSystem2_001 not found";
      return false;
    }

    auto* rawEntity = reinterpret_cast<std::uint8_t*>(created);
    *reinterpret_cast<void**>(rawEntity + kEnvSkyMaterialOffset) = material;
    *reinterpret_cast<void**>(rawEntity + kEnvSkyMaterialLightingOnlyOffset) = material;
    *reinterpret_cast<bool*>(rawEntity + kEnvSkyStartDisabledOffset) = false;
    *reinterpret_cast<float*>(rawEntity + kEnvSkyBrightnessOffset) = 1.0f;
    *reinterpret_cast<bool*>(rawEntity + kEnvSkyEnabledOffset) = true;

    g_pUtils->SetStateChanged(created, "CEnvSky", "m_hSkyMaterial");
    g_pUtils->SetStateChanged(created, "CEnvSky", "m_hSkyMaterialLightingOnly");
    g_pUtils->SetStateChanged(created, "CEnvSky", "m_bStartDisabled");
    g_pUtils->SetStateChanged(created, "CEnvSky", "m_flBrightnessScale");
    g_pUtils->SetStateChanged(created, "CEnvSky", "m_bEnabled");

    return true;
  }

  bool ExecuteSkyChange(const std::string& requested, int slot, bool reloadMap)
  {
    const std::string normalized = NormalizeSkyName(requested);
    const std::string materialPath = NormalizeSkyMaterialPath(requested);
    if (normalized.empty())
    {
      PrintResult(slot, "[SkyboxChangerCpp] Invalid sky name.\n");
      return true;
    }

    char command[512] = {};
    g_SMAPI->Format(command, sizeof(command), "sv_skyname \"%s\"", normalized.c_str());
    engine->ServerCommand(command);
    g_CurrentSkyName = normalized;
    g_CurrentSkyMaterial = materialPath;

    const bool skyEntityReplaced = ReplaceMapSkyEntity(materialPath);

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
      "[SkyboxChangerCpp] Sky set to '%s' (%s)%s",
      normalized.c_str(),
      skyEntityReplaced ? "env_sky replaced" : "env_sky replace failed",
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
      PrintResult(slot, "[SkyboxChangerCpp] Usage: mm_sky_set <skyname-or-path>\n");
      return true;
    }

    return ExecuteSkyChange(args.Arg(1), slot, reloadMap);
  }

  bool HandleSkyCommandArgs(const CCommand& args, int slot, bool reloadMap)
  {
    if (args.ArgC() < 2)
    {
      PrintResult(slot, "[SkyboxChangerCpp] Usage: mm_sky_set <skyname-or-path>\n");
      return true;
    }

    return ExecuteSkyChange(args.Arg(1), slot, reloadMap);
  }

  void StartupServer()
  {
    g_pGameEntitySystem = nullptr;
    gpGlobals = g_pUtils->GetCGlobalVars();
    g_pGameEntitySystem = g_pUtils->GetCGameEntitySystem();

    if (!g_StartupInitialized)
    {
      g_DefaultSkyName.clear();
      g_CurrentSkyName.clear();
      g_CurrentSkyMaterial.clear();
      g_StartupInitialized = true;
    }
  }
}

CON_COMMAND_F(mm_sky_set, "Set server skybox", FCVAR_GAMEDLL)
{
  HandleSkyCommandArgs(args, -1, false);
}

CON_COMMAND_F(mm_sky_reload, "Reload current map after sky change", FCVAR_GAMEDLL)
{
  const std::string current = g_CurrentSkyName;
  if (current.empty())
  {
    PrintResult(-1, "[SkyboxChangerCpp] No current sky is stored yet.\n");
    return;
  }

  ExecuteSkyChange(current, -1, true);
}

CON_COMMAND_F(mm_sky_reset, "Reset sky to the startup default", FCVAR_GAMEDLL)
{
  if (g_DefaultSkyName.empty())
  {
    PrintResult(-1, "[SkyboxChangerCpp] Default sky is unknown for this session.\n");
    return;
  }

  ExecuteSkyChange(g_DefaultSkyName, -1, false);
}

CON_COMMAND_F(mm_sky_status, "Show current sky status", FCVAR_GAMEDLL)
{
  char buffer[512] = {};
  g_SMAPI->Format(buffer, sizeof(buffer),
    "[SkyboxChangerCpp] default='%s' current='%s' material='%s' last='%s'\n",
    g_DefaultSkyName.c_str(),
    g_CurrentSkyName.c_str(),
    g_CurrentSkyMaterial.c_str(),
    g_LastReplaceError.c_str());
  PrintResult(-1, buffer);
}

bool SkyboxChanger::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
  PLUGIN_SAVEVARS();

  GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
  GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
  GET_V_IFACE_ANY(GetEngineFactory, g_pMaterialSystem, void, "VMaterialSystem2_001");
  AcquireMaterialSystem();

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

  META_CONPRINTF("[SkyboxChangerCpp] Loaded. Commands: mm_sky_set, mm_sky_reload, mm_sky_reset, mm_sky_status\n");
}

const char* SkyboxChanger::GetLicense() { return "GPL"; }
const char* SkyboxChanger::GetVersion() { return "0.1.5"; }
const char* SkyboxChanger::GetDate() { return __DATE__; }
const char* SkyboxChanger::GetLogTag() { return "SkyboxChangerCpp"; }
const char* SkyboxChanger::GetAuthor() { return "OpenAI Codex"; }
const char* SkyboxChanger::GetDescription() { return "Native CS2 skybox changer baseline"; }
const char* SkyboxChanger::GetName() { return "SkyboxChangerCpp"; }
const char* SkyboxChanger::GetURL() { return "https://openai.com"; }
