/*
 * ============================================================
 *  2-Player Reaction Game
 * ============================================================
 *  Left  matrix : DIN=11, CLK=13, CS=12   |  Player 1 button: pin 3
 *  Right matrix : DIN=47, CLK=51, CS=49   |  Player 2 button: pin 43
 *  LCD (16x2)   : RS=10, EN=9, D4=7, D5=6, D6=5, D7=4
 *
 *  Game flow:
 *    IDLE     → either button pressed
 *    COUNTDOWN→ matrices show 3 → 2 → 1 (1 s each)
 *    WAITING  → matrices blank, random 1–4 s delay
 *    READY    → matrices show 0 ("GO!"), reaction timer starts
 *    RESULT   → winner gets smiley / loser gets sad face
 *             → any button press returns to IDLE
 *
 *  False start: pressing during COUNTDOWN or WAITING → instant loss.
 *  Timeout   : if a player doesn't press within 5 s of GO → disqualified.
 * ============================================================
 */

#include <LiquidCrystal.h>
#include <LedControl.h>

// ── Hardware ──────────────────────────────────────────────────────────────────

#define LEFT_DIN   11
#define LEFT_CLK   13
#define LEFT_CS    12
#define RIGHT_DIN  47
#define RIGHT_CLK  51
#define RIGHT_CS   49

LedControl    lcLeft (LEFT_DIN,  LEFT_CLK,  LEFT_CS,  1);
LedControl    lcRight(RIGHT_DIN, RIGHT_CLK, RIGHT_CS, 1);
LiquidCrystal lcd(10, 9, 7, 6, 5, 4);   // RS, EN, D4, D5, D6, D7

const int BTN_P1 = 3;    // Player 1 – left matrix side
const int BTN_P2 = 43;   // Player 2 – right matrix side

// ── LED patterns ──────────────────────────────────────────────────────────────
// bit7 = leftmost LED. Defined as the intended visual appearance;
// displayPattern() applies a 90° CCW rotation to compensate for the physical
// 90° CW mount.

const byte DIGIT_0[8] = { 0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C };
const byte DIGIT_1[8] = { 0x10, 0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 0x38 };
const byte DIGIT_2[8] = { 0x3C, 0x42, 0x02, 0x04, 0x08, 0x10, 0x20, 0x7E };
const byte DIGIT_3[8] = { 0x3C, 0x42, 0x02, 0x3E, 0x02, 0x02, 0x42, 0x3C };
const byte SMILEY[8]  = { 0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C };
const byte SAD[8]     = { 0x3C, 0x42, 0xA5, 0x81, 0x99, 0xA5, 0x42, 0x3C };

// Countdown sequence: index 0 = first digit shown (3), 2 = last (1)
const byte* const COUNTDOWN_PAT[3] = { DIGIT_3, DIGIT_2, DIGIT_1 };

// ── Game state ────────────────────────────────────────────────────────────────

enum GameState { IDLE, COUNTDOWN, WAITING, READY, RESULT };
GameState gameState = IDLE;

unsigned long stateStart    = 0;   // millis() when current state was entered
unsigned long waitDur       = 0;   // random hold time for WAITING (ms)
int           cdStep        = -1;  // last countdown digit index drawn (0–2)

long p1Time  = -1;    // ms from GO! to P1 press; -1 = not yet pressed
long p2Time  = -1;
bool p1False = false; // pressed before GO! (false start)
bool p2False = false;

bool lastP1 = HIGH;   // previous raw button state (INPUT_PULLUP → HIGH = open)
bool lastP2 = HIGH;

unsigned long lastLcdRefresh = 0;

// ── Timing constants ──────────────────────────────────────────────────────────

const unsigned long CD_STEP_MS = 1000;   // each countdown digit stays for 1 s
const unsigned long TIMEOUT_MS = 5000;   // auto-end READY after 5 s
const unsigned long RESULT_MS  = 8000;   // hold result screen for 8 s

// ── Matrix helpers ────────────────────────────────────────────────────────────

// Rotate pattern 90° CCW before sending to compensate for 90° CW physical mount.
// Mapping: visual(row, col) → physical(7-col, row)
void displayPattern(LedControl &lc, const byte pat[8]) {
    for (int p = 0; p < 8; p++) {
        byte row = 0;
        for (int q = 0; q < 8; q++)
            if ((pat[q] >> p) & 1) row |= (1 << (7 - q));
        lc.setRow(0, p, row);
    }
}

void fillAll(LedControl &lc, byte val) {
    for (int r = 0; r < 8; r++) lc.setRow(0, r, val);
}

void initMatrix(LedControl &lc) {
    lc.shutdown(0, false);
    lc.setIntensity(0, 8);
    lc.clearDisplay(0);
}

void startupFlash(LedControl &lc) {
    fillAll(lc, 0xFF); delay(200);
    fillAll(lc, 0x00); delay(100);
    fillAll(lc, 0xFF); delay(200);
    fillAll(lc, 0x00); delay(200);
}

// ── LCD helpers ───────────────────────────────────────────────────────────────

// Print text to an LCD row, padding with spaces to 16 chars so no ghost chars remain.
void lcdLine(uint8_t row, const char *s) {
    lcd.setCursor(0, row);
    int i = 0;
    while (s[i] && i < 16) lcd.print(s[i++]);
    while (i++ < 16)        lcd.print(' ');
}

// Print one 16-char result row, e.g. "P1: 234ms  WINS!" or "P2:FALSE START  "
// prefix  = "P1:" or "P2:" (exactly 3 chars)
// ms      = reaction time in ms (-1 = not pressed / timed out)
// isFalse = player pressed before GO!
// wins    = this player won
void resultLine(uint8_t row, const char *prefix,
                long ms, bool isFalse, bool wins) {
    char buf[17];
    if (isFalse)
        snprintf(buf, sizeof(buf), "%sFALSE START  ", prefix);
        //                          3 + 13 = 16 chars
    else if (ms < 0)
        snprintf(buf, sizeof(buf), "%s TIMEOUT     ", prefix);
        //                          3 + 13 = 16 chars
    else if (wins)
        snprintf(buf, sizeof(buf), "%s%4ldms  WINS!", prefix, ms);
        //                          3 + 6  + 7  = 16 chars
    else
        snprintf(buf, sizeof(buf), "%s%4ldms       ", prefix, ms);
        //                          3 + 6  + 7  = 16 chars
    lcdLine(row, buf);
}

// ── Result display ────────────────────────────────────────────────────────────

void showResult() {
    bool p1ok = !p1False && (p1Time >= 0);
    bool p2ok = !p2False && (p2Time >= 0);

    // Work out who won
    bool p1w = false, p2w = false;
    if      (p1False && p2False)  { /* both lose  */ }
    else if (p1False)             { p2w = true;       }   // P1 false-started
    else if (p2False)             { p1w = true;       }   // P2 false-started
    else if (!p1ok && !p2ok)      { /* both timed out */ }
    else if (!p1ok)               { p2w = true;       }   // P1 timed out
    else if (!p2ok)               { p1w = true;       }   // P2 timed out
    else if (p1Time < p2Time)     { p1w = true;       }
    else if (p2Time < p1Time)     { p2w = true;       }
    else                          { p1w = p2w = true; }   // exact tie

    // LED faces
    if      ( p1w && !p2w) { displayPattern(lcLeft, SMILEY); displayPattern(lcRight, SAD);    }
    else if (!p1w &&  p2w) { displayPattern(lcLeft, SAD);    displayPattern(lcRight, SMILEY); }
    else if ( p1w &&  p2w) { displayPattern(lcLeft, SMILEY); displayPattern(lcRight, SMILEY); }
    else                   { fillAll(lcLeft, 0x00);           fillAll(lcRight, 0x00);          }

    // LCD
    resultLine(0, "P1:", p1Time, p1False, p1w);
    resultLine(1, "P2:", p2Time, p2False, p2w);
}

// ── State transitions ─────────────────────────────────────────────────────────

void enterState(GameState s) {
    gameState  = s;
    stateStart = millis();

    switch (s) {
        case IDLE:
            fillAll(lcLeft, 0x00); fillAll(lcRight, 0x00);
            lcdLine(0, "2-Player Reflex!");
            lcdLine(1, " Press to start ");
            break;

        case COUNTDOWN:
            p1Time = p2Time = -1;
            p1False = p2False = false;
            cdStep = -1;                   // force first digit to draw immediately
            lcdLine(0, "   Get Ready!   ");
            lcdLine(1, "                ");
            break;

        case WAITING:
            fillAll(lcLeft, 0x00); fillAll(lcRight, 0x00);
            waitDur = random(1000, 4001);  // 1–4 s random delay
            lcdLine(0, " Wait for it... ");
            lcdLine(1, "                ");
            break;

        case READY:
            displayPattern(lcLeft,  DIGIT_0);
            displayPattern(lcRight, DIGIT_0);
            lcdLine(0, "     GO!!!      ");
            lcdLine(1, "Time: 00.00s    ");
            lastLcdRefresh = stateStart;
            break;

        case RESULT:
            showResult();
            break;
    }
}

// ── Button edge detection ─────────────────────────────────────────────────────

void readButtons() {
    bool curP1 = digitalRead(BTN_P1);
    bool curP2 = digitalRead(BTN_P2);

    // Detect falling edge (HIGH→LOW = button pressed with INPUT_PULLUP)
    bool pressP1 = (lastP1 == HIGH && curP1 == LOW);
    bool pressP2 = (lastP2 == HIGH && curP2 == LOW);
    lastP1 = curP1;
    lastP2 = curP2;

    switch (gameState) {
        case IDLE:
            if (pressP1 || pressP2) enterState(COUNTDOWN);
            break;

        case COUNTDOWN:
        case WAITING:
            // Any press before GO! is a false start → instant result
            if (pressP1) p1False = true;
            if (pressP2) p2False = true;
            if (p1False || p2False) enterState(RESULT);
            break;

        case READY: {
            unsigned long now = millis();
            if (pressP1 && p1Time < 0) p1Time = (long)(now - stateStart);
            if (pressP2 && p2Time < 0) p2Time = (long)(now - stateStart);
            // Both pressed → show result immediately
            if (p1Time >= 0 && p2Time >= 0) enterState(RESULT);
            break;
        }

        case RESULT:
            // Any press restarts the game
            if (pressP1 || pressP2) enterState(IDLE);
            break;
    }
}

// ── setup / loop ──────────────────────────────────────────────────────────────

void setup() {
    pinMode(BTN_P1, INPUT_PULLUP);
    pinMode(BTN_P2, INPUT_PULLUP);

    initMatrix(lcLeft);
    initMatrix(lcRight);
    startupFlash(lcLeft);
    startupFlash(lcRight);

    lcd.begin(16, 2);
    lcd.clear();
    randomSeed(analogRead(A0));   // seed RNG from floating analogue pin

    enterState(IDLE);
}

void loop() {
    readButtons();

    unsigned long now     = millis();
    unsigned long elapsed = now - stateStart;

    switch (gameState) {

        case COUNTDOWN: {
            // Advance to the next digit every CD_STEP_MS
            int step = (int)(elapsed / CD_STEP_MS);
            if (step < 3 && step != cdStep) {
                cdStep = step;
                displayPattern(lcLeft,  COUNTDOWN_PAT[step]);
                displayPattern(lcRight, COUNTDOWN_PAT[step]);
            }
            // After 3 digits (3 s) → enter random-wait phase
            if (elapsed >= 3UL * CD_STEP_MS) enterState(WAITING);
            break;
        }

        case WAITING:
            if (elapsed >= waitDur) enterState(READY);
            break;

        case READY:
            // Refresh the running reaction timer on the LCD ~every 50 ms
            if (now - lastLcdRefresh >= 50) {
                lastLcdRefresh = now;
                int sec = (int)(elapsed / 1000);
                int cs  = (int)((elapsed % 1000) / 10);   // centiseconds
                lcd.setCursor(6, 1);                       // after "Time: "
                if (sec < 10) lcd.print('0');
                lcd.print(sec);
                lcd.print('.');
                if (cs  < 10) lcd.print('0');
                lcd.print(cs);
                lcd.print("s  ");
            }
            // Timeout: end round if neither/one player reacted in time
            if (elapsed >= TIMEOUT_MS) enterState(RESULT);
            break;

        case RESULT:
            // Auto-return to IDLE after RESULT_MS
            if (elapsed >= RESULT_MS) enterState(IDLE);
            break;

        default: break;
    }
}
