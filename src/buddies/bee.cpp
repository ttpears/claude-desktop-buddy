#include "../buddy.h"
#include "../buddy_common.h"
#include <M5StickCPlus.h>
#include <string.h>

extern TFT_eSprite spr;

namespace bee {

static const uint16_t COL = 0xFEA0;   // amber-yellow

// ─── SLEEP ───  folded on a little perch
static void doSleep(uint32_t t) {
  static const char* const A[5] = { "   \\  /    ", "  ( -- )   ", "  ((==))   ", "   (__)    ", "   ~~~~~   " };
  static const char* const B[5] = { "            ", "   \\  /    ", "  ( -- )   ", "  ((==))   ", "  ~(__)~   " };
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

// ─── IDLE ───  gentle hover, wings flap, occasional blink
static void doIdle(uint32_t t) {
  static const char* const WO[5] = { "   \\  /    ", "  ( oo )   ", " <((==))>  ", "   (__)    ", "    \\/     " };
  static const char* const WF[5] = { "   \\  /    ", "  ( oo )   ", " =((==))=  ", "   (__)    ", "    \\/     " };
  static const char* const BL[5] = { "   \\  /    ", "  ( -- )   ", " <((==))>  ", "   (__)    ", "    \\/     " };
  const char* const* P[3] = { WO, WF, BL };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1, 2,0, 1,0,1,0 };
  static const int8_t YB[] = { 0,-1,0,-1,0,-1, 0,0, -1,0,-1,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, YB[beat], COL);
}

// ─── BUSY ───  fast flight, motion lines + buzz
static void doBusy(uint32_t t) {
  static const char* const A[5] = { "   \\  /    ", "  ( oo )   ", " <((==))>  ", "   (__)    ", "    \\/     " };
  static const char* const B[5] = { "   \\  /    ", "  ( oo )   ", " =((==))=  ", "   (__)    ", "    \\/     " };
  const char* const* P[2] = { A, B };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1,0,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  int xo = (t & 1) ? 1 : -1;
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL, xo);

  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER - 30, BUDDY_Y_BASE + 4);
  buddyPrint((t & 1) ? "~~" : " ~");
  buddySetColor(BUDDY_YEL);
  buddySetCursor(BUDDY_X_CENTER + 18, BUDDY_Y_OVERLAY + 12);
  buddyPrint((t / 2) & 1 ? "bzz" : "   ");
}

// ─── ATTENTION ───  alert hover, ! pulses
static void doAttention(uint32_t t) {
  static const char* const A[5] = { "   \\  /    ", "  ( OO )   ", " <((!!))>  ", "   (__)    ", "    \\/     " };
  static const char* const B[5] = { "   \\  /    ", "  ( OO )   ", " =((!!))=  ", "   (__)    ", "    \\/     " };
  const char* const* P[2] = { A, B };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  int xo = (t & 1) ? 2 : -2;
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL, xo);

  if ((t / 2) & 1) { buddySetColor(BUDDY_YEL); buddySetCursor(BUDDY_X_CENTER - 6, BUDDY_Y_OVERLAY - 6); buddyPrint("!"); }
  if ((t / 3) & 1) { buddySetColor(BUDDY_RED); buddySetCursor(BUDDY_X_CENTER + 16, BUDDY_Y_OVERLAY - 2); buddyPrint("!"); }
}

// ─── CELEBRATE ───  loops + confetti
static void doCelebrate(uint32_t t) {
  static const char* const A[5] = { "   \\  /    ", "  ( ^^ )   ", " <((==))>  ", "   (__)    ", "    \\/     " };
  static const char* const B[5] = { "   \\  /    ", "  ( ^^ )   ", " =((==))=  ", "   (__)    ", "    \\/     " };
  const char* const* P[2] = { A, B };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1,0,1 };
  static const int8_t YB[] = { 0,-3,0,-3,0,-3,0,-3 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  int xo = ((t / 2) % 4) - 2;
  buddyPrintSprite(P[SEQ[beat]], 5, YB[beat], COL, xo * 2);

  static const uint16_t CC[] = { BUDDY_YEL, BUDDY_CYAN, BUDDY_HEART, BUDDY_GREEN };
  for (int i = 0; i < 5; i++) {
    int ph = (t + i * 3) % 14;
    buddySetColor(CC[i % 4]);
    buddySetCursor(BUDDY_X_CENTER - 22 + i * 11, BUDDY_Y_OVERLAY - 4 + ph);
    buddyPrint(ph & 1 ? "*" : "+");
  }
}

// ─── DIZZY ───  spiral eyes, wobble, stars
static void doDizzy(uint32_t t) {
  static const char* const D[5] = { "   \\  /    ", "  ( @@ )   ", " ~((==))~  ", "   (__)    ", "    \\/     " };
  const char* const* P[1] = { D };
  int xo = ((t / 2) & 1) ? 3 : -3;
  buddyPrintSprite(P[0], 5, 0, COL, xo);

  int a = t % 8;
  buddySetColor(BUDDY_YEL);
  buddySetCursor(BUDDY_X_CENTER - 18 + a * 4, BUDDY_Y_OVERLAY - 6 + ((a < 4) ? a : 8 - a));
  buddyPrint("*");
}

// ─── HEART ───  floaty, hearts
static void doHeart(uint32_t t) {
  static const char* const A[5] = { "   \\  /    ", "  ( ^^ )   ", " <((==))>  ", "   (__)    ", "    \\/     " };
  static const char* const B[5] = { "   \\  /    ", "  ( ^^ )   ", " =((==))=  ", "   (__)    ", "    \\/     " };
  const char* const* P[2] = { A, B };
  static const uint8_t SEQ[] = { 0,1,0,1 };
  static const int8_t YB[] = { 0,-1,0,-1 };
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

}  // namespace bee

extern const Species BEE_SPECIES = {
  "bee",
  0xFEA0,
  { bee::doSleep, bee::doIdle, bee::doBusy, bee::doAttention,
    bee::doCelebrate, bee::doDizzy, bee::doHeart }
};
