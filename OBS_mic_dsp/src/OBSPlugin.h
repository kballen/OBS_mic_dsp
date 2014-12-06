#ifndef INCLUDED_OBSPlugin_H
#define INCLUDED_OBSPlugin_H

#include "../../OBS/OBSApi/OBSApi.h"

#define PLUGIN_VERSION_STRING "1.0"

class OBSPlugin
{
public:
    OBSPlugin() = default;
    OBSPlugin(const OBSPlugin &) = delete;
    virtual ~OBSPlugin() {}
    OBSPlugin &operator=(const OBSPlugin &) = delete;

    virtual void OnStartStream(void) {}
    virtual void OnStopStream(void) {}
    virtual void OnMicVolumeChanged(float level, bool muted, bool finalValue) {}

    static OBSPlugin *g_instance;
    static HINSTANCE g_dllInstance;
};

#endif
