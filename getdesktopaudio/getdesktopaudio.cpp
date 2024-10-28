#include <iostream>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <comdef.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

#define NOMINMAX
#include <windows.h>
#undef NOMINMAX

using json = nlohmann::json;

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "winmm.lib")

void InitializeCOM()
{
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr))
    {
        std::cerr << "Failed to initialize COM library. Error: " << _com_error(hr).ErrorMessage() << std::endl;
        exit(1);
    }
}

IMMDevice *GetDefaultAudioEndpoint()
{
    IMMDeviceEnumerator *deviceEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void **)&deviceEnumerator);

    if (FAILED(hr))
    {
        std::cerr << "Failed to create device enumerator. Error: " << _com_error(hr).ErrorMessage() << std::endl;
        exit(1);
    }

    IMMDevice *device = NULL;
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    deviceEnumerator->Release();

    if (FAILED(hr))
    {
        std::cerr << "Failed to get default audio endpoint. Error: " << _com_error(hr).ErrorMessage() << std::endl;
        exit(1);
    }

    return device;
}

IAudioClient *InitializeAudioClient(IMMDevice *device)
{
    IAudioClient *audioClient = NULL;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&audioClient);

    if (FAILED(hr))
    {
        std::cerr << "Failed to activate audio client. Error: " << _com_error(hr).ErrorMessage() << std::endl;
        device->Release();
        exit(1);
    }

    WAVEFORMATEX *waveFormat = NULL;
    hr = audioClient->GetMixFormat(&waveFormat);

    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, waveFormat, NULL);

    CoTaskMemFree(waveFormat);

    if (FAILED(hr))
    {
        std::cerr << "Failed to initialize audio client. Error: " << _com_error(hr).ErrorMessage() << std::endl;
        audioClient->Release();
        device->Release();
        exit(1);
    }

    return audioClient;
}

IAudioCaptureClient *GetCaptureClient(IAudioClient *audioClient)
{
    IAudioCaptureClient *captureClient = NULL;
    HRESULT hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void **)&captureClient);

    if (FAILED(hr))
    {
        std::cerr << "Failed to get capture client. Error: " << _com_error(hr).ErrorMessage() << std::endl;
        audioClient->Release();
        exit(1);
    }

    return captureClient;
}

json ProcessAudioData(BYTE *data, UINT32 numFramesAvailable, int samplesCount)
{
    std::vector<float> audioSamples(samplesCount * 2, 0.0f);

    UINT32 framesToProcess = std::min<UINT32>(numFramesAvailable, 64);

    for (UINT32 i = 0; i < framesToProcess; ++i)
    {
        float leftSample = *(float *)(data + i * sizeof(float) * 2);
        float rightSample = *(float *)(data + i * sizeof(float) * 2 + sizeof(float));

        audioSamples[i] = std::max<float>(0.0f, std::min<float>(1.0f, std::fabs(leftSample)));
        audioSamples[i + samplesCount] = std::max<float>(0.0f, std::min<float>(1.0f, std::fabs(rightSample)));
    }

    json audioArray = json::array();
    for (const auto &sample : audioSamples)
    {
        audioArray.push_back(sample);
    }

    return audioArray;
}

void CaptureAudio(int sampleCount, int interval)
{
    IMMDevice *device = GetDefaultAudioEndpoint();
    IAudioClient *audioClient = InitializeAudioClient(device);

    IAudioCaptureClient *captureClient = GetCaptureClient(audioClient);

    audioClient->Start();

    while (true)
    {
        UINT32 packetLength = 0;
        HRESULT hr = captureClient->GetNextPacketSize(&packetLength);

        while (packetLength != 0)
        {
            BYTE *data = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = captureClient->GetBuffer(&data, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr))
            {
                std::cerr << "Failed to get buffer: " << _com_error(hr).ErrorMessage() << std::endl;
                break;
            }

            json outputJson = ProcessAudioData(data, numFramesAvailable, sampleCount);

            std::cout << outputJson.dump(-1) << std::endl;
            std::cout.flush();

            hr = captureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr))
            {
                std::cerr << "Failed to release buffer: " << _com_error(hr).ErrorMessage() << std::endl;
                break;
            }

            hr = captureClient->GetNextPacketSize(&packetLength);
            break;
        }

        Sleep(interval);
    }

    audioClient->Stop();
    captureClient->Release();
    audioClient->Release();
    device->Release();
}

int main(int argc, char *argv[])
{

    InitializeCOM();

    int sampleCount = 64;
    int interval = 15;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-samples") == 0)
        {
            if (i + 1 < argc)
            {
                sampleCount = atoi(argv[i + 1]);
                i++;
            }
            else
            {
                std::cerr << "No value provided for [ -samples ]" << std::endl;
                return 1;
            }
        }
        else if (strcmp(argv[i], "-interval") == 0)
        {
            if (i + 1 < argc)
            {
                interval = atoi(argv[i + 1]);
                i++;
            }
            else
            {
                std::cerr << "No value provided for [ -interval ]" << std::endl;
                return 1;
            }
        }
    }

    try
    {
        CaptureAudio(sampleCount, interval);
    }
    catch (...)
    {
        std::cerr << "An error occurred during audio capture." << std::endl;
    }

    CoUninitialize();

#ifdef _DEBUG
    system("pause");
#endif

    return 0;
}