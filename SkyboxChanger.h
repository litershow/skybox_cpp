#ifndef _INCLUDE_METAMOD_SOURCE_SKYBOX_CHANGER_H_
#define _INCLUDE_METAMOD_SOURCE_SKYBOX_CHANGER_H_

#include <ISmmPlugin.h>

class SkyboxChanger final : public ISmmPlugin, public IMetamodListener
{
public:
  bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
  bool Unload(char* error, size_t maxlen);
  void AllPluginsLoaded();

private:
  const char* GetAuthor();
  const char* GetName();
  const char* GetDescription();
  const char* GetURL();
  const char* GetLicense();
  const char* GetVersion();
  const char* GetDate();
  const char* GetLogTag();
};

#endif
