#include "OBSPlugin.h"
#include "win_voicecapturedmo.h"

OBSPlugin *OBSPlugin::g_instance = nullptr;
HINSTANCE OBSPlugin::g_dllInstance = nullptr;

extern "C" __declspec(dllexport) bool LoadPlugin(void)
{
    if(OBSPlugin::g_instance)
        return false;

    OBSPlugin::g_instance = new WinVoiceCaptureDMOMethod();
    return true;
}

extern "C" __declspec(dllexport) void UnloadPlugin(void)
{
    delete OBSPlugin::g_instance;
    OBSPlugin::g_instance = nullptr;
}

extern "C" __declspec(dllexport) void OnStartStream(void)
{
    if(OBSPlugin::g_instance)
        OBSPlugin::g_instance->OnStartStream();
}

extern "C" __declspec(dllexport) void OnStopStream(void)
{
    if(OBSPlugin::g_instance)
        OBSPlugin::g_instance->OnStopStream();
}

extern "C" __declspec(dllexport) void OnMicVolumeChanged(float level, bool muted, bool finalValue)
{
    if(OBSPlugin::g_instance)
        OBSPlugin::g_instance->OnMicVolumeChanged(level, muted, finalValue);
}

extern "C" __declspec(dllexport) CTSTR GetPluginName(void)
{
    return TEXT("OBS microphone DSP plugin");
}

extern "C" __declspec(dllexport) CTSTR GetPluginDescription(void)
{
    return TEXT("Installs a DSP that automatically improves the quality of the microphone audio.");
}

BOOL CALLBACK DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if(fdwReason == DLL_PROCESS_ATTACH)
    {
#if defined _M_X64 && _MSC_VER == 1800
        //workaround AVX2 bug in VS2013, http://connect.microsoft.com/VisualStudio/feedback/details/811093
        _set_FMA3_enable(0);
#endif
        OBSPlugin::g_dllInstance = hinstDLL;
    }
    return TRUE;
}