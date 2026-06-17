#pragma once
// Hardware abstraction seam for the buddy firmware.
//
// Both supported boards run on M5Unified; this header hides the handful of
// things that genuinely differ between them:
//
//   M5StickC Plus  (BOARD_STICKC_PLUS) : ESP32   + AXP192 PMIC, BM8563 RTC,
//                                        buzzer, red LED on GPIO10
//   M5StickS3      (BOARD_STICKS3)     : ESP32-S3 + M5PM1 PMIC, no RTC,
//                                        ES8311 speaker, no user LED
//
// The board is selected at compile time by the per-env flag the PlatformIO
// env defines. Battery voltage/level/USB/brightness/beep are uniform across
// M5Unified and stay inline here; only the AXP192-only extras (coulomb counter,
// battery current, internal temp), the LED, and the clock source diverge and
// live in board.cpp behind #ifdef.
#include <M5Unified.h>
#include <time.h>

#if !defined(BOARD_STICKC_PLUS) && !defined(BOARD_STICKS3)
#define BOARD_STICKC_PLUS 1   // default target keeps the original device
#endif

// Both M5.Display and an M5Canvas sprite derive from LovyanGFX, so render
// helpers that used to take a TFT_eSPI* now take a GfxSurface*.
using GfxSurface = LovyanGFX;

namespace board {

// ---- lifecycle ----
void begin();                  // M5.begin + per-board power/IMU/charge setup

// ---- battery / power (generic across both PMICs via M5.Power) ----
inline int  batteryMilliVolts() { int mv = M5.Power.getBatteryVoltage(); return mv < 0 ? 0 : mv; }
// Coarse linear voltage estimate, 3.2V..4.2V -> 0..100%. Matches the historic
// DEVICE-page figure (reads high on USB); also the S3's only battery gauge.
inline int  batteryPercent()    { int p = (batteryMilliVolts() - 3200) / 10; return p < 0 ? 0 : p > 100 ? 100 : p; }
inline int  usbMilliVolts()     { int mv = M5.Power.getVBUSVoltage(); return mv < 0 ? 0 : mv; }
inline bool onUsb()             { return usbMilliVolts() > 4000; }
inline bool isCharging()        { return M5.Power.isCharging() == m5::Power_Class::is_charging; }
void powerOff();

// ---- AXP192-only battery extras (return false / 0 where unsupported) ----
bool  hasBatteryCurrent();
int   batteryCurrentMa();      // signed: + charging, - discharging
bool  hasInternalTemp();
int   internalTempC();

// ---- coulomb counter (AXP192 only; powers avg-draw / runtime estimate) ----
bool  hasCoulomb();
void  coulombEnable();
void  coulombClear();
float coulombMah();            // net mAh (charge - discharge) since last clear

// ---- audio + LED ----
void  beep(uint16_t freq, uint16_t durMs);
void  attentionLed(bool on);   // no-op on boards without a user LED

// ---- display brightness / screen power ----
void  setBrightness(uint8_t v0_255);
void  screenPower(bool on);    // backlight + panel sleep/wake

// ---- clock / time-of-day ----
// setClock() seeds local time from the BLE bridge's {"time":[...]} push.
// getClock() returns current local time. On the StickC Plus this is the BM8563
// RTC; on the StickS3 (no RTC) it's a millis-based software clock that resets
// on reboot until the bridge re-pushes time.
void  setClock(const struct tm* lt);
bool  getClock(struct tm* out);  // false until first sync
bool  clockValid();

}  // namespace board
