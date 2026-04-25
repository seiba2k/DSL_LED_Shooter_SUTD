/*
 * LED Shooter — Survival Mode
 * ─────────────────────────────────────────────────
 * Board:   Freenove ESP32-S3-WROOM
 * Strip:   WS2812B on GPIO 9 (33 LEDs)
 * Inputs:  Yellow SHOOT button → GPIO 7
 *          EC56 Rotary Encoder → GPIO 17 (A), GPIO 18 (B)
 * Display: GC9A01A round TFT (SPI)
 *            CS=20  DC=21  RST=47  SCLK=45  SDA=48  BLK=19
 */

#include <FastLED.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <SPI.h>

// ═══════════════════════════════════════════════════
//  SEED MODE
// ═══════════════════════════════════════════════════
//  0 = Wait for FPGA seed (won't start until received)
//  1 = Use fixed seed below (for testing without FPGA)
#define USE_FIXED_SEED  0

uint32_t FPGA_SEED = 0b10110011111010011100101001110110;  // only used if USE_FIXED_SEED = 1

// ═══════════════════════════════════════════════════
//  HARDWARE — PIN ASSIGNMENTS
// ═══════════════════════════════════════════════════

// LED strip
#define LED_PIN       9
#define LEDS_COUNT    33

// Shoot button
#define BTN_SHOOT     7

// Buzzer
#define BUZZER_PIN    14

// FPGA UART (receive only — FPGA streams hex values at 9600 baud)
#define FPGA_RX_PIN   44
#define FPGA_BAUD     9600

// EC56 rotary encoder
#define ENCODER_A             17
#define ENCODER_B             18
#define ENCODER_SETTLE_US     1000    // 1ms settle after edge
#define ENCODER_DEBOUNCE_MS   120     // min ms between clicks

// TFT display (GC9A01A round 240x240) — hardware SPI with custom pins
#define TFT_CS    20
#define TFT_DC    21
#define TFT_RST   47
#define TFT_SCLK  45
#define TFT_SDA   48
#define TFT_BLK   19
#define TFT_FREQ  8000000

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);

// ═══════════════════════════════════════════════════
//  GAME SETTINGS
// ═══════════════════════════════════════════════════
#define BUFFER_TOTAL        225     // 212 - 12 buffer = 200 obstacles
#define BUFFER_ZONE         25

// Speed ramp — gets faster as you clear obstacles
#define SPEED_Q0            300     // 0–25% cleared
#define SPEED_Q1            150    // 25–50% cleared
#define SPEED_Q2            75   // 50–75% cleared
#define SPEED_Q3            35    // 75–100% cleared

#define MAX_LIVES           3
#define BULLET_SPEED_MS     40
#define DEBOUNCE_MS         200
#define BRIGHTNESS          30
#define OBSTACLE_CHANCE     100     // 100% = no gaps
#define COLOR_EMPTY         3
#define MUZZLE_LED          2       // Muzzle LED Offset
#define GAME_LEDS           (LEDS_COUNT - MUZZLE_LED)  // usable game positions

const CRGB OBSTACLE_COLORS[] = { CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::Black };

// ═══════════════════════════════════════════════════
//  XORSHIFT32 — stretches 32-bit seed to any length
// ═══════════════════════════════════════════════════
struct Xorshift32 {
    uint32_t state;
    void seed(uint32_t s) { state = s ? s : 0x12345678; }
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    uint32_t nextRange(uint32_t max) {
        if (max <= 1) return 0;
        return next() % max;
    }
};

Xorshift32 rng;

// ═══════════════════════════════════════════════════
//  STATE
// ═══════════════════════════════════════════════════
CRGB leds[LEDS_COUNT];           // physical LEDs
uint8_t  ledMap[GAME_LEDS];      // game positions (offset by MUZZLE_LED)
uint8_t  queue[BUFFER_TOTAL];
uint16_t queueHead = 0;
uint16_t queueLen  = 0;

uint8_t  lives          = MAX_LIVES;
uint16_t remaining      = 0;
uint16_t totalObstacles = 0;
uint16_t score          = 0;
uint16_t streak         = 0;        // consecutive hits without a miss

// Combo tier thresholds and point values
//   Hits  1–5:   +1   (warming up)
//   Hits  6–15:  +2   (STREAK)
//   Hits 16–30:  +5   (ON FIRE)
//   Hits  31+:   +10  (UNSTOPPABLE)
uint8_t getComboPoints() {
    if      (streak >= 31) return 10;
    else if (streak >= 16) return 5;
    else if (streak >= 6)  return 2;
    else                   return 1;
}

uint8_t getComboTier() {
    // 0 = none, 1 = STREAK, 2 = ON FIRE, 3 = UNSTOPPABLE
    if      (streak >= 31) return 3;
    else if (streak >= 16) return 2;
    else if (streak >= 6)  return 1;
    else                   return 0;
}

const char* comboTierNames[] = {"", "STREAK", "ON FIRE", "UNSTOPPABLE"};
bool     gameOver       = false;
bool     gameOverShown  = false;    // true after animation played
bool     gameWon        = false;
bool     shooting       = false;
bool     gameStarted    = false;
int8_t   selectedColor  = 0;         // start on RED

unsigned long lastScrollTime = 0;

// Rotary encoder state (dual-edge, settle + confirm)
int  lastEncoderA              = HIGH;
unsigned long lastEncoderClickTime = 0;
const char* colorNames[]       = {"RED", "GREEN", "BLUE"};

// FPGA UART receive state
char     fpgaBuffer[16];        // buffer for incoming hex string
uint8_t  fpgaBufferIdx = 0;
uint32_t fpgaLatestSeed = 0;    // most recent complete value from FPGA
bool     fpgaSeedReady = false; // true once at least one value received

// ═══════════════════════════════════════════════════
//  FPGA UART RECEIVE
// ═══════════════════════════════════════════════════
// Call this every loop iteration.
// FPGA streams "3A7F1B2C\n" at 9600 baud continuously.
// We just keep the latest complete value.
void readFPGAUart() {
    while (Serial1.available()) {
        char c = Serial1.read();

        if (c == '\n' || c == '\r') {
            // End of hex string — parse it
            if (fpgaBufferIdx > 0 && fpgaBufferIdx <= 8) {
                fpgaBuffer[fpgaBufferIdx] = '\0';
                fpgaLatestSeed = strtoul(fpgaBuffer, NULL, 16);
                fpgaSeedReady = true;
                if (!gameStarted || gameOver)
                    Serial.printf("[FPGA] Received seed: 0x%08X\n", fpgaLatestSeed);
            }
            fpgaBufferIdx = 0;
        } else if (fpgaBufferIdx < 8) {
            // Accumulate hex characters
            fpgaBuffer[fpgaBufferIdx++] = c;
        }
        // Ignore characters beyond 8 (overflow protection)
    }
}

// Get the seed based on mode
uint32_t getGameSeed() {
    if (USE_FIXED_SEED) {
        Serial.printf("[SEED] Using fixed seed: 0x%08X\n", FPGA_SEED);
        return FPGA_SEED;
    } else {
        Serial.printf("[SEED] Using FPGA seed: 0x%08X\n", fpgaLatestSeed);
        return fpgaLatestSeed;
    }
}

// ═══════════════════════════════════════════════════
//  SEED EXPANSION
// ═══════════════════════════════════════════════════
void expandSeed(uint32_t seed) {
    rng.seed(seed);
    remaining = 0;

    uint8_t full[BUFFER_TOTAL];
    for (uint16_t i = 0; i < BUFFER_TOTAL; i++) {
        if (i == 0 || i < BUFFER_ZONE) {
            full[i] = COLOR_EMPTY;
        } else {
            uint32_t roll = rng.nextRange(100);
            if (roll < OBSTACLE_CHANCE) {
                full[i] = rng.nextRange(3);
                remaining++;
            } else {
                full[i] = COLOR_EMPTY;
            }
        }
    }

    for (uint16_t i = 0; i < GAME_LEDS && i < BUFFER_TOTAL; i++)
        ledMap[i] = full[i];

    queueLen  = (BUFFER_TOTAL > GAME_LEDS) ? (BUFFER_TOTAL - GAME_LEDS) : 0;
    queueHead = 0;
    for (uint16_t i = 0; i < queueLen; i++)
        queue[i] = full[GAME_LEDS + i];

    Serial.printf("[SEED] 0x%08X → %d obstacles | game: %d, queued: %d\n",
                  seed, remaining, GAME_LEDS, queueLen);
    totalObstacles = remaining;
}

// ═══════════════════════════════════════════════════
//  SCROLL
// ═══════════════════════════════════════════════════
bool scrollStrip() {
    if (ledMap[1] != COLOR_EMPTY) return false;

    for (uint16_t i = 1; i < GAME_LEDS - 1; i++)
        ledMap[i] = ledMap[i + 1];

    if (queueHead < queueLen)
        ledMap[GAME_LEDS - 1] = queue[queueHead++];
    else
        ledMap[GAME_LEDS - 1] = COLOR_EMPTY;

    return true;
}

uint16_t countStripActive() {
    uint16_t c = 0;
    for (uint16_t i = 1; i < GAME_LEDS; i++)
        if (ledMap[i] != COLOR_EMPTY) c++;
    return c;
}

uint16_t countQueueActive() {
    uint16_t c = 0;
    for (uint16_t i = queueHead; i < queueLen; i++)
        if (queue[i] != COLOR_EMPTY) c++;
    return c;
}

// ═══════════════════════════════════════════════════
//  SPEED RAMP
// ═══════════════════════════════════════════════════
uint16_t getCurrentScrollSpeed() {
    if (totalObstacles == 0) return SPEED_Q0;

    uint16_t cleared = totalObstacles - remaining;
    uint16_t quarter = totalObstacles / 4;

    uint16_t speed;
    if      (cleared < quarter * 1) speed = SPEED_Q0;
    else if (cleared < quarter * 2) speed = SPEED_Q1;
    else if (cleared < quarter * 3) speed = SPEED_Q2;
    else                            speed = SPEED_Q3;

    static uint16_t lastSpeed = 0;
    if (speed != lastSpeed) {
        lastSpeed = speed;
        Serial.printf("[SPEED] %dms/tick (%d%% cleared)\n",
                      speed, (cleared * 100) / totalObstacles);
    }

    return speed;
}

// ═══════════════════════════════════════════════════
//  RENDER
// ═══════════════════════════════════════════════════
void renderStrip() {
    // LEDs 0 to MUZZLE_LED-1: off (clamping area)
    for (uint16_t i = 0; i < MUZZLE_LED; i++)
        leds[i] = CRGB::Black;

    // Game positions → physical LEDs (offset by MUZZLE_LED)
    for (uint16_t i = 0; i < GAME_LEDS; i++) {
        if (ledMap[i] != COLOR_EMPTY) {
            leds[i + MUZZLE_LED] = OBSTACLE_COLORS[ledMap[i]];
        } else {
            leds[i + MUZZLE_LED] = CRGB(2, 2, 2);
        }
    }

    // Muzzle (game position 0 → physical LED MUZZLE_LED)
    if (selectedColor >= 0 && selectedColor < 3)
        leds[MUZZLE_LED] = OBSTACLE_COLORS[selectedColor];
    else
        leds[MUZZLE_LED] = CRGB(15, 15, 15);

    FastLED.show();
}

// ═══════════════════════════════════════════════════
//  FIND / GET / CLEAR — supports shooting into queue
// ═══════════════════════════════════════════════════
int findNearestObstacle() {
    for (int i = 1; i < GAME_LEDS; i++)
        if (ledMap[i] != COLOR_EMPTY) return i;
    for (uint16_t i = queueHead; i < queueLen; i++)
        if (queue[i] != COLOR_EMPTY)
            return GAME_LEDS + (i - queueHead);
    return -1;
}

uint8_t getColorAt(int virtualPos) {
    if (virtualPos < GAME_LEDS)
        return ledMap[virtualPos];
    else {
        uint16_t qi = queueHead + (virtualPos - GAME_LEDS);
        if (qi < queueLen) return queue[qi];
        return COLOR_EMPTY;
    }
}

void clearObstacleAt(int virtualPos) {
    if (virtualPos < GAME_LEDS)
        ledMap[virtualPos] = COLOR_EMPTY;
    else {
        uint16_t qi = queueHead + (virtualPos - GAME_LEDS);
        if (qi < queueLen) queue[qi] = COLOR_EMPTY;
    }
}

// ═══════════════════════════════════════════════════
//  SHOOTING
// ═══════════════════════════════════════════════════
void handleShot(uint8_t colorIndex) {
    int target = findNearestObstacle();
    if (target < 0) return;

    shooting = true;
    playShootSound();
    uint8_t targetColor = getColorAt(target);
    bool hit = (targetColor == colorIndex);
    bool inQueue = (target >= GAME_LEDS);

    Serial.printf("[FIRE] %s → %s pos %d (has %s) → %s\n",
        colorIndex == 0 ? "RED" : colorIndex == 1 ? "GREEN" : "BLUE",
        inQueue ? "queue" : "LED",
        target,
        targetColor == 0 ? "RED" : targetColor == 1 ? "GREEN" : "BLUE",
        hit ? "HIT!" : "MISS");

    // Bullet animation (game pos → physical LED with offset)
    CRGB c = OBSTACLE_COLORS[colorIndex];
    int animEnd = (target < GAME_LEDS) ? target : (GAME_LEDS - 1);
    for (int pos = 0; pos <= animEnd; pos++) {
        renderStrip();
        int pLed = pos + MUZZLE_LED;
        leds[pLed] = c;
        if (pos > 0) { leds[pLed-1] = c; leds[pLed-1].fadeToBlackBy(160); }
        if (pos > 1) { leds[pLed-2] = c; leds[pLed-2].fadeToBlackBy(220); }
        FastLED.show();
        delay(BULLET_SPEED_MS);
    }

    if (hit) {
        clearObstacleAt(target);
        remaining--;
        streak++;

        // Score
        uint8_t pts = getComboPoints();
        uint8_t tier = getComboTier();
        score += pts;

        Serial.printf("[HIT]  +%d pts (streak: %d, tier: %s, score: %d)\n",
            pts, streak,
            tier > 0 ? comboTierNames[tier] : "---",
            score);

        // Update TFT
        tftDrawScore(score);
        tftDrawCombo(tier, streak);

        // White flash (physical offset)
        int flashGame = (target < GAME_LEDS) ? target : (GAME_LEDS - 1);
        int flashPhys = flashGame + MUZZLE_LED;
        for (int r = 0; r < 4; r++) {
            fill_solid(leds, LEDS_COUNT, CRGB::Black);
            for (int d = -r; d <= r; d++) {
                int p = flashPhys + d;
                if (p >= MUZZLE_LED && p < LEDS_COUNT)
                    leds[p] = CRGB(255 - abs(d) * 60, 255 - abs(d) * 60, 255 - abs(d) * 60);
            }
            FastLED.show(); delay(40);
        }

        if (countStripActive() == 0 && countQueueActive() == 0) {
            gameWon = true; gameOver = true;
            Serial.printf("[WIN] All cleared! Final score: %d\n", score);
        }
    } else {
        lives--;
        uint8_t oldTier = getComboTier();
        streak = 0;     // reset combo

        if (oldTier > 0) {
            Serial.printf("[MISS] Lost %s combo!\n", comboTierNames[oldTier]);
        }

        tftDrawLives(lives);
        tftDrawScore(score);
        tftDrawCombo(0, 0);
        playLoseLifeSound();

        // Red flash
        for (int i = 0; i < 3; i++) {
            fill_solid(leds, MUZZLE_LED, CRGB::Black);
            fill_solid(leds + MUZZLE_LED, GAME_LEDS, CRGB(60, 0, 0));
            FastLED.show(); delay(80);
            fill_solid(leds, LEDS_COUNT, CRGB::Black);
            FastLED.show(); delay(80);
        }
        Serial.printf("[MISS] Lives left: %d | Score: %d\n", lives, score);
        if (lives == 0) {
            gameOver = true;
            Serial.printf("[GAME OVER] Final score: %d\n", score);
        }
    }

    shooting = false;
    lastScrollTime = millis();
}

// ═══════════════════════════════════════════════════
//  BUZZER SOUNDS
// ═══════════════════════════════════════════════════
void silenceMs(int ms) {
    ledcWriteTone(BUZZER_PIN, 0);
    delay(ms);
}

void toneMs(int freq, int ms) {
    ledcWriteTone(BUZZER_PIN, freq);
    delay(ms);
}

void chirpDown(int startFreq, int endFreq, int step, int stepDelayMs) {
    for (int f = startFreq; f >= endFreq; f -= step) {
        ledcWriteTone(BUZZER_PIN, f);
        delay(stepDelayMs);
    }
    ledcWriteTone(BUZZER_PIN, 0);
}

void playShootSound() {
    chirpDown(3200, 1800, 120, 6);
    silenceMs(15);
}

void playLoseLifeSound() {
    toneMs(1400, 90);
    silenceMs(20);
    toneMs(1100, 110);
    silenceMs(20);
    toneMs(800, 140);
    silenceMs(20);
    toneMs(500, 220);
    silenceMs(20);
}

void playGameOverSound() {
    chirpDown(2000, 300, 50, 12);
    silenceMs(40);
    toneMs(400, 200);
    silenceMs(50);
    toneMs(300, 250);
    silenceMs(50);
    toneMs(200, 500);
    silenceMs(100);
}

// ═══════════════════════════════════════════════════
//  TFT DISPLAY FUNCTIONS
//  Layout:   Score (top)  |  Combo (middle)  |  Lives (bottom)
// ═══════════════════════════════════════════════════

void tftDrawScore(int value) {
    tft.fillRect(25, 25, 190, 55, GC9A01A_BLACK);
    tft.setTextColor(GC9A01A_WHITE, GC9A01A_BLACK);
    tft.setTextSize(2);
    tft.setCursor(90, 28);
    tft.print("SCORE");

    tft.setTextSize(4);
    // Center the number (estimate width: ~24px per digit)
    int digits = 1;
    int tmp = value;
    while (tmp >= 10) { tmp /= 10; digits++; }
    int xPos = 120 - (digits * 12);
    tft.setCursor(xPos, 50);
    tft.print(value);
}

void tftDrawCombo(uint8_t tier, uint16_t currentStreak) {
    tft.fillRect(20, 95, 200, 40, GC9A01A_BLACK);

    if (currentStreak < 2) return;  // only show at 2+ streak

    // Color scales with tier
    uint16_t color;
    switch (tier) {
        case 1: color = GC9A01A_YELLOW;  break;
        case 2: color = 0xFD20;          break;  // orange
        case 3: color = GC9A01A_RED;     break;
        default: color = GC9A01A_WHITE;  break;
    }

    // "x14 streak" centered
    char buf[16];
    snprintf(buf, sizeof(buf), "x%d streak", currentStreak);
    int bufLen = strlen(buf);
    tft.setTextColor(color, GC9A01A_BLACK);
    tft.setTextSize(2);
    tft.setCursor(120 - (bufLen * 6), 105);
    tft.print(buf);
}

void tftDrawLives(int currentLives) {
    tft.fillRect(40, 155, 160, 50, GC9A01A_BLACK);

    // "..." style — filled dots for remaining, hollow for lost
    tft.setTextSize(1);
    tft.setTextColor(GC9A01A_WHITE, GC9A01A_BLACK);
    tft.setCursor(96, 160);
    tft.print("LIVES");

    int y = 185;
    int radius = 8;
    int spacing = 28;
    int startX = 120 - ((MAX_LIVES - 1) * spacing / 2);

    for (int i = 0; i < MAX_LIVES; i++) {
        int x = startX + (i * spacing);
        if (i < currentLives) {
            tft.fillCircle(x, y, radius, GC9A01A_RED);
        } else {
            tft.drawCircle(x, y, radius, 0x4208);  // dim gray outline
        }
    }
}

void tftDrawUI() {
    for (int i = 0; i < 3; i++) {
        tft.fillScreen(GC9A01A_BLACK);
        delay(30);
    }
    tft.drawCircle(120, 120, 118, GC9A01A_WHITE);
    tftDrawScore(0);
    tftDrawCombo(0, 0);
    tftDrawLives(lives);
}

void tftDrawGameOver(bool won) {
    // Aggressive clear — triple fill with delays to fully flush display
    for (int i = 0; i < 3; i++) {
        tft.fillScreen(GC9A01A_BLACK);
        delay(30);
    }

    tft.drawCircle(120, 120, 118, won ? GC9A01A_GREEN : GC9A01A_RED);

    // Title
    tft.setTextSize(3);
    tft.setTextColor(won ? GC9A01A_GREEN : GC9A01A_RED, GC9A01A_BLACK);
    tft.setCursor(won ? 52 : 40, 45);
    tft.print(won ? "YOU WIN" : "GAME OVER");

    // Final score
    tft.setTextSize(2);
    tft.setTextColor(GC9A01A_WHITE, GC9A01A_BLACK);
    tft.setCursor(52, 90);
    tft.printf("Score: %d", score);

    // Hit / total
    uint16_t hits = totalObstacles - remaining;
    tft.setTextSize(2);
    tft.setTextColor(GC9A01A_YELLOW, GC9A01A_BLACK);
    char buf[20];
    snprintf(buf, sizeof(buf), "Hit: %d/%d", hits, totalObstacles);
    int bufLen = strlen(buf);
    tft.setCursor(120 - (bufLen * 6), 115);
    tft.print(buf);

    // Prompt
    tft.setTextSize(1);
    tft.setTextColor(GC9A01A_WHITE, GC9A01A_BLACK);
    tft.setCursor(55, 155);
    tft.print("Press SHOOT to play");
}

// ═══════════════════════════════════════════════════
//  LED ANIMATIONS
// ═══════════════════════════════════════════════════
void showWin() {
    for (int cycle = 0; cycle < 3; cycle++)
        for (int h = 0; h < 256; h += 8) {
            fill_solid(leds, MUZZLE_LED, CRGB::Black);
            fill_rainbow(leds + MUZZLE_LED, GAME_LEDS, h, 32);
            FastLED.show(); delay(20);
        }
    fill_solid(leds, MUZZLE_LED, CRGB::Black);
    fill_solid(leds + MUZZLE_LED, GAME_LEDS, CRGB::Green);
    FastLED.show();
}

void showLose() {
    for (int b = 255; b >= 0; b -= 5) {
        fill_solid(leds, MUZZLE_LED, CRGB::Black);
        fill_solid(leds + MUZZLE_LED, GAME_LEDS, CRGB(b, 0, 0));
        FastLED.show(); delay(20);
    }
}

void showStandby() {
    static uint8_t brightness = 0;
    static int8_t direction = 1;

    brightness += direction * 2;
    if (brightness >= 40) direction = -1;
    if (brightness <= 2)  direction = 1;

    fill_solid(leds, MUZZLE_LED, CRGB::Black);  // clamp LEDs off
    fill_solid(leds + MUZZLE_LED, GAME_LEDS, CRGB(brightness, brightness, brightness));
    FastLED.show();
}

void showLives() {
    for (int i = 0; i < lives; i++) {
        leds[MUZZLE_LED] = CRGB::White;  FastLED.show(); delay(200);
        leds[MUZZLE_LED] = CRGB::Black;  FastLED.show(); delay(200);
    }
}

// ═══════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════
bool debounce(unsigned long &last, unsigned long now) {
    if (now - last > DEBOUNCE_MS) { last = now; return true; }
    return false;
}

void waitForPress(uint8_t pin) {
    while (digitalRead(pin) == LOW) delay(10);
    delay(50);
    while (digitalRead(pin) == HIGH) delay(10);
    delay(DEBOUNCE_MS);
}

// ═══════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial1.begin(FPGA_BAUD, SERIAL_8N1, FPGA_RX_PIN, -1);  // RX only, no TX
    delay(500);
    Serial.println("\n════════════════════════════════════");
    Serial.println("  LED SHOOTER — SURVIVAL MODE");
    Serial.println("════════════════════════════════════");

    // LED strip
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LEDS_COUNT);
    FastLED.setBrightness(BRIGHTNESS);
    fill_solid(leds, LEDS_COUNT, CRGB::Black);
    FastLED.show();

    // TFT display — remap SPI pins BEFORE tft.begin()
    SPI.begin(TFT_SCLK, -1, TFT_SDA, TFT_CS);
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, HIGH);
    tft.begin(TFT_FREQ);
    tft.setRotation(0);
    tft.fillScreen(GC9A01A_BLACK);
    delay(50);
    tftDrawUI();

    // Buttons & encoder
    pinMode(BTN_SHOOT,  INPUT_PULLUP);
    pinMode(ENCODER_A,  INPUT_PULLUP);
    pinMode(ENCODER_B,  INPUT_PULLUP);

    // Buzzer
    ledcAttach(BUZZER_PIN, 2000, 8);

    // Diagnostics
    delay(100);
    Serial.println("\n[DIAG] Pin states at boot:");
    Serial.printf("  SHOOT     (GPIO %d): %s\n", BTN_SHOOT,  digitalRead(BTN_SHOOT)  ? "HIGH (ok)" : "LOW (stuck?)");
    Serial.printf("  ENCODER_A (GPIO %d): %s\n", ENCODER_A,  digitalRead(ENCODER_A)  ? "HIGH (ok)" : "LOW (stuck?)");
    Serial.printf("  ENCODER_B (GPIO %d): %s\n", ENCODER_B,  digitalRead(ENCODER_B)  ? "HIGH (ok)" : "LOW (stuck?)");
    Serial.println();

    // Init encoder tracking
    lastEncoderA = digitalRead(ENCODER_A);

    Serial.printf("Strip: %d LEDs | Speed: %d→%dms | Lives: %d\n",
                  LEDS_COUNT, SPEED_Q0, SPEED_Q3, MAX_LIVES);
    Serial.printf("Seed mode: %s\n", USE_FIXED_SEED ? "FIXED" : "FPGA (waiting for seed...)");
    Serial.println("\nPress SHOOT to start...\n");
}

// ═══════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════
void loop() {

    // Always read FPGA UART (captures latest seed in background)
    readFPGAUart();

    // ══════════════════════════════════════
    //  STATE 1: WAITING TO START
    // ══════════════════════════════════════
    if (!gameStarted) {
        showStandby();

        // In FPGA mode, block until a seed arrives
        if (!USE_FIXED_SEED && !fpgaSeedReady) {
            static unsigned long lastWaitMsg = 0;
            if (millis() - lastWaitMsg > 3000) {
                Serial.println("[WAIT] Waiting for FPGA seed...");
                lastWaitMsg = millis();
            }
            delay(30);
            return;
        }

        if (digitalRead(BTN_SHOOT) == LOW) {
            delay(DEBOUNCE_MS);
            if (digitalRead(BTN_SHOOT) == LOW) {
                gameStarted = true;
                score = 0;
                streak = 0;
                expandSeed(getGameSeed());
                Serial.println("[START] Game running!");
                showLives();
                tftDrawLives(lives);
                renderStrip();
                lastScrollTime = millis();

                while (digitalRead(BTN_SHOOT) == LOW) delay(10);
                delay(100);
            }
        }
        delay(30);
        return;
    }

    // ══════════════════════════════════════
    //  STATE 2: GAME OVER
    // ══════════════════════════════════════
    if (gameOver) {
        // Play animation only once
        if (!gameOverShown) {
            gameWon ? showWin() : showLose();
            if (!gameWon) playGameOverSound();
            tftDrawGameOver(gameWon);
            Serial.println("Press SHOOT to play again...");
            Serial.println("(Showing incoming FPGA seeds)\n");
            gameOverShown = true;
        }

        // Keep reading FPGA seeds (readFPGAUart runs at top of loop)
        // Wait for SHOOT press (non-blocking, edge-triggered)
        static bool prevShootGameOver = false;
        bool shootNow = (digitalRead(BTN_SHOOT) == LOW);

        if (shootNow && !prevShootGameOver) {
            // Reset and start immediately with new seed
            lives         = MAX_LIVES;
            gameOver      = false;
            gameOverShown = false;
            gameWon       = false;
            shooting      = false;
            gameStarted   = true;
            selectedColor = 0;
            score         = 0;
            streak        = 0;

            expandSeed(getGameSeed());
            tftDrawUI();
            showLives();
            tftDrawLives(lives);
            renderStrip();
            lastScrollTime = millis();
            Serial.println("[START] New game running!");
        }
        prevShootGameOver = shootNow;
        delay(10);
        return;
    }

    // ══════════════════════════════════════
    //  STATE 3: GAME RUNNING
    // ══════════════════════════════════════
    unsigned long now = millis();

    // ── Read inputs ──
    bool shootPressed  = (digitalRead(BTN_SHOOT) == LOW);
    int  currentA      = digitalRead(ENCODER_A);

    static bool lastShootState = false;
    static bool prevShoot = false;

    // ── Log SHOOT button changes ──
    if (shootPressed != prevShoot) {
        Serial.printf("[BTN] SHOOT (GPIO %d): %s\n", BTN_SHOOT, shootPressed ? "PRESSED ▼" : "RELEASED ▲");
        prevShoot = shootPressed;
    }

    // ── Rotary encoder: dual-edge with settle + confirm ──
    if (currentA != lastEncoderA) {
        if (now - lastEncoderClickTime >= ENCODER_DEBOUNCE_MS) {
            delayMicroseconds(ENCODER_SETTLE_US);

            int aConfirm = digitalRead(ENCODER_A);
            if (aConfirm != lastEncoderA) {
                // Majority vote on B
                int bSum = 0;
                for (int i = 0; i < 5; i++) {
                    bSum += digitalRead(ENCODER_B);
                    delayMicroseconds(200);
                }
                bool bHigh = (bSum >= 3);

                // Direction flips on rising vs falling edge
                bool isCW = (aConfirm == LOW) ? bHigh : !bHigh;

                if (isCW) {
                    selectedColor = (selectedColor + 1) % 3;
                    Serial.printf("[ENCODER] CW  → %s\n", colorNames[selectedColor]);
                } else {
                    selectedColor = (selectedColor + 2) % 3;
                    Serial.printf("[ENCODER] CCW → %s\n", colorNames[selectedColor]);
                }

                lastEncoderClickTime = millis();
                lastEncoderA = aConfirm;
            }
        } else {
            lastEncoderA = currentA;
        }
    }

    // ── Periodic status ──
    static unsigned long lastDebugTime = 0;
    if (now - lastDebugTime > 2000) {
        lastDebugTime = now;
        Serial.printf("[STATUS] SHOOT=%d ENC_A=%d | selected=%s lives=%d remaining=%d\n",
                      shootPressed, currentA,
                      colorNames[selectedColor],
                      lives, remaining);
    }

    // ── Scroll tick ──
    if (!shooting && (now - lastScrollTime >= getCurrentScrollSpeed())) {
        lastScrollTime = now;

        if (!scrollStrip()) {
            gameOver = true;
            Serial.println("[BREACH] Obstacle reached LED 0!");
            return;
        }

        if (queueHead >= queueLen && countStripActive() == 0) {
            gameWon = true;
            gameOver = true;
            return;
        }
    }

    // ── Shoot (edge-triggered) ──
    if (shootPressed && !lastShootState) {
        if (selectedColor >= 0 && !shooting) {
            Serial.printf("[FIRE] Shooting %s\n", colorNames[selectedColor]);
            handleShot(selectedColor);
        }
    }
    lastShootState = shootPressed;

    renderStrip();
    delay(10);
}
