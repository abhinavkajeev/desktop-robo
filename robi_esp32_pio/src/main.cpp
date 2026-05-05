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
#define BLINK_DURATION_MS 150UL
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
//  DRAWING PRIMITIVES
// ═══════════════════════════════════════════════════════════════════════

/* Normal rounded-rectangle eye with pupil */
void drawEye(int cx, int cy, int w, int h, int pdx, int pdy) {
    int hw = w / 2, hh = h / 2;
    u8g2.setDrawColor(1);
    u8g2.drawRBox(cx - hw, cy - hh, w, h, 6);

    int px = constrain(cx + pdx, cx - hw + PUPIL_R + 2, cx + hw - PUPIL_R - 2);
    int py = constrain(cy + pdy, cy - hh + PUPIL_R + 2, cy + hh - PUPIL_R - 2);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(px, py, PUPIL_R);
    u8g2.setDrawColor(1);
}

/* Half-closed eye for blink — just a thin slit */
void drawBlinkEye(int cx, int cy, int w) {
    int hw = w / 2;
    u8g2.setDrawColor(1);
    u8g2.drawRBox(cx - hw, cy - 3, w, 6, 3);
}

/* Happy ^_^ arc eyes */
void drawHappyEye(int cx, int cy, int w) {
    int hw = w / 2;
    u8g2.setDrawColor(1);
    // Draw a downward arc (happy squint) using multiple lines
    for (int i = -hw; i <= hw; i++) {
        int y = cy - 4 + (int)(8.0f * (1.0f - (float)(i*i) / (float)(hw*hw)));
        u8g2.drawPixel(cx + i, y);
        u8g2.drawPixel(cx + i, y + 1);
        u8g2.drawPixel(cx + i, y + 2);
    }
}

/* Star eye ★ for excited state */
void drawStarEye(int cx, int cy, int r) {
    u8g2.setDrawColor(1);
    // 5-pointed star
    float angle = -PI / 2;
    float step  = PI * 2.0f / 5.0f;
    for (int i = 0; i < 5; i++) {
        float outerAngle = angle + step * i;
        float innerAngle = outerAngle + step / 2.0f;
        int x1 = cx + (int)(r * cos(outerAngle));
        int y1 = cy + (int)(r * sin(outerAngle));
        int x2 = cx + (int)((r * 0.4f) * cos(innerAngle));
        int y2 = cy + (int)((r * 0.4f) * sin(innerAngle));
        u8g2.drawLine(x1, y1, x2, y2);
        // Connect to next outer point
        float nextOuter = outerAngle + step;
        int x3 = cx + (int)(r * cos(nextOuter));
        int y3 = cy + (int)(r * sin(nextOuter));
        u8g2.drawLine(x2, y2, x3, y3);
    }
}

/* Sparkle particle at (x,y) */
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

/* Draw word-wrapped text inside a padded area.
   marginX = left/right margin, startY = first line baseline, maxY = bottom limit */
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

/* Show centered status text */
void showStatus(const char* msg) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x13B_tr);
    int w = u8g2.getStrWidth(msg);
    u8g2.drawStr((128 - w) / 2, 35, msg);
    u8g2.sendBuffer();
}

// ═══════════════════════════════════════════════════════════════════════
//  RENDER EACH STATE
// ═══════════════════════════════════════════════════════════════════════
int frameCounter = 0;

void renderIdle() {
    // Smooth pupil movement
    if (millis() - lastPupilMove > 2500UL) {
        const int pos[] = {-7, -3, 0, 3, 7};
        targetPX = pos[random(0, 5)];
        targetPY = pos[random(0, 3)];   // less vertical movement
        lastPupilMove = millis();
    }
    pupilX += (targetPX - pupilX) * 0.08f;
    pupilY += (targetPY - pupilY) * 0.08f;

    u8g2.clearBuffer();
    drawEye(EYE_L_X, EYE_Y, EYE_W, EYE_H, (int)pupilX, (int)pupilY);
    drawEye(EYE_R_X, EYE_Y, EYE_W, EYE_H, (int)pupilX, (int)pupilY);

    // Cute little mouth — a small subtle line
    u8g2.drawRBox(56, 48, 16, 4, 2);

    u8g2.sendBuffer();
}

void renderBlink() {
    u8g2.clearBuffer();
    unsigned long elapsed = millis() - blinkStart;

    if (elapsed < BLINK_DURATION_MS / 3) {
        // Closing
        int squeeze = map(elapsed, 0, BLINK_DURATION_MS / 3, 0, EYE_H / 2 - 3);
        drawEye(EYE_L_X, EYE_Y, EYE_W, EYE_H - squeeze * 2, (int)pupilX, 0);
        drawEye(EYE_R_X, EYE_Y, EYE_W, EYE_H - squeeze * 2, (int)pupilX, 0);
    } else if (elapsed < BLINK_DURATION_MS * 2 / 3) {
        // Closed
        drawBlinkEye(EYE_L_X, EYE_Y, EYE_W);
        drawBlinkEye(EYE_R_X, EYE_Y, EYE_W);
    } else {
        // Opening
        unsigned long openT = elapsed - BLINK_DURATION_MS * 2 / 3;
        int squeeze = map(openT, 0, BLINK_DURATION_MS / 3, EYE_H / 2 - 3, 0);
        drawEye(EYE_L_X, EYE_Y, EYE_W, EYE_H - squeeze * 2, (int)pupilX, 0);
        drawEye(EYE_R_X, EYE_Y, EYE_W, EYE_H - squeeze * 2, (int)pupilX, 0);
    }

    u8g2.drawRBox(56, 48, 16, 4, 2);
    u8g2.sendBuffer();

    if (elapsed >= BLINK_DURATION_MS) {
        currentState = previousState;
    }
}

void renderListening() {
    unsigned long t = millis() - stateStart;
    float pulse = 1.0f + 0.15f * sin(t / 150.0f);   // gentle pulse

    int w = (int)(EYE_W * pulse);
    int h = (int)((EYE_H + 4) * pulse);  // slightly bigger eyes

    u8g2.clearBuffer();

    // Eyes wide open, pupils centered, pulsing
    drawEye(EYE_L_X, EYE_Y, w, h, 0, 0);
    drawEye(EYE_R_X, EYE_Y, w, h, 0, 0);

    // Small "ears up" indicator — two small triangles above
    int bounce = (int)(2.0f * sin(t / 200.0f));
    u8g2.drawTriangle(EYE_L_X - 4, EYE_Y - h/2 - 4 + bounce,
                       EYE_L_X + 4, EYE_Y - h/2 - 4 + bounce,
                       EYE_L_X, EYE_Y - h/2 - 10 + bounce);
    u8g2.drawTriangle(EYE_R_X - 4, EYE_Y - h/2 - 4 + bounce,
                       EYE_R_X + 4, EYE_Y - h/2 - 4 + bounce,
                       EYE_R_X, EYE_Y - h/2 - 10 + bounce);

    // Open mouth (excited "o")
    u8g2.drawCircle(64, 50, 4);

    u8g2.sendBuffer();
}

void renderThinking() {
    unsigned long t = millis() - stateStart;
    float lookX = -6;   // looking up-left
    float lookY = -5;

    u8g2.clearBuffer();

    // Eyes looking up-left
    drawEye(EYE_L_X, EYE_Y, EYE_W, EYE_H, (int)lookX, (int)lookY);
    drawEye(EYE_R_X, EYE_Y, EYE_W, EYE_H, (int)lookX, (int)lookY);

    // Thinking dots animation
    drawThinkingDots(t / 40);

    // Small squiggle above head (thought bubble)
    int bubbleY = 5 + (int)(2.0f * sin(t / 300.0f));
    u8g2.drawCircle(100, bubbleY, 3);
    u8g2.drawCircle(108, bubbleY - 2, 2);
    u8g2.drawDisc(113, bubbleY - 3, 1);

    u8g2.sendBuffer();
}

void renderHappy() {
    unsigned long t = millis() - animStart;

    // Bounce effect
    int bounceY = (int)(3.0f * sin(t / 100.0f) * max(0.0f, 1.0f - t / 2000.0f));

    u8g2.clearBuffer();

    // Happy squint eyes ^_^
    drawHappyEye(EYE_L_X, EYE_Y + bounceY, EYE_W);
    drawHappyEye(EYE_R_X, EYE_Y + bounceY, EYE_W);

    // Blush circles
    u8g2.drawCircle(EYE_L_X - 2, EYE_Y + 10 + bounceY, 4);
    u8g2.drawCircle(EYE_R_X + 2, EYE_Y + 10 + bounceY, 4);

    // Wide smile
    for (int i = -12; i <= 12; i++) {
        int smileY = 50 + bounceY + (int)(4.0f * (float)(i*i) / 144.0f);
        u8g2.drawPixel(64 + i, smileY);
        u8g2.drawPixel(64 + i, smileY + 1);
    }

    // Small hearts floating up
    if (t < 2000) {
        int heartY = 10 - (int)(t / 200.0f);
        if (heartY > -5) {
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.drawGlyph(10, max(0, heartY + 15), '<');
            u8g2.drawGlyph(110, max(0, heartY + 10), '3');
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

    // Star eyes ★_★
    int starR = 10 + (int)(2.0f * sin(t / 100.0f));
    drawStarEye(EYE_L_X, EYE_Y, starR);
    drawStarEye(EYE_R_X, EYE_Y, starR);

    // Sparkles around the face
    int sparklePhase = (t / 100) % 8;
    int sparklePositions[][2] = {
        {10, 10}, {118, 10}, {15, 55}, {113, 55},
        {64, 5},  {30, 58},  {98, 58}, {64, 60}
    };
    for (int i = 0; i < 4; i++) {
        int idx = (sparklePhase + i) % 8;
        int sz  = 2 + (i % 2);
        drawSparkle(sparklePositions[idx][0], sparklePositions[idx][1], sz);
    }

    // Wide open mouth "O"
    int mouthR = 5 + (int)(2.0f * sin(t / 80.0f));
    u8g2.drawCircle(64, 52, mouthR);

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
