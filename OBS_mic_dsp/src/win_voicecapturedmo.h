#ifndef INCLUDED_win_voicecapturedmo_H
#define INCLUDED_win_voicecapturedmo_H

#include "OBSPlugin.h"
#include <dmo.h>

class WinVoiceCaptureDMOMethod : public OBSPlugin
{
public:
    WinVoiceCaptureDMOMethod();
    ~WinVoiceCaptureDMOMethod();

    void OnStartStream(void);
    void OnStopStream(void);
    void OnMicVolumeChanged(float level, bool muted, bool finalValue);

private:
    // Audio filter to discard real mic's audio data
    class MicDiscardFilter : public AudioFilter
    {
    public:
        AudioSegment *Process(AudioSegment *segment);
    };

    // Aux audio input to provide audio data from the DMO
    class VoiceCaptureDMOSource : public AudioSource
    {
    public:
        VoiceCaptureDMOSource();
        ~VoiceCaptureDMOSource();

        bool Initialize(void);

    protected:
        CTSTR GetDeviceName(void) const;
        bool GetNextBuffer(void **buffer, UINT *numFrames, QWORD *timestamp);
        void ReleaseBuffer(void);

    private:
        IMediaObject *_dmo;
        List<int16_t> _audioBuf;
        unsigned int _numSamples;
        bool _skipNextRead;
        float _micBoost;

        // Push-to-talk hotkey support
        static void STDCALL PushToTalkHotkeyCB(DWORD hotkey, UPARAM param, bool keyDown);
        bool _usePushToTalk;
        UINT _pttHotkeyID, _pttHotkey2ID;
        int _pttKeysDown;
        int _pttDelay;
        UINT _pttDelayExpires;

        static const int k_SampleRate = 16000;
        static const int k_SegmentSize = k_SampleRate / 100;

#include "CMediaBuffer.h"
    };

    MicDiscardFilter *_micFilter;
    VoiceCaptureDMOSource *_auxSource;
};

#endif
