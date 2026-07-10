// Путь: src/core/audiomanager_win.cpp
#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0A00
    #endif
    #ifndef COBJMACROS
        #define COBJMACROS
    #endif
    #include <winsock2.h>
    #include <windows.h>
    #include <mmdeviceapi.h>
    #include <endpointvolume.h>

    // Статические GUID для линкера GCC/MinGW
    static const GUID CLSID_MMDeviceEnumerator_Local = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    static const GUID IID_IMMDeviceEnumerator_Local = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
    static const GUID IID_IAudioEndpointVolume_Local = {0x5cdf2c82, 0x841e, 0x4546, {0x97, 0x22, 0x0c, 0x74, 0x06, 0x1c, 0x16, 0x24}};
#endif

#include "audiomanager_win.h"

void win32_init_com() {
#ifdef _WIN32
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
#endif
}

void win32_uninit_com() {
#ifdef _WIN32
    CoUninitialize();
#endif
}

void win32_set_master_volume(int level) {
#ifdef _WIN32
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    float volumeValue = static_cast<float>(level) / 100.0f;

    win32_init_com();

    IMMDeviceEnumerator *deviceEnumerator = NULL;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IMMDeviceEnumerator_Local, (LPVOID *)&deviceEnumerator);

    if (SUCCEEDED(hr) && deviceEnumerator) {
        IMMDevice *defaultDevice = NULL;
        hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice);

        if (SUCCEEDED(hr) && defaultDevice) {
            IAudioEndpointVolume *endpointVolume = NULL;
            hr = defaultDevice->Activate(IID_IAudioEndpointVolume_Local, CLSCTX_INPROC_SERVER,
                                         NULL, (LPVOID *)&endpointVolume);

            if (SUCCEEDED(hr) && endpointVolume) {
                endpointVolume->SetMasterVolumeLevelScalar(volumeValue, NULL);

                BOOL isMuted = FALSE;
                endpointVolume->GetMute(&isMuted);
                if (isMuted && level > 0) {
                    endpointVolume->SetMute(FALSE, NULL);
                }
                endpointVolume->Release();
            }
            defaultDevice->Release();
        }
        deviceEnumerator->Release();
    }
    win32_uninit_com();
#else
    (void)level; // Фоллбэк для не-Windows платформ
#endif
}