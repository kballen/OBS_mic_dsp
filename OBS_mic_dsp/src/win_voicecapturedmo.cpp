#include "win_voicecapturedmo.h"
#include <wmcodecdsp.h>
#include <propsys.h>
#include <uuids.h>
#include <mmreg.h>

/// WinVoiceCaptureDMOMethod::MicDiscardFilter implementation ///

AudioSegment *WinVoiceCaptureDMOMethod::MicDiscardFilter::Process(AudioSegment *segment)
{
    // Discard audio
    delete segment;
    return nullptr;
}

/// WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource implementation ///

WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::VoiceCaptureDMOSource()
    : _dmo(nullptr),
    _numSamples(0),
    _skipNextRead(false)
{
}

WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::~VoiceCaptureDMOSource()
{
    SafeRelease(_dmo);
}

static HRESULT SetVtI4Property(IPropertyStore *ps, REFPROPERTYKEY key, LONG value)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_I4;
    pv.lVal = value;
    HRESULT hr = ps->SetValue(key, pv);
    PropVariantClear(&pv);
    return hr;
}

static HRESULT SetBoolProperty(IPropertyStore *ps, REFPROPERTYKEY key, bool value)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BOOL;
    pv.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    HRESULT hr = ps->SetValue(key, pv);
    PropVariantClear(&pv);
    return hr;
}

bool WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::Initialize(void)
{
#define TRACE(x) traceCall = TEXT(#x), hr = x
    const TCHAR *traceCall = TEXT("<nothing>");
    HRESULT hr = 0;

    if(_dmo)
        // Only call once!
        return false;

    TRACE(CoCreateInstance(__uuidof(CWMAudioAEC), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_dmo)));

    if(FAILED(hr) || !_dmo)
    {
        SafeRelease(_dmo);
        Log(TEXT("Initialization of VoiceCaptureDMOSource failed on %s with hr = 0x%lX"), traceCall, (unsigned long) hr);
        return false;
    }
    
    // Set DMO properties
    IPropertyStore *ps = nullptr;
    TRACE(_dmo->QueryInterface(IID_PPV_ARGS(&ps)));
    if(ps && SUCCEEDED(hr))
    {
        // AEC enabled, no microphone array
        TRACE(SetVtI4Property(ps, MFPKEY_WMAAECMA_SYSTEM_MODE, SINGLE_CHANNEL_AEC));

        // Enable feature mode - unlocks other properties
        if(SUCCEEDED(hr))
            TRACE(SetBoolProperty(ps, MFPKEY_WMAAECMA_FEATURE_MODE, true));

        // Enable AGC
        if(SUCCEEDED(hr))
            TRACE(SetBoolProperty(ps, MFPKEY_WMAAECMA_FEATR_AGC, true));
    }
    SafeRelease(ps);

    if(SUCCEEDED(hr))
    {
        // Set output media type
        DMO_MEDIA_TYPE mt;
        mt.majortype = MEDIATYPE_Audio;
        mt.subtype = MEDIASUBTYPE_PCM;
        mt.lSampleSize = 0;
        mt.bFixedSizeSamples = TRUE;
        mt.bTemporalCompression = FALSE;
        mt.formattype = FORMAT_WaveFormatEx;

        TRACE(MoInitMediaType(&mt, sizeof(WAVEFORMATEX)));
        if(SUCCEEDED(hr))
        {
            WAVEFORMATEX *wav = (WAVEFORMATEX *) mt.pbFormat;
            wav->wFormatTag = WAVE_FORMAT_PCM;
            wav->nChannels = 1;
            wav->nSamplesPerSec = k_SampleRate;
            wav->nAvgBytesPerSec = k_SampleRate * 2;
            wav->nBlockAlign = 2;
            wav->wBitsPerSample = 16;
            wav->cbSize = 0;

            TRACE(_dmo->SetOutputType(0, &mt, 0));
            MoFreeMediaType(&mt);
        }
    }

    if(SUCCEEDED(hr))
        TRACE(_dmo->AllocateStreamingResources());

    if(SUCCEEDED(hr))
    {
        InitAudioData(false, 1, k_SampleRate, 16, 2, 0);
        _audioBuf.Clear();
        _numSamples = 0;
        _skipNextRead = false;
        return true;
    }
    else
    {
        SafeRelease(_dmo);
        Log(TEXT("Initialization of VoiceCaptureDMOSource failed on %s with hr = 0x%lX"), traceCall, (unsigned long) hr);
        return false;
    }
#undef TRACE
}

CTSTR WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::GetDeviceName(void) const
{
    return TEXT("Voice Capture DMO");
}

bool WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::GetNextBuffer(void **buffer, UINT *numFrames, QWORD *timestamp)
{
    // This is horrible.
    // When I completed the bulk of this plugin and got initialization to pass, I wasn't expecting it to lock up the
    // audio thread and crash OBS on stop stream. It turns out that you _cannot_ always return true from this function
    // or you will get OBS stuck in an infinite loop. OBS will call this repeatedly until it returns false to drain
    // data from the input sources, so it's expected that we return false after reading out all available data.
    // Unfortunately DMOs don't seem to have an asynchronous mode or anything useful, so this hack will have to do.
    if(_skipNextRead)
    {
        _skipNextRead = false;
        return false;
    }

    while(_numSamples < k_SegmentSize)
    {
        // Fill a buffer from the DMO
        IMediaBuffer *buf;
        HRESULT hr = CMediaBuffer::Create(k_SegmentSize * 2, &buf);
        if(FAILED(hr))
        {
            return false;
        }

        DMO_OUTPUT_DATA_BUFFER dodb;
        DWORD status;
        dodb.pBuffer = buf;
        hr = _dmo->ProcessOutput(0, 1, &dodb, &status);
        if(SUCCEEDED(hr))
        {
            BYTE *data;
            DWORD len;
            buf->GetBufferAndLength(&data, &len);
            unsigned int newSamples = len / 2;
            unsigned int newNumSamples = _numSamples + newSamples;

            // Expand audio buffer to accomodate new samples if necessary
            if(newNumSamples > _audioBuf.Num())
                _audioBuf.SetSize(newNumSamples);

            // Apply OBS mic volume level to new samples
            // TODO: Add push to talk support somehow
            float micVolume = OBSGetMicVolume();
            for(unsigned int i = 0; i < newSamples; i++)
                ((int16_t *) data)[i] *= micVolume;

            // Copy new samples into audio buffer
            mcpy(_audioBuf.Array() + _numSamples, data, newSamples * 2);
            _numSamples = newNumSamples;

            // If the next call to ProcessOutput would block, force the next read to be skipped and return false
            if(!(dodb.dwStatus & DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE))
                _skipNextRead = true;
        }

        buf->Release();

        if(FAILED(hr))
        {
            return false;
        }
    }

    *buffer = _audioBuf.Array();
    *numFrames = k_SegmentSize;
    *timestamp = OBSGetAudioTime();  // TODO: Is this right? Maybe look at Get/SetTimeOffset()

    return true;
}

void WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::ReleaseBuffer(void)
{
    // Delete one segment-sized chunk from the beginning of the audio buffer
    if(_numSamples >= k_SegmentSize)
    {
        mcpy(_audioBuf.Array(), _audioBuf.Array() + k_SegmentSize, (_numSamples - k_SegmentSize) * 2);
        _numSamples -= k_SegmentSize;
    }
}

/// WinVoiceCaptureDMOMethod implementation ///

WinVoiceCaptureDMOMethod::WinVoiceCaptureDMOMethod()
    : _micFilter(nullptr),
    _auxSource(nullptr)
{
}

WinVoiceCaptureDMOMethod::~WinVoiceCaptureDMOMethod()
{
    OnStopStream();
}

void WinVoiceCaptureDMOMethod::OnStartStream(void)
{
    OnStopStream();

    _micFilter = new MicDiscardFilter();
    _auxSource = new VoiceCaptureDMOSource();
    if(!_micFilter || !_auxSource || !_auxSource->Initialize())
    {
        delete _micFilter; _micFilter = nullptr;
        delete _auxSource; _auxSource = nullptr;
    }
    else
    {
        if(OBSGetMicAudioSource())
            OBSGetMicAudioSource()->AddAudioFilter(_micFilter);
        OBSAddAudioSource(_auxSource);
    }
}

void WinVoiceCaptureDMOMethod::OnStopStream(void)
{
    if(_micFilter)
    {
        if(OBSGetMicAudioSource())
            OBSGetMicAudioSource()->RemoveAudioFilter(_micFilter);
        delete _micFilter;
        _micFilter = nullptr;
    }
    if(_auxSource)
    {
        OBSRemoveAudioSource(_auxSource);
        delete _auxSource;
        _auxSource = nullptr;
    }
}

void WinVoiceCaptureDMOMethod::OnMicVolumeChanged(float level, bool muted, bool finalValue)
{
    // I don't actually need this callback.
}