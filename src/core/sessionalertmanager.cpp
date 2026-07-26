#include "sessionalertmanager.h"

#include <QDebug>
#include <QFont>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRasterWindow>
#include <QScreen>
#include <algorithm>
#include <cmath>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <sapi.h>
#endif

namespace {

constexpr int kToastW = 420;
constexpr int kToastH = 88;
constexpr int kSlideMs = 420;
constexpr int kHoldMs = 10000;
constexpr int kAnimIntervalMs = 16;

} // namespace

// Always-on-top right-edge slide panel (visible even when shell is hidden for game).
class SessionWarningToast : public QRasterWindow
{
public:
    SessionWarningToast()
        : QRasterWindow()
    {
        setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        resize(kToastW, kToastH);
        setTitle(QStringLiteral("REACTOR Session Warning"));
        setOpacity(0.98);
        m_anim.setInterval(kAnimIntervalMs);
        QObject::connect(&m_anim, &QTimer::timeout, [this]() { onAnimTick(); });
        m_hold.setSingleShot(true);
        QObject::connect(&m_hold, &QTimer::timeout, [this]() { dismiss(); });
    }

    void showMessage(const QString &text)
    {
        m_text = text;
        m_closing = false;
        m_progress = 0.0;
        repositionHidden();
        setVisible(true);
        show();
        raise();
        raiseTopmostNative();
        update();
        m_hold.stop();
        m_anim.start();
    }

    void dismiss()
    {
        if (!isVisible())
            return;
        m_closing = true;
        m_hold.stop();
        if (!m_anim.isActive())
            m_anim.start();
    }

    void raiseTopmostNative()
    {
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd)
            return;
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
#endif
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRect area(0, 0, width(), height());
        p.fillRect(area, Qt::transparent);

        const QRectF r = QRectF(area).adjusted(1.0, 1.0, -1.0, -1.0);
        QPainterPath path;
        path.addRoundedRect(r, 10.0, 10.0);

        p.setBrush(QColor(3, 7, 4, 240));
        p.setPen(QPen(QColor(0x22, 0xc5, 0x5e, 220), 1.8));
        p.drawPath(path);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x22, 0xc5, 0x5e));
        p.drawRoundedRect(QRectF(10, 18, 4, height() - 36), 2, 2);

        p.setPen(QColor(0xe5, 0xe5, 0xe5));
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(15);
        p.setFont(f);
        p.drawText(QRect(28, 0, width() - 52, height()),
                   Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap,
                   m_text);

        p.setPen(QColor(0x22, 0xc5, 0x5e, 160));
        f.setPixelSize(16);
        p.setFont(f);
        p.drawText(QRect(width() - 32, 8, 24, 24), Qt::AlignCenter, QStringLiteral("×"));
    }

    void mousePressEvent(QMouseEvent *ev) override
    {
        if (ev->button() == Qt::LeftButton)
            dismiss();
    }

private:
    QRect screenGeom() const
    {
        QScreen *screen = QGuiApplication::primaryScreen();
        return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    }

    void applyXFromProgress()
    {
        const QRect geo = screenGeom();
        const int visibleX = geo.x() + geo.width() - width() - 16;
        const int hiddenX = geo.x() + geo.width() + 8;
        const double eased = 1.0 - std::pow(1.0 - m_progress, 3.0);
        const int x = hiddenX + int((visibleX - hiddenX) * eased);
        const int y = geo.y() + geo.height() / 2 - height() / 2 - geo.height() / 10;
        setPosition(x, y);
    }

    void repositionHidden()
    {
        m_progress = 0.0;
        applyXFromProgress();
    }

    void onAnimTick()
    {
        const double step = double(kAnimIntervalMs) / double(kSlideMs);
        if (!m_closing) {
            m_progress = std::min(1.0, m_progress + step);
            applyXFromProgress();
            raiseTopmostNative();
            if (m_progress >= 1.0) {
                m_anim.stop();
                m_hold.start(kHoldMs);
            }
        } else {
            m_progress = std::max(0.0, m_progress - step);
            applyXFromProgress();
            if (m_progress <= 0.0) {
                m_anim.stop();
                hide();
            }
        }
        update();
    }

    QString m_text;
    QTimer m_anim;
    QTimer m_hold;
    double m_progress = 0.0;
    bool m_closing = false;
};

SessionAlertManager::SessionAlertManager(QObject *parent)
    : QObject(parent)
{
    m_tickTimer.setInterval(1000);
    connect(&m_tickTimer, &QTimer::timeout, this, &SessionAlertManager::onTick);

    m_toast = new SessionWarningToast();
    m_toastTopmostTimer.setInterval(1500);
    connect(&m_toastTopmostTimer, &QTimer::timeout, this, [this]() {
        if (m_toast && m_toast->isVisible())
            m_toast->raiseTopmostNative();
    });
}

SessionAlertManager::~SessionAlertManager()
{
    m_tickTimer.stop();
    m_toastTopmostTimer.stop();
    delete m_toast;
    m_toast = nullptr;
}

void SessionAlertManager::startSession(const QString &timeRemaining)
{
    m_remainingSeconds = std::max(0, parseTimeToSeconds(timeRemaining));
    m_warned15 = false;
    m_warned10 = false;
    m_warned5 = false;
    setTimeRemaining(formatSeconds(m_remainingSeconds));
    setSessionActive(m_remainingSeconds > 0);

    if (m_remainingSeconds <= 0) {
        m_tickTimer.stop();
        qWarning() << "[SESSION-ALERT] start ignored — zero/invalid time:" << timeRemaining;
        return;
    }

    // Treat previous as +1s so an exact 15:00 / 10:00 / 5:00 login fires once.
    checkThresholdCrossings(m_remainingSeconds + 1);

    if (!m_tickTimer.isActive())
        m_tickTimer.start();

    qWarning() << "[SESSION-ALERT] started |" << m_timeRemaining
               << "| seconds" << m_remainingSeconds;
}

void SessionAlertManager::reset()
{
    m_tickTimer.stop();
    m_remainingSeconds = 0;
    m_warned15 = false;
    m_warned10 = false;
    m_warned5 = false;
    setTimeRemaining(QStringLiteral("00:00:00"));
    setSessionActive(false);
    if (m_toast)
        m_toast->dismiss();
    m_toastTopmostTimer.stop();
    qWarning() << "[SESSION-ALERT] reset";
}

void SessionAlertManager::requestExtendTime()
{
    qWarning() << "[SESSION-ALERT] extend time stub (no API yet)";
    emit extendTimeRequested();
    showToast(QStringLiteral("Продление времени — скоро"));
    speakRussian(QStringLiteral("Продление времени. Функция скоро будет доступна."));
}

void SessionAlertManager::onTick()
{
    if (!m_sessionActive)
        return;

    const int previous = m_remainingSeconds;
    if (m_remainingSeconds > 0)
        --m_remainingSeconds;

    setTimeRemaining(formatSeconds(m_remainingSeconds));
    checkThresholdCrossings(previous);

    if (m_remainingSeconds <= 0) {
        m_tickTimer.stop();
        setSessionActive(false);
        qWarning() << "[SESSION-ALERT] countdown reached zero";
    }
}

void SessionAlertManager::checkThresholdCrossings(int previousSeconds)
{
    const struct {
        int minutes;
        bool *flag;
    } thresholds[] = {
        {15, &m_warned15},
        {10, &m_warned10},
        {5, &m_warned5},
    };

    for (const auto &th : thresholds) {
        if (*th.flag)
            continue;
        const int edge = th.minutes * 60;
        if (previousSeconds > edge && m_remainingSeconds <= edge) {
            fireWarning(th.minutes);
            *th.flag = true;
        }
    }
}

int SessionAlertManager::parseTimeToSeconds(const QString &value)
{
    const QString t = value.trimmed();
    if (t.isEmpty())
        return 0;

    const QStringList parts = t.split(QLatin1Char(':'));
    bool ok = true;
    auto toInt = [&ok](const QString &s) -> int {
        bool localOk = false;
        const int v = s.trimmed().toInt(&localOk);
        ok = ok && localOk;
        return v;
    };

    if (parts.size() == 3) {
        const int h = toInt(parts[0]);
        const int m = toInt(parts[1]);
        const int s = toInt(parts[2]);
        if (!ok || h < 0 || m < 0 || m > 59 || s < 0 || s > 59)
            return 0;
        return h * 3600 + m * 60 + s;
    }
    if (parts.size() == 2) {
        const int m = toInt(parts[0]);
        const int s = toInt(parts[1]);
        if (!ok || m < 0 || s < 0 || s > 59)
            return 0;
        return m * 60 + s;
    }

    bool numOk = false;
    const int mins = t.toInt(&numOk);
    if (numOk && mins >= 0)
        return mins * 60;
    return 0;
}

QString SessionAlertManager::formatSeconds(int totalSeconds)
{
    const int sec = std::max(0, totalSeconds);
    const int h = sec / 3600;
    const int m = (sec % 3600) / 60;
    const int s = sec % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

QString SessionAlertManager::minutesWord(int n)
{
    const int absN = std::abs(n);
    const int mod100 = absN % 100;
    const int mod10 = absN % 10;
    if (mod100 >= 11 && mod100 <= 14)
        return QStringLiteral("минут");
    if (mod10 == 1)
        return QStringLiteral("минута");
    if (mod10 >= 2 && mod10 <= 4)
        return QStringLiteral("минуты");
    return QStringLiteral("минут");
}

QString SessionAlertManager::warningPhrase(int minutes)
{
    return QStringLiteral("До окончания сессии осталось %1 %2")
        .arg(minutes)
        .arg(minutesWord(minutes));
}

void SessionAlertManager::setTimeRemaining(const QString &value)
{
    if (m_timeRemaining == value)
        return;
    m_timeRemaining = value;
    emit timeRemainingChanged();
}

void SessionAlertManager::setSessionActive(bool active)
{
    if (m_sessionActive == active)
        return;
    m_sessionActive = active;
    emit sessionActiveChanged();
}

void SessionAlertManager::fireWarning(int minutes)
{
    const QString phrase = warningPhrase(minutes);
    qWarning() << "[SESSION-ALERT] warning" << minutes << "|" << phrase;
    showToast(phrase);
    speakRussian(phrase);
    emit warningShown(minutes);
}

void SessionAlertManager::showToast(const QString &text)
{
    if (!m_toast)
        return;
    m_toast->showMessage(text);
    if (!m_toastTopmostTimer.isActive())
        m_toastTopmostTimer.start();
}

bool SessionAlertManager::speakWithSapi(const QString &text)
{
#ifdef Q_OS_WIN
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninit = (hrInit == S_OK);

    ISpVoice *voice = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                        IID_ISpVoice, reinterpret_cast<void **>(&voice));
    if (FAILED(hr) || !voice) {
        if (shouldUninit)
            CoUninitialize();
        return false;
    }

    voice->SetRate(0);
    voice->SetVolume(100);
    const std::wstring wtext = text.toStdWString();
    const HRESULT speakHr = voice->Speak(wtext.c_str(), SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
    voice->Release();
    if (shouldUninit)
        CoUninitialize();
    return SUCCEEDED(speakHr);
#else
    Q_UNUSED(text);
    return false;
#endif
}

bool SessionAlertManager::speakWithPowerShell(const QString &text)
{
#ifdef Q_OS_WIN
    // Escape for single-quoted PowerShell string
    QString escaped = text;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));

    const QString script =
        QStringLiteral(
            "Add-Type -AssemblyName System.Speech; "
            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
            "try { "
            "  $s.SelectVoiceByHints([System.Speech.Synthesis.VoiceGender]::NotSet, "
            "    [System.Speech.Synthesis.VoiceAge]::NotSet, 0, "
            "    [System.Globalization.CultureInfo]::GetCultureInfo('ru-RU')) "
            "} catch {}; "
            "$s.Rate = 0; $s.Volume = 100; "
            "$s.Speak([string]'%1')")
            .arg(escaped);

    return QProcess::startDetached(
        QStringLiteral("powershell"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
         QStringLiteral("-Command"), script});
#else
    Q_UNUSED(text);
    return false;
#endif
}

void SessionAlertManager::speakRussian(const QString &text)
{
#ifdef Q_OS_WIN
    if (speakWithSapi(text)) {
        qWarning() << "[SESSION-ALERT] TTS (SAPI):" << text;
        return;
    }
    if (speakWithPowerShell(text)) {
        qWarning() << "[SESSION-ALERT] TTS (PowerShell Speech):" << text;
        return;
    }
    qWarning() << "[SESSION-ALERT] TTS unavailable — MessageBeep";
    MessageBeep(MB_ICONEXCLAMATION);
#else
    Q_UNUSED(text);
#endif
}
