#ifndef DEVICEPREFS_H
#define DEVICEPREFS_H

/**
 * Windows pointer speed (1–20) and Enhance Pointer Precision.
 * Hardware DPI stays in the mouse. These two follow the player between PCs.
 */
namespace DevicePrefs {

constexpr int kDefaultSpeed = 10;

int mouseSpeed();
bool mouseAccel();
void applyMouse(int speed, bool accel);
void resetMouse();

}

#endif
