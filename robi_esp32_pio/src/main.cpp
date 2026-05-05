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
enum Anim { ANIM_NONE, ANIM_LAUGH, ANIM_CONFUSED, ANIM_WAKEUP };
Anim currentAnim = ANIM_NONE;
unsigned long animStart = 0;
unsigned long animDuration = 0;

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

    // Draw eye outline (filled rounded rect)
    u8g2.setDrawColor(1);
    u8g2.drawRBox(cx - hw, drawTop, w, drawH, radius);

    // Pupil position — map normalized (-1 to 1) to pixel offsets
    if (drawH > PUPIL_H + 4) {
        int maxPupilX = hw - PUPIL_W / 2 - 3;
        int maxPupilY = drawH / 2 - PUPIL_H / 2 - 2;
        int px = cx + (int)(pupilX * maxPupilX);
        int py = drawTop + drawH / 2 + (int)(pupilY * maxPupilY);

        u8g2.setDrawColor(0);
        u8g2.drawRBox(px - PUPIL_W / 2, py - PUPIL_H / 2, PUPIL_W, PUPIL_H, 3);

        // Specular highlight
        u8g2.setDrawColor(1);
        u8g2.drawDisc(px - 3, py - 3, 2);
    }
    u8g2.setDrawColor(1);
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
        // Random position — normalized -0.8 to 0.8
        eyeTargetX = (random(-80, 81)) / 100.0f;
        eyeTargetY = (random(-40, 41)) / 100.0f;
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

// Draw a single crescent eye: filled ellipse masked from top
void drawCrescentEye(int cx, int cy, int rx, int ry, int cutShift) {
    // White filled ellipse — the full eye shape
    u8g2.setDrawColor(1);
    u8g2.drawFilledEllipse(cx, cy, rx, ry);

    // Black filled ellipse shifted UP — carves out the top,
    // leaving only a crescent at the bottom (like Emo's happy eyes)
    u8g2.setDrawColor(0);
    u8g2.drawFilledEllipse(cx, cy - cutShift, rx + 1, ry);
    u8g2.setDrawColor(1);
}

void drawCuteHappyFace(int offsetY) {
    unsigned long t = millis() - stateStart;

    // Smooth entry: eyes close from top over 300ms
    float transition = min(1.0f, (float)t / 300.0f);
    int cutShift = (int)(transition * 8);   // how much crescent to show

    int lcx = EYE_L_CX, rcx = EYE_R_CX, cy = EYE_CY + offsetY;
    int rx = EYE_W / 2, ry = EYE_H / 2;

    // Two clean crescent eyes
    drawCrescentEye(lcx, cy, rx, ry, cutShift);
    drawCrescentEye(rcx, cy, rx, ry, cutShift);
}

void drawCuteExcitedFace(int offsetY) {
    unsigned long t = millis() - stateStart;

    // Smooth entry + slight bounce
    float transition = min(1.0f, (float)t / 250.0f);
    int cutShift = (int)(transition * 10);  // slightly more closed than happy

    // Gentle bounce that fades
    float damping = max(0.0f, 1.0f - (float)t / 2000.0f);
    int bounceY = (int)(3.0f * sin(t / 70.0f) * damping);

    int lcx = EYE_L_CX, rcx = EYE_R_CX, cy = EYE_CY + offsetY + bounceY;
    int rx = EYE_W / 2, ry = EYE_H / 2;

    // Two crescent eyes (slightly more closed + bouncing)
    drawCrescentEye(lcx, cy, rx, ry, cutShift);
    drawCrescentEye(rcx, cy, rx, ry, cutShift);
}

// ═══════════════════════════════════════════════════════════════════════
//  DRAW BOTH EYES
// ═══════════════════════════════════════════════════════════════════════
void drawEyes() {
    // Smooth easing for all parameters
    eyeCurrentX = lerp(eyeCurrentX, eyeTargetX, 0.08f);
    eyeCurrentY = lerp(eyeCurrentY, eyeTargetY, 0.08f);
    eyeLidTop = lerp(eyeLidTop, eyeLidTargetTop, 0.25f);
    eyeLidBot = lerp(eyeLidBot, eyeLidTargetBot, 0.25f);

    // Animation offset
    float animY = getAnimOffsetY();
    int offsetY = (int)animY;

    u8g2.clearBuffer();

    // Use special cute rendering for HAPPY and EXCITED
    if (currentState == STATE_HAPPY) {
        drawCuteHappyFace(offsetY);
    } else if (currentState == STATE_EXCITED) {
        drawCuteExcitedFace(offsetY);
    } else {
        // Normal eye rendering
        MoodParams mp = getMoodParams(currentMood);

        int curiosityL = 0, curiosityR = 0;
        if (curiosityOn) {
            if (eyeCurrentX < -0.3f) curiosityL = (int)(abs(eyeCurrentX) * 6);
            if (eyeCurrentX >  0.3f) curiosityR = (int)(abs(eyeCurrentX) * 6);
        }

        drawEye(EYE_L_CX, EYE_CY + offsetY,
                EYE_W, EYE_H + curiosityL,
                eyeCurrentX, eyeCurrentY,
                mp.topTrimL, mp.bottomTrimL,
                eyeLidTop, eyeLidBot);

        drawEye(EYE_R_CX, EYE_CY + offsetY,
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

    switch (state) {
        case STATE_IDLE:
            Serial.println("→ IDLE");
            currentMood = MOOD_DEFAULT;
            autoBlinkOn = true;
            idleModeOn = true;
            curiosityOn = true;
            eyeTargetX = 0; eyeTargetY = 0;
            eyeLidTargetTop = 0; eyeLidTargetBot = 0;
            break;

        case STATE_LISTENING:
            Serial.println("→ LISTENING");
            currentMood = MOOD_HAPPY;
            autoBlinkOn = false;
            idleModeOn = false;
            curiosityOn = true;
            eyeTargetX = 0; eyeTargetY = 0;
            // Wake-up: eyes start closed, snap open
            eyeLidTop = 1.0f; eyeLidBot = 1.0f;
            startAnim(ANIM_WAKEUP, 500);
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

    // Render
    if (currentState == STATE_TALKING) {
        renderTalking();
    } else {
        updateAutoBlink();
        updateIdleMode();
        drawEyes();
    }

    delay(16);   // ~60 FPS
}
