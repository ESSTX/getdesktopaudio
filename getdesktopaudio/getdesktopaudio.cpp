#include <iostream>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <comdef.h>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>

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

json ProcessAudioData(BYTE *data, UINT32 numFramesAvailable)
{
    json mergedAmplitudeArray = json::array();

    for (UINT32 i = 0; i < numFramesAvailable && i < 64; ++i)
    {
        int16_t leftSample = *(int16_t *)(data + i * sizeof(int16_t) * 2);
        float leftAmplitude = static_cast<float>(abs(leftSample)) / INT16_MAX;

        int16_t rightSample = *(int16_t *)(data + i * sizeof(int16_t) * 2 + sizeof(int16_t));
        float rightAmplitude = static_cast<float>(abs(rightSample)) / INT16_MAX;

        mergedAmplitudeArray.push_back(leftAmplitude);
        mergedAmplitudeArray.push_back(rightAmplitude);
    }

    return mergedAmplitudeArray;
}

void CaptureAudio()
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

            json outputJson = ProcessAudioData(data, numFramesAvailable);

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

        Sleep(50);
    }

    audioClient->Stop();
    captureClient->Release();
    audioClient->Release();
    device->Release();
}

int main()
{
    InitializeCOM();

    try
    {
        CaptureAudio();
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