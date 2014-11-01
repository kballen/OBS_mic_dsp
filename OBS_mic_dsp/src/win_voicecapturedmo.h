#ifndef INCLUDED_win_voicecapturedmo_H
#define INCLUDED_win_voicecapturedmo_H

#include "OBS_mic_dsp.h"
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
        List<float> _audioBuf;
        unsigned int _numSamples;

        static const int k_SampleRate = 16000;
        static const int k_SegmentSize = k_SampleRate / 100;

#include "CMediaBuffer.h"
    };

    MicDiscardFilter *_micFilter;
    VoiceCaptureDMOSource *_auxSource;
};

#endif