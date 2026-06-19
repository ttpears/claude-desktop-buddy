// Per-board hardware backend. See board.h for the seam contract. Everything
// here is split on the compile-time board flag; the generic battery/USB/
// brightness/beep paths live inline in board.h.
#include "board.h"

namespace board {

// ---------------------------------------------------------------------------
// Display brightness / screen power (uniform under M5GFX)
// ---------------------------------------------------------------------------
void setBrightness(uint8_t v0_255) { M5.Display.setBrightness(v0_255); }

void screenPower(bool on) {
  if (on) { M5.Display.wakeup(); }
  else    { M5.Display.setBrightness(0); M5.Display.sleep(); }
}

// ---------------------------------------------------------------------------
// Audio — the StickC Plus buzzer and the StickS3 ES8311 speaker both drive
// through M5.Speaker under M5Unified.
// ---------------------------------------------------------------------------
void beep(uint16_t freq, uint16_t durMs) { M5.Speaker.tone((float)freq, durMs); }

void powerOff() { M5.Power.powerOff(); }

#if defined(BOARD_STICKS3)
// ===========================================================================
// M5StickS3 — ESP32-S3 + M5PM1 PMIC, no RTC, no user LED, BMI270 IMU
// ===========================================================================

void begin() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.begin();
  M5.Speaker.setVolume(160);
  // No AXP192 coulomb counter and no MPU6886 gyro-standby register on this
  // board; the IMU here is a BMI270 and M5.begin() has already configured it.
}

// No battery-current sense, internal temp, or coulomb counter on the M5PM1.
bool  hasBatteryCurrent() { return false; }
int   batteryCurrentMa()  { return 0; }
bool  hasInternalTemp()   { return false; }
int   internalTempC()     { return 0; }

bool  hasCoulomb()        { return false; }
void  coulombEnable()     {}
void  coulombClear()      {}
float coulombMah()        { return 0.0f; }

void  attentionLed(bool)  {}   // no user-controllable LED

// Software clock: seed an epoch baseline from the bridge time push and project
// it with millis(). The stored "epoch" is the local-time value treated as UTC
// (never timezone-adjusted again), so gmtime_r reverses it exactly.
//
// newlib here has no timegm(), so convert with the days-from-civil algorithm
// (Howard Hinnant, proleptic Gregorian) — pure integer arithmetic, no TZ.
static time_t tmToEpochUtc(const struct tm* t) {
  int y = t->tm_year + 1900;
  int m = t->tm_mon + 1;
  long d = t->tm_mday;
  y -= (m <= 2);
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static bool     _swSet  = false;
static time_t   _swBase = 0;     // local-time epoch at the last sync
static uint32_t _swMs   = 0;     // millis() at the last sync

void setClock(const struct tm* lt) {
  _swBase = tmToEpochUtc(lt);    // treat fields as-is (already local)
  _swMs   = millis();
  _swSet  = true;
}

bool getClock(struct tm* out) {
  if (!_swSet) return false;
  time_t now = _swBase + (time_t)((millis() - _swMs) / 1000);
  gmtime_r(&now, out);
  return true;
}

bool clockValid() { return _swSet; }

#else
// ===========================================================================
// M5StickC Plus — ESP32 + AXP192 PMIC, BM8563 RTC, buzzer, red LED (GPIO10)
// ===========================================================================
static const int LED_PIN = 10;   // red LED, active-low

void begin() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.begin();
  M5.Speaker.setVolume(160);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // off
  coulombEnable();               // free — rides the already-on battery ADC
  // The buddy only reads the accelerometer; park the MPU6886 gyro (PWR_MGMT_2
  // STBY_G bits) to cut its draw. Only valid on the MPU6886, not a BMI270.
  if (M5.Imu.getType() == m5::imu_mpu6886) {
    Wire1.beginTransmission(0x68);
    Wire1.write(0x6C); Wire1.write(0x07);
    Wire1.endTransmission();
  }
}

bool hasBatteryCurrent() { return true; }
int  batteryCurrentMa() {
  // + when charging, - when discharging, matching the old GetBatCurrent sign.
  return (int)(M5.Power.Axp192.getBatteryChargeCurrent()
             - M5.Power.Axp192.getBatteryDischargeCurrent());
}
bool hasInternalTemp() { return true; }
int  internalTempC()   { return (int)M5.Power.Axp192.getInternalTemperature(); }

// Coulomb counter: control reg 0xB8 (bit7 enable, bit5 clear), 32-bit charge /
// discharge accumulators at 0xB0 / 0xB4. Formula matches the M5StickCPlus lib:
// mAh = 65536 * 0.5 * (charge - discharge) / 3600 / 25 (ADC rate).
static uint32_t axpRead32(uint8_t reg) {
  uint8_t b[4] = {0};
  M5.Power.Axp192.readRegister(reg, b, 4);
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
       | ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

bool  hasCoulomb()    { return true; }
void  coulombEnable() { M5.Power.Axp192.writeRegister8(0xB8, 0x80); }
void  coulombClear()  { M5.Power.Axp192.writeRegister8(0xB8, 0xA0); }  // enable+clear
float coulombMah() {
  int32_t coin  = (int32_t)axpRead32(0xB0);
  int32_t coout = (int32_t)axpRead32(0xB4);
  return 65536.0f * 0.5f * (float)(coin - coout) / 3600.0f / 25.0f;
}

void attentionLed(bool on) { digitalWrite(LED_PIN, on ? LOW : HIGH); }

void setClock(const struct tm* lt) { M5.Rtc.setDateTime(lt); }

bool getClock(struct tm* out) {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) return false;
  *out = dt.get_tm();
  return true;
}

bool clockValid() { return !M5.Rtc.getVoltLow(); }

#endif

}  // namespace board
