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
#include <complex>
#include <fftw3.h>

#define NOMINMAX
#include <windows.h>
#undef NOMINMAX

using json = nlohmann::json;

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "winmm.lib")

class COMInitializer
{
public:
    COMInitializer()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr))
        {
            throw std::runtime_error("Failed to initialize COM library.");
        }
    }

    ~COMInitializer()
    {
        CoUninitialize();
    }
};

class AudioDeviceManager
{
public:
    AudioDeviceManager()
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&deviceEnumerator));
        if (FAILED(hr))
        {
            throw std::runtime_error("Failed to create MMDeviceEnumerator instance.");
        }

        hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr))
        {
            throw std::runtime_error("Failed to get default audio endpoint.");
        }
    }

    ~AudioDeviceManager()
    {
        if (device)
            device->Release();
        if (deviceEnumerator)
            deviceEnumerator->Release();
    }

    auto CreateAudioClient() -> std::unique_ptr<IAudioClient>
    {
        IAudioClient *audioClient = nullptr;
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&audioClient));
        if (FAILED(hr))
        {
            throw std::runtime_error("Failed to activate audio client.");
        }

        WAVEFORMATEX *waveFormat = nullptr;
        hr = audioClient->GetMixFormat(&waveFormat);
        if (FAILED(hr))
        {
            audioClient->Release();
            throw std::runtime_error("Failed to get mix format.");
        }

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, waveFormat, nullptr);
        CoTaskMemFree(waveFormat);

        if (FAILED(hr))
        {
            audioClient->Release();
            throw std::runtime_error("Failed to initialize audio client.");
        }

        return std::unique_ptr<IAudioClient>(audioClient);
    }

private:
    IMMDeviceEnumerator *deviceEnumerator = nullptr;
    IMMDevice *device = nullptr;
};

class AudioStreamCapture
{
public:
    explicit AudioStreamCapture(IAudioClient *audioClient) : audioClient(audioClient)
    {
        HRESULT hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&captureClient));
        if (FAILED(hr))
        {
            throw std::runtime_error("Failed to get capture client service.");
        }
    }

    ~AudioStreamCapture()
    {
        if (captureClient)
            captureClient->Release();
    }

    void StartCapture(int sampleCount, int interval)
    {
        audioClient->Start();

        while (isRunning)
        {
            UINT32 packetLength = 0;
            captureClient->GetNextPacketSize(&packetLength);

            while (packetLength != 0)
            {
                CaptureAndProcess(sampleCount);
                captureClient->GetNextPacketSize(&packetLength);
                std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }

        audioClient->Stop();
    }

    void StopCapture() { isRunning = false; }

private:
    void CaptureAndProcess(int sampleCount)
    {
        BYTE *bufferData = nullptr;
        UINT32 framesAvailable = 0;
        DWORD flags = 0;

        captureClient->GetBuffer(&bufferData, &framesAvailable, &flags, nullptr, nullptr);

        json outputJson = ProcessAudio(bufferData, framesAvailable, sampleCount);

        std::cout << outputJson.dump(-1) << std::endl;

        captureClient->ReleaseBuffer(framesAvailable);
    }

    json ProcessAudio(BYTE *bufferData, UINT32 framesAvailable, int maxSamples)
    {
        int frameCount = static_cast<int>(framesAvailable);
        int sampleCount = std::min<int>(maxSamples, frameCount * 2);

        std::vector<double> leftSamples(sampleCount / 2);
        std::vector<double> rightSamples(sampleCount / 2);

        for (int i = 0; i < sampleCount / 2; ++i)
        {
            float leftSample = *(reinterpret_cast<float *>(bufferData + i * sizeof(float) * 2));
            float rightSample = *(reinterpret_cast<float *>(bufferData + i * sizeof(float) * 2 + sizeof(float)));

            leftSamples[i] = leftSample;
            rightSamples[i] = rightSample;
        }

        ApplyCompression(leftSamples);
        ApplyCompression(rightSamples);

        json outputJson;
        outputJson["leftSamples"] = leftSamples;
        outputJson["rightSamples"] = rightSamples;

        return outputJson;
    }

    void ApplyCompression(std::vector<double> &samples)
    {
        const int N = static_cast<int>(samples.size());
        if (N == 0)
            return;

        std::vector<std::complex<double>> fftInput(N);
        std::vector<std::complex<double>> fftOutput(N);

        for (int i = 0; i < N; ++i)
        {
            fftInput[i] = {samples[i], 0.0};
        }

        fftw_plan forwardPlan = fftw_plan_dft_1d(N, reinterpret_cast<fftw_complex *>(fftInput.data()), reinterpret_cast<fftw_complex *>(fftOutput.data()), FFTW_FORWARD, FFTW_ESTIMATE);
        fftw_execute(forwardPlan);
        fftw_destroy_plan(forwardPlan);

        const double THRESHOLD = 0.5;
        const double RATIO = 4.0;

        for (auto &frequency : fftOutput)
        {
            double magnitude = std::abs(frequency);
            if (magnitude > THRESHOLD)
            {
                frequency *= (THRESHOLD + (magnitude - THRESHOLD) / RATIO) / magnitude;
            }
        }

        fftw_plan inversePlan = fftw_plan_dft_1d(N, reinterpret_cast<fftw_complex *>(fftOutput.data()), reinterpret_cast<fftw_complex *>(fftInput.data()), FFTW_BACKWARD, FFTW_ESTIMATE);
        fftw_execute(inversePlan);
        fftw_destroy_plan(inversePlan);

        for (int i = 0; i < N; ++i)
        {
            samples[i] = fftInput[i].real() / N;
        }
    }

    IAudioClient *audioClient;
    IAudioCaptureClient *captureClient = nullptr;
    bool isRunning = true;
};

int main(int argc, char *argv[])
{
    try
    {
        COMInitializer comInit;

        int sampleCount = 64;
        int intervalDuration = 15;

        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "-samples") == 0 && i + 1 < argc)
            {
                sampleCount = atoi(argv[++i]);
                if (sampleCount <= 0)
                    throw std::invalid_argument("Sample count must be positive.");
            }
            else if (strcmp(argv[i], "-interval") == 0 && i + 1 < argc)
            {
                intervalDuration = atoi(argv[++i]);
                if (intervalDuration <= 0)
                    throw std::invalid_argument("Interval must be positive.");
            }
        }

        AudioDeviceManager audioManager;

        auto audioClientPtr = audioManager.CreateAudioClient();

        AudioStreamCapture streamCapture(audioClientPtr.get());

#ifdef _DEBUG
        std::thread captureThread([&]()
                                  { streamCapture.StartCapture(sampleCount - 1, intervalDuration); });
#else
        streamCapture.StartCapture(sampleCount - 1, intervalDuration);
#endif

        streamCapture.StopCapture();
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}