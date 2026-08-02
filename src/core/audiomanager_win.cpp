// Путь: src/core/audiomanager_win.cpp
#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0A00
    #endif
    #include <winsock2.h>
    #include <windows.h>
    #include <mmdeviceapi.h>
    #include <endpointvolume.h>
    #include <functiondiscoverykeys_devpkey.h>
    #include <objbase.h>
    #include <propidl.h>
    #include <mmsystem.h>
    #include <string>

    // Static GUIDs for MinGW (uuid.lib / DEFINE_GUID may be unavailable).
    // IID_IAudioEndpointVolume MUST match Windows SDK / endpointvolume.h:
    //   5CDF2C82-841E-4546-9722-0CF74078229A
    // (A common docs typo 0C74061C1624 causes Activate → E_NOINTERFACE.)
    static const GUID CLSID_MMDeviceEnumerator_Local =
        {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    static const GUID IID_IMMDeviceEnumerator_Local =
        {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
    static const GUID IID_IAudioEndpointVolume_Local =
        {0x5cdf2c82, 0x841e, 0x4546, {0x97, 0x22, 0x0c, 0xf7, 0x40, 0x78, 0x22, 0x9a}};
    static const GUID IID_IMMNotificationClient_Local =
        {0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};

    // Undocumented PolicyConfig (used by SoundSwitch / EarTrumpet-style tools).
    static const GUID CLSID_PolicyConfigClient_Local =
        {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf6, 0x31, 0x2b, 0x08}};
    static const GUID IID_IPolicyConfigVista_Local =
        {0x568b9108, 0x44bf, 0x40b4, {0x90, 0x06, 0x86, 0xaf, 0xe5, 0xb5, 0xa6, 0x20}};
    static const GUID IID_IPolicyConfig_Local =
        {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

    // PROPERTYKEY symbols are often DECLSPEC_SELECTANY and missing at link on MinGW.
    static const PROPERTYKEY PKEY_Device_FriendlyName_Local =
        {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    static const PROPERTYKEY PKEY_AudioEndpoint_FormFactor_Local =
        {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
#endif

#include "audiomanager_win.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QStringList>
#include <QTimer>

#ifdef _WIN32
namespace {

QString hrHex(HRESULT hr)
{
    return QString::number(static_cast<quint32>(hr), 16);
}

QString deviceIdOf(IMMDevice *device)
{
    if (!device)
        return QStringLiteral("(null)");
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id)
        return QStringLiteral("(unknown)");
    const QString out = QString::fromWCharArray(id);
    CoTaskMemFree(id);
    return out;
}

bool setEndpointVolume(IMMDevice *device, float volumeValue, int level, HRESULT *activateHr)
{
    IAudioEndpointVolume *endpointVolume = nullptr;
    const HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                        nullptr, reinterpret_cast<void **>(&endpointVolume));
    if (activateHr)
        *activateHr = hr;
    if (FAILED(hr) || !endpointVolume) {
        // Retry with explicit SDK IID in case __uuidof is unavailable/wrong on this toolchain.
        IAudioEndpointVolume *retry = nullptr;
        const HRESULT hr2 = device->Activate(IID_IAudioEndpointVolume_Local, CLSCTX_ALL,
                                             nullptr, reinterpret_cast<void **>(&retry));
        if (activateHr)
            *activateHr = hr2;
        if (FAILED(hr2) || !retry)
            return false;
        endpointVolume = retry;
    }

    const HRESULT setHr = endpointVolume->SetMasterVolumeLevelScalar(volumeValue, nullptr);
    if (FAILED(setHr)) {
        qWarning() << "[AUDIO] setVolume" << level
                   << "— SetMasterVolumeLevelScalar failed hr=" << hrHex(setHr);
        endpointVolume->Release();
        return false;
    }

    if (level == 0)
        endpointVolume->SetMute(TRUE, nullptr);
    else
        endpointVolume->SetMute(FALSE, nullptr);

    endpointVolume->Release();
    return true;
}

bool tryWasapiRole(IMMDeviceEnumerator *enumerator, ERole role, float volumeValue, int level,
                   const char *roleName)
{
    IMMDevice *device = nullptr;
    const HRESULT epHr = enumerator->GetDefaultAudioEndpoint(eRender, role, &device);
    if (FAILED(epHr) || !device) {
        qWarning() << "[AUDIO] setVolume" << level
                   << "— GetDefaultAudioEndpoint" << roleName << "failed hr=" << hrHex(epHr);
        return false;
    }

    HRESULT activateHr = E_FAIL;
    const bool ok = setEndpointVolume(device, volumeValue, level, &activateHr);
    if (!ok) {
        qWarning() << "[AUDIO] setVolume" << level
                   << "— Activate IAudioEndpointVolume failed hr=" << hrHex(activateHr)
                   << "role=" << roleName
                   << "deviceId=" << deviceIdOf(device);
    }
    device->Release();
    return ok;
}

bool tryWaveOutSetVolume(int level)
{
    // waveOutSetVolume: low 16 bits = left, high 16 bits = right (0x0000..0xFFFF).
    const DWORD sample = static_cast<DWORD>((level * 0xFFFF) / 100);
    const DWORD packed = (sample << 16) | sample;
    const MMRESULT mr = waveOutSetVolume(nullptr, packed);
    if (mr != MMSYSERR_NOERROR) {
        qWarning() << "[AUDIO] setVolume" << level
                   << "— waveOutSetVolume failed mmr=" << static_cast<int>(mr);
        return false;
    }
    return true;
}

void sendVolumeKey(WORD vk)
{
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

bool tryVolumeKeysRelative(int level)
{
    // Last resort: approximate target via system volume keys (relative, coarse).
    // Reset toward 0, then step up. Not precise — only when WASAPI + waveOut fail.
    for (int i = 0; i < 50; ++i)
        sendVolumeKey(VK_VOLUME_DOWN);
    if (level <= 0) {
        sendVolumeKey(VK_VOLUME_MUTE);
        return true;
    }
    const int ups = qBound(0, (level * 50) / 100, 50);
    for (int i = 0; i < ups; ++i)
        sendVolumeKey(VK_VOLUME_UP);
    return true;
}

// --- PolicyConfig (undocumented) -------------------------------------------

struct IPolicyConfigVista : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, const PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

bool setDefaultEndpointAllRoles(const QString &deviceId)
{
    if (deviceId.isEmpty())
        return false;

    const std::wstring wide = deviceId.toStdWString();
    const LPCWSTR id = wide.c_str();

    IPolicyConfigVista *vista = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_PolicyConfigClient_Local, nullptr, CLSCTX_ALL,
                                  IID_IPolicyConfigVista_Local,
                                  reinterpret_cast<void **>(&vista));
    if (SUCCEEDED(hr) && vista) {
        const HRESULT h1 = vista->SetDefaultEndpoint(id, eConsole);
        const HRESULT h2 = vista->SetDefaultEndpoint(id, eMultimedia);
        const HRESULT h3 = vista->SetDefaultEndpoint(id, eCommunications);
        vista->Release();
        if (SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3))
            return true;
        qWarning() << "[AUDIO] IPolicyConfigVista SetDefaultEndpoint partial/fail"
                   << "hr=" << hrHex(h1) << hrHex(h2) << hrHex(h3);
    }

    IPolicyConfig *cfg = nullptr;
    hr = CoCreateInstance(CLSID_PolicyConfigClient_Local, nullptr, CLSCTX_ALL,
                          IID_IPolicyConfig_Local, reinterpret_cast<void **>(&cfg));
    if (SUCCEEDED(hr) && cfg) {
        const HRESULT h1 = cfg->SetDefaultEndpoint(id, eConsole);
        const HRESULT h2 = cfg->SetDefaultEndpoint(id, eMultimedia);
        const HRESULT h3 = cfg->SetDefaultEndpoint(id, eCommunications);
        cfg->Release();
        if (SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3))
            return true;
        qWarning() << "[AUDIO] IPolicyConfig SetDefaultEndpoint partial/fail"
                   << "hr=" << hrHex(h1) << hrHex(h2) << hrHex(h3);
        return false;
    }

    qWarning() << "[AUDIO] PolicyConfig CoCreateInstance failed hr=" << hrHex(hr);
    return false;
}

QString friendlyNameOf(IMMDevice *device)
{
    if (!device)
        return QString();
    IPropertyStore *store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store)
        return QString();
    PROPVARIANT var;
    PropVariantInit(&var);
    QString name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName_Local, &var)) && var.vt == VT_LPWSTR && var.pwszVal)
        name = QString::fromWCharArray(var.pwszVal);
    PropVariantClear(&var);
    store->Release();
    return name;
}

UINT formFactorOf(IMMDevice *device)
{
    if (!device)
        return 0;
    IPropertyStore *store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store)
        return 0;
    PROPVARIANT var;
    PropVariantInit(&var);
    UINT ff = 0;
    if (SUCCEEDED(store->GetValue(PKEY_AudioEndpoint_FormFactor_Local, &var))) {
        if (var.vt == VT_UI4)
            ff = var.ulVal;
        else if (var.vt == VT_I4)
            ff = static_cast<UINT>(var.lVal);
    }
    PropVariantClear(&var);
    store->Release();
    return ff;
}

const char *formFactorName(UINT ff)
{
    // EndpointFormFactor (mmdeviceapi.h)
    switch (ff) {
    case 0: return "RemoteNetworkDevice";
    case 1: return "Speakers";
    case 2: return "LineLevel";
    case 3: return "Headphones";
    case 4: return "Microphone";
    case 5: return "Headset";
    case 6: return "Handset";
    case 7: return "UnknownDigitalPassthrough";
    case 8: return "SPDIF";
    case 9: return "DigitalAudioDisplayDevice";
    case 10: return "UnknownFormFactor";
    default: return "Other";
    }
}

bool formFactorIsHeadphones(UINT ff)
{
    return ff == static_cast<UINT>(Headphones) || ff == static_cast<UINT>(Headset);
}

bool nameLooksLikeHeadphones(const QString &friendlyName)
{
    const QString n = friendlyName.toLower();
    static const QStringList needles = {
        QStringLiteral("наушник"),
        QStringLiteral("headphone"),
        QStringLiteral("headset"),
        QStringLiteral("earphone"),
        QStringLiteral("гарнитур"),
    };
    for (const QString &needle : needles) {
        if (n.contains(needle))
            return true;
    }
    return false;
}

bool nameLooksLikeMonitorOutput(const QString &friendlyName)
{
    const QString n = friendlyName.toLower();
    static const QStringList needles = {
        QStringLiteral("hdmi"),
        QStringLiteral("display"),
        QStringLiteral("monitor"),
        QStringLiteral("nvidia"),
        QStringLiteral("amd hdmi"),
        QStringLiteral("intel(r) display"),
        QStringLiteral("digital output"),
        QStringLiteral("spdif"),
        QStringLiteral("optical"),
        QStringLiteral("tv"),
        QStringLiteral("projector"),
    };
    for (const QString &needle : needles) {
        if (n.contains(needle))
            return true;
    }
    return false;
}

bool nameLooksLikeAnalogPreferred(const QString &friendlyName)
{
    const QString n = friendlyName.toLower();
    static const QStringList needles = {
        QStringLiteral("realtek"),
        QStringLiteral("usb"),
        QStringLiteral("audio"),
        QStringLiteral("headset"),
        QStringLiteral("mic"),
        QStringLiteral("jack"),
    };
    for (const QString &needle : needles) {
        if (n.contains(needle))
            return true;
    }
    return false;
}

bool formFactorIsAnalogLike(UINT ff)
{
    // Speakers / LineLevel — typical Realtek 3.5mm jack endpoints.
    return ff == Speakers || ff == LineLevel;
}

struct HeadphonesCandidate {
    QString id;
    QString name;
    UINT formFactor = 0;
    bool byFormFactor = false;
    bool byName = false;
    bool byAnalog = false;
    bool isMonitor = false;
};

enum class PreferredKind { None, HeadphonesForm, HeadphonesName, AnalogNonMonitor };

QString preferredKindLabel(PreferredKind kind)
{
    switch (kind) {
    case PreferredKind::HeadphonesForm: return QStringLiteral("headphones(form)");
    case PreferredKind::HeadphonesName: return QStringLiteral("headphones(name)");
    case PreferredKind::AnalogNonMonitor: return QStringLiteral("analog/non-hdmi");
    default: return QStringLiteral("none");
    }
}

void classifyCandidate(HeadphonesCandidate *c)
{
    if (!c)
        return;
    c->byFormFactor = formFactorIsHeadphones(c->formFactor);
    c->byName = nameLooksLikeHeadphones(c->name);
    c->isMonitor = nameLooksLikeMonitorOutput(c->name)
                   || c->formFactor == DigitalAudioDisplayDevice
                   || c->formFactor == SPDIF;
    c->byAnalog = !c->isMonitor
                  && (formFactorIsAnalogLike(c->formFactor) || nameLooksLikeAnalogPreferred(c->name));
}

HeadphonesCandidate findHeadphonesDevice(IMMDeviceEnumerator *enumerator, const QString &rememberedId,
                                         PreferredKind *outKind, bool logDevices)
{
    HeadphonesCandidate best;
    PreferredKind kind = PreferredKind::None;
    if (outKind)
        *outKind = kind;
    if (!enumerator)
        return best;

    IMMDeviceCollection *collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))
        || !collection)
        return best;

    UINT count = 0;
    collection->GetCount(&count);

    HeadphonesCandidate formHit;
    HeadphonesCandidate nameHit;
    HeadphonesCandidate realtekHit;
    HeadphonesCandidate analogHit;
    HeadphonesCandidate anyNonMonitor;
    int nonMonitorCount = 0;

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device)
            continue;

        HeadphonesCandidate c;
        c.id = deviceIdOf(device);
        c.name = friendlyNameOf(device);
        c.formFactor = formFactorOf(device);
        classifyCandidate(&c);

        if (logDevices) {
            qWarning() << "[AUDIO] devices:"
                       << "id=" << c.id
                       << "name=" << (c.name.isEmpty() ? QStringLiteral("(unnamed)") : c.name)
                       << "formFactor=" << formFactorName(c.formFactor)
                       << (c.isMonitor ? " [monitor/hdmi]" : "")
                       << (c.byFormFactor ? " [hp-form]" : "")
                       << (c.byName ? " [hp-name]" : "");
        }

        if (c.byFormFactor && formHit.id.isEmpty())
            formHit = c;
        if (c.byName && nameHit.id.isEmpty())
            nameHit = c;

        if (!c.isMonitor) {
            ++nonMonitorCount;
            if (anyNonMonitor.id.isEmpty())
                anyNonMonitor = c;
            const QString n = c.name.toLower();
            if (n.contains(QStringLiteral("realtek")) && realtekHit.id.isEmpty())
                realtekHit = c;
            if (c.byAnalog && analogHit.id.isEmpty())
                analogHit = c;
        }

        device->Release();
    }
    collection->Release();

    // a) FormFactor Headphones/Headset
    if (!formHit.id.isEmpty()) {
        kind = PreferredKind::HeadphonesForm;
        best = formHit;
    // b) Name keywords
    } else if (!nameHit.id.isEmpty()) {
        kind = PreferredKind::HeadphonesName;
        best = nameHit;
    // c/d) Best non-monitor: Realtek > analog-looking Speakers/Line > only non-HDMI left
    } else if (!realtekHit.id.isEmpty()) {
        kind = PreferredKind::AnalogNonMonitor;
        best = realtekHit;
    } else if (!analogHit.id.isEmpty()) {
        kind = PreferredKind::AnalogNonMonitor;
        best = analogHit;
    } else if (nonMonitorCount >= 1 && !anyNonMonitor.id.isEmpty()) {
        kind = PreferredKind::AnalogNonMonitor;
        best = anyNonMonitor;
    }

    // Keep last-known id only if it still matches the same preferred pick (stable across polls).
    // Do not short-circuit earlier — a newly plugged USB headset must beat remembered Speakers.
    if (best.id.isEmpty() && !rememberedId.isEmpty()) {
        const std::wstring wid = rememberedId.toStdWString();
        IMMDevice *remembered = nullptr;
        if (SUCCEEDED(enumerator->GetDevice(wid.c_str(), &remembered)) && remembered) {
            DWORD state = 0;
            remembered->GetState(&state);
            if (state == DEVICE_STATE_ACTIVE) {
                HeadphonesCandidate c;
                c.id = rememberedId;
                c.name = friendlyNameOf(remembered);
                c.formFactor = formFactorOf(remembered);
                classifyCandidate(&c);
                if (c.byFormFactor || c.byName || (!c.isMonitor && c.byAnalog)) {
                    best = c;
                    kind = c.byFormFactor ? PreferredKind::HeadphonesForm
                         : c.byName       ? PreferredKind::HeadphonesName
                                          : PreferredKind::AnalogNonMonitor;
                }
            }
            remembered->Release();
        }
    }

    if (outKind)
        *outKind = kind;
    return best;
}

bool nameOrDefaultIsMonitor(IMMDeviceEnumerator *enumerator, const QString &defaultId)
{
    if (!enumerator || defaultId.isEmpty())
        return false;
    const std::wstring wid = defaultId.toStdWString();
    IMMDevice *device = nullptr;
    if (FAILED(enumerator->GetDevice(wid.c_str(), &device)) || !device)
        return nameLooksLikeMonitorOutput(defaultId);
    HeadphonesCandidate c;
    c.id = defaultId;
    c.name = friendlyNameOf(device);
    c.formFactor = formFactorOf(device);
    classifyCandidate(&c);
    device->Release();
    return c.isMonitor;
}

QString defaultRenderId(IMMDeviceEnumerator *enumerator, ERole role)
{
    IMMDevice *device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device)) || !device)
        return QString();
    const QString id = deviceIdOf(device);
    device->Release();
    return id;
}

QString defaultRenderName(IMMDeviceEnumerator *enumerator, ERole role)
{
    IMMDevice *device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device)) || !device)
        return QStringLiteral("(none)");
    QString name = friendlyNameOf(device);
    if (name.isEmpty())
        name = deviceIdOf(device);
    device->Release();
    return name;
}

// Forward-declared so the notification client can poke the guard.
class HeadphonesGuard;

class AudioNotifyClient : public IMMNotificationClient {
public:
    explicit AudioNotifyClient(HeadphonesGuard *owner)
        : m_ref(1)
        , m_owner(owner)
    {
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMMNotificationClient_Local
            || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&m_ref);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG r = InterlockedDecrement(&m_ref);
        if (r == 0)
            delete this;
        return static_cast<ULONG>(r);
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override;

private:
    LONG m_ref;
    HeadphonesGuard *m_owner;
};

class HeadphonesGuard : public QObject {
public:
    explicit HeadphonesGuard(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_timer.setInterval(1500);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this]() { ensureHeadphones(false); });
    }

    ~HeadphonesGuard() override
    {
        stop();
    }

    void start()
    {
        QMutexLocker lock(&m_mutex);
        if (m_running)
            return;

        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_comUninit = (comHr == S_OK || comHr == S_FALSE);

        HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, nullptr, CLSCTX_ALL,
                                      IID_IMMDeviceEnumerator_Local,
                                      reinterpret_cast<void **>(&m_enumerator));
        if (FAILED(hr) || !m_enumerator) {
            qWarning() << "[AUDIO] headphones guard: MMDeviceEnumerator failed hr=" << hrHex(hr);
            if (m_comUninit) {
                CoUninitialize();
                m_comUninit = false;
            }
            return;
        }

        m_notify = new AudioNotifyClient(this);
        hr = m_enumerator->RegisterEndpointNotificationCallback(m_notify);
        if (FAILED(hr)) {
            qWarning() << "[AUDIO] headphones guard: RegisterEndpointNotificationCallback failed hr="
                       << hrHex(hr);
            m_notify->Release();
            m_notify = nullptr;
        }

        m_running = true;
        m_timer.start();
        qWarning() << "[AUDIO] headphones guard started (notify + 1.5s poll)";
        lock.unlock();
        ensureHeadphones(true);
    }

    void stop()
    {
        QMutexLocker lock(&m_mutex);
        if (!m_running && !m_enumerator)
            return;

        m_timer.stop();
        m_running = false;

        if (m_enumerator && m_notify) {
            m_enumerator->UnregisterEndpointNotificationCallback(m_notify);
            m_notify->Release();
            m_notify = nullptr;
        }
        if (m_enumerator) {
            m_enumerator->Release();
            m_enumerator = nullptr;
        }
        if (m_comUninit) {
            CoUninitialize();
            m_comUninit = false;
        }
        qWarning() << "[AUDIO] headphones guard stopped";
    }

    void onHotplug(const QString &reason, const QString &deviceId = QString())
    {
        {
            QMutexLocker lock(&m_mutex);
            // Drop sticky pick so a newly ACTIVE headset can beat remembered Speakers.
            m_lastHeadphonesId.clear();
            m_forceDeviceLog = true;
        }
        if (!deviceId.isEmpty())
            qWarning() << "[AUDIO] hotplug:" << reason << "deviceId=" << deviceId;
        else
            qWarning() << "[AUDIO] hotplug:" << reason;
        ensureHeadphones(true);
    }

    void setPaused(bool paused)
    {
        QMutexLocker lock(&m_mutex);
        m_paused = paused;
    }

    void ensureHeadphones(bool fromStart)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_running || !m_enumerator || m_paused)
            return;

        // Avoid re-entrancy while we ourselves change the default.
        if (m_forcing)
            return;

        bool logDevices = fromStart || m_forceDeviceLog;
        m_forceDeviceLog = false;

        PreferredKind kind = PreferredKind::None;
        HeadphonesCandidate hp =
            findHeadphonesDevice(m_enumerator, m_lastHeadphonesId, &kind, logDevices);

        if (hp.id.isEmpty()) {
            m_lastHeadphonesId.clear();
            // На многих ПК только HDMI — не спамим poll каждые N секунд.
            if (fromStart) {
                if (!logDevices)
                    findHeadphonesDevice(m_enumerator, QString(), &kind, true);
                qWarning() << "[AUDIO] WARN: only HDMI/monitor (or no) preferred output — not forcing default";
            }
            return;
        }

        const QString hpLabel = hp.name.isEmpty() ? hp.id : hp.name;
        const QString curConsole = defaultRenderId(m_enumerator, eConsole);
        const QString curMulti = defaultRenderId(m_enumerator, eMultimedia);
        const QString curComm = defaultRenderId(m_enumerator, eCommunications);
        const QString wasName = defaultRenderName(m_enumerator, eConsole);
        const bool defaultIsMonitor = nameOrDefaultIsMonitor(m_enumerator, curConsole);

        const bool needsForce = (curConsole != hp.id) || (curMulti != hp.id) || (curComm != hp.id);

        if (fromStart || needsForce || m_lastHeadphonesId != hp.id) {
            qWarning() << "[AUDIO] preferred headphones/analog:" << hpLabel
                       << "via" << preferredKindLabel(kind)
                       << "| default was" << wasName
                       << (defaultIsMonitor ? "(monitor/hdmi)" : "")
                       << (needsForce ? "→ will force" : "→ already default");
        }

        m_lastHeadphonesId = hp.id;

        if (!needsForce) {
            if (fromStart) {
                qWarning() << "[AUDIO] default already headphones/analog:" << hpLabel;
            }
            return;
        }

        // If default is HDMI/monitor and we have a non-HDMI candidate → force (already selected).
        m_forcing = true;
        lock.unlock();

        const bool ok = setDefaultEndpointAllRoles(hp.id);

        lock.relock();
        m_forcing = false;

        if (ok) {
            qWarning() << "[AUDIO] preferred headphones/analog:" << hpLabel
                       << "| default was" << wasName << "→ forced";
        } else {
            qWarning() << "[AUDIO] failed to force headphones/analog" << hpLabel;
        }
    }

private:
    friend class AudioNotifyClient;

    QMutex m_mutex;
    QTimer m_timer;
    IMMDeviceEnumerator *m_enumerator = nullptr;
    AudioNotifyClient *m_notify = nullptr;
    QString m_lastHeadphonesId;
    bool m_running = false;
    bool m_forcing = false;
    bool m_paused = false;
    bool m_comUninit = false;
    bool m_forceDeviceLog = true;
};

HRESULT STDMETHODCALLTYPE AudioNotifyClient::OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState)
{
    if (!m_owner)
        return S_OK;
    const QString id = pwstrDeviceId ? QString::fromWCharArray(pwstrDeviceId) : QString();
    if (dwNewState == DEVICE_STATE_ACTIVE) {
        m_owner->onHotplug(QStringLiteral("device ACTIVE (plugged/enabled)"), id);
    } else if (dwNewState == DEVICE_STATE_UNPLUGGED
               || dwNewState == DEVICE_STATE_DISABLED
               || dwNewState == DEVICE_STATE_NOTPRESENT) {
        m_owner->onHotplug(QStringLiteral("device inactive/unplugged"), id);
    } else {
        m_owner->ensureHeadphones(false);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioNotifyClient::OnDeviceAdded(LPCWSTR pwstrDeviceId)
{
    if (!m_owner)
        return S_OK;
    const QString id = pwstrDeviceId ? QString::fromWCharArray(pwstrDeviceId) : QString();
    m_owner->onHotplug(QStringLiteral("OnDeviceAdded"), id);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioNotifyClient::OnDeviceRemoved(LPCWSTR pwstrDeviceId)
{
    if (!m_owner)
        return S_OK;
    const QString id = pwstrDeviceId ? QString::fromWCharArray(pwstrDeviceId) : QString();
    m_owner->onHotplug(QStringLiteral("OnDeviceRemoved"), id);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioNotifyClient::OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR)
{
    if (flow == eRender && m_owner)
        m_owner->ensureHeadphones(false);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioNotifyClient::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY)
{
    return S_OK;
}

HeadphonesGuard *g_headphonesGuard = nullptr;

} // namespace
#endif

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

void win32_start_headphones_guard()
{
#ifdef _WIN32
    if (g_headphonesGuard)
        return;
    QObject *parent = QCoreApplication::instance();
    g_headphonesGuard = new HeadphonesGuard(parent);
    g_headphonesGuard->start();
#else
    // no-op
#endif
}

void win32_stop_headphones_guard()
{
#ifdef _WIN32
    if (!g_headphonesGuard)
        return;
    g_headphonesGuard->stop();
    delete g_headphonesGuard;
    g_headphonesGuard = nullptr;
#endif
}

void win32_set_headphones_guard_paused(int paused)
{
#ifdef _WIN32
    if (g_headphonesGuard)
        g_headphonesGuard->setPaused(paused != 0);
#else
    (void)paused;
#endif
}

#ifdef _WIN32
namespace {

HeadphonesCandidate findSpeakersDevice(IMMDeviceEnumerator *enumerator, bool logDevices)
{
    HeadphonesCandidate best;
    if (!enumerator)
        return best;

    IMMDeviceCollection *collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))
        || !collection)
        return best;

    UINT count = 0;
    collection->GetCount(&count);

    HeadphonesCandidate speakersForm;
    HeadphonesCandidate speakersName;
    HeadphonesCandidate analogNonHp;
    HeadphonesCandidate hdmiFallback;

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device)
            continue;

        HeadphonesCandidate c;
        c.id = deviceIdOf(device);
        c.name = friendlyNameOf(device);
        c.formFactor = formFactorOf(device);
        classifyCandidate(&c);

        if (logDevices) {
            qWarning() << "[AUDIO] speakers-scan:"
                       << "name=" << (c.name.isEmpty() ? QStringLiteral("(unnamed)") : c.name)
                       << "formFactor=" << formFactorName(c.formFactor)
                       << (c.byFormFactor || c.byName ? " [headphones]" : "")
                       << (c.isMonitor ? " [hdmi/monitor]" : "");
        }

        const bool isHp = c.byFormFactor || c.byName;
        if (!isHp && c.formFactor == Speakers && speakersForm.id.isEmpty())
            speakersForm = c;
        if (!isHp) {
            const QString n = c.name.toLower();
            if ((n.contains(QStringLiteral("speaker")) || n.contains(QStringLiteral("колонк")))
                && speakersName.id.isEmpty())
                speakersName = c;
        }
        if (!isHp && !c.isMonitor && c.byAnalog && analogNonHp.id.isEmpty())
            analogNonHp = c;
        if (!isHp && c.isMonitor && hdmiFallback.id.isEmpty())
            hdmiFallback = c;

        device->Release();
    }
    collection->Release();

    if (!speakersForm.id.isEmpty())
        best = speakersForm;
    else if (!speakersName.id.isEmpty())
        best = speakersName;
    else if (!analogNonHp.id.isEmpty())
        best = analogNonHp;
    else if (!hdmiFallback.id.isEmpty())
        best = hdmiFallback;

    return best;
}

} // namespace
#endif

QString win32_find_speakers_device_id()
{
#ifdef _WIN32
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninit = (comHr == S_OK || comHr == S_FALSE);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
        return {};

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator_Local,
                                  reinterpret_cast<void **>(&enumerator));
    QString id;
    if (SUCCEEDED(hr) && enumerator) {
        const HeadphonesCandidate sp = findSpeakersDevice(enumerator, true);
        id = sp.id;
        if (!id.isEmpty())
            qWarning() << "[AUDIO] speakers device:" << (sp.name.isEmpty() ? id : sp.name);
        else
            qWarning() << "[AUDIO] no speakers device found";
        enumerator->Release();
    }

    if (shouldUninit)
        CoUninitialize();
    return id;
#else
    return {};
#endif
}

bool win32_force_output_device(const QString &deviceId)
{
#ifdef _WIN32
    if (deviceId.isEmpty())
        return false;

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninit = (comHr == S_OK || comHr == S_FALSE);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
        return false;

    const bool ok = setDefaultEndpointAllRoles(deviceId);
    if (shouldUninit)
        CoUninitialize();
    qWarning() << "[AUDIO] force output device" << deviceId << "ok=" << ok;
    return ok;
#else
    Q_UNUSED(deviceId);
    return false;
#endif
}

namespace {

struct MasterDuckState {
    bool active = false;
    float savedScalar = 1.0f;
    BOOL savedMute = FALSE;
};

MasterDuckState g_masterDuck;

bool activateEndpointVolume(IMMDevice *device, IAudioEndpointVolume **out)
{
    if (!device || !out)
        return false;
    *out = nullptr;
    IAudioEndpointVolume *endpointVolume = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                  nullptr, reinterpret_cast<void **>(&endpointVolume));
    if (FAILED(hr) || !endpointVolume) {
        hr = device->Activate(IID_IAudioEndpointVolume_Local, CLSCTX_ALL,
                              nullptr, reinterpret_cast<void **>(&endpointVolume));
    }
    if (FAILED(hr) || !endpointVolume)
        return false;
    *out = endpointVolume;
    return true;
}

bool duckOnDevice(IMMDevice *device, float duckScalar)
{
    IAudioEndpointVolume *ev = nullptr;
    if (!activateEndpointVolume(device, &ev))
        return false;

    float scalar = 1.0f;
    BOOL muted = FALSE;
    ev->GetMasterVolumeLevelScalar(&scalar);
    ev->GetMute(&muted);

    if (!g_masterDuck.active) {
        g_masterDuck.savedScalar = scalar;
        g_masterDuck.savedMute = muted;
        g_masterDuck.active = true;
    }

    const HRESULT setHr = ev->SetMasterVolumeLevelScalar(duckScalar, nullptr);
    if (SUCCEEDED(setHr))
        ev->SetMute(FALSE, nullptr);
    ev->Release();
    return SUCCEEDED(setHr);
}

bool restoreOnDevice(IMMDevice *device)
{
    IAudioEndpointVolume *ev = nullptr;
    if (!activateEndpointVolume(device, &ev))
        return false;
    const HRESULT setHr = ev->SetMasterVolumeLevelScalar(g_masterDuck.savedScalar, nullptr);
    if (SUCCEEDED(setHr))
        ev->SetMute(g_masterDuck.savedMute, nullptr);
    ev->Release();
    return SUCCEEDED(setHr);
}

} // namespace

void win32_duck_master(int duckPercent)
{
#ifdef _WIN32
    if (duckPercent < 0) duckPercent = 0;
    if (duckPercent > 100) duckPercent = 100;
    if (g_masterDuck.active)
        return;

    const float duckScalar = static_cast<float>(duckPercent) / 100.0f;
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninit = (comHr == S_OK || comHr == S_FALSE);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        qWarning() << "[AUDIO] duck — CoInitializeEx failed hr=" << hrHex(comHr);
        return;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator_Local,
                                  reinterpret_cast<void **>(&enumerator));
    bool ok = false;
    if (SUCCEEDED(hr) && enumerator) {
        IMMDevice *device = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && device) {
            ok = duckOnDevice(device, duckScalar);
            device->Release();
        }
        if (!ok) {
            device = nullptr;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) && device) {
                ok = duckOnDevice(device, duckScalar);
                device->Release();
            }
        }
        enumerator->Release();
    }

    if (shouldUninit)
        CoUninitialize();

    if (ok)
        qWarning() << "[AUDIO] duck to" << duckPercent << "% (saved"
                   << int(g_masterDuck.savedScalar * 100.0f) << "%)";
    else
        qWarning() << "[AUDIO] duck failed";
#else
    (void)duckPercent;
#endif
}

void win32_restore_master()
{
#ifdef _WIN32
    if (!g_masterDuck.active)
        return;

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninit = (comHr == S_OK || comHr == S_FALSE);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        qWarning() << "[AUDIO] restore — CoInitializeEx failed hr=" << hrHex(comHr);
        g_masterDuck.active = false;
        return;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator_Local,
                                  reinterpret_cast<void **>(&enumerator));
    bool ok = false;
    if (SUCCEEDED(hr) && enumerator) {
        IMMDevice *device = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && device) {
            ok = restoreOnDevice(device);
            device->Release();
        }
        if (!ok) {
            device = nullptr;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) && device) {
                ok = restoreOnDevice(device);
                device->Release();
            }
        }
        enumerator->Release();
    }

    g_masterDuck.active = false;

    if (shouldUninit)
        CoUninitialize();

    if (ok)
        qWarning() << "[AUDIO] volume restored after duck";
    else
        qWarning() << "[AUDIO] restore after duck failed";
#endif
}

void win32_set_master_volume(int level) {
#ifdef _WIN32
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    const float volumeValue = static_cast<float>(level) / 100.0f;

    // Balance COM init with Qt (may already own the apartment on this thread).
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninit = (comHr == S_OK || comHr == S_FALSE);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        qWarning() << "[AUDIO] setVolume" << level << "— CoInitializeEx failed hr="
                   << hrHex(comHr);
        return;
    }

    bool ok = false;
    const char *path = nullptr;

    IMMDeviceEnumerator *deviceEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator_Local,
                                  reinterpret_cast<void **>(&deviceEnumerator));

    if (SUCCEEDED(hr) && deviceEnumerator) {
        if (tryWasapiRole(deviceEnumerator, eConsole, volumeValue, level, "eConsole")) {
            ok = true;
            path = "WASAPI eConsole";
        } else if (tryWasapiRole(deviceEnumerator, eMultimedia, volumeValue, level, "eMultimedia")) {
            ok = true;
            path = "WASAPI eMultimedia";
        }
        deviceEnumerator->Release();
    } else {
        qWarning() << "[AUDIO] setVolume" << level
                   << "— CoCreateInstance MMDeviceEnumerator failed hr=" << hrHex(hr);
    }

    if (!ok && tryWaveOutSetVolume(level)) {
        ok = true;
        path = "waveOutSetVolume";
    }

    if (!ok && tryVolumeKeysRelative(level)) {
        ok = true;
        path = "VK_VOLUME keys";
    }

    if (shouldUninit)
        CoUninitialize();

    if (ok)
        qWarning() << "[AUDIO] setVolume" << level << "— ok via" << path;
    else
        qWarning() << "[AUDIO] setVolume" << level << "— all methods failed";
#else
    (void)level;
#endif
}
