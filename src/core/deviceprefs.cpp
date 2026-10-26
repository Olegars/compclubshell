#include "deviceprefs.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace DevicePrefs {

int mouseSpeed()
{
#ifdef Q_OS_WIN
    int speed = kDefaultSpeed;
    if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0))
        return kDefaultSpeed;
    return qBound(1, speed, 20);
#else
    return kDefaultSpeed;
#endif
}

bool mouseAccel()
{
#ifdef Q_OS_WIN
    int params[3] = {0, 0, 0};
    if (!SystemParametersInfoW(SPI_GETMOUSE, 0, params, 0))
        return false;
    return params[2] != 0;
#else
    return false;
#endif
}

void applyMouse(int speed, bool accel)
{
#ifdef Q_OS_WIN
    const int clamped = qBound(1, speed, 20);
    SystemParametersInfoW(
        SPI_SETMOUSESPEED,
        0,
        reinterpret_cast<void *>(static_cast<INT_PTR>(clamped)),
        SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    int params[3];
    if (accel) {
        params[0] = 6;
        params[1] = 10;
        params[2] = 1;
    } else {
        params[0] = 0;
        params[1] = 0;
        params[2] = 0;
    }
    SystemParametersInfoW(SPI_SETMOUSE, 0, params, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
#else
    Q_UNUSED(speed);
    Q_UNUSED(accel);
#endif
}

void resetMouse()
{
    applyMouse(kDefaultSpeed, false);
}

}
