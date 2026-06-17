#include "board.h"
#include <LittleFS.h>
#include "esp_sleep.h"
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

M5Canvas spr = M5Canvas(&M5.Display);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = 135, H = 240;
const int CX = W / 2;
const int CY_BASE = 120;

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 2;           // 0..4 → ScreenBreath 20..100; loaded from NVS in setup()
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     idleDim = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
// Idle screen-off timeout (battery), selectable via settings().screenOff.
// 15s leads as the aggressive low end (ToxicOrca's tuned default for battery).
const uint32_t SCREEN_OFF_OPTS[] = { 15000, 30000, 60000, 120000, 300000 };
const char* const SCREEN_OFF_LBL[] = { "15s", "30s", "1m", "2m", "5m" };
// Pre-off dim level, selectable via settings().dim. 0 = no dim step.
const uint8_t SCREEN_DIM_OPTS[] = { 0, 20, 8 };
const char* const SCREEN_DIM_LBL[] = { "off", "dim", "low" };
// Core clock while the screen is off and idle. The BLE controller runs off the
// independent 40MHz XTAL so the link survives well below 80MHz; tuned by
// on-device current measurement. Overridable from the bench build.
#ifndef SCREEN_OFF_CPU_MHZ
#define SCREEN_OFF_CPU_MHZ 40
#endif
// When the screen is off (always on battery), light-sleep the idle gaps instead
// of busy-waiting. The BLE controller's connection survives the ~100ms naps
// (validated by soak), so prompts still wake within a sleep period (<0.5s). The
// CPU never tickless-idles on this core, so this is where the real savings are.
#ifndef SCREEN_OFF_USE_LIGHTSLEEP
#define SCREEN_OFF_USE_LIGHTSLEEP 1
#endif
#ifndef LIGHTSLEEP_US
#define LIGHTSLEEP_US 100000   // 100ms wake cadence: polls buttons + serves BLE
#endif
static uint32_t screenOffMs() { return SCREEN_OFF_OPTS[settings().screenOff]; }
static uint8_t  dimBreath()   { return SCREEN_DIM_OPTS[settings().dim]; }
// Dim ~5s before screen-off (or 2/3 of the way for short timeouts).
static uint32_t screenDimMs() { uint32_t o = screenOffMs(); return o > 5000 ? o - 5000 : o * 2 / 3; }

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Battery-session tracking. The AXP192 coulomb counter integrates real draw in
// hardware (through light sleep), so it gives a true average — unlike the
// instantaneous current, which on the DEVICE page reflects screen-on draw.
// Snapshot the counter when we go on battery; the page shows used→avg→runtime.
uint32_t battSessStartMs = 0;        // 0 = on USB / no active battery session
float    battSessStartCoulomb = 0;   // GetCoulombData() net mAh at unplug

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

// Backlight level on the historic AXP192 0..100 scale, remapped to M5GFX's
// 0..255 so the tuned dim/breath values below carry over unchanged.
static void screenBreath(uint8_t v0_100) { board::setBrightness((uint8_t)((uint16_t)v0_100 * 255 / 100)); }
static void applyBrightness() { screenBreath(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    setCpuFrequencyMhz(160);   // restore full clock for smooth rendering
#ifdef BUDDY_BENCH
    Serial.updateBaudRate(115200);
#endif
    board::screenPower(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
  if (idleDim) { applyBrightness(); idleDim = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) board::beep(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_DEVICE = 3;   // battery %/V/mA + charge state
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "battery", "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 7;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "screen off", "dim", "reset", "back" };
const uint8_t SETTINGS_N = 12;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      brightSave(brightLevel);
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: s.screenOff = (s.screenOff + 1) % 5; break;
    case 9: s.dim       = (s.dim + 1) % 3;       break;
    case 10: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 11: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
// Bottom-sheet menus dock below the character peek. 70 matches PEEK_TOP in
// character.cpp, so the pet's peek window (top 70px) and the sheet (70..H)
// tile the screen with no overlap. The 10-item settings list fills it exactly.
const int SHEET_TOP = 70;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

// Docked bottom-sheet frame shared by the three menus: fill SHEET_TOP..H and
// draw the divider that separates the menu from the live pet peek above it.
static void drawSheet(uint16_t border) {
  spr.fillRect(0, SHEET_TOP, W, H - SHEET_TOP, PANEL);
  spr.drawFastHLine(0, SHEET_TOP, W, border);
  spr.setTextSize(1);
}

// Row pitch that keeps n rows + the hint footer inside the sheet. Comfortable
// 14px until the list gets tall (settings = 12 items), then tightens to fit.
static int sheetPitch(int n) {
  int avail = (H - MENU_HINT_H) - (SHEET_TOP + 6);
  int pitch = avail / n;
  return pitch > 14 ? 14 : pitch;
}

static void drawSettings() {
  const Palette& p = characterPalette();
  drawSheet(p.textDim);
  int pitch = sheetPitch(SETTINGS_N);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    int y = SHEET_TOP + 6 + i * pitch;
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(6, y);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(W - 36, y);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    } else if (i == 8) {
      spr.print(SCREEN_OFF_LBL[s.screenOff]);
    } else if (i == 9) {
      spr.print(SCREEN_DIM_LBL[s.dim]);
    }
  }
  drawMenuHints(p, 0, W, H - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  drawSheet(HOT);
  int pitch = sheetPitch(RESET_N);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    int y = SHEET_TOP + 6 + i * pitch;
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(6, y);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, 0, W, H - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0:   // battery — jump to the DEVICE info page (%/V/mA/charge)
    case 3:   // help
    case 4:   // about
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 0) ? INFO_PG_DEVICE
               : (menuSel == 3) ? INFO_PG_BUTTONS
                                : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 1: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 2: board::powerOff(); break;
    case 5: dataSetDemo(!dataDemo()); break;
    case 6: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  drawSheet(p.textDim);
  int pitch = sheetPitch(MENU_N);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    int y = SHEET_TOP + 6 + i * pitch;
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(6, y);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 0) {
      // Live battery % inline so it's readable without leaving the menu.
      // Same coarse linear estimate as the DEVICE page (reads high on USB).
      spr.printf("  %d%%", board::batteryPercent());
    }
    if (i == 5) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, 0, W, H - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static struct tm       _clk = {};
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = board::onUsb();
  board::getClock(&_clk);
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return ((_clk.tm_wday % 7) + 7) % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clk.tm_hour, _clk.tm_min);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clk.tm_sec);
  uint8_t mi = (_clk.tm_mon >= 0 && _clk.tm_mon <= 11) ? _clk.tm_mon : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clk.tm_mday);

  if (clockOrient == 0) {
    paintedOrient = 0;
    // Bottom half — buddy naturally lives at y=0..82, GIF peeks at top
    // via peek mode. Clearing from 90 leaves both untouched.
    spr.fillRect(0, 90, W, H - 90, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 140);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 175);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 200);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: 240×135 direct-to-LCD. Full fill only on entry; after that
  // text glyph bg cells repaint themselves and the pet box (small, ~90×50)
  // gets a fillRect each pet tick — small enough not to tear.
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clk.tm_sec != lastSec) {
    lastSec = _clk.tm_sec;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clk.tm_mday);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clk.tm_sec);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 170, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 170, 72);
                                                                  M5.Lcd.drawString(wdl, 170, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet on left at 5 fps. Clear includes the overlay-particle zone above
  // the body (y<30) — species draw Zzz/hearts there via BUDDY_Y_OVERLAY=6
  // which doesn't go through _yb, so the box has to cover it.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (buddyMode) {
      // ASCII glyphs don't self-clear; wipe the box each tick. Species
      // hardcode BUDDY_X_CENTER=67 / BUDDY_Y_OVERLAY=6 for particles so
      // keep portrait coords and just swap the surface — pet lands
      // upper-left of landscape, which is where we want it anyway.
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // Full-frame GIFs paint every pixel (transparent → pal.bg), so a
      // per-tick clear just adds a visible black flash between wipe and
      // last scanline. The entry fillScreen on paintedOrient change
      // already covers the surround.
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(0);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A");
    spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap = screen off");
    ln("    hold 6s = off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = board::batteryMilliVolts();
    int vBus_mV = board::usbMilliVolts();
    int pct = board::batteryPercent();
    bool usb = board::onUsb();
    bool haveCur = board::hasBatteryCurrent();
    int iBat_mA = haveCur ? board::batteryCurrentMa() : 0;
    // With a battery-current sense (AXP192) we can tell charging from full;
    // without one (M5PM1) fall back to the PMIC charge flag + a near-full %.
    bool charging = usb && (haveCur ? iBat_mA > 1 : board::isCharging());
    bool full = usb && (haveCur ? (vBat_mV > 4100 && iBat_mA < 10)
                                : (!board::isCharging() && pct >= 99));

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    if (haveCur) ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    // On battery: coulomb-integrated average draw + projected runtime. This is
    // the true idle-inclusive figure; the line above is just the screen-on now.
    // Only on boards with a hardware coulomb counter (AXP192).
    if (board::hasCoulomb() && !usb && battSessStartMs) {
      uint32_t es = (millis() - battSessStartMs) / 1000;
      float used = battSessStartCoulomb - board::coulombMah();   // mAh out
      if (used < 0) used = 0;
      ln("  on batt  %luh%02lum", (unsigned long)(es / 3600), (unsigned long)((es / 60) % 60));
      if (es > 30 && used > 0.05f) {
        float avg = used * 3600.0f / es;                 // mA, hardware-integrated
        ln("  avg draw %dmA", (int)(avg + 0.5f));
        float rem = (pct / 100.0f) * 120.0f / avg;       // h left at this average
        ln("  ~%dh%02dm left", (int)rem, (int)((rem - (int)rem) * 60));
      }
    }
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    if (board::hasInternalTemp()) ln("  temp     %dC", board::internalTempC());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
#if defined(BOARD_STICKS3)
    ln("M5StickS3");
    ln("ESP32-S3 + M5PM1");
#else
    ln("M5StickC Plus");
    ln("ESP32 + AXP192");
#endif
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Size 2 only if it fits one line (~10 chars at 12px on 135px screen)
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~21 chars to two lines under the tool name
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(4, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(4, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, H - 12);
    spr.print("B: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int SHOW = 10, LH = 8, WIDTH = 21;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  // Reset scroll on new output, but do NOT wake(): normal session output must
  // not block sleep. Attention (prompt / waiting) wakes the screen elsewhere.
  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, H - LH - 2);
    spr.print(tama.msg);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[32][24];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(4, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  // board::begin() runs M5.begin(), the speaker, the coulomb counter (free —
  // rides the always-on battery-current ADC), the LED pin, and the MPU6886
  // gyro-standby tweak where applicable.
  board::begin();
#ifdef BUDDY_BENCH
  // Disable battery charging so VBUS input current ≈ pure system draw — with
  // charging on, the ~85mA charge current masks the CPU/radio savings we want
  // to measure. AXP192 reg 0x33 bit7 = charge enable.
  { uint8_t r = M5.Power.Axp192.readRegister8(0x33); M5.Power.Axp192.writeRegister8(0x33, r & ~0x80); }
#endif
#ifdef BENCH_BATMETER
  // Ground-truth battery meter: the AXP192 coulomb counter integrates real
  // discharge in hardware (through light sleep). On boot, dump the previous
  // run's result; the counter is cleared at the unplug edge in loop().
  LittleFS.begin(true);
  board::coulombEnable();
  { File f = LittleFS.open("/batlog.txt", "r");
    Serial.print("PREV-RUN ");
    if (f) { while (f.available()) Serial.write(f.read()); f.close(); }
    else Serial.println("(none)"); }
#endif
  M5.Lcd.setRotation(0);
#ifdef BUDDY_BENCH
  { float ax,ay,az; M5.Imu.getAccelData(&ax,&ay,&az);
    Serial.printf("IMU accel after gyro-standby: %.2f %.2f %.2f g (|a|=%.2f)\n",
      ax,ay,az, sqrtf(ax*ax+ay*ay+az*az)); }
#endif
#ifndef BENCH_NO_BLE
  startBt();
#endif
  brightLevel = brightLoad();
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  spr.createSprite(W, H);
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  t++;
  uint32_t now = millis();

#ifdef BENCH_BATMETER
  // True battery-current measurement: clear the coulomb counter the moment USB
  // is pulled, then log avg discharge (mAh/elapsed) to flash every 10s. Read it
  // back on the next boot via PREV-RUN. Integrates the light-sleep duty cycle.
  {
    static bool wasVbus = true; static uint32_t unplugMs = 0, lastLog = 0;
    bool vbus = board::onUsb();
    if (wasVbus && !vbus) { board::coulombClear(); unplugMs = now; lastLog = now; }
    wasVbus = vbus;
    if (!vbus && unplugMs && now - lastLog >= 10000) {
      lastLog = now;
      float dis = -board::coulombMah();             // mAh discharged since unplug
      uint32_t el = (now - unplugMs) / 1000;        // seconds on battery
      float mA = el ? dis * 3600.0f / el : 0;
      File f = LittleFS.open("/batlog.txt", "w");
      if (f) { f.printf("avg=%.1fmA discharged=%.2fmAh elapsed=%lus vbat=%.2fV\n",
                        mA, dis, (unsigned long)el, board::batteryMilliVolts() / 1000.0f); f.close(); }
    }
  }
#endif

#ifdef BUDDY_BENCH
  // Power telemetry: VBUS input current ≈ whole-system draw when the battery
  // is near full (charging current ~0). Compare ibus across builds for the
  // relative CPU/radio savings; spot-check absolute ibat on battery at the end.
  static uint32_t lastPwr = 0;
  if (now - lastPwr >= 2000) {
    lastPwr = now;
    // Burst-average VBUS current over ~400ms to smooth BLE radio bursts. The
    // CPU never tickless-sleeps in delay() on this core, so busy-sampling at
    // the live clock is representative of the real idle draw, and the method
    // is identical across builds so deltas are apples-to-apples.
    // Light-sleep builds: single sample so the loop stays ~99% asleep
    // (realistic duty cycle) to observe true BLE link behavior.
#if SCREEN_OFF_USE_LIGHTSLEEP
    float sum = 0; const int N = 1;
#else
    float sum = 0; const int N = 200;
#endif
    for (int i = 0; i < N; i++) { sum += M5.Power.Axp192.getVBUSCurrent(); delay(2); }
    Serial.printf("PWR ibus_avg=%.1fmA vbat=%.2fV cpu=%uMHz so=%d nap=%d dim=%d ble=%d st=%s\n",
      sum / N, M5.Power.Axp192.getBatteryVoltage(), (unsigned)getCpuFrequencyMhz(),
      screenOff, napping, idleDim, bleConnected(), stateNames[activeState]);
  }
#endif

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (activeState == P_ATTENTION && settings().led) {
    board::attentionLed((now / 400) % 2);
  } else {
    board::attentionLed(false);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 200) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp — don't seize the screen. An open menu
      // keeps the display (the dispatch shows the approval once it closes); on
      // the home/info/pet screen the approval surfaces right away.
    }
  }

  // A session newly needing attention (waiting goes 0 -> >0) lights the screen
  // once; it then sleeps on the idle timer if ignored, with the LED flashing
  // (P_ATTENTION) as the persistent signal. Prompt arrivals wake above.
  // Gate on `connected` and freeze across disconnects: when data goes stale
  // (dataConnected() flips after 30s) dataPoll zeroes sessionsWaiting, so the
  // bridge's next update would otherwise look like a fresh 0->>0 edge and
  // re-alert a session that was never dismissed.
  static uint8_t lastWaiting = 0;
  if (tama.connected) {
    if (tama.sessionsWaiting > 0 && lastWaiting == 0) wake();
    lastWaiting = tama.sessionsWaiting;
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    // First press while off OR dimmed is "just wake" — swallow its action so it
    // doesn't also open the menu. (idleDim is still set here; wake() clears it.)
    if (screenOff || idleDim) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // Power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via PMIC hardware.
  if (M5.BtnPWR.wasClicked()) {
    if (screenOff) {
      wake();
    } else {
      board::screenPower(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
#ifdef BUDDY_BENCH
  // Bench builds force the battery/idle power path while USB-tethered so the
  // screen-off + downclock + sleep logic can be measured with the AXP192
  // current meter. Without this the device just shows the charging clock.
  _onUsb = false;
#endif
  // Just unplugged (USB -> battery): start the dim/off countdown fresh so we
  // begin with the screen ON, not instantly past the timeout from idle USB time.
  static bool wasOnUsb = true;
  if (wasOnUsb && !_onUsb) lastInteractMs = millis();
  wasOnUsb = _onUsb;
  // Battery-session window for the coulomb-based avg/runtime on the DEVICE page.
  // Reset on USB; (re)start on battery — covers both unplug and boot-on-battery.
  if (_onUsb) {
    battSessStartMs = 0;
  } else if (battSessStartMs == 0) {
    battSessStartMs = millis();
    battSessStartCoulomb = board::coulombMah();
  }
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  static uint32_t lastOrientCheck = 0;
  if (clocking && now - lastOrientCheck >= 200) {
    lastOrientCheck = now;
    clockUpdateOrient();
  }
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clk.tm_hour;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // Lower-screen owner, by priority: an open menu/settings/reset overlay (2)
  // outranks a pending approval (1), which outranks the normal/info/pet view
  // (0). So an approval that lands mid-menu waits — the menu keeps the screen
  // and the approval surfaces only once the overlay closes. Wipe the sprite on
  // each ownership change: the pet renderers only repaint their small box, and
  // the menu sheet, drawApproval (bottom 78px), and the HUD claim overlapping
  // parts of the lower screen, so without a full clear their pixels bleed
  // through. A menu peeks the pet up top for preview; an approval shows it
  // full-size like the classic alert.
  bool overlayNow  = menuOpen || settingsOpen || resetOpen;
  bool approvalNow = tama.promptId[0];
  uint8_t lowerOwner = overlayNow ? 2 : (approvalNow ? 1 : 0);
  static uint8_t prevLowerOwner = 0;
  if (lowerOwner != prevLowerOwner) {
    bool peek = overlayNow || (lowerOwner == 0 && displayMode != DISP_NORMAL);
    characterSetPeek(peek);
    buddySetPeek(peek);
    spr.fillSprite(characterPalette().bg);
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
  }
  prevLowerOwner = lowerOwner;

  if (napping || screenOff || landscapeClock) {
    // skip sprite render — face-down, powered off, or landscape clock
    // (which draws direct-to-LCD below)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscapeClock) {
    drawClock();
  } else if (!napping && !screenOff) {
    // Throttle SPI display pushes: 5 FPS when idle (matches animation tick),
    // full rate when anything interactive is on screen. Cuts SPI traffic by
    // ~90% during idle. Keep our richer draw stack (overlay/approval/menu peek).
    static uint32_t lastPushMs = 0;
    bool active = tama.sessionsRunning > 0 || tama.sessionsWaiting > 0
               || overlayNow || approvalNow || inPrompt || blePasskey();
    bool pushDue = active || (now - lastPushMs >= 200);
    if (pushDue) {
      if (blePasskey()) drawPasskey();
      else if (overlayNow) {
        // Menu owns the screen; a pending approval waits until it closes. Pet is
        // peeked above; dock the active menu sheet below it.
        if (resetOpen) drawReset();
        else if (settingsOpen) drawSettings();
        else drawMenu();
      }
      else if (approvalNow) drawApproval();   // surfaces over home/info/pet/clock
      else if (clocking) drawClock();
      else if (displayMode == DISP_INFO) drawInfo();
      else if (displayMode == DISP_PET) drawPet();
      else if (settings().hud) drawHUD();
      spr.pushSprite(0, 0);
      lastPushMs = now;
    }
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  // Polled at 5Hz — plenty for gravity-based detection, saves IMU power.
  static int8_t faceDownFrames = 0;
  static uint32_t lastFaceDownCheck = 0;
  if (!inPrompt && now - lastFaceDownCheck >= 200) {
    lastFaceDownCheck = now;
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    screenBreath(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  // Pre-off dim: on battery, drop the backlight a few seconds before the full
  // screen-off below. Saves backlight in the idle window + signals "sleeping".
  // Skipped while napping (face-down owns its own ScreenBreath).
  if (!screenOff && !idleDim && !napping && !_onUsb && dimBreath() > 0
      && millis() - lastInteractMs > screenDimMs()) {
    screenBreath(dimBreath());
    idleDim = true;
  }

  if (!screenOff && !_onUsb
      && millis() - lastInteractMs > screenOffMs()) {
    board::screenPower(false);
    screenOff = true;
    // Idle on battery: drop the core clock hard. BLE keeps advertising/serving
    // (its controller clock is the independent 40MHz XTAL), so the bridge still
    // delivers prompts — we just stop burning cycles on a slow idle tick.
    setCpuFrequencyMhz(SCREEN_OFF_CPU_MHZ);
#ifdef BUDDY_BENCH
    Serial.updateBaudRate(115200);   // APB changed; keep telemetry readable
#endif
  }

  // Adaptive frame rate: fast when active, slow when idle, slowest when the
  // screen is off — where it light-sleeps the idle gaps, stacking with the
  // 40MHz screen-off CPU drop above for the bulk of the battery savings.
  // (wake() restores 160MHz.)
  if (screenOff || napping) {
#if SCREEN_OFF_USE_LIGHTSLEEP
    esp_sleep_enable_timer_wakeup(LIGHTSLEEP_US);
    esp_light_sleep_start();
#ifdef BUDDY_BENCH
    // First op on wake: the AXP192 current ADC kept sampling while the ESP32
    // was asleep, so its register still holds the sleep-state draw. Reading it
    // here — before the wake ramp does any work — captures the sleep floor.
    { float i = M5.Power.Axp192.getVBUSCurrent();
      static float sum = 0; static uint32_t n = 0, last = 0;
      sum += i; n++;
      if (millis() - last >= 2000) { last = millis();
        Serial.printf("SLEEPFLOOR ibus=%.1fmA n=%lu vbat=%.2f cpu=%uMHz\n",
          sum / n, (unsigned long)n, M5.Power.Axp192.getBatteryVoltage(), (unsigned)getCpuFrequencyMhz());
        sum = 0; n = 0; }
    }
#endif
#else
    delay(500);
#endif
  } else if (tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
             && !menuOpen && !settingsOpen && !resetOpen && !inPrompt) {
    delay(100);   // ~10 FPS idle — clock, pet animation, face-down check
  } else {
    delay(16);    // ~60 FPS active — responsive UI during sessions/prompts
  }
}
