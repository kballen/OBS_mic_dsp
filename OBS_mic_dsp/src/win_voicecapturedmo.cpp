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
    _numSamples(0)
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
    if(_dmo)
        // Only call once!
        return false;

    HRESULT hr = CoCreateInstance(
        __uuidof(CWMAudioAEC),
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&_dmo));

    if(FAILED(hr) || !_dmo)
    {
        SafeRelease(_dmo);
        return false;
    }
    
    // Set DMO properties
    IPropertyStore *ps = nullptr;
    hr = _dmo->QueryInterface(IID_PPV_ARGS(&ps));
    if(ps && SUCCEEDED(hr))
    {
        // AEC enabled, no microphone array
        hr = SetVtI4Property(ps, MFPKEY_WMAAECMA_SYSTEM_MODE, SINGLE_CHANNEL_AEC);

        // Enable feature mode - unlocks other properties
        if(SUCCEEDED(hr))
            hr = SetBoolProperty(ps, MFPKEY_WMAAECMA_FEATURE_MODE, true);

        // Enable AGC
        if(SUCCEEDED(hr))
            hr = SetBoolProperty(ps, MFPKEY_WMAAECMA_FEATR_AGC, true);
    }
    SafeRelease(ps);

    if(SUCCEEDED(hr))
    {
        // Set output media type
        DMO_MEDIA_TYPE mt;
        mt.majortype = MEDIATYPE_Audio;
        mt.subtype = MEDIASUBTYPE_IEEE_FLOAT;
        mt.lSampleSize = 0;
        mt.bFixedSizeSamples = TRUE;
        mt.bTemporalCompression = FALSE;
        mt.formattype = FORMAT_WaveFormatEx;

        hr = MoInitMediaType(&mt, sizeof(WAVEFORMATEX));
        if(SUCCEEDED(hr))
        {
            WAVEFORMATEX *wav = (WAVEFORMATEX *) mt.pbFormat;
            wav->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            wav->nChannels = 1;
            wav->nSamplesPerSec = k_SampleRate;
            wav->nAvgBytesPerSec = k_SampleRate * sizeof(float);
            wav->nBlockAlign = sizeof(float);
            wav->wBitsPerSample = 8 * sizeof(float);
            wav->cbSize = 0;

            hr = _dmo->SetOutputType(0, &mt, 0);
            MoFreeMediaType(&mt);
        }
    }

    if(SUCCEEDED(hr))
        hr = _dmo->AllocateStreamingResources();

    if(SUCCEEDED(hr))
    {
        InitAudioData(true, 1, k_SampleRate, 0, 0, 0);
        _audioBuf.Clear();
        _numSamples = 0;
        return true;
    }
    else
    {
        SafeRelease(_dmo);
        return false;
    }
}

CTSTR WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::GetDeviceName(void) const
{
    return TEXT("Voice Capture DMO");
}

bool WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::GetNextBuffer(void **buffer, UINT *numFrames, QWORD *timestamp)
{
    while(_numSamples < k_SegmentSize)
    {
        // Fill a buffer from the DMO
        IMediaBuffer *buf;
        HRESULT hr = CMediaBuffer::Create(k_SegmentSize * sizeof(float), &buf);
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
            unsigned int newSamples = len / sizeof(float);
            unsigned int newNumSamples = _numSamples + newSamples;

            // Expand audio buffer to accomodate new samples if necessary
            if(newNumSamples > _audioBuf.Num())
                _audioBuf.SetSize(newNumSamples);

            // Apply OBS mic volume level to new samples
            // TODO: Add push to talk support somehow
            float micVolume = OBSGetMicVolume();
            for(unsigned int i = 0; i < newSamples; i++)
                ((float *) data)[i] *= micVolume;

            // Copy new samples into audio buffer
            mcpy(_audioBuf.Array() + _numSamples, data, newSamples * sizeof(float));
            _numSamples = newNumSamples;
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
    if(_numSamples >= k_SegmentSize)
    {
        mcpy(_audioBuf.Array(), _audioBuf.Array() + k_SegmentSize, (_numSamples - k_SegmentSize) * sizeof(float));
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