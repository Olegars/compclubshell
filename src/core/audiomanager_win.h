// Путь: src/core/audiomanager_win.h
#ifndef AUDIOMANAGER_WIN_H
#define AUDIOMANAGER_WIN_H

#ifdef __cplusplus
extern "C" {
#endif

void win32_init_com();
void win32_uninit_com();
void win32_set_master_volume(int level);

/**
 * Temporarily lower master endpoint volume to duckPercent (0–100), saving prior level+mute.
 * No-op if already ducked. Pair with win32_restore_master().
 */
void win32_duck_master(int duckPercent);
/** Restore volume saved by win32_duck_master(). Safe to call when not ducked. */
void win32_restore_master();

/** Continuously force default playback to headphones (Win32). No-op on other platforms. */
void win32_start_headphones_guard();
void win32_stop_headphones_guard();
/** Skip force-default while mic capture / voice reply is active. */
void win32_set_headphones_guard_paused(int paused);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <QString>

/** Best effort speakers/room output id (not headphones/headset). Empty if none. */
QString win32_find_speakers_device_id();
/** Temporarily set Windows default render device for Console/Multimedia/Communications. */
bool win32_force_output_device(const QString &deviceId);
#endif

#endif // AUDIOMANAGER_WIN_H
