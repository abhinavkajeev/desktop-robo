/*
 * Robi ESP32-S3 Mini — RoboEyes-Style Animations on U8g2
 * ========================================================
 * Smooth animated robot eyes inspired by FluxGarage RoboEyes,
 * re-implemented on U8g2 for stable SSD1306 support.
 *
 * Features:
 *   - Smooth eased eye movement with curiosity (outer eye stretches)
 *   - Auto-blinker with random intervals
 *   - Idle mode with random eye repositioning
 *   - Moods: DEFAULT, HAPPY, TIRED, ANGRY
 *   - Animations: laugh (bounce), confused (shake), wake-up blink
 *   - Touch: single tap → happy, double tap → excited
 *   - Text display with typewriter reveal
 *
 * Hardware: SSD1306 128×64 I2C (SDA=6, SCL=7), Touch GPIO 4
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>

// ═══════════════════════════════════════════════════════════════════════
//  CONFIG
// ═══════════════════════════════════════════════════════════════════════
static const char* WIFI_SSID     = "Spiderman_4g";
static const char* WIFI_PASSWORD = "akashtheboss";
static const char* SERVER        = "http://192.168.29.209:5001";

#define I2C_SDA   6
#define I2C_SCL   7
#define TOUCH_PIN 13

// MPU6050 shares I2C bus with OLED (GPIO 6/7)

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

// ═══════════════════════════════════════════════════════════════════════
//  TIMING
// ═══════════════════════════════════════════════════════════════════════
#define MS_PER_WORD       330UL
#define EXTRA_DISPLAY_MS  2000UL
#define POLL_INTERVAL_MS  2000UL
// TTP223 touch module: digital HIGH when touched, LOW when idle

// ═══════════════════════════════════════════════════════════════════════
//  EYE CONFIGURATION (RoboEyes-style)
// ═══════════════════════════════════════════════════════════════════════
#define EYE_W         36    // eye width
#define EYE_H         36    // eye height (default mood)
#define EYE_RADIUS    8     // border radius
#define EYE_SPACE     10    // space between eyes
#define PUPIL_W       12    // pupil width
#define PUPIL_H       12    // pupil height
#define SCREEN_W      128
#define SCREEN_H      64

// Eye center positions (calculated)
#define EYE_L_CX  (SCREEN_W / 2 - EYE_SPACE / 2 - EYE_W / 2)
#define EYE_R_CX  (SCREEN_W / 2 + EYE_SPACE / 2 + EYE_W / 2)
#define EYE_CY    (SCREEN_H / 2)

// ═══════════════════════════════════════════════════════════════════════
//  MOOD SYSTEM
// ═══════════════════════════════════════════════════════════════════════
enum Mood { MOOD_DEFAULT, MOOD_HAPPY, MOOD_TIRED, MOOD_ANGRY };
Mood currentMood = MOOD_DEFAULT;

// Mood modifiers: changes to eye shape per mood
struct MoodParams {
    int topTrimL, topTrimR;     // pixels trimmed from top of each eye
    int bottomTrimL, bottomTrimR; // pixels trimmed from bottom
};

MoodParams getMoodParams(Mood m) {
    switch (m) {
        case MOOD_HAPPY:  return {0, 0, 8, 8};    // bottom trimmed → ^_^ look
        case MOOD_TIRED:  return {8, 8, 0, 0};     // top trimmed → droopy
        case MOOD_ANGRY:  return {10, 10, 0, 0};   // heavy top trim → angry squint
        default:          return {0, 0, 0, 0};
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  STATE MACHINE
// ═══════════════════════════════════════════════════════════════════════
enum RobiState {
    STATE_IDLE,
    STATE_LISTENING,
    STATE_THINKING,
    STATE_HAPPY,
    STATE_EXCITED,
    STATE_TALKING
};

RobiState currentState = STATE_IDLE;
bool  wifiOK = false;

// Eye animation state
float eyeTargetX = 0, eyeTargetY = 0;   // target pupil position (-1.0 to 1.0)
float eyeCurrentX = 0, eyeCurrentY = 0; // current smoothed position
float eyeLidTop = 0, eyeLidBot = 0;     // 0 = open, 1 = closed (for blink)
float eyeLidTargetTop = 0, eyeLidTargetBot = 0;

// Auto-blinker
bool        autoBlinkOn = true;
unsigned long nextBlinkTime = 0;
bool        isBlinking = false;
unsigned long blinkStartTime = 0;

// Idle mode
bool        idleModeOn = true;
unsigned long nextIdleMove = 0;

// Curiosity
bool        curiosityOn = true;

// One-shot animations
enum Anim { ANIM_NONE, ANIM_LAUGH, ANIM_CONFUSED, ANIM_WAKEUP, ANIM_SLEEP };
Anim currentAnim = ANIM_NONE;
unsigned long animStart = 0;
unsigned long animDuration = 0;

// Sleep animation (very rare — every 3-5 min)
unsigned long nextSleepTime = 0;
bool isSleeping = false;

// Emo idle emotions — random short expressions
enum EmoEmotion {
    EMO_NONE,
    EMO_SUSPICIOUS,   EMO_SAD,          EMO_BORED,
    EMO_SIGH,         EMO_CURIOUS,      EMO_DIZZY,
    EMO_SNEEZE,       EMO_WINK,         EMO_STARTLED,
    EMO_SHY,          EMO_MISCHIEVOUS,  EMO_DAYDREAM,
    // New batch
    EMO_ANGRY,        EMO_EXCITED_BOUNCE, EMO_LOVE,
    EMO_CONFUSED,     EMO_PROUD,        EMO_SCARED,
    EMO_PLAYFUL,      EMO_GRUMPY,       EMO_GLITCH,
    EMO_PEEK,         EMO_ROLL_EYES,    EMO_CROSS_EYED,
    EMO_FOCUSED,      EMO_VIBING,       EMO_SMUG,
    EMO_SHOCKED,      EMO_SLEEPY_BLINK, EMO_GIGGLE,
    EMO_COUNT = 30
};
EmoEmotion currentEmotion = EMO_NONE;
unsigned long emotionStart = 0;
unsigned long emotionDuration = 0;
unsigned long nextEmotionTime = 0;

// Smooth transition timer
unsigned long transitionStart = 0;
#define TRANSITION_MS 300

// Motion Tracking (MPU6050 via raw I2C)
#define MPU_ADDR 0x68
bool mpuOK = false;
float gyroLeanX = 0, gyroLeanY = 0;
float gyroOffsetX = 0, gyroOffsetY = 0;  // smoothed pixel offset for whole-eye shift
unsigned long lastMotionCheck = 0;

// State timing
unsigned long stateStart = 0;
RobiState prevAnimState = STATE_IDLE;

// Text
String        aiText      = "";
unsigned long textStart   = 0;
unsigned long textDisplayDuration = 5000UL;

// Touch — interrupt-driven for reliability
volatile bool touchFlag      = false;   // set by ISR
unsigned long lastTouchTime   = 0;
int           tapCount        = 0;
unsigned long doubleTapWindow = 400;

// Polling
unsigned long lastPoll = 0;

// ═══════════════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════════════
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("WiFi");
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print('.');
        if (millis() - t > 20000UL) {
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            t = millis();
        }
    }
    Serial.print(" OK: "); Serial.println(WiFi.localIP());
    wifiOK = true;
}

// ═══════════════════════════════════════════════════════════════════════
//  EASING FUNCTION
// ═══════════════════════════════════════════════════════════════════════
float lerp(float current, float target, float speed) {
    return current + (target - current) * speed;
}

// ═══════════════════════════════════════════════════════════════════════
//  DRAW SINGLE EYE (RoboEyes style)
// ═══════════════════════════════════════════════════════════════════════
void drawEye(int cx, int cy, int w, int h, float pupilX, float pupilY,
             int topTrim, int bottomTrim, float lidTop, float lidBot) {
    int hw = w / 2, hh = h / 2;

    // Effective height after mood trims
    int effectiveTop = cy - hh + topTrim;
    int effectiveBot = cy + hh - bottomTrim;
    int effectiveH = effectiveBot - effectiveTop;
    if (effectiveH < 4) effectiveH = 4;

    // Apply eyelid (blink) — shrink from top/bottom
    int lidTopPx = (int)(lidTop * effectiveH / 2);
    int lidBotPx = (int)(lidBot * effectiveH / 2);
    int drawTop = effectiveTop + lidTopPx;
    int drawBot = effectiveBot - lidBotPx;
    int drawH = drawBot - drawTop;
    if (drawH < 2) drawH = 2;

    int radius = min(EYE_RADIUS, drawH / 2);

    // Emo style: whole eye shifts position based on look direction
    int shiftX = (int)(pupilX * 10.0f);  // max ±10px horizontal shift
    int shiftY = (int)(pupilY * 5.0f);   // max ±5px vertical shift

    // Draw eye (filled rounded rect) — shifted by look direction
    u8g2.setDrawColor(1);
    u8g2.drawRBox(cx - hw + shiftX, drawTop + shiftY, w, drawH, radius);
}

// ═══════════════════════════════════════════════════════════════════════
//  AUTO-BLINKER
// ═══════════════════════════════════════════════════════════════════════
void updateAutoBlink() {
    if (!autoBlinkOn) return;

    unsigned long now = millis();

    if (!isBlinking && now >= nextBlinkTime) {
        isBlinking = true;
        blinkStartTime = now;
        eyeLidTargetTop = 1.0f;
        eyeLidTargetBot = 1.0f;
    }

    if (isBlinking) {
        unsigned long elapsed = now - blinkStartTime;
        if (elapsed < 80) {
            // Closing
            eyeLidTargetTop = 1.0f;
            eyeLidTargetBot = 1.0f;
        } else if (elapsed < 180) {
            // Opening
            eyeLidTargetTop = 0.0f;
            eyeLidTargetBot = 0.0f;
        } else {
            isBlinking = false;
            eyeLidTargetTop = 0.0f;
            eyeLidTargetBot = 0.0f;
            // Schedule next blink: 2-5 seconds
            nextBlinkTime = now + 2000 + random(0, 3000);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  IDLE MODE — random eye repositioning
// ═══════════════════════════════════════════════════════════════════════
void updateIdleMode() {
    if (!idleModeOn) return;

    unsigned long now = millis();
    if (now >= nextIdleMove) {
        // Emo style: bigger wandering range for whole-eye movement
        eyeTargetX = (random(-100, 101)) / 100.0f;
        eyeTargetY = (random(-60, 61)) / 100.0f;
        nextIdleMove = now + 1500 + random(0, 2500);
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  ONE-SHOT ANIMATIONS
// ═══════════════════════════════════════════════════════════════════════
void startAnim(Anim a, unsigned long duration) {
    currentAnim = a;
    animStart = millis();
    animDuration = duration;
}

// Returns a Y offset for the current animation frame
float getAnimOffsetY() {
    if (currentAnim == ANIM_NONE) return 0;
    unsigned long t = millis() - animStart;
    if (t > animDuration) {
        currentAnim = ANIM_NONE;
        return 0;
    }
    float progress = (float)t / (float)animDuration;

    switch (currentAnim) {
        case ANIM_LAUGH: {
            // Rapid vertical bouncing that decays
            float decay = 1.0f - progress;
            return sin(t / 40.0f) * 4.0f * decay;
        }
        case ANIM_CONFUSED: {
            // Horizontal shaking
            float decay = 1.0f - progress;
            eyeCurrentX = sin(t / 30.0f) * 0.6f * decay;
            return 0;
        }
        case ANIM_WAKEUP: {
            // Eyes snap open from closed
            if (progress < 0.3f) {
                eyeLidTargetTop = 1.0f - (progress / 0.3f);
                eyeLidTargetBot = 1.0f - (progress / 0.3f);
            } else if (progress < 0.5f) {
                // Overshoot — slightly past open
                eyeLidTargetTop = 0.0f;
                eyeLidTargetBot = 0.0f;
            } else {
                eyeLidTargetTop = 0.0f;
                eyeLidTargetBot = 0.0f;
            }
            return 0;
        }
        default: return 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  CUTE FACES — Clean Emo-style crescent eyes
// ═══════════════════════════════════════════════════════════════════════

// Draw a single crescent eye: filled ellipse masked from bottom
void drawCrescentEye(int cx, int cy, int rx, int ry, int cutShift) {
    u8g2.setDrawColor(1);
    u8g2.drawFilledEllipse(cx, cy, rx, ry);
    u8g2.setDrawColor(0);
    u8g2.drawFilledEllipse(cx, cy + cutShift, rx + 1, ry);
    u8g2.setDrawColor(1);
}

void drawCuteHappyFace(int offsetY) {
    unsigned long t = millis() - stateStart;

    // Smooth entry: ramp up over 300ms
    float entry = min(1.0f, (float)t / 300.0f);
    int cutShift = (int)(entry * 8);

    int lcx = EYE_L_CX, rcx = EYE_R_CX, cy = EYE_CY + offsetY;
    int rx = EYE_W / 2, ry = EYE_H / 2;
    drawCrescentEye(lcx, cy, rx, ry, cutShift);
    drawCrescentEye(rcx, cy, rx, ry, cutShift);
}

void drawCuteExcitedFace(int offsetY) {
    unsigned long t = millis() - stateStart;
    float transition = min(1.0f, (float)t / 250.0f);
    int cutShift = (int)(transition * 10);
    float damping = max(0.0f, 1.0f - (float)t / 2000.0f);
    int bounceY = (int)(3.0f * sin(t / 70.0f) * damping);
    int lcx = EYE_L_CX, rcx = EYE_R_CX, cy = EYE_CY + offsetY + bounceY;
    int rx = EYE_W / 2, ry = EYE_H / 2;
    drawCrescentEye(lcx, cy, rx, ry, cutShift);
    drawCrescentEye(rcx, cy, rx, ry, cutShift);
}

// ═══════════════════════════════════════════════════════════════════════
//  SLEEPING FACE — Emo-style yawn → sleep → wake
// ═══════════════════════════════════════════════════════════════════════
//  Phase 1 (0-2s):     Yawn — eyes slowly droop + mouth opens
//  Phase 2 (2s-27s):   Sleep — cute curved closed eyes + breathing + Zzz
//  Phase 3 (27s-30s):  Wake — stretch, eyes open, double blink

#define SLEEP_TOTAL_MS 30000UL
#define YAWN_END_MS    2000UL
#define WAKE_START_MS  27000UL

// Draw a cute curved closed eye (gentle smile arc) — like Emo's sleeping face
void drawSleepArc(int cx, int cy, int halfW, int thickness) {
    for (int i = -halfW; i <= halfW; i++) {
        float norm = (float)(i * i) / (float)(halfW * halfW);
        // Gentle downward curve
        int y = cy + (int)(4.0f * (1.0f - norm));
        for (int t = 0; t < thickness; t++) {
            u8g2.drawPixel(cx + i, y - t);
        }
    }
}

void drawSleepingFace() {
    unsigned long t = millis() - animStart;
    int lcx = EYE_L_CX, rcx = EYE_R_CX, cy = EYE_CY;
    int rx = EYE_W / 2, ry = EYE_H / 2;

    // ── PHASE 1: YAWN (0 – 2s) ─────────────────────────────────────
    if (t < YAWN_END_MS) {
        float p = (float)t / (float)YAWN_END_MS;

        // Eyes slowly droop into crescents
        int cutShift = (int)(p * 12);
        drawCrescentEye(lcx, cy, rx, ry, cutShift);
        drawCrescentEye(rcx, cy, rx, ry, cutShift);

        // Mouth opens into an oval (yawning)
        int mouthRx = (int)(p * 6);
        int mouthRy = (int)(p * 9);
        if (mouthRx > 1) {
            u8g2.drawEllipse(SCREEN_W / 2, cy + 22, mouthRx, mouthRy);
            // Fill the mouth oval slightly for depth
            if (mouthRx > 3) {
                u8g2.drawEllipse(SCREEN_W / 2, cy + 22, mouthRx - 2, mouthRy - 2);
            }
        }

    // ── PHASE 2: SLEEP (2s – 27s) ─────────────────────────────────
    } else if (t < WAKE_START_MS) {
        unsigned long st = t - YAWN_END_MS;

        // Gentle breathing: slow up/down bob
        int breathe = (int)(2.0f * sin(st / 600.0f));

        // Cute curved closed eyes (like gentle smile arcs)
        drawSleepArc(lcx, cy + 2 + breathe, rx + 2, 3);
        drawSleepArc(rcx, cy + 2 + breathe, rx + 2, 3);

        // Z bubble — loops every 3 seconds, resets position
        unsigned long zCycle = st % 3000UL;
        float zp = (float)zCycle / 3000.0f;
        int zBaseX = rcx + 14;
        int zBaseY = cy - 8;
        float bob = 1.5f * sin(zCycle / 200.0f);

        // Three Z's at different sizes, rising up
        int z1Y = zBaseY - (int)(zp * 20) + (int)bob;
        int z2Y = z1Y - 8 - (int)(zp * 6);
        int z3Y = z2Y - 6 - (int)(zp * 4);

        // Fade: only show Z's that are on-screen
        u8g2.setFont(u8g2_font_7x13B_tr);
        if (z1Y > 0 && z1Y < 60) u8g2.drawStr(zBaseX, z1Y, "Z");
        u8g2.setFont(u8g2_font_6x10_tr);
        if (z2Y > 0 && z2Y < 55) u8g2.drawStr(zBaseX + 8, z2Y, "z");
        u8g2.setFont(u8g2_font_5x8_tr);
        if (z3Y > 0 && z3Y < 50) u8g2.drawStr(zBaseX + 14, z3Y, "z");

    // ── PHASE 3: WAKE UP (27s – 30s) ──────────────────────────────
    } else {
        unsigned long wt = t - WAKE_START_MS;
        float p = min(1.0f, (float)wt / 2000.0f);

        if (p < 0.3f) {
            // Still groggy — curved sleep arcs, slight stretch
            float stretch = p / 0.3f;
            int breathe = (int)(3.0f * stretch);
            drawSleepArc(lcx, cy + 2 - breathe, rx + 2, 3);
            drawSleepArc(rcx, cy + 2 - breathe, rx + 2, 3);
        } else if (p < 0.6f) {
            // Eyes opening — growing from slit to full
            float op = (p - 0.3f) / 0.3f;
            int eyeH = max(4, (int)(EYE_H * op));
            drawEye(lcx, cy, EYE_W, eyeH, 0, 0, 0, 0, 0, 0);
            drawEye(rcx, cy, EYE_W, eyeH, 0, 0, 0, 0, 0, 0);
        } else if (p < 0.75f) {
            // First blink — quick close
            float bp = (p - 0.6f) / 0.15f;
            float sq = (bp < 0.5f) ? bp * 2.0f : (1.0f - bp) * 2.0f;
            drawEye(lcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, sq, sq);
            drawEye(rcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, sq, sq);
        } else if (p < 0.9f) {
            // Second blink — quick close
            float bp = (p - 0.75f) / 0.15f;
            float sq = (bp < 0.5f) ? bp * 2.0f : (1.0f - bp) * 2.0f;
            drawEye(lcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, sq, sq);
            drawEye(rcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, sq, sq);
        } else {
            // Fully awake
            drawEye(lcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, 0, 0);
            drawEye(rcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, 0, 0);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  EMO IDLE EMOTIONS — random short expressions
// ═══════════════════════════════════════════════════════════════════════

void drawEmoEmotion() {
    unsigned long t = millis() - emotionStart;
    float p = min(1.0f, (float)t / (float)emotionDuration);
    int lcx = EYE_L_CX, rcx = EYE_R_CX, cy = EYE_CY;

    switch (currentEmotion) {

    // ── SUSPICIOUS: one eye squints, looks sideways ───────────────
    case EMO_SUSPICIOUS: {
        float entry = min(1.0f, p * 4.0f);       // 0→1 in first 25%
        float hold = (p > 0.75f) ? (p - 0.75f) * 4.0f : 0.0f;  // exit
        float e = entry * (1.0f - hold);         // ramp up then down

        // Left eye normal, right eye squints
        float lookX = e * 0.7f;
        drawEye(lcx, cy, EYE_W, EYE_H, lookX, 0,
                0, 0, 0, 0);
        drawEye(rcx, cy, EYE_W, (int)(EYE_H * (1.0f - e * 0.4f)), lookX, 0,
                (int)(e * 6), 0, 0, 0);
        break;
    }

    // ── SAD: droopy eyes + tear ───────────────────────────────
    case EMO_SAD: {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);

        // Both eyes droop (top trim increases)
        int droop = (int)(e * 10);
        drawEye(lcx, cy + (int)(e * 3), EYE_W, EYE_H, 0, e * 0.5f,
                droop, 0, 0, 0);
        drawEye(rcx, cy + (int)(e * 3), EYE_W, EYE_H, 0, e * 0.5f,
                droop, 0, 0, 0);

        // Tear drop falling from left eye
        if (e > 0.3f) {
            float tearP = min(1.0f, (e - 0.3f) / 0.7f);
            int tearY = cy + (int)(e * 3) + EYE_H / 2 + (int)(tearP * 16);
            u8g2.drawDisc(lcx + 4, tearY, 2);
            // Tear trail
            if (tearP > 0.3f) {
                u8g2.drawPixel(lcx + 4, tearY - 3);
                u8g2.drawPixel(lcx + 4, tearY - 5);
            }
        }
        break;
    }

    // ── BORED: half-lidded, slow drift to side ─────────────────
    case EMO_BORED: {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);

        // Half-lidded (top eyelid drops)
        float lidDroop = e * 0.5f;
        // Slow drift to one side
        float driftX = sin(p * PI) * 0.6f;

        drawEye(lcx, cy, EYE_W, EYE_H, driftX, 0,
                (int)(e * 8), 0, lidDroop, 0);
        drawEye(rcx, cy, EYE_W, EYE_H, driftX, 0,
                (int)(e * 8), 0, lidDroop, 0);
        break;
    }

    // ── DRAMATIC SIGH: close, hold, slow reopen looking away ─────
    case EMO_SIGH: {
        if (p < 0.2f) {
            // Close eyes slowly
            float cp = p / 0.2f;
            drawEye(lcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, cp, cp);
            drawEye(rcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, cp, cp);
        } else if (p < 0.6f) {
            // Held closed — just flat lines with breathing
            int breathe = (int)(1.5f * sin((p - 0.2f) * 15.0f));
            drawSleepArc(lcx, cy + breathe, EYE_W/2 + 2, 2);
            drawSleepArc(rcx, cy + breathe, EYE_W/2 + 2, 2);
        } else {
            // Slow reopen, looking away dramatically
            float op = (p - 0.6f) / 0.4f;
            float lookAway = (1.0f - op) * -0.8f;
            drawEye(lcx, cy, EYE_W, EYE_H, lookAway, -0.3f * (1.0f - op),
                    0, 0, 1.0f - op, 1.0f - op);
            drawEye(rcx, cy, EYE_W, EYE_H, lookAway, -0.3f * (1.0f - op),
                    0, 0, 1.0f - op, 1.0f - op);
        }
        break;
    }

    // ── CURIOUS: one eye bigger, peek to side ──────────────────
    case EMO_CURIOUS: {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);

        float peek = e * 0.8f;
        int bigEye = (int)(e * 6);  // right eye grows

        drawEye(lcx, cy, EYE_W, EYE_H, peek, -e * 0.2f,
                0, 0, 0, 0);
        drawEye(rcx, cy - (int)(e * 2), EYE_W + bigEye, EYE_H + bigEye,
                peek, -e * 0.2f, 0, 0, 0, 0);
        break;
    }

    // ── DIZZY: eyes shake rapidly ─────────────────────────────
    case EMO_DIZZY: {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.7f) ? (p - 0.7f) / 0.3f : 0.0f;
        float e = entry * (1.0f - hold);

        // Rapid X shake + spiral pupils
        float shake = sin(t / 25.0f) * e * 0.8f;
        float spiral = cos(t / 40.0f) * e * 0.4f;

        drawEye(lcx + (int)(shake * 4), cy, EYE_W, EYE_H,
                shake, spiral, 0, 0, 0, 0);
        drawEye(rcx + (int)(shake * 4), cy, EYE_W, EYE_H,
                shake, spiral, 0, 0, 0, 0);

        // Spiral circles around eyes for dizziness
        if (e > 0.3f) {
            float sa = t / 100.0f;
            int sr = (int)(8 * e);
            u8g2.drawPixel(lcx + (int)(sr * cos(sa)), cy - 20 + (int)(sr * sin(sa)));
            u8g2.drawPixel(rcx + (int)(sr * cos(sa + PI)), cy - 20 + (int)(sr * sin(sa + PI)));
        }
        break;
    }

    default:
        break;
    }

    // ── NEW EMOTIONS ──────────────────────────────────────────────────
    // (using if-else since they're after the switch)
    if (currentEmotion == EMO_SNEEZE) {
        // Squeeze eyes shut → head bobs forward → recover
        if (p < 0.3f) {
            // Building up — eyes slowly squeezing
            float sq = p / 0.3f;
            drawEye(lcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, sq * 0.8f, sq * 0.8f);
            drawEye(rcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, sq * 0.8f, sq * 0.8f);
        } else if (p < 0.5f) {
            // ACHOO! — eyes fully shut, head bobs down
            float bob = sin((p - 0.3f) / 0.2f * PI) * 8;
            drawSleepArc(lcx, cy + (int)bob, EYE_W/2, 3);
            drawSleepArc(rcx, cy + (int)bob, EYE_W/2, 3);
            // Sneeze particles
            for (int i = 0; i < 4; i++) {
                float a = (p - 0.3f) * 20.0f + i * 1.5f;
                int px = SCREEN_W/2 + (int)((p - 0.3f) * 200 * cos(a));
                int py = cy + 20 + (int)((p - 0.3f) * 100 * sin(a));
                if (px > 0 && px < 128 && py > 0 && py < 64)
                    u8g2.drawPixel(px, py);
            }
        } else {
            // Recovery — eyes reopen with a blink
            float op = (p - 0.5f) / 0.5f;
            float sq = (op < 0.3f) ? (1.0f - op / 0.3f) * 0.6f : 0;
            drawEye(lcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, sq, sq);
            drawEye(rcx, cy, EYE_W, EYE_H, 0, 0, 0, 0, sq, sq);
        }

    } else if (currentEmotion == EMO_WINK) {
        // Smooth wink with right eye
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.7f) ? (p - 0.7f) / 0.3f : 0.0f;
        float e = entry * (1.0f - hold);

        // Left eye stays open, right closes
        drawEye(lcx, cy, EYE_W, EYE_H, 0.2f * e, 0, 0, 0, 0, 0);
        drawEye(rcx, cy, EYE_W, EYE_H, 0.2f * e, 0, 0, 0, e, e);

    } else if (currentEmotion == EMO_STARTLED) {
        // Eyes snap wide, then slowly relax
        float entry = min(1.0f, p * 6.0f);  // very fast ramp
        float hold = (p > 0.5f) ? (p - 0.5f) / 0.5f : 0.0f;
        float e = entry * (1.0f - hold);

        int extraSize = (int)(e * 8);  // eyes grow bigger
        drawEye(lcx, cy - (int)(e * 3), EYE_W + extraSize, EYE_H + extraSize,
                0, -e * 0.3f, 0, 0, 0, 0);
        drawEye(rcx, cy - (int)(e * 3), EYE_W + extraSize, EYE_H + extraSize,
                0, -e * 0.3f, 0, 0, 0, 0);

    } else if (currentEmotion == EMO_SHY) {
        // Eyes shrink, look down, slight inward tilt
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.75f) ? (p - 0.75f) * 4.0f : 0.0f;
        float e = entry * (1.0f - hold);

        int shrink = (int)(e * 6);
        drawEye(lcx + (int)(e * 3), cy + (int)(e * 4),
                EYE_W - shrink, EYE_H - shrink,
                0.2f * e, e * 0.6f, 0, 0, 0, 0);
        drawEye(rcx - (int)(e * 3), cy + (int)(e * 4),
                EYE_W - shrink, EYE_H - shrink,
                -0.2f * e, e * 0.6f, 0, 0, 0, 0);

    } else if (currentEmotion == EMO_MISCHIEVOUS) {
        // Narrow sly eyes, looking to one side
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);

        int squish = (int)(e * 10);  // compress vertically
        float slyLook = e * 0.8f;
        drawEye(lcx, cy, EYE_W, EYE_H - squish, slyLook, 0,
                (int)(e * 5), (int)(e * 3), 0, 0);
        drawEye(rcx, cy, EYE_W, EYE_H - squish, slyLook, 0,
                (int)(e * 5), (int)(e * 3), 0, 0);

    } else if (currentEmotion == EMO_DAYDREAM) {
        // Eyes float upward dreamily, slow gentle motion
        float entry = min(1.0f, p * 2.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);

        float floatY = -e * 0.6f;  // look up
        float gentle = sin(p * PI * 2) * 0.15f;  // gentle sway

        // Slightly droopy/dreamy lids
        drawEye(lcx, cy - (int)(e * 2), EYE_W, EYE_H, gentle, floatY,
                (int)(e * 4), 0, e * 0.2f, 0);
        drawEye(rcx, cy - (int)(e * 2), EYE_W, EYE_H, gentle, floatY,
                (int)(e * 4), 0, e * 0.2f, 0);

        // Floating sparkle
        if (e > 0.3f) {
            float sa = t / 200.0f;
            int sx = SCREEN_W/2 + (int)(20.0f * cos(sa));
            int sy = 8 + (int)(4.0f * sin(sa * 1.5f));
            u8g2.drawLine(sx - 2, sy, sx + 2, sy);
            u8g2.drawLine(sx, sy - 2, sx, sy + 2);
        }
    }

    // ── ANGRY: squint + shake ──────────────────────────────────
    if (currentEmotion == EMO_ANGRY) {
        float entry = min(1.0f, p * 4.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);
        float shake = sin(t / 20.0f) * e * 3;
        drawEye(lcx + (int)shake, cy, EYE_W, (int)(EYE_H * (1.0f - e*0.35f)), 0, 0,
                (int)(e * 10), (int)(e * 4), 0, 0);
        drawEye(rcx + (int)shake, cy, EYE_W, (int)(EYE_H * (1.0f - e*0.35f)), 0, 0,
                (int)(e * 10), (int)(e * 4), 0, 0);
    }

    // ── EXCITED BOUNCE: rapid vertical bouncing ────────────────
    else if (currentEmotion == EMO_EXCITED_BOUNCE) {
        float e = (p < 0.1f) ? p * 10 : ((p > 0.85f) ? (1.0f - p) * 6.67f : 1.0f);
        e = min(1.0f, max(0.0f, e));
        int bounce = (int)(sin(t / 40.0f) * 6 * e);
        int grow = (int)(e * 4);
        drawEye(lcx, cy + bounce, EYE_W + grow, EYE_H + grow, 0, 0, 0, 0, 0, 0);
        drawEye(rcx, cy + bounce, EYE_W + grow, EYE_H + grow, 0, 0, 0, 0, 0, 0);
    }

    // ── LOVE: heart-shaped eyes ────────────────────────────────
    else if (currentEmotion == EMO_LOVE) {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);
        float pulse = 1.0f + 0.15f * sin(t / 80.0f) * e;
        for (int eye = 0; eye < 2; eye++) {
            int ecx = (eye == 0) ? lcx : rcx;
            int r = (int)(9 * e * pulse);
            if (r > 2) {
                u8g2.drawDisc(ecx - r/2, cy - r/3, r/2);
                u8g2.drawDisc(ecx + r/2, cy - r/3, r/2);
                u8g2.drawTriangle(ecx - r, cy, ecx + r, cy, ecx, cy + r);
            }
        }
    }

    // ── CONFUSED: one eye bigger, head tilt ─────────────────────
    else if (currentEmotion == EMO_CONFUSED) {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.75f) ? (p - 0.75f) * 4.0f : 0.0f;
        float e = entry * (1.0f - hold);
        int tilt = (int)(e * 4);
        drawEye(lcx, cy - tilt, EYE_W, EYE_H + (int)(e * 4), -0.3f * e, -0.2f * e, 0, 0, 0, 0);
        drawEye(rcx, cy + tilt, EYE_W, EYE_H - (int)(e * 3), 0.3f * e, 0.2f * e,
                (int)(e * 3), 0, 0, 0);
        if (e > 0.5f) { u8g2.drawStr(rcx + 14, cy - 14, "?"); }
    }

    // ── PROUD: chin up, eyes narrow upward ──────────────────────
    else if (currentEmotion == EMO_PROUD) {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);
        drawEye(lcx, cy - (int)(e * 5), EYE_W, EYE_H, 0, -e * 0.4f,
                0, (int)(e * 6), e * 0.3f, 0);
        drawEye(rcx, cy - (int)(e * 5), EYE_W, EYE_H, 0, -e * 0.4f,
                0, (int)(e * 6), e * 0.3f, 0);
    }

    // ── SCARED: wide eyes, trembling ────────────────────────────
    else if (currentEmotion == EMO_SCARED) {
        float entry = min(1.0f, p * 5.0f);
        float hold = (p > 0.7f) ? (p - 0.7f) / 0.3f : 0.0f;
        float e = entry * (1.0f - hold);
        int tremble = (int)(sin(t / 15.0f) * 2 * e);
        int grow = (int)(e * 5);
        drawEye(lcx + tremble, cy, EYE_W + grow, EYE_H + grow, 0, -e*0.2f, 0, 0, 0, 0);
        drawEye(rcx + tremble, cy, EYE_W + grow, EYE_H + grow, 0, -e*0.2f, 0, 0, 0, 0);
    }

    // ── PLAYFUL: alternating bounce ─────────────────────────────
    else if (currentEmotion == EMO_PLAYFUL) {
        float e = (p < 0.1f) ? p * 10 : ((p > 0.85f) ? (1.0f - p) * 6.67f : 1.0f);
        e = min(1.0f, max(0.0f, e));
        int b1 = (int)(sin(t / 50.0f) * 5 * e);
        int b2 = (int)(sin(t / 50.0f + PI) * 5 * e);
        drawEye(lcx, cy + b1, EYE_W, EYE_H, sin(t/200.0f)*0.5f*e, 0, 0, 0, 0, 0);
        drawEye(rcx, cy + b2, EYE_W, EYE_H, sin(t/200.0f)*0.5f*e, 0, 0, 0, 0, 0);
    }

    // ── GRUMPY: heavy droopy squint ─────────────────────────────
    else if (currentEmotion == EMO_GRUMPY) {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);
        drawEye(lcx, cy + (int)(e * 2), EYE_W, (int)(EYE_H * (1.0f - e*0.3f)),
                0, e * 0.3f, (int)(e * 12), 0, e * 0.3f, 0);
        drawEye(rcx, cy + (int)(e * 2), EYE_W, (int)(EYE_H * (1.0f - e*0.3f)),
                0, e * 0.3f, (int)(e * 12), 0, e * 0.3f, 0);
    }

    // ── GLITCH: screen tear/static ──────────────────────────────
    else if (currentEmotion == EMO_GLITCH) {
        float e = (p < 0.15f) ? p / 0.15f : ((p > 0.7f) ? (1.0f - p) / 0.3f : 1.0f);
        e = min(1.0f, max(0.0f, e));
        int tearOff = (int)(sin(t / 30.0f) * 8 * e);
        int split = (tearOff > 0) ? tearOff : 0;
        drawEye(lcx + (int)(e * tearOff), cy, EYE_W, EYE_H, 0, 0, 0, 0, 0, 0);
        drawEye(rcx - (int)(e * tearOff), cy, EYE_W, EYE_H, 0, 0, 0, 0, 0, 0);
        if (e > 0.5f) {
            int scanY = (t / 8) % 64;
            u8g2.drawHLine(0, scanY, 128);
        }
    }

    // ── PEEK: eyes slide to edge ────────────────────────────────
    else if (currentEmotion == EMO_PEEK) {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.75f) ? (p - 0.75f) * 4.0f : 0.0f;
        float e = entry * (1.0f - hold);
        int slide = (int)(e * 20);
        drawEye(lcx + slide, cy, EYE_W, EYE_H, e, 0, 0, 0, 0, 0);
        drawEye(rcx + slide, cy, EYE_W, EYE_H, e, 0, 0, 0, 0, 0);
    }

    // ── ROLL EYES: pupils do a full circle ──────────────────────
    else if (currentEmotion == EMO_ROLL_EYES) {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.85f) ? (p - 0.85f) / 0.15f : 0.0f;
        float e = entry * (1.0f - hold);
        float angle = p * PI * 2 * 1.5f;
        float px = cos(angle) * 0.8f * e;
        float py = sin(angle) * 0.6f * e;
        drawEye(lcx, cy, EYE_W, EYE_H, px, py, 0, 0, 0, 0);
        drawEye(rcx, cy, EYE_W, EYE_H, px, py, 0, 0, 0, 0);
    }

    // ── CROSS-EYED: pupils converge ─────────────────────────────
    else if (currentEmotion == EMO_CROSS_EYED) {
        float entry = min(1.0f, p * 4.0f);
        float hold = (p > 0.7f) ? (p - 0.7f) / 0.3f : 0.0f;
        float e = entry * (1.0f - hold);
        drawEye(lcx, cy, EYE_W, EYE_H, e * 0.9f, 0, 0, 0, 0, 0);
        drawEye(rcx, cy, EYE_W, EYE_H, -e * 0.9f, 0, 0, 0, 0, 0);
    }

    // ── FOCUSED: narrow + lock forward ──────────────────────────
    else if (currentEmotion == EMO_FOCUSED) {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);
        drawEye(lcx, cy, EYE_W + (int)(e*4), (int)(EYE_H * (1.0f - e*0.4f)), 0, 0,
                (int)(e * 4), (int)(e * 4), 0, 0);
        drawEye(rcx, cy, EYE_W + (int)(e*4), (int)(EYE_H * (1.0f - e*0.4f)), 0, 0,
                (int)(e * 4), (int)(e * 4), 0, 0);
    }

    // ── VIBING: bounce to a beat ────────────────────────────────
    else if (currentEmotion == EMO_VIBING) {
        float e = (p < 0.1f) ? p * 10 : ((p > 0.85f) ? (1.0f - p) * 6.67f : 1.0f);
        e = min(1.0f, max(0.0f, e));
        float beatPhase = fmod(t / 250.0f, 1.0f);
        int beatBounce = (beatPhase < 0.3f) ? (int)(sin(beatPhase / 0.3f * PI) * 5 * e) : 0;
        float tilt = sin(t / 500.0f) * 0.4f * e;
        drawEye(lcx, cy - beatBounce, EYE_W, EYE_H, tilt, 0, 0, 0, 0, 0);
        drawEye(rcx, cy - beatBounce, EYE_W, EYE_H, tilt, 0, 0, 0, 0, 0);
    }

    // ── SMUG: one eye half-closed ───────────────────────────────
    else if (currentEmotion == EMO_SMUG) {
        float entry = min(1.0f, p * 3.0f);
        float hold = (p > 0.8f) ? (p - 0.8f) * 5.0f : 0.0f;
        float e = entry * (1.0f - hold);
        drawEye(lcx, cy, EYE_W, EYE_H, e * 0.5f, 0, (int)(e * 5), 0, e * 0.4f, 0);
        drawEye(rcx, cy, EYE_W, EYE_H, e * 0.5f, 0, 0, 0, 0, 0);
    }

    // ── SHOCKED: stretch wide + vibrate ─────────────────────────
    else if (currentEmotion == EMO_SHOCKED) {
        float entry = min(1.0f, p * 8.0f);
        float hold = (p > 0.6f) ? (p - 0.6f) / 0.4f : 0.0f;
        float e = entry * (1.0f - hold);
        int vib = (int)(sin(t / 10.0f) * 2 * e);
        int grow = (int)(e * 10);
        drawEye(lcx + vib, cy - (int)(e*4), EYE_W + grow, EYE_H + grow, 0, -e*0.3f, 0, 0, 0, 0);
        drawEye(rcx + vib, cy - (int)(e*4), EYE_W + grow, EYE_H + grow, 0, -e*0.3f, 0, 0, 0, 0);
        if (e > 0.6f) {
            u8g2.setFont(u8g2_font_7x13B_tr);
            u8g2.drawStr(SCREEN_W/2 - 3, cy + 20, "!");
        }
    }

    // ── SLEEPY BLINK: slow heavy blinks ─────────────────────────
    else if (currentEmotion == EMO_SLEEPY_BLINK) {
        float blinkCycle = fmod(p * 3.0f, 1.0f);
        float exit = (p > 0.85f) ? (p - 0.85f) / 0.15f : 0.0f;
        float lid;
        if (blinkCycle < 0.4f) lid = blinkCycle / 0.4f;
        else if (blinkCycle < 0.6f) lid = 1.0f;
        else lid = (1.0f - (blinkCycle - 0.6f) / 0.4f);
        lid *= (1.0f - exit);
        drawEye(lcx, cy, EYE_W, EYE_H, 0, 0, (int)(lid * 6), 0, lid * 0.7f, lid * 0.3f);
        drawEye(rcx, cy, EYE_W, EYE_H, 0, 0, (int)(lid * 6), 0, lid * 0.7f, lid * 0.3f);
    }

    // ── GIGGLE: rapid small bounces + squish ────────────────────
    else if (currentEmotion == EMO_GIGGLE) {
        float e = (p < 0.1f) ? p * 10 : ((p > 0.85f) ? (1.0f - p) * 6.67f : 1.0f);
        e = min(1.0f, max(0.0f, e));
        int gigBounce = (int)(sin(t / 30.0f) * 3 * e);
        int squish = (int)(abs(sin(t / 30.0f)) * 3 * e);
        drawCrescentEye(lcx, cy + gigBounce, EYE_W/2, EYE_H/2, (int)(e * 6));
        drawCrescentEye(rcx, cy + gigBounce, EYE_W/2, EYE_H/2, (int)(e * 6));
    }
}

void startRandomEmotion() {
    // Pick a random emotion (1-12)
    currentEmotion = (EmoEmotion)(1 + random(0, EMO_COUNT));
    emotionStart = millis();
    emotionDuration = 2000 + random(0, 2500);  // 2-4.5s hyperactive

    const char* names[] = {"?", "Suspicious", "Sad", "Bored", "Sigh", "Curious",
                           "Dizzy", "Sneeze", "Wink", "Startled", "Shy",
                           "Mischievous", "Daydream", "Angry", "ExcitedBounce",
                           "Love", "Confused", "Proud", "Scared", "Playful",
                           "Grumpy", "Glitch", "Peek", "RollEyes", "CrossEyed",
                           "Focused", "Vibing", "Smug", "Shocked", "SleepyBlink",
                           "Giggle"};
    Serial.printf("Emo: %s (%lums)\n", names[currentEmotion], emotionDuration);
}

// ═══════════════════════════════════════════════════════════════════════
//  MOTION TRACKING
// ═══════════════════════════════════════════════════════════════════════
void updateMotion() {
    if (!mpuOK) return;
    if (millis() - lastMotionCheck < 20) return; // 50Hz
    lastMotionCheck = millis();

    // Read 6 bytes starting at register 0x3B (ACCEL_XOUT_H)
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6, (uint8_t)true);

    int16_t rawX = (Wire.read() << 8) | Wire.read();
    int16_t rawY = (Wire.read() << 8) | Wire.read();
    int16_t rawZ = (Wire.read() << 8) | Wire.read();

    // Convert to m/s^2 (range ±8g, sensitivity 4096 LSB/g)
    float ax = rawX / 4096.0f * 9.81f;
    float ay = rawY / 4096.0f * 9.81f;
    float az = rawZ / 4096.0f * 9.81f;

    gyroLeanX = ay / 8.0f;
    if (gyroLeanX > 1.0f) gyroLeanX = 1.0f;
    if (gyroLeanX < -1.0f) gyroLeanX = -1.0f;

    gyroLeanY = ax / 8.0f;
    if (gyroLeanY > 1.0f) gyroLeanY = 1.0f;
    if (gyroLeanY < -1.0f) gyroLeanY = -1.0f;

    float totalAccel = sqrt(ax*ax + ay*ay + az*az);

    // Debug every 500ms
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 500) {
        lastDebug = millis();
        Serial.printf("MPU: X=%.2f Y=%.2f Z=%.2f | total=%.2f | lean X=%.2f Y=%.2f\n",
                      ax, ay, az, totalAccel, gyroLeanX, gyroLeanY);
    }

    if (totalAccel > 18.0f && currentState == STATE_IDLE && currentEmotion == EMO_NONE) {
        startRandomEmotion();
        currentEmotion = EMO_STARTLED;
        emotionDuration = 2000;
        Serial.println("!!! Startled by motion !!!");
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  DRAW BOTH EYES
// ═══════════════════════════════════════════════════════════════════════
void drawEyes() {
    updateMotion();

    // Smooth whole-eye position offset (vector shift)
    float targetOffX = gyroLeanX * 18.0f;  // max ±18px horizontal shift
    float targetOffY = gyroLeanY * 10.0f;  // max ±10px vertical shift
    gyroOffsetX = lerp(gyroOffsetX, targetOffX, 0.12f);
    gyroOffsetY = lerp(gyroOffsetY, targetOffY, 0.12f);
    int gox = (int)gyroOffsetX;
    int goy = (int)gyroOffsetY;

    // Smooth easing for pupil direction (smaller contribution now)
    float targetX = eyeTargetX + gyroLeanX * 0.3f;
    float targetY = eyeTargetY + gyroLeanY * 0.3f;
    
    // Clamp
    if (targetX > 1.0f) targetX = 1.0f; if (targetX < -1.0f) targetX = -1.0f;
    if (targetY > 1.0f) targetY = 1.0f; if (targetY < -1.0f) targetY = -1.0f;

    eyeCurrentX = lerp(eyeCurrentX, targetX, 0.08f);
    eyeCurrentY = lerp(eyeCurrentY, targetY, 0.08f);
    eyeLidTop = lerp(eyeLidTop, eyeLidTargetTop, 0.25f);
    eyeLidBot = lerp(eyeLidBot, eyeLidTargetBot, 0.25f);

    // Animation offset
    float animY = getAnimOffsetY();
    int offsetY = (int)animY;

    u8g2.clearBuffer();

    // Use special rendering for different states
    if (isSleeping) {
        drawSleepingFace();
    } else if (currentEmotion != EMO_NONE) {
        drawEmoEmotion();
    } else if (currentState == STATE_HAPPY) {
        drawCuteHappyFace(offsetY);
    } else if (currentState == STATE_EXCITED) {
        drawCuteExcitedFace(offsetY);
    } else {
        // Normal eye rendering with gyro offset
        MoodParams mp = getMoodParams(currentMood);

        int curiosityL = 0, curiosityR = 0;
        if (curiosityOn) {
            if (eyeCurrentX < -0.3f) curiosityL = (int)(abs(eyeCurrentX) * 6);
            if (eyeCurrentX >  0.3f) curiosityR = (int)(abs(eyeCurrentX) * 6);
        }

        drawEye(EYE_L_CX + gox, EYE_CY + offsetY + goy,
                EYE_W, EYE_H + curiosityL,
                eyeCurrentX, eyeCurrentY,
                mp.topTrimL, mp.bottomTrimL,
                eyeLidTop, eyeLidBot);

        drawEye(EYE_R_CX + gox, EYE_CY + offsetY + goy,
                EYE_W, EYE_H + curiosityR,
                eyeCurrentX, eyeCurrentY,
                mp.topTrimR, mp.bottomTrimR,
                eyeLidTop, eyeLidBot);
    }

    u8g2.sendBuffer();
}

// ═══════════════════════════════════════════════════════════════════════
//  TEXT DISPLAY
// ═══════════════════════════════════════════════════════════════════════
void drawWrappedText(const String& msg, int marginX, int startY, int maxY) {
    u8g2.setFont(u8g2_font_5x8_tr);
    const int charW = 5, lineH = 10;
    const int usableW = 128 - marginX * 2;
    const int maxCols  = usableW / charW;
    int y = startY, start = 0, len = (int)msg.length();
    while (start < len && y < maxY) {
        int end = start + maxCols;
        if (end >= len) { end = len; }
        else {
            int sp = msg.lastIndexOf(' ', end - 1);
            if (sp > start) end = sp + 1;
        }
        String line = msg.substring(start, end);
        line.trim();
        u8g2.drawStr(marginX, y, line.c_str());
        y += lineH;
        start = end;
    }
}

void renderTalking() {
    unsigned long elapsed = millis() - textStart;
    u8g2.clearBuffer();

    // Rounded border frame
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(0, 0, 128, 64, 4);

    // Header bar
    u8g2.drawBox(0, 0, 128, 13);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(5, 10, "\x04 Robi says:");
    u8g2.setDrawColor(1);
    u8g2.drawHLine(2, 14, 124);

    // Typewriter reveal
    int charsToShow = (int)(elapsed / 25);
    if (charsToShow > (int)aiText.length()) charsToShow = aiText.length();
    String visibleText = aiText.substring(0, charsToShow);
    drawWrappedText(visibleText, 5, 25, 60);

    // Typing dots
    if (charsToShow < (int)aiText.length()) {
        int dotPhase = (elapsed / 300) % 3;
        for (int i = 0; i <= dotPhase; i++) {
            u8g2.drawDisc(58 + i * 6, 60, 1);
        }
    }

    u8g2.sendBuffer();

    if (millis() - textStart > textDisplayDuration) {
        // Smooth exit: close eyes briefly before going to idle
        eyeLidTargetTop = 1.0f;
        eyeLidTargetBot = 1.0f;
        currentState = STATE_IDLE;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  STATE MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════
RobiState lastAppliedState = (RobiState)99;  // force first apply

void applyState(RobiState state) {
    if (state == lastAppliedState) return;
    lastAppliedState = state;
    stateStart = millis();
    transitionStart = millis();

    switch (state) {
        case STATE_IDLE:
            Serial.println("→ IDLE");
            currentMood = MOOD_DEFAULT;
            autoBlinkOn = true;
            idleModeOn = true;
            curiosityOn = true;
            eyeTargetX = 0; eyeTargetY = 0;
            // Smooth return: let lerp bring lids back to open
            eyeLidTargetTop = 0; eyeLidTargetBot = 0;
            break;

        case STATE_LISTENING:
            Serial.println("→ LISTENING");
            currentMood = MOOD_HAPPY;
            autoBlinkOn = false;
            idleModeOn = false;
            curiosityOn = true;
            eyeTargetX = 0; eyeTargetY = 0;
            // Quick wake-up: eyes start closed, snap open (shorter)
            eyeLidTop = 1.0f; eyeLidBot = 1.0f;
            startAnim(ANIM_WAKEUP, 250);
            break;

        case STATE_THINKING:
            Serial.println("→ THINKING");
            currentMood = MOOD_TIRED;
            autoBlinkOn = false;
            idleModeOn = false;
            curiosityOn = false;
            eyeTargetX = -0.6f; eyeTargetY = -0.4f;  // look up-left
            startAnim(ANIM_CONFUSED, 800);
            break;

        case STATE_HAPPY:
            Serial.println("→ HAPPY");
            currentMood = MOOD_HAPPY;
            autoBlinkOn = false;
            idleModeOn = false;
            curiosityOn = false;
            eyeTargetX = 0; eyeTargetY = 0;
            startAnim(ANIM_LAUGH, 1500);
            break;

        case STATE_EXCITED:
            Serial.println("→ EXCITED");
            currentMood = MOOD_HAPPY;
            autoBlinkOn = false;
            idleModeOn = false;
            curiosityOn = false;
            eyeTargetX = 0; eyeTargetY = 0;
            startAnim(ANIM_LAUGH, 2000);
            break;

        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  TOUCH — Interrupt-driven
// ═══════════════════════════════════════════════════════════════════════

// ISR: runs instantly when touch pin goes HIGH, even during HTTP
void IRAM_ATTR touchISR() {
    touchFlag = true;
}

void handleTouch() {
    if (!touchFlag) return;
    touchFlag = false;

    unsigned long now = millis();
    Serial.printf("*** TOUCH @ %lu ms ***\n", now);

    // Debounce: ignore if less than 150ms since last touch
    if (now - lastTouchTime < 150) return;

    if (now - lastTouchTime < doubleTapWindow) {
        tapCount++;
    } else {
        tapCount = 1;
    }
    lastTouchTime = now;
}

void processTaps() {
    // Wait for double-tap window to expire before deciding
    if (tapCount > 0 && millis() - lastTouchTime > doubleTapWindow) {
        if (tapCount >= 2) {
            Serial.println("Double tap → EXCITED");
            prevAnimState = currentState;
            currentState = STATE_EXCITED;
            lastAppliedState = (RobiState)99;
        } else {
            Serial.println("Single tap → HAPPY");
            prevAnimState = currentState;
            currentState = STATE_HAPPY;
            lastAppliedState = (RobiState)99;
        }
        tapCount = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  HTTP
// ═══════════════════════════════════════════════════════════════════════
String httpGet(const String& url) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(3000);
    int code = http.GET();
    String body = "";
    if (code == 200) body = http.getString();
    http.end();
    return body;
}

void pollServer() {
    if (!wifiOK || millis() - lastPoll < POLL_INTERVAL_MS) return;
    lastPoll = millis();

    // State changes
    String stateBody = httpGet(String(SERVER) + "/state");
    if (stateBody.length() > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, stateBody) == DeserializationError::Ok) {
            String s = doc["state"].as<String>();
            if (s == "listening" && currentState != STATE_LISTENING) {
                currentState = STATE_LISTENING;
                lastAppliedState = (RobiState)99;
            } else if (s == "thinking" && currentState != STATE_THINKING) {
                currentState = STATE_THINKING;
                lastAppliedState = (RobiState)99;
            } else if (s == "idle" && currentState == STATE_LISTENING) {
                // Only cancel listening on idle, NOT thinking.
                // Thinking persists until the AI answer arrives → TALKING
                currentState = STATE_IDLE;
                lastAppliedState = (RobiState)99;
            }
        }
    }

    // AI reply
    if (currentState != STATE_HAPPY && currentState != STATE_EXCITED) {
        String textBody = httpGet(String(SERVER) + "/latest");
        if (textBody.length() > 0) {
            JsonDocument doc;
            if (deserializeJson(doc, textBody) == DeserializationError::Ok) {
                aiText = doc["text"].as<String>();
                int wordCount = 1;
                for (int i = 0; i < (int)aiText.length(); i++) {
                    if (aiText[i] == ' ') wordCount++;
                }
                textDisplayDuration = (unsigned long)wordCount * MS_PER_WORD + EXTRA_DISPLAY_MS;
                Serial.printf("Reply (%d words, %lums): ", wordCount, textDisplayDuration);
                Serial.println(aiText);
                currentState = STATE_TALKING;
                textStart = millis();
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Robi (RoboEyes-style on U8g2) ===");

    u8g2.begin();
    u8g2.setContrast(200);
    pinMode(TOUCH_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchISR, RISING);
    Serial.printf("Touch interrupt attached on GPIO %d\n", TOUCH_PIN);

    // Boot animation: eyes closed
    eyeLidTop = 1.0f; eyeLidBot = 1.0f;
    eyeLidTargetTop = 1.0f; eyeLidTargetBot = 1.0f;
    drawEyes();
    delay(400);

    // Show connecting text
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.drawStr(18, 35, "Connecting...");
    u8g2.sendBuffer();

    connectWiFi();

    // Happy wake-up
    currentMood = MOOD_HAPPY;
    eyeLidTargetTop = 0; eyeLidTargetBot = 0;
    startAnim(ANIM_LAUGH, 1000);
    for (int i = 0; i < 40; i++) {
        updateAutoBlink();
        getAnimOffsetY();
        drawEyes();
        delay(25);
    }

    // Back to default
    currentMood = MOOD_DEFAULT;
    currentAnim = ANIM_NONE;
    nextBlinkTime = millis() + 2000 + random(0, 3000);
    nextIdleMove = millis() + 1500 + random(0, 2000);
    nextSleepTime = millis() + 180000UL + random(0, 120000);  // 3-5 min
    nextEmotionTime = millis() + 4000UL + random(0, 4000);  // 4-8s hyperactive     // 8-15s

    // MPU6050 on same I2C bus as OLED (GPIO 6/7)
    // Wire already initialized by u8g2
    
    // I2C Scanner
    Serial.println("Scanning I2C bus (GPIO 6/7)...");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  Found device at 0x%02X\n", addr);
            found++;
        }
    }
    if (found == 0) Serial.println("  No devices found!");

    // Wake up MPU6050: write 0 to PWR_MGMT_1 (register 0x6B)
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x6B);
    Wire.write(0x00);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
        Serial.println("MPU6050 AWAKE!");
        mpuOK = true;
        // Set range to ±8g (register 0x1C, value 0x10)
        Wire.beginTransmission(MPU_ADDR);
        Wire.write(0x1C);
        Wire.write(0x10);
        Wire.endTransmission();
    } else {
        Serial.printf("MPU6050 FAILED (err=%d)\n", err);
        mpuOK = false;
    }

    Serial.println("Ready!");
}

// ═══════════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════════
void loop() {
    handleTouch();
    processTaps();
    pollServer();

    // Tap animation timeout → back to idle
    if ((currentState == STATE_HAPPY || currentState == STATE_EXCITED) &&
        millis() - stateStart > 2500) {
        currentState = STATE_IDLE;
        lastAppliedState = (RobiState)99;
    }

    // Apply mood/config for current state
    applyState(currentState);

    // Random sleep trigger (very rare)
    if (currentState == STATE_IDLE && !isSleeping && currentEmotion == EMO_NONE) {
        if (millis() >= nextSleepTime) {
            isSleeping = true;
            animStart = millis();
            autoBlinkOn = false;
            idleModeOn = false;
            Serial.println("Zzz... sleeping");
        }
    }

    // End sleep animation after full duration
    if (isSleeping && millis() - animStart > SLEEP_TOTAL_MS) {
        isSleeping = false;
        autoBlinkOn = true;
        idleModeOn = true;
        nextSleepTime = millis() + 180000UL + random(0, 120000);  // 3-5 min
        Serial.println("Woke up!");
    }

    // Cancel sleep if state changes from idle
    if (isSleeping && currentState != STATE_IDLE) {
        isSleeping = false;
        autoBlinkOn = true;
        idleModeOn = true;
    }

    // Random emo emotions during idle (every 8-15s)
    if (currentState == STATE_IDLE && !isSleeping && currentEmotion == EMO_NONE) {
        if (millis() >= nextEmotionTime) {
            startRandomEmotion();
            autoBlinkOn = false;
            idleModeOn = false;
        }
    }

    // End emo emotion
    if (currentEmotion != EMO_NONE && millis() - emotionStart > emotionDuration) {
        currentEmotion = EMO_NONE;
        autoBlinkOn = true;
        idleModeOn = true;
        nextEmotionTime = millis() + 4000UL + random(0, 4000);  // 4-8s hyperactive  // 8-15s
    }

    // Cancel emotion if state changes
    if (currentEmotion != EMO_NONE && currentState != STATE_IDLE) {
        currentEmotion = EMO_NONE;
        autoBlinkOn = true;
        idleModeOn = true;
    }

    // Render
    if (currentState == STATE_TALKING) {
        renderTalking();
    } else {
        if (!isSleeping && currentEmotion == EMO_NONE) {
            updateAutoBlink();
            updateIdleMode();
        }
        drawEyes();
    }

    delay(16);   // ~60 FPS
}
