#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <math.h>

extern "C" {
#include "dualsense.h"
}

#define RING_SIZE 4096

static float ring_l[RING_SIZE];
static float ring_r[RING_SIZE];
static volatile LONG ring_write = 0;
static volatile LONG ring_read = 0;
static volatile int audio_running = 0;
static volatile float audio_level = 0;
static HANDLE audio_thread = NULL;

class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    LONG refs = 1;
    HANDLE done;
    HRESULT result = E_FAIL;
    IAudioClient *client = NULL;

    ActivateHandler() { done = CreateEventW(NULL, TRUE, FALSE, NULL); }
    ~ActivateHandler() { CloseHandle(done); }

    STDMETHODIMP QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject *>(this);
        } else {
            *out = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (r == 0)
            delete this;
        return r;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override
    {
        HRESULT hr2 = E_FAIL;
        IUnknown *unk = NULL;
        HRESULT hr = op->GetActivateResult(&hr2, &unk);
        if (SUCCEEDED(hr) && SUCCEEDED(hr2) && unk) {
            result = unk->QueryInterface(__uuidof(IAudioClient), (void **)&client);
            unk->Release();
        } else {
            result = FAILED(hr) ? hr : hr2;
        }
        SetEvent(done);
        return S_OK;
    }
};

struct Biquad {
    float b0, b1, b2, a1, a2;
    float z1 = 0, z2 = 0;
    void lowpass(float freq, float rate)
    {
        float w = 6.2831853f * freq / rate;
        float c = cosf(w), s = sinf(w);
        float alpha = s / (2.0f * 0.7071f);
        float a0 = 1 + alpha;
        b0 = (1 - c) / 2 / a0;
        b1 = (1 - c) / a0;
        b2 = (1 - c) / 2 / a0;
        a1 = -2 * c / a0;
        a2 = (1 - alpha) / a0;
    }
    void highpass(float freq, float rate)
    {
        float w = 6.2831853f * freq / rate;
        float c = cosf(w), s = sinf(w);
        float alpha = s / (2.0f * 0.7071f);
        float a0 = 1 + alpha;
        b0 = (1 + c) / 2 / a0;
        b1 = -(1 + c) / a0;
        b2 = (1 + c) / 2 / a0;
        a1 = -2 * c / a0;
        a2 = (1 - alpha) / a0;
    }
    float run(float x)
    {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

static void push_sample(float l, float r)
{
    LONG w = ring_write;
    LONG next = (w + 1) % RING_SIZE;
    if (next == ring_read)
        return;
    ring_l[w] = l;
    ring_r[w] = r;
    ring_write = next;
}

static DWORD WINAPI audio_main(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    AUDIOCLIENT_ACTIVATION_PARAMS ap = {};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv = {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(ap);
    pv.blob.pBlobData = (BYTE *)&ap;

    ActivateHandler *handler = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, &op);
    if (FAILED(hr)) {
        log_msg("audio haptics: activate failed %08lx", hr);
        handler->Release();
        CoUninitialize();
        return 0;
    }
    WaitForSingleObject(handler->done, 5000);
    IAudioClient *client = handler->client;
    if (client)
        client->AddRef();
    hr = handler->result;
    if (op)
        op->Release();
    handler->Release();
    if (!client) {
        log_msg("audio haptics: no audio client %08lx", hr);
        CoUninitialize();
        return 0;
    }

    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = 48000 * 4;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            200000, 0, &fmt, NULL);
    IAudioCaptureClient *cap = NULL;
    if (SUCCEEDED(hr))
        hr = client->SetEventHandle(ev);
    if (SUCCEEDED(hr))
        hr = client->GetService(__uuidof(IAudioCaptureClient), (void **)&cap);
    if (SUCCEEDED(hr))
        hr = client->Start();
    if (FAILED(hr)) {
        log_msg("audio haptics: capture setup failed %08lx", hr);
        if (cap)
            cap->Release();
        client->Release();
        CloseHandle(ev);
        CoUninitialize();
        return 0;
    }
    log_msg("audio haptics: capturing game audio");

    Biquad lp1[2], lp2[2], hp[2];
    for (int c = 0; c < 2; c++) {
        lp1[c].lowpass(220, 48000);
        lp2[c].lowpass(220, 48000);
        hp[c].highpass(18, 48000);
    }
    float acc_l = 0, acc_r = 0;
    int acc_n = 0;
    float env = 0;

    while (audio_running) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0)
            continue;
        UINT32 packet = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE *data = NULL;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, NULL, NULL)))
                break;
            short *s = (short *)data;
            for (UINT32 i = 0; i < frames; i++) {
                float l = 0, r = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    l = s[i * 2] / 32768.0f;
                    r = s[i * 2 + 1] / 32768.0f;
                }
                l = lp2[0].run(lp1[0].run(hp[0].run(l)));
                r = lp2[1].run(lp1[1].run(hp[1].run(r)));
                acc_l += l;
                acc_r += r;
                acc_n++;
                if (acc_n == 16) {
                    float ol = acc_l / 16, orr = acc_r / 16;
                    float m = fabsf(ol) > fabsf(orr) ? fabsf(ol) : fabsf(orr);
                    env = m > env ? m : env * 0.995f;
                    push_sample(ol, orr);
                    acc_l = acc_r = 0;
                    acc_n = 0;
                }
            }
            cap->ReleaseBuffer(frames);
        }
        audio_level = env;
    }
    client->Stop();
    cap->Release();
    client->Release();
    CloseHandle(ev);
    CoUninitialize();
    return 0;
}

extern "C" void audio_start(void)
{
    if (audio_running)
        return;
    audio_running = 1;
    audio_thread = CreateThread(NULL, 0, audio_main, NULL, 0, NULL);
}

extern "C" void audio_stop(void)
{
    audio_running = 0;
}

extern "C" int audio_take(float *l, float *r)
{
    LONG rd = ring_read;
    LONG avail = (ring_write - rd + RING_SIZE) % RING_SIZE;
    if (avail == 0)
        return 0;
    if (avail > 150)
        rd = (ring_write - 60 + RING_SIZE) % RING_SIZE;
    *l = ring_l[rd];
    *r = ring_r[rd];
    ring_read = (rd + 1) % RING_SIZE;
    return 1;
}

extern "C" float audio_loudness(void)
{
    return audio_level;
}
