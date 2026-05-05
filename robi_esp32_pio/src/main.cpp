/*
 * Robi ESP32-S3 Mini — Animated Robot Face
 * =========================================
 * Animation states:
 *   IDLE       — Normal eye animation, random pupil movement, occasional blink
 *   LISTENING  — Eyes go wide & bright, pupils centered, pulsing glow effect
 *   THINKING   — Eyes look up-left, animated "..." dots
 *   HAPPY      — Eyes become ^_^ arcs, small bounce (single tap)
 *   EXCITED    — Eyes become ★_★ stars with sparkle (double tap)
 *   TALKING    — Word-wrapped text display with scroll
 *
 * Touch:  GPIO 4 (capacitive touch, wire/foil attached)
 *   - Single tap → HAPPY animation (2s)
 *   - Double tap → EXCITED animation (2s)
 *
 * Server:
 *   GET /latest → AI reply text
 *   GET /state  → animation state (listening, thinking, idle)
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
#define TOUCH_PIN 4     // capacitive touch GPIO

// ═══════════════════════════════════════════════════════════════════════
//  DISPLAY
// ═══════════════════════════════════════════════════════════════════════
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ═══════════════════════════════════════════════════════════════════════
//  EYE GEOMETRY
// ═══════════════════════════════════════════════════════════════════════
#define EYE_L_X   32
#define EYE_R_X   96
#define EYE_Y     28
#define EYE_W     30
#define EYE_H     26
#define PUPIL_R    6

// ═══════════════════════════════════════════════════════════════════════
//  TIMING
// ═══════════════════════════════════════════════════════════════════════
#define MS_PER_WORD       330UL     // ~3 words/sec at macOS `say -r 175`
#define EXTRA_DISPLAY_MS  2000UL    // keep text on screen 2s after speaking
#define POLL_INTERVAL_MS  2000UL
#define ANIM_DURATION_MS  2500UL
#define BLINK_DURATION_MS 250UL
#define TOUCH_THRESHOLD   25000     // adjust based on your setup

// ═══════════════════════════════════════════════════════════════════════
//  STATE MACHINE
// ═══════════════════════════════════════════════════════════════════════
enum RobiState {
    STATE_IDLE,
    STATE_LISTENING,
    STATE_THINKING,
    STATE_HAPPY,
    STATE_EXCITED,
    STATE_TALKING,
    STATE_BLINK
};

RobiState currentState  = STATE_IDLE;
RobiState previousState = STATE_IDLE;

bool  wifiOK = false;

// Pupil
float pupilX = 0, pupilY = 0;
float targetPX = 0, targetPY = 0;
unsigned long lastPupilMove = 0;

// Blink
unsigned long lastBlink   = 0;
unsigned long blinkStart  = 0;
unsigned long nextBlinkIn = 3000;

// Animation
unsigned long animStart   = 0;
unsigned long stateStart  = 0;

// Text
String        aiText      = "";
unsigned long textStart   = 0;
unsigned long textDisplayDuration = 5000UL;  // recalculated per reply

// Touch
unsigned long lastTouchTime    = 0;
int           tapCount         = 0;
bool          wasTouched       = false;
unsigned long doubleTapWindow  = 400;

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
//  DRAWING PRIMITIVES — Emo-inspired
// ═══════════════════════════════════════════════════════════════════════

/* Eased eye with optional squeeze, highlight, and eyelid.
   squeeze: 0.0 = fully open, 1.0 = fully closed
   highlight: draw a specular glint on the pupil */
void drawEyeFull(int cx, int cy, int w, int h, int pdx, int pdy,
                 float squeeze, bool highlight) {
    int hw = w / 2, hh = h / 2;

    // Squeeze shrinks height from top and bottom
    int visH = max(2, (int)(h * (1.0f - squeeze)));
    int visHH = visH / 2;

    u8g2.setDrawColor(1);
    u8g2.drawRBox(cx - hw, cy - visHH, w, visH, min(6, visH / 2));

    if (squeeze < 0.85f) {
        // Pupil
        int px = constrain(cx + pdx, cx - hw + PUPIL_R + 2, cx + hw - PUPIL_R - 2);
        int py = constrain(cy + pdy, cy - visHH + PUPIL_R + 2, cy + visHH - PUPIL_R - 2);
        u8g2.setDrawColor(0);
        u8g2.drawDisc(px, py, PUPIL_R);
        u8g2.setDrawColor(1);

        // Specular highlight — tiny white dot on pupil (gives depth)
        if (highlight) {
            u8g2.drawDisc(px - 2, py - 2, 2);
        }
    }
}

/* Simple eye wrapper (backward compatible) */
void drawEye(int cx, int cy, int w, int h, int pdx, int pdy) {
    drawEyeFull(cx, cy, w, h, pdx, pdy, 0.0f, true);
}

/* Eyebrow — angled line above eye.
   angle: negative = angry (\ /), positive = sad (/ \), 0 = neutral (— —) */
void drawEyebrow(int cx, int ey_top, int w, float angle) {
    int hw = w / 2 - 2;
    int y1 = ey_top - 5 + (int)(angle * hw * 0.04f);
    int y2 = ey_top - 5 - (int)(angle * hw * 0.04f);
    u8g2.drawLine(cx - hw, y1, cx + hw, y2);
    u8g2.drawLine(cx - hw, y1 + 1, cx + hw, y2 + 1);  // thicker
}

/* Happy squint eye — curved arc */
void drawHappyEye(int cx, int cy, int w) {
    int hw = w / 2;
    u8g2.setDrawColor(1);
    for (int i = -hw; i <= hw; i++) {
        float norm = (float)(i * i) / (float)(hw * hw);
        int y = cy - 5 + (int)(10.0f * (1.0f - norm));
        u8g2.drawPixel(cx + i, y);
        u8g2.drawPixel(cx + i, y + 1);
        u8g2.drawPixel(cx + i, y + 2);
    }
}

/* Star eye ★ */
void drawStarEye(int cx, int cy, int r) {
    u8g2.setDrawColor(1);
    float angle = -PI / 2;
    float step  = PI * 2.0f / 5.0f;
    for (int i = 0; i < 5; i++) {
        float oa = angle + step * i;
        float ia = oa + step / 2.0f;
        float na = oa + step;
        u8g2.drawLine(cx + (int)(r * cos(oa)), cy + (int)(r * sin(oa)),
                      cx + (int)(r * 0.4f * cos(ia)), cy + (int)(r * 0.4f * sin(ia)));
        u8g2.drawLine(cx + (int)(r * 0.4f * cos(ia)), cy + (int)(r * 0.4f * sin(ia)),
                      cx + (int)(r * cos(na)), cy + (int)(r * sin(na)));
    }
}

/* Sparkle particle */
void drawSparkle(int x, int y, int s) {
    u8g2.setDrawColor(1);
    u8g2.drawLine(x - s, y, x + s, y);
    u8g2.drawLine(x, y - s, x, y + s);
    u8g2.drawPixel(x - s + 1, y - s + 1);
    u8g2.drawPixel(x + s - 1, y - s + 1);
    u8g2.drawPixel(x - s + 1, y + s - 1);
    u8g2.drawPixel(x + s - 1, y + s - 1);
}

/* Animated thinking dots "..." */
void drawThinkingDots(int frame) {
    u8g2.setFont(u8g2_font_7x13B_tr);
    int dotCount = (frame / 8) % 4;
    String dots = "";
    for (int i = 0; i < dotCount; i++) dots += ".";
    u8g2.drawStr(52, 55, dots.c_str());
}

/* Mouth shapes */
void drawMouthSmile(int cx, int cy, int w) {
    // Upward arc
    for (int i = -w; i <= w; i++) {
        float norm = (float)(i * i) / (float)(w * w);
        int y = cy + (int)(3.0f * norm);
        u8g2.drawPixel(cx + i, y);
    }
}
void drawMouthO(int cx, int cy, int r) {
    u8g2.drawCircle(cx, cy, r);
}
void drawMouthLine(int cx, int cy, int w) {
    u8g2.drawRBox(cx - w/2, cy - 1, w, 3, 1);
}

/* Word-wrap text inside a padded area */
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

void showStatus(const char* msg) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x13B_tr);
    int w = u8g2.getStrWidth(msg);
    u8g2.drawStr((128 - w) / 2, 35, msg);
    u8g2.sendBuffer();
}

// ═══════════════════════════════════════════════════════════════════════
//  RENDER EACH STATE — Emo-inspired
// ═══════════════════════════════════════════════════════════════════════
int frameCounter = 0;

void renderIdle() {
    unsigned long t = millis();

    // ── Breathing: subtle vertical float ─────────────────────────────
    float breathe = sin(t / 1200.0f) * 2.0f;         // slow up/down
    float eyeScale = 1.0f + 0.03f * sin(t / 1500.0f); // tiny size pulse

    int eyeY = EYE_Y + (int)breathe;
    int ew = (int)(EYE_W * eyeScale);
    int eh = (int)(EYE_H * eyeScale);

    // ── Smooth pupil with micro-saccades (tiny random jitters) ───────
    if (t - lastPupilMove > 2500UL) {
        const int pos[] = {-7, -4, 0, 4, 7};
        targetPX = pos[random(0, 5)];
        targetPY = pos[random(0, 3)];
        lastPupilMove = t;
    }
    // Micro-saccade jitter — like real eyes
    float jitterX = (random(-10, 11) / 10.0f) * 0.3f;
    float jitterY = (random(-10, 11) / 10.0f) * 0.2f;
    pupilX += (targetPX - pupilX) * 0.06f;
    pupilY += (targetPY - pupilY) * 0.06f;

    u8g2.clearBuffer();
    drawEyeFull(EYE_L_X, eyeY, ew, eh, (int)(pupilX + jitterX), (int)(pupilY + jitterY), 0.0f, true);
    drawEyeFull(EYE_R_X, eyeY, ew, eh, (int)(pupilX + jitterX), (int)(pupilY + jitterY), 0.0f, true);

    // Subtle mouth — gentle smile
    drawMouthSmile(64, 50 + (int)breathe, 8);

    u8g2.sendBuffer();
}

void renderBlink() {
    unsigned long elapsed = millis() - blinkStart;
    float progress = (float)elapsed / (float)BLINK_DURATION_MS;
    if (progress > 1.0f) progress = 1.0f;

    // Smooth ease-in-out squeeze curve
    float squeeze;
    if (progress < 0.35f) {
        // Closing: ease-in (accelerate)
        float t = progress / 0.35f;
        squeeze = t * t;
    } else if (progress < 0.55f) {
        // Held closed
        squeeze = 1.0f;
    } else {
        // Opening: ease-out (decelerate)
        float t = (progress - 0.55f) / 0.45f;
        squeeze = 1.0f - (t * (2.0f - t));  // quadratic ease-out
    }

    u8g2.clearBuffer();
    drawEyeFull(EYE_L_X, EYE_Y, EYE_W, EYE_H, (int)pupilX, 0, squeeze, squeeze < 0.5f);
    drawEyeFull(EYE_R_X, EYE_Y, EYE_W, EYE_H, (int)pupilX, 0, squeeze, squeeze < 0.5f);
    drawMouthLine(64, 50, 12);
    u8g2.sendBuffer();

    if (elapsed >= BLINK_DURATION_MS) {
        currentState = previousState;
    }
}

void renderListening() {
    unsigned long t = millis() - stateStart;

    // Pulsing wide-open eyes
    float pulse = 1.0f + 0.12f * sin(t / 120.0f);
    int w = (int)(EYE_W * pulse);
    int h = (int)((EYE_H + 6) * pulse);
    float bounce = 2.0f * sin(t / 200.0f);

    u8g2.clearBuffer();

    // Big alert eyes — pupils centered, highlighted
    drawEyeFull(EYE_L_X, EYE_Y + (int)bounce, w, h, 0, 0, 0.0f, true);
    drawEyeFull(EYE_R_X, EYE_Y + (int)bounce, w, h, 0, 0, 0.0f, true);

    // Raised eyebrows (curious)
    drawEyebrow(EYE_L_X, EYE_Y - h/2 + (int)bounce, w, 4);
    drawEyebrow(EYE_R_X, EYE_Y - h/2 + (int)bounce, w, -4);

    // Sound wave indicators on sides
    int wavePhase = (t / 100) % 4;
    for (int i = 0; i < 3; i++) {
        int r = 3 + i * 4 + ((wavePhase + i) % 3);
        int alpha = (i == wavePhase % 3) ? 1 : 0;
        if (alpha || i < 2) {
            u8g2.drawCircle(8, 32, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_LOWER_LEFT);
            u8g2.drawCircle(120, 32, r, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT);
        }
    }

    // Open "o" mouth
    drawMouthO(64, 52 + (int)bounce, 4);

    u8g2.sendBuffer();
}

void renderThinking() {
    unsigned long t = millis() - stateStart;

    // Eyes looking up-left, slowly drifting
    float lookX = -6.0f + 2.0f * sin(t / 800.0f);
    float lookY = -5.0f + 1.5f * sin(t / 600.0f);

    u8g2.clearBuffer();

    // Slightly squinted eyes (one more than other for personality)
    drawEyeFull(EYE_L_X, EYE_Y, EYE_W, EYE_H - 2, (int)lookX, (int)lookY, 0.08f, true);
    drawEyeFull(EYE_R_X, EYE_Y, EYE_W, EYE_H, (int)lookX, (int)lookY, 0.0f, true);

    // Furrowed eyebrows (concentrating)
    drawEyebrow(EYE_L_X, EYE_Y - EYE_H/2, EYE_W, -3);
    drawEyebrow(EYE_R_X, EYE_Y - EYE_H/2, EYE_W, 3);

    // Animated thinking dots
    drawThinkingDots(t / 40);

    // Floating thought bubbles — chain of circles
    float bobY = 2.0f * sin(t / 300.0f);
    u8g2.drawDisc(105, 12 + (int)bobY, 4);
    u8g2.drawDisc(112, 8 + (int)(bobY * 0.7f), 3);
    u8g2.drawDisc(117, 5 + (int)(bobY * 0.4f), 2);

    // Wavy mouth (uncertain)
    for (int i = -6; i <= 6; i++) {
        int my = 52 + (int)(1.5f * sin((t / 200.0f) + i * 0.5f));
        u8g2.drawPixel(64 + i, my);
    }

    u8g2.sendBuffer();
}

void renderHappy() {
    unsigned long t = millis() - animStart;

    // Damped bounce — fast at first, fades out
    float damping = max(0.0f, 1.0f - (float)t / 2200.0f);
    int bounceY = (int)(4.0f * sin(t / 80.0f) * damping);

    u8g2.clearBuffer();

    // Happy squint eyes ^_^
    drawHappyEye(EYE_L_X, EYE_Y + bounceY, EYE_W + 2);
    drawHappyEye(EYE_R_X, EYE_Y + bounceY, EYE_W + 2);

    // Rosy blush — dithered circles under eyes
    for (int dx = -3; dx <= 3; dx++) {
        for (int dy = -3; dy <= 3; dy++) {
            if (dx*dx + dy*dy <= 9 && (dx + dy) % 2 == 0) {
                u8g2.drawPixel(EYE_L_X - 3 + dx, EYE_Y + 12 + bounceY + dy);
                u8g2.drawPixel(EYE_R_X + 3 + dx, EYE_Y + 12 + bounceY + dy);
            }
        }
    }

    // Big happy smile — thick upward arc
    for (int i = -14; i <= 14; i++) {
        float norm = (float)(i * i) / (14.0f * 14.0f);
        int smileY = 48 + bounceY + (int)(5.0f * norm);
        u8g2.drawPixel(64 + i, smileY);
        u8g2.drawPixel(64 + i, smileY + 1);
    }

    // Floating hearts at different positions and speeds
    if (t < 2300) {
        float heartPositions[][3] = {
            {12, 0.18f, 0},    // x, speed, phase offset
            {50, 0.14f, 500},
            {78, 0.20f, 200},
            {115, 0.16f, 700},
        };
        u8g2.setFont(u8g2_font_6x10_tr);
        for (int h = 0; h < 4; h++) {
            int ht = max(0, (int)t - (int)heartPositions[h][2]);
            if (ht > 0) {
                int hy = 55 - (int)(ht * heartPositions[h][1]);
                if (hy > -5 && hy < 60) {
                    u8g2.drawGlyph((int)heartPositions[h][0], hy, '*');
                }
            }
        }
    }

    u8g2.sendBuffer();

    if (t > ANIM_DURATION_MS) {
        currentState = STATE_IDLE;
    }
}

void renderExcited() {
    unsigned long t = millis() - animStart;

    u8g2.clearBuffer();

    // Spinning star eyes ★_★ — rotation + size pulse
    float spinAngle = t / 300.0f;
    int starR = 10 + (int)(3.0f * sin(t / 80.0f));
    // Draw rotated stars by offsetting the base angle
    // Left star
    for (int i = 0; i < 5; i++) {
        float step = PI * 2.0f / 5.0f;
        float oa = -PI/2 + spinAngle + step * i;
        float ia = oa + step / 2.0f;
        float na = oa + step;
        u8g2.drawLine(EYE_L_X + (int)(starR * cos(oa)), EYE_Y + (int)(starR * sin(oa)),
                      EYE_L_X + (int)(starR * 0.4f * cos(ia)), EYE_Y + (int)(starR * 0.4f * sin(ia)));
        u8g2.drawLine(EYE_L_X + (int)(starR * 0.4f * cos(ia)), EYE_Y + (int)(starR * 0.4f * sin(ia)),
                      EYE_L_X + (int)(starR * cos(na)), EYE_Y + (int)(starR * sin(na)));
    }
    // Right star
    for (int i = 0; i < 5; i++) {
        float step = PI * 2.0f / 5.0f;
        float oa = -PI/2 - spinAngle + step * i;  // opposite spin
        float ia = oa + step / 2.0f;
        float na = oa + step;
        u8g2.drawLine(EYE_R_X + (int)(starR * cos(oa)), EYE_Y + (int)(starR * sin(oa)),
                      EYE_R_X + (int)(starR * 0.4f * cos(ia)), EYE_Y + (int)(starR * 0.4f * sin(ia)));
        u8g2.drawLine(EYE_R_X + (int)(starR * 0.4f * cos(ia)), EYE_Y + (int)(starR * 0.4f * sin(ia)),
                      EYE_R_X + (int)(starR * cos(na)), EYE_Y + (int)(starR * sin(na)));
    }

    // Sparkles — rotating around the face
    int numSparkles = 6;
    for (int i = 0; i < numSparkles; i++) {
        float a = (t / 400.0f) + i * (PI * 2.0f / numSparkles);
        int sx = 64 + (int)(45.0f * cos(a));
        int sy = 30 + (int)(25.0f * sin(a));
        int sz = 2 + ((t / 150 + i) % 3);
        if (sx > 2 && sx < 126 && sy > 2 && sy < 62) {
            drawSparkle(sx, sy, sz);
        }
    }

    // Excited open mouth
    int mouthR = 5 + (int)(2.0f * sin(t / 60.0f));
    drawMouthO(64, 52, mouthR);

    u8g2.sendBuffer();

    if (t > ANIM_DURATION_MS) {
        currentState = STATE_IDLE;
    }
}

void renderTalking() {
    unsigned long elapsed = millis() - textStart;
    u8g2.clearBuffer();

    // ── Rounded border frame ─────────────────────────────────────────
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(0, 0, 128, 64, 4);

    // ── Header bar with label ────────────────────────────────────────
    u8g2.drawBox(0, 0, 128, 13);
    u8g2.setDrawColor(0);  // black text on white header
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(5, 10, "\x04 Robi says:");  // \x04 = diamond glyph
    u8g2.setDrawColor(1);

    // ── Thin separator line ──────────────────────────────────────────
    u8g2.drawHLine(2, 14, 124);

    // ── Typewriter reveal: show only N characters so far ─────────────
    int charsToShow = (int)(elapsed / 25);  // ~40 chars per second
    if (charsToShow > (int)aiText.length()) charsToShow = aiText.length();
    String visibleText = aiText.substring(0, charsToShow);

    // ── Word-wrapped text inside the frame with margins ──────────────
    drawWrappedText(visibleText, 5, 25, 60);

    // ── Small bottom dots indicator if text is still typing ──────────
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
//  TOUCH DETECTION
// ═══════════════════════════════════════════════════════════════════════
void handleTouch() {
    uint32_t val = touchRead(TOUCH_PIN);
    bool touching = (val > TOUCH_THRESHOLD);

    if (touching && !wasTouched) {
        // New touch event
        unsigned long now = millis();
        if (now - lastTouchTime < doubleTapWindow) {
            tapCount++;
        } else {
            tapCount = 1;
        }
        lastTouchTime = now;
        wasTouched = true;
    }
    if (!touching) {
        wasTouched = false;
    }

    // Process taps after the double-tap window closes
    if (tapCount > 0 && millis() - lastTouchTime > doubleTapWindow) {
        if (tapCount >= 2) {
            Serial.println("Double tap → EXCITED");
            currentState = STATE_EXCITED;
            animStart    = millis();
        } else {
            Serial.println("Single tap → HAPPY");
            currentState = STATE_HAPPY;
            animStart    = millis();
        }
        tapCount = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  RANDOM BLINK (idle only)
// ═══════════════════════════════════════════════════════════════════════
void checkBlink() {
    if (currentState != STATE_IDLE) return;
    if (millis() - lastBlink > nextBlinkIn) {
        previousState = currentState;
        currentState  = STATE_BLINK;
        blinkStart    = millis();
        lastBlink     = millis();
        nextBlinkIn   = 2000 + random(1000, 5000);
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

    // ── Check for state changes ──────────────────────────────────────
    String stateBody = httpGet(String(SERVER) + "/state");
    if (stateBody.length() > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, stateBody) == DeserializationError::Ok) {
            String s = doc["state"].as<String>();
            Serial.print("State → "); Serial.println(s);
            if (s == "listening") {
                currentState = STATE_LISTENING;
                stateStart   = millis();
            } else if (s == "thinking") {
                currentState = STATE_THINKING;
                stateStart   = millis();
            } else if (s == "idle") {
                if (currentState == STATE_LISTENING || currentState == STATE_THINKING) {
                    currentState = STATE_IDLE;
                }
            }
        }
    }

    // ── Check for new AI reply ───────────────────────────────────────
    // Only poll /latest when not in the middle of a tap animation
    if (currentState != STATE_HAPPY && currentState != STATE_EXCITED) {
        String textBody = httpGet(String(SERVER) + "/latest");
        if (textBody.length() > 0) {
            JsonDocument doc;
            if (deserializeJson(doc, textBody) == DeserializationError::Ok) {
                aiText       = doc["text"].as<String>();
                // Count words to set display duration
                int wordCount = 1;
                for (int i = 0; i < (int)aiText.length(); i++) {
                    if (aiText[i] == ' ') wordCount++;
                }
                textDisplayDuration = (unsigned long)wordCount * MS_PER_WORD + EXTRA_DISPLAY_MS;
                Serial.printf("Reply (%d words, %lums): ", wordCount, textDisplayDuration);
                Serial.println(aiText);
                currentState = STATE_TALKING;
                textStart    = millis();
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
    Serial.println("\n=== Robi ===");

    Wire.begin(I2C_SDA, I2C_SCL);
    u8g2.begin();

    // Boot animation — eyes open up
    for (int h = 2; h <= EYE_H; h += 2) {
        u8g2.clearBuffer();
        drawEye(EYE_L_X, EYE_Y, EYE_W, h, 0, 0);
        drawEye(EYE_R_X, EYE_Y, EYE_W, h, 0, 0);
        u8g2.sendBuffer();
        delay(40);
    }
    delay(300);

    showStatus("Connecting...");
    connectWiFi();

    // Happy wiggle on connect
    for (int i = 0; i < 6; i++) {
        int offset = (i % 2 == 0) ? 3 : -3;
        u8g2.clearBuffer();
        drawEye(EYE_L_X + offset, EYE_Y, EYE_W, EYE_H, 0, 0);
        drawEye(EYE_R_X + offset, EYE_Y, EYE_W, EYE_H, 0, 0);
        u8g2.sendBuffer();
        delay(80);
    }

    showStatus("Say: Hey Robi!");
    delay(1200);

    lastBlink     = millis();
    lastPupilMove = millis();
}

// ═══════════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════════
void loop() {
    frameCounter++;

    handleTouch();
    checkBlink();
    pollServer();

    switch (currentState) {
        case STATE_IDLE:      renderIdle();      break;
        case STATE_BLINK:     renderBlink();     break;
        case STATE_LISTENING: renderListening(); break;
        case STATE_THINKING:  renderThinking();  break;
        case STATE_HAPPY:     renderHappy();     break;
        case STATE_EXCITED:   renderExcited();   break;
        case STATE_TALKING:   renderTalking();   break;
    }

    delay(25);  // ~40 fps
}
