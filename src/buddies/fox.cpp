#include "../buddy.h"
#include "../buddy_common.h"
#include <string.h>

extern M5Canvas spr;

namespace fox {

static const uint16_t COL = 0xFD20;   // orange

// swishing tail off to the lower-right, shared by calm states
static void tail(uint32_t t, uint16_t c) {
  int s = (t / 2) % 4;
  static const char* const T[4] = { "_/", "~/", "_\\", "~\\" };
  buddySetColor(c);
  buddySetCursor(BUDDY_X_CENTER + 24, BUDDY_Y_BASE + 6 + (s & 1));
  buddyPrint(T[s]);
}

// ─── SLEEP ───
static void doSleep(uint32_t t) {
  static const char* const A[5] = { "   /\\__/\\  ", "  ( -  - )  ", "  (      )  ", "   \\____/   ", "   `~~~~`   " };
  static const char* const B[5] = { "            ", "   /\\__/\\  ", "  ( -  - )  ", "  (~~~~~~)  ", "   `____`   " };
  const char* const* P[2] = { A, B };
  static const uint8_t SEQ[] = { 0,0,1,1, 0,0,1,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL);

  int p1 = t % 10, p2 = (t + 5) % 10;
  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER + 20 + p1, BUDDY_Y_OVERLAY + 16 - p1 * 2);
  buddyPrint("z");
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 26 + p2, BUDDY_Y_OVERLAY + 12 - p2);
  buddyPrint("Z");
}

// ─── IDLE ───
static void doIdle(uint32_t t) {
  static const char* const SIT[5]  = { "   /\\__/\\  ", "  (      )  ", "  ( o  o )  ", "   \\ ~~ /   ", "    `--`    " };
  static const char* const BLNK[5] = { "   /\\__/\\  ", "  (      )  ", "  ( -  - )  ", "   \\ ~~ /   ", "    `--`    " };
  static const char* const LFT[5]  = { "   /\\__/\\  ", "  (      )  ", "  (o   o )  ", "   \\ ~~ /   ", "    `--`    " };
  static const char* const RGT[5]  = { "   /\\__/\\  ", "  (      )  ", "  ( o   o)  ", "   \\ ~~ /   ", "    `--`    " };
  const char* const* P[4] = { SIT, BLNK, LFT, RGT };
  static const uint8_t SEQ[] = { 0,0,0,1,0,2,0,3,0,1, 0,0,2,3,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL);
  tail(t, COL);
}

// ─── BUSY ───
static void doBusy(uint32_t t) {
  static const char* const FA[5] = { "   /\\__/\\  ", "  (      )  ", "  ( o  o )  ", "   \\ .. /   ", "    `--`    " };
  static const char* const FB[5] = { "   /\\__/\\  ", "  (      )  ", "  ( o  o )  ", "   \\ vv /   ", "    `--`    " };
  const char* const* P[2] = { FA, FB };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1,0,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL);

  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 22, BUDDY_Y_OVERLAY + 12);
  buddyPrint(DOTS[t % 6]);
  tail(t, COL);
}

// ─── ATTENTION ───
static void doAttention(uint32_t t) {
  static const char* const UP[5] = { "   /\\  /\\   ", "  ( |  | )  ", "  ( O  O )  ", "   \\ !! /   ", "    `--`    " };
  static const char* const LN[5] = { "   /\\  /\\   ", "  (|    |)  ", "  ( O  O )  ", "   \\ !! /   ", "    `--`    " };
  const char* const* P[2] = { UP, LN };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  int xo = (t & 1) ? 1 : -1;
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL, xo);

  if ((t / 2) & 1) { buddySetColor(BUDDY_YEL); buddySetCursor(BUDDY_X_CENTER - 4, BUDDY_Y_OVERLAY - 6); buddyPrint("!"); }
  if ((t / 3) & 1) { buddySetColor(BUDDY_RED); buddySetCursor(BUDDY_X_CENTER + 18, BUDDY_Y_OVERLAY); buddyPrint("!"); }
}

// ─── CELEBRATE ───
static void doCelebrate(uint32_t t) {
  static const char* const HA[5] = { "   /\\__/\\  ", "  (      )  ", "  ( ^  ^ )  ", "   \\ ww /   ", "    `--`    " };
  static const char* const HB[5] = { "  \\/\\__/\\/ ", "  (      )  ", "  ( ^  ^ )  ", "   \\ ww /   ", "    `--`    " };
  const char* const* P[2] = { HA, HB };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1,0,1 };
  static const int8_t YB[] = { 0,-3,0,-3,0,-3,0,-3 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, YB[beat], COL);

  static const uint16_t CC[] = { BUDDY_YEL, BUDDY_CYAN, BUDDY_HEART, BUDDY_GREEN };
  for (int i = 0; i < 5; i++) {
    int ph = (t + i * 3) % 14;
    buddySetColor(CC[i % 4]);
    buddySetCursor(BUDDY_X_CENTER - 22 + i * 11, BUDDY_Y_OVERLAY - 4 + ph);
    buddyPrint(ph & 1 ? "*" : "+");
  }
}

// ─── DIZZY ───
static void doDizzy(uint32_t t) {
  static const char* const D[5] = { "   /\\__/\\  ", "  (      )  ", "  ( @  @ )  ", "   \\ ~~ /   ", "    `--`    " };
  const char* const* P[1] = { D };
  uint8_t beat = (t / 5) % 1;
  int xo = ((t / 2) & 1) ? 2 : -2;
  buddyPrintSprite(P[beat], 5, 0, COL, xo);

  int a = t % 8;
  buddySetColor(BUDDY_YEL);
  buddySetCursor(BUDDY_X_CENTER - 18 + a * 4, BUDDY_Y_OVERLAY - 6 + ((a < 4) ? a : 8 - a));
  buddyPrint("*");
}

// ─── HEART ───
static void doHeart(uint32_t t) {
  static const char* const L1[5] = { "   /\\__/\\  ", "  (      )  ", "  ( ^  ^ )  ", "   \\ -- /   ", "    `--`    " };
  static const char* const L2[5] = { "   /\\__/\\  ", "  ( *  * )  ", "  ( ^  ^ )  ", "   \\ uu /   ", "    `--`    " };
  const char* const* P[2] = { L1, L2 };
  static const uint8_t SEQ[] = { 0,0,1,0, 0,1,0,0 };
  static const int8_t YB[] = { 0,-1,0,-1, 0,-1,0,-1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, YB[beat], COL);

  buddySetColor(BUDDY_HEART);
  for (int i = 0; i < 4; i++) {
    int ph = (t + i * 4) % 16;
    int y = BUDDY_Y_OVERLAY + 14 - ph;
    if (y < -2 || y > BUDDY_Y_BASE) continue;
    buddySetCursor(BUDDY_X_CENTER - 16 + i * 10 + ((ph / 3) & 1) * 2, y);
    buddyPrint("v");
  }
}

}  // namespace fox

extern const Species FOX_SPECIES = {
  "fox",
  0xFD20,
  { fox::doSleep, fox::doIdle, fox::doBusy, fox::doAttention,
    fox::doCelebrate, fox::doDizzy, fox::doHeart }
};
