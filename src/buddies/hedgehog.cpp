#include "../buddy.h"
#include "../buddy_common.h"
#include <M5StickCPlus.h>
#include <string.h>

extern TFT_eSprite spr;

namespace hedgehog {

static const uint16_t COL = 0xCA8B;   // tan
static const uint16_t NOSE = 0xF810;  // pink

// ─── SLEEP ───  curled into a spiky ball
static void doSleep(uint32_t t) {
  static const char* const A[5] = { "            ", "  ^^^^^^^^  ", " (^^^^^^^^) ", "  ^^^^^^^^  ", "   `----`   " };
  static const char* const B[5] = { "   ^^^^^^   ", "  ^^^^^^^^  ", " (^^^^^^^^) ", "  ~^^^^^~~  ", "   `----`   " };
  const char* const* P[2] = { A, B };
  static const uint8_t SEQ[] = { 0,0,1,1, 0,0,1,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL);

  int p1 = t % 10, p2 = (t + 5) % 10;
  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER + 22 + p1, BUDDY_Y_OVERLAY + 16 - p1 * 2);
  buddyPrint("z");
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 28 + p2, BUDDY_Y_OVERLAY + 12 - p2);
  buddyPrint("Z");
}

// ─── IDLE ───  face out, blink, nose twitch, glance
static void doIdle(uint32_t t) {
  static const char* const SIT[5]  = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " ( o  o )~ ", "  `----`   ", "   ''  ''  " };
  static const char* const BLNK[5] = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " ( -  - )~ ", "  `----`   ", "   ''  ''  " };
  static const char* const TWCH[5] = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " ( o  o )- ", "  `----`   ", "   ''  ''  " };
  static const char* const LOOK[5] = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " (o    o)~ ", "  `----`   ", "   ''  ''  " };
  const char* const* P[4] = { SIT, BLNK, TWCH, LOOK };
  static const uint8_t SEQ[] = { 0,0,0,1,0,2,0,3,0,1, 0,2,0,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL);
  buddySetColor(NOSE);
  buddySetCursor(BUDDY_X_CENTER + 21, BUDDY_Y_BASE + 8);
  buddyPrint((t / 2) & 1 ? "~" : "-");
}

// ─── BUSY ───  scurrying, little dust
static void doBusy(uint32_t t) {
  static const char* const A[5] = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " ( o  o )> ", "  `----`   ", "  '' '' '' " };
  static const char* const B[5] = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " ( o  o )> ", "  `----`   ", " '' '' ''  " };
  const char* const* P[2] = { A, B };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1,0,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  int xo = (t & 1) ? 2 : -2;
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL, xo);

  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER - 28, BUDDY_Y_BASE + 12);
  buddyPrint((t & 1) ? ".o" : "o.");
}

// ─── ATTENTION ───  spikes bristle up, !
static void doAttention(uint32_t t) {
  static const char* const A[5] = { " /\\/\\/\\/\\ ", "  ^^^^^^   ", " ( O  O )! ", "  `----`   ", "   ''  ''  " };
  static const char* const B[5] = { " ^^^^^^^^^ ", " /\\/\\/\\/\\ ", " ( O  O )! ", "  `----`   ", "   ''  ''  " };
  const char* const* P[2] = { A, B };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  int xo = (t & 1) ? 1 : -1;
  buddyPrintSprite(P[SEQ[beat]], 5, 0, COL, xo);

  if ((t / 2) & 1) { buddySetColor(BUDDY_YEL); buddySetCursor(BUDDY_X_CENTER - 2, BUDDY_Y_OVERLAY - 6); buddyPrint("!"); }
  if ((t / 3) & 1) { buddySetColor(BUDDY_RED); buddySetCursor(BUDDY_X_CENTER + 20, BUDDY_Y_OVERLAY - 2); buddyPrint("!"); }
}

// ─── CELEBRATE ───  happy bounce + confetti
static void doCelebrate(uint32_t t) {
  static const char* const A[5] = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " ( ^  ^ )~ ", "  `----`   ", "   ''  ''  " };
  static const char* const B[5] = { " \\^^^^^^/  ", " ^/\\/\\/\\^  ", " ( ^  ^ )~ ", "  `----`   ", "   ''  ''  " };
  const char* const* P[2] = { A, B };
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

// ─── DIZZY ───  spiral eyes, wobble, stars
static void doDizzy(uint32_t t) {
  static const char* const D[5] = { "  ^^^^^^   ", " ~/\\/\\/\\~  ", " ( @  @ )~ ", "  `----`   ", "   ''  ''  " };
  buddyPrintSprite(D, 5, 0, COL, ((t / 2) & 1) ? 2 : -2);
  int a = t % 8;
  buddySetColor(BUDDY_YEL);
  buddySetCursor(BUDDY_X_CENTER - 18 + a * 4, BUDDY_Y_OVERLAY - 6 + ((a < 4) ? a : 8 - a));
  buddyPrint("*");
}

// ─── HEART ───  blush + floating hearts
static void doHeart(uint32_t t) {
  static const char* const A[5] = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " ( ^  ^ )~ ", "  `----`   ", "   ''  ''  " };
  static const char* const B[5] = { "  ^^^^^^   ", " ^/\\/\\/\\^  ", " (*^  ^*)~ ", "  `----`   ", "   ''  ''  " };
  const char* const* P[2] = { A, B };
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

}  // namespace hedgehog

extern const Species HEDGEHOG_SPECIES = {
  "hedgehog",
  0xCA8B,
  { hedgehog::doSleep, hedgehog::doIdle, hedgehog::doBusy, hedgehog::doAttention,
    hedgehog::doCelebrate, hedgehog::doDizzy, hedgehog::doHeart }
};
