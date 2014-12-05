#include "win_voicecapturedmo.h"
#include <tchar.h>
#include <wmcodecdsp.h>
#include <propsys.h>
#include <uuids.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include "../../speex/include/speex/speex_types.h"
#include "../../speex/include/speex/speex_preprocess.h"

#define LOG_NAME TEXT("OBS_mic_dsp (WinVoiceCaptureDMOMethod)")
#define DEVICE_NAME TEXT("Voice Capture DMO")

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
    _skipNextRead(false),
    _micBoost(0),
    _usePushToTalk(false),
    _pttHotkeyID(0), _pttHotkey2ID(0),
    _pttKeysDown(0),
    _pttDelay(0),
    _pttDelayExpires(0),
    _speexState(nullptr)
{
}

WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::~VoiceCaptureDMOSource()
{
    SafeRelease(_dmo);

    if(_pttHotkeyID)
        OBSDeleteHotkey(_pttHotkeyID);
    if(_pttHotkey2ID)
        OBSDeleteHotkey(_pttHotkey2ID);

    if(_speexState)
        speex_preprocess_state_destroy(_speexState);
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

// This sure is a garbage function.
static int FindEndpointIndex(String deviceId, EDataFlow dataFlow)
{
    // It seems that the voice capture DMO has trouble picking the correct devices if one of the device indicies, but
    // not both, is -1. So, I'm modifying this function to look up the default endpoints explicitly.

#define CHECK(x) if(FAILED(x)) goto error
    int rv = -1;
    IMMDeviceEnumerator *devEnum = nullptr;
    IMMDeviceCollection *devColl = nullptr;
    IMMDevice *dev = nullptr;
    UINT devCount;
    LPWSTR devId = nullptr;
    LPWSTR defaultId = nullptr;

    CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum)));
    CHECK(devEnum->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &devColl));
    CHECK(devColl->GetCount(&devCount));

    if(deviceId.IsValid())
    {
        for(UINT i = 0; i < devCount; i++)
        {
            CHECK(devColl->Item(i, &dev));
            CHECK(dev->GetId(&devId));

            // Why does this have to be complicated...
#ifdef UNICODE
            if(wcscmp(deviceId.Array(), devId) == 0)
            {
                // Found device
                rv = i;
                break;
            }
#else
#error Use Unicode.
#endif

            CoTaskMemFree(devId);
            devId = nullptr;
            SafeRelease(dev);
        }
    }

    // Look up the default endpoint explicitly if needed
    if(rv < 0)
    {
        CHECK(devEnum->GetDefaultAudioEndpoint(dataFlow, eConsole, &dev));
        CHECK(dev->GetId(&defaultId));
        SafeRelease(dev);

        for(UINT i = 0; i < devCount; i++)
        {
            CHECK(devColl->Item(i, &dev));
            CHECK(dev->GetId(&devId));

            if(wcscmp(defaultId, devId) == 0)
            {
                // Found device
                rv = i;
                break;
            }

            CoTaskMemFree(devId);
            devId = nullptr;
            SafeRelease(dev);
        }
    }

error:
    CoTaskMemFree(devId);
    CoTaskMemFree(defaultId);
    SafeRelease(dev);
    SafeRelease(devColl);
    SafeRelease(devEnum);
    return rv;
#undef CHECK
}

static void LogEndpointInfo(int deviceIdx, EDataFlow dataFlow)
{
#define CHECK(x) if(FAILED(x)) goto error
    IMMDeviceEnumerator *devEnum = nullptr;
    IMMDeviceCollection *devColl = nullptr;
    IMMDevice *dev = nullptr;
    IPropertyStore *ps = nullptr;

    CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum)));
    if(deviceIdx < 0)
    {
        // Look up default device
        CHECK(devEnum->GetDefaultAudioEndpoint(dataFlow, eConsole, &dev));
    }
    else
    {
        // Look up device by index
        UINT devCount;
        CHECK(devEnum->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &devColl));
        CHECK(devColl->GetCount(&devCount));
        if(deviceIdx < devCount)
        {
            CHECK(devColl->Item(deviceIdx, &dev));
        }
    }

    if(dev)
    {
        PROPVARIANT friendlyName;
        CHECK(dev->OpenPropertyStore(STGM_READ, &ps));
        PropVariantInit(&friendlyName);
        CHECK(ps->GetValue(PKEY_Device_FriendlyName, &friendlyName));
#ifdef UNICODE
        Log(TEXT("%s: Using %s device: %s%s"),
#else
        Log(TEXT("%s: Using %s device: %S%s"),
#endif
            LOG_NAME,
            dataFlow == eCapture ? TEXT("capture") : TEXT("render"),
            friendlyName.pwszVal,
            deviceIdx < 0 ? TEXT(" (default)") : TEXT(""));
        PropVariantClear(&friendlyName);
    }

error:
    SafeRelease(ps);
    SafeRelease(dev);
    SafeRelease(devColl);
    SafeRelease(devEnum);
#undef CHECK
}

bool WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::Initialize(void)
{
#define TRACE(x) traceCall = TEXT(#x), hr = x
    const TCHAR *traceCall;
    HRESULT hr;

    if(_dmo)
        // Only call once!
        return false;

    // Get OBS settings
    String micDeviceId;
    String playbackDeviceId;
    _micBoost = 1.0;

    if(_pttHotkeyID) { OBSDeleteHotkey(_pttHotkeyID); _pttHotkeyID = 0; }
    if(_pttHotkey2ID) { OBSDeleteHotkey(_pttHotkey2ID); _pttHotkey2ID = 0; }
    _usePushToTalk = false;
    _pttKeysDown = 0;
    _pttDelayExpires = 0;

    if(_speexState)
    {
        speex_preprocess_state_destroy(_speexState);
        _speexState = nullptr;
    }

    ConfigFile cfg;
    String cfgName;
    cfgName << OBSGetAppDataPath() << TEXT("\\global.ini");
    if(cfg.Open(cfgName))
    {
        String cfgProfile = cfg.GetString(TEXT("General"), TEXT("Profile"));
        if(cfgProfile.IsValid())
        {
            cfgName.Clear() << OBSGetAppDataPath() << TEXT("\\profiles\\") << cfgProfile << TEXT(".ini");
            if(cfg.Open(cfgName))
            {
                // Mic boost
                int micBoost = cfg.GetInt(TEXT("Audio"), TEXT("MicBoostMultiple"), 1);
                if(micBoost < 1)
                    micBoost = 1;
                else if(micBoost > 20)
                    micBoost = 20;
                _micBoost = (float) micBoost;

                // Audio devices
                micDeviceId = cfg.GetString(TEXT("Audio"), TEXT("Device"));
                playbackDeviceId = cfg.GetString(TEXT("Audio"), TEXT("PlaybackDevice"));

                // Push-to-talk hotkey
                _usePushToTalk = cfg.GetInt(TEXT("Audio"), TEXT("UsePushToTalk")) != 0;
                _pttDelay = cfg.GetInt(TEXT("Audio"), TEXT("PushToTalkDelay"), 200);
                if(_pttDelay < 0)
                    _pttDelay = 0;
                DWORD pttHotkey = cfg.GetInt(TEXT("Audio"), TEXT("PushToTalkHotkey"));
                DWORD pttHotkey2 = cfg.GetInt(TEXT("Audio"), TEXT("PushToTalkHotkey2"));
                if(_usePushToTalk && pttHotkey)
                    _pttHotkeyID = OBSCreateHotkey(pttHotkey, PushToTalkHotkeyCB, (UPARAM) this);
                if(_usePushToTalk && pttHotkey2)
                    _pttHotkey2ID = OBSCreateHotkey(pttHotkey2, PushToTalkHotkeyCB, (UPARAM) this);
            }
        }
    }

    // Look up audio endpoints
    int micDeviceIdx = FindEndpointIndex(micDeviceId, eCapture);
    int playbackDeviceIdx = FindEndpointIndex(playbackDeviceId, eRender);

    // No longer allowing the DMO to choose default devices
    if(micDeviceIdx < 0 || playbackDeviceIdx < 0)
    {
        Log(TEXT("%s: There was a problem looking up the default audio endpoints... mic=%d playback=%d"), LOG_NAME, micDeviceIdx, playbackDeviceIdx);
        return false;
    }

    LogEndpointInfo(micDeviceIdx, eCapture);
    LogEndpointInfo(playbackDeviceIdx, eRender);

    // Initialize Speex preprocessor for post-gain noise removal if mic boost is used
    //if(_micBoost > 1)
    {
        //Log(TEXT("%s: Mic boost > 1, enabling post-gain noise removal."), LOG_NAME);

        _speexState = speex_preprocess_state_init(k_SegmentSize, k_SampleRate);
        if(_speexState)
        {
            spx_int32_t noiseSuppress = -30;
            speex_preprocess_ctl(_speexState, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noiseSuppress);
        }
        else
            Log(TEXT("%s: Warning! Failed to create Speex preprocessor state for post-gain noise removal."), LOG_NAME);
    }

    TRACE(CoCreateInstance(__uuidof(CWMAudioAEC), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_dmo)));

    if(FAILED(hr) || !_dmo)
    {
        SafeRelease(_dmo);
        Log(TEXT("%s: Initialization of VoiceCaptureDMOSource failed on %s with hr = 0x%lX"), LOG_NAME, traceCall, (unsigned long) hr);
        return false;
    }
    
    // Set DMO properties
    IPropertyStore *ps = nullptr;
    TRACE(_dmo->QueryInterface(IID_PPV_ARGS(&ps)));
    if(ps && SUCCEEDED(hr))
    {
        // AEC enabled, no microphone array
        TRACE(SetVtI4Property(ps, MFPKEY_WMAAECMA_SYSTEM_MODE, SINGLE_CHANNEL_AEC));

        // Select audio endpoints
        if(SUCCEEDED(hr))
            TRACE(SetVtI4Property(ps, MFPKEY_WMAAECMA_DEVICE_INDEXES, (unsigned long) (playbackDeviceIdx << 16) | (unsigned long) (micDeviceIdx & 0xffff)));

        // Enable feature mode - unlocks other properties
        if(SUCCEEDED(hr))
            TRACE(SetBoolProperty(ps, MFPKEY_WMAAECMA_FEATURE_MODE, true));

        // Enable AGC
        if(SUCCEEDED(hr))
            TRACE(SetBoolProperty(ps, MFPKEY_WMAAECMA_FEATR_AGC, true));

        // Disable microphone gain bounding - this prevents the DMO from messing with the Windows mic device volume
        if(SUCCEEDED(hr))
            TRACE(SetBoolProperty(ps, MFPKEY_WMAAECMA_MIC_GAIN_BOUNDER, false));
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
        Log(TEXT("%s: Initialization of VoiceCaptureDMOSource failed on %s with hr = 0x%lX"), LOG_NAME, traceCall, (unsigned long) hr);
        return false;
    }
#undef TRACE
}

void STDCALL WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::PushToTalkHotkeyCB(DWORD hotkey, UPARAM param, bool keyDown)
{
    VoiceCaptureDMOSource *me = (VoiceCaptureDMOSource *) param;

    if(keyDown)
    {
        me->_pttKeysDown++;
    }
    else
    {
        me->_pttKeysDown--;
        if(me->_pttKeysDown == 0)
        {
            me->_pttDelayExpires = OBSGetTotalStreamTime() + me->_pttDelay;
        }
    }
}

CTSTR WinVoiceCaptureDMOMethod::VoiceCaptureDMOSource::GetDeviceName(void) const
{
    return DEVICE_NAME;
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

            // Push-to-talk audio muting and volume level
            bool pttMute = _usePushToTalk && _pttKeysDown == 0 && OBSGetTotalStreamTime() >= _pttDelayExpires;
            float micVolume = OBSGetMicVolume() * _micBoost;
            if(pttMute || micVolume == 0)
            {
                memset(_audioBuf.Array() + _numSamples, 0, newSamples * 2);
            }
            else
            {
                if(micVolume != 1)
                {
                    for(unsigned int i = 0; i < newSamples; i++)
                    {
                        long sample = ((int16_t *) data)[i];
                        sample *= micVolume;
                        if(sample > 32767)
                            sample = 32767;
                        else if(sample < -32767)
                            sample = -32767;
                        ((int16_t *) data)[i] = sample;
                    }
                }

                // Copy new samples into audio buffer
                mcpy(_audioBuf.Array() + _numSamples, data, newSamples * 2);
            }
            
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

    // Apply Speex noise removal if enabled
    if(_speexState)
    {
        speex_preprocess_run(_speexState, _audioBuf.Array());
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

    for(UINT i = 0; i < OBSNumAuxAudioSources(); i++)
    {
        if(_tcscmp(DEVICE_NAME, OBSGetAuxAudioSource(i)->GetDeviceName2()) == 0)
        {
            Log(TEXT("%s: Another aux source with the same device name was found. Are there two copies of this plugin?"), LOG_NAME);
            return;
        }
    }

    if(OBSGetMicAudioSource())
    {
        _micFilter = new MicDiscardFilter();
        _auxSource = new VoiceCaptureDMOSource();
        if(!_micFilter || !_auxSource || !_auxSource->Initialize())
        {
            delete _micFilter; _micFilter = nullptr;
            delete _auxSource; _auxSource = nullptr;
            Log(TEXT("%s: DMO audio source initialization failed. Mic processing is NOT active."), LOG_NAME);
        }
        else
        {
            OBSGetMicAudioSource()->AddAudioFilter(_micFilter);
            OBSAddAudioSource(_auxSource);
            Log(TEXT("%s: Supplying processed microphone audio via aux audio source."), LOG_NAME);
        }
    }
    else
        Log(TEXT("%s: Microphone input disabled in OBS settings."), LOG_NAME);
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