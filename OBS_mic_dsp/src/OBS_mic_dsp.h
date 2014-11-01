#ifndef INCLUDED_OBS_mic_dsp_H
#define INCLUDED_OBS_mic_dsp_H

#include "..\..\OBS\OBSApi\OBSApi.h"

class OBSPlugin
{
public:
    virtual ~OBSPlugin() {}

    virtual void OnStartStream(void) = 0;
    virtual void OnStopStream(void) = 0;
    virtual void OnMicVolumeChanged(float level, bool muted, bool finalValue) = 0;

    static OBSPlugin *g_instance;
    static HINSTANCE g_dllInstance;
};

#endif