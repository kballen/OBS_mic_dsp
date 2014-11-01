// Minimal implementation of IMediaBuffer
// Taken from http://msdn.microsoft.com/en-us/library/windows/desktop/dd376684%28v=vs.85%29.aspx

class CMediaBuffer : public IMediaBuffer
{
private:
    DWORD        _length;
    const DWORD  _maxLength;
    LONG         _refCount;  // Reference count
    BYTE         *_data;

    CMediaBuffer(DWORD maxLength, HRESULT &hr)
        : _length(0),
        _maxLength(maxLength),
        _refCount(1),
        _data(nullptr)
    {
        _data = new BYTE[maxLength];
        if(!_data)
        {
            hr = E_OUTOFMEMORY;
        }
    }

    ~CMediaBuffer()
    {
        if(_data)
        {
            delete[] _data;
        }
    }

public:

    CMediaBuffer(const CMediaBuffer &) = delete;
    CMediaBuffer &operator=(const CMediaBuffer &) = delete;

    // Function to create a new IMediaBuffer object and return 
    // an AddRef'd interface pointer.
    static HRESULT Create(long maxLength, IMediaBuffer **buffer)
    {
        HRESULT hr = S_OK;
        CMediaBuffer *buf;

        if(!buffer)
        {
            return E_POINTER;
        }

        buf = new CMediaBuffer(maxLength, hr);

        if(!buf)
        {
            hr = E_OUTOFMEMORY;
        }

        if(SUCCEEDED(hr))
        {
            *buffer = buf;
            buf->AddRef();
        }

        if(buf)
        {
            buf->Release();
        }

        return hr;
    }

    // IUnknown methods.
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if(!ppv)
        {
            return E_POINTER;
        }
        else if(riid == __uuidof(IMediaBuffer) || riid == __uuidof(IUnknown))
        {
            *ppv = static_cast<IMediaBuffer *>(this);
            AddRef();
            return S_OK;
        }
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
    }

    STDMETHODIMP_(ULONG) AddRef(void)
    {
        return InterlockedIncrement(&_refCount);
    }

    STDMETHODIMP_(ULONG) Release(void)
    {
        LONG ref = InterlockedDecrement(&_refCount);
        if(ref == 0)
        {
            delete this;
        }
        return ref;
    }

    // IMediaBuffer methods.
    STDMETHODIMP SetLength(DWORD length)
    {
        if(length > _maxLength)
        {
            return E_INVALIDARG;
        }
        _length = length;
        return S_OK;
    }

    STDMETHODIMP GetMaxLength(DWORD *maxLength)
    {
        if(!maxLength)
        {
            return E_POINTER;
        }
        *maxLength = _maxLength;
        return S_OK;
    }

    STDMETHODIMP GetBufferAndLength(BYTE **buffer, DWORD *length)
    {
        // Either parameter can be NULL, but not both.
        if(!buffer && !length)
        {
            return E_POINTER;
        }
        if(buffer)
        {
            *buffer = _data;
        }
        if(length)
        {
            *length = _length;
        }
        return S_OK;
    }
};
