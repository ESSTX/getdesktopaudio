#include <iostream>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <comdef.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <memory>
#include <format>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <atlbase.h>

#define NOMINMAX
#include <windows.h>
#undef NOMINMAX

using json = nlohmann::json;

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "winmm.lib")

class ComErrorHandler
{
public:
    ComErrorHandler() = default;

    bool CheckHRESULT(HRESULT hr)
    {
        if (FAILED(hr))
        {
            CComPtr<IErrorInfo> err;
            ::GetErrorInfo(0, &err);
            if (err)
            {
                CComBSTR description;
                err->GetDescription(&description);
                errorMessage = description;
            }
            else
            {
                errorMessage = _com_error(hr).ErrorMessage();
            }
            return false;
        }
        return true;
    }

    std::wstring GetErrorMessage() const
    {
        return errorMessage;
    }

private:
    std::wstring errorMessage;
};

class COMInitializer
{
public:
    COMInitializer()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComErrorHandler errorHandler;

        if (!errorHandler.CheckHRESULT(hr))
        {
            // throw std::runtime_error("" + ConvertWStringToString(errorHandler.GetErrorMessage()));
        }
    }

    ~COMInitializer()
    {
        CoUninitialize();
    }
};

class AudioDevice
{
public:
    AudioDevice()
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&deviceEnumerator));

        ComErrorHandler errorHandler;
        if (!errorHandler.CheckHRESULT(hr))
        {
            throw std::runtime_error("Failed audio device: " + ConvertWStringToString(errorHandler.GetErrorMessage()));
        }

        hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (!errorHandler.CheckHRESULT(hr))
        {
            throw std::runtime_error("Failed audio device endpoint: " + ConvertWStringToString(errorHandler.GetErrorMessage()));
        }
    }

    ~AudioDevice()
    {
        if (device)
            device->Release();
        if (deviceEnumerator)
            deviceEnumerator->Release();
    }

    auto InitializeAudioClient() -> std::unique_ptr<IAudioClient>
    {
        IAudioClient *audioClient = nullptr;
        ComErrorHandler errorHandler;

        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&audioClient));
        if (!errorHandler.CheckHRESULT(hr))
        {
            throw std::runtime_error("Failed to activate audio client: " + ConvertWStringToString(errorHandler.GetErrorMessage()));
        }

        WAVEFORMATEX *waveFormat = nullptr;
        hr = audioClient->GetMixFormat(&waveFormat);
        if (!errorHandler.CheckHRESULT(hr))
        {
            audioClient->Release();
            throw std::runtime_error("Failed to get mix format: " + ConvertWStringToString(errorHandler.GetErrorMessage()));
        }

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, waveFormat, nullptr);
        CoTaskMemFree(waveFormat);

        if (!errorHandler.CheckHRESULT(hr))
        {
            audioClient->Release();
            throw std::runtime_error("Failed to initialize audio client: " + ConvertWStringToString(errorHandler.GetErrorMessage()));
        }

        return std::unique_ptr<IAudioClient>(audioClient);
    }

    std::string ConvertWStringToString(const std::wstring &wstr)
    {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string str(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &str[0], size_needed, nullptr, nullptr);
        return str;
    }

private:
    IMMDeviceEnumerator *deviceEnumerator = nullptr;
    IMMDevice *device = nullptr;
};

class AudioCapture
{
public:
    explicit AudioCapture(IAudioClient *audioClient) : audioClient(audioClient)
    {
        HRESULT hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&captureClient));
        if (FAILED(hr))
        {
            // throw std::runtime_error("capt client: " + std::string(_com_error(hr).ErrorMessage()));
        }
    }

    ~AudioCapture()
    {
        if (captureClient)
            captureClient->Release();
    }

    void StartCapturing(int samplesCount, int interval)
    {
        audioClient->Start();
        std::vector<json> jsonOutputs;

        while (true)
        {
            UINT32 packetLength = 0;
            HRESULT hr = captureClient->GetNextPacketSize(&packetLength);

            while (packetLength != 0)
            {
                CaptureAndProcessAudio(samplesCount);
                hr = captureClient->GetNextPacketSize(&packetLength);

                std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }

        audioClient->Stop();
    }

private:
    void CaptureAndProcessAudio(int samplesCount)
    {
        BYTE *data = nullptr;
        UINT32 numFramesAvailable = 0;
        DWORD flags = 0;

        HRESULT hr = captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);

        if (FAILED(hr))
        {
            // throw std::runtime_error("get buff: " + std::string(_com_error(hr).ErrorMessage()));
        }

        json outputJson = ProcessAudioData(data, numFramesAvailable, samplesCount);

        std::cout << outputJson.dump(-1) << std::endl;

        hr = captureClient->ReleaseBuffer(numFramesAvailable);

        if (FAILED(hr))
        {
            // throw std::runtime_error("release buff: " + std::string(_com_error(hr).ErrorMessage()));
        }
    }

    json ProcessAudioData(BYTE *data, UINT32 numFramesAvailable, int samplesCount)
    {
        std::vector<float> audioSamples(samplesCount);

        UINT32 framesToProcess = std::min<UINT32>(numFramesAvailable, static_cast<UINT32>(samplesCount / 2));

        for (UINT32 i = 0; i < framesToProcess; ++i)
        {
            float leftSample = *(reinterpret_cast<float *>(data + i * sizeof(float) * 2));
            float rightSample = *(reinterpret_cast<float *>(data + i * sizeof(float) * 2 + sizeof(float)));

            audioSamples[i * 2] = std::clamp(std::fabs(leftSample), 0.0f, 1.0f);
            audioSamples[i * 2 + 1] = std::clamp(std::fabs(rightSample), 0.0f, 1.0f);
        }

        audioSamples.resize(framesToProcess * 2);

        /*
        std::cout << "Audio Samples: ";
        for (const auto& sample : audioSamples)
        {
            std::cout << sample << " ";
        }
        std::cout << std::endl;
        */

        return json(audioSamples);
    }

    IAudioClient *audioClient;
    IAudioCaptureClient *captureClient = nullptr;
};

int main(int argc, char *argv[])
{
    try
    {
        COMInitializer comInit;

        int samplesCount = 64;
        int interval = 15;

        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "-samples") == 0 && i + 1 < argc)
            {
                samplesCount = atoi(argv[++i]);
                if (samplesCount <= 0)
                    throw std::invalid_argument("Samples count must be positive.");
            }
            else if (strcmp(argv[i], "-interval") == 0 && i + 1 < argc)
            {
                interval = atoi(argv[++i]);
                if (interval <= 0)
                    throw std::invalid_argument("Interval must be positive.");
            }
        }

        AudioDevice audioDevice;

        auto audioClientPtr = audioDevice.InitializeAudioClient();

        AudioCapture capture(audioClientPtr.get());

        capture.StartCapturing(samplesCount, interval);
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

#ifdef _DEBUG
    system("pause");
#endif

    return EXIT_SUCCESS;
}