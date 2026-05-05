/*
 * Robi ESP32-S3 Mini — FluxGarage RoboEyes Edition
 * ==================================================
 * Uses FluxGarage RoboEyes library for smooth animated expressions.
 *
 * States:
 *   IDLE       — Default mood, autoblink + idle look-around
 *   LISTENING  — Happy mood, wide eyes, curious look
 *   THINKING   — Tired mood, confused animation, look up
 *   HAPPY      — Happy mood, laugh animation  (single tap)
 *   EXCITED    — Happy mood, flicker + laugh   (double tap)
 *   TALKING    — Word-wrapped text overlay
 *
 * Touch:  GPIO 4 (capacitive)
 * Server: GET /latest, GET /state
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>          // MUST come before RoboEyes (N/E/S/W macros conflict)
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

// ═══════════════════════════════════════════════════════════════════════
//  CONFIG
// ═══════════════════════════════════════════════════════════════════════
static const char* WIFI_SSID     = "Spiderman_4g";
static const char* WIFI_PASSWORD = "akashtheboss";
static const char* SERVER        = "http://192.168.29.209:5001";

#define I2C_SDA   6
#define I2C_SCL   7
#define TOUCH_PIN 4

// ═══════════════════════════════════════════════════════════════════════
//  DISPLAY + ROBOEYES
// ═══════════════════════════════════════════════════════════════════════
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
RoboEyes<Adafruit_SSD1306> roboEyes(display);

// ═══════════════════════════════════════════════════════════════════════
//  TIMING
// ═══════════════════════════════════════════════════════════════════════
#define MS_PER_WORD       330UL
#define EXTRA_DISPLAY_MS  2000UL
#define POLL_INTERVAL_MS  2000UL
#define ANIM_DURATION_MS  2500UL
#define TOUCH_THRESHOLD   25000

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

RobiState currentState  = STATE_IDLE;
RobiState prevAnimState = STATE_IDLE;   // state before tap animation
bool  wifiOK = false;

// Animation
unsigned long animStart   = 0;
unsigned long stateStart  = 0;

// Text
String        aiText      = "";
unsigned long textStart   = 0;
unsigned long textDisplayDuration = 5000UL;

// Touch
unsigned long lastTouchTime    = 0;
int           tapCount         = 0;
bool          wasTouched       = false;
unsigned long doubleTapWindow  = 400;

// Polling
unsigned long lastPoll = 0;

// Track if we already applied mood for this state
RobiState lastAppliedState = STATE_IDLE;

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
//  TEXT DISPLAY (uses Adafruit GFX directly when in TALKING state)
// ═══════════════════════════════════════════════════════════════════════
void drawWrappedText(const String& msg, int marginX, int startY, int maxY) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);

    const int charW = 6, lineH = 10;
    const int usableW = SCREEN_W - marginX * 2;
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
        display.setCursor(marginX, y);
        display.print(line);
        y += lineH;
        start = end;
    }
}

void renderTalking() {
    unsigned long elapsed = millis() - textStart;
    display.clearDisplay();

    // Rounded border frame
    display.drawRoundRect(0, 0, 128, 64, 4, SSD1306_WHITE);

    // Header bar
    display.fillRect(0, 0, 128, 13, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(5, 3);
    display.print("* Robi says:");
    display.setTextColor(SSD1306_WHITE);

    // Separator
    display.drawFastHLine(2, 14, 124, SSD1306_WHITE);

    // Typewriter reveal
    int charsToShow = (int)(elapsed / 25);
    if (charsToShow > (int)aiText.length()) charsToShow = aiText.length();
    String visibleText = aiText.substring(0, charsToShow);

    drawWrappedText(visibleText, 5, 18, 58);

    // Typing indicator dots
    if (charsToShow < (int)aiText.length()) {
        int dotPhase = (elapsed / 300) % 3;
        for (int i = 0; i <= dotPhase; i++) {
            display.fillCircle(58 + i * 6, 59, 1, SSD1306_WHITE);
        }
    }

    display.display();

    if (millis() - textStart > textDisplayDuration) {
        currentState = STATE_IDLE;
        lastAppliedState = STATE_TALKING; // force re-apply idle mood
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  APPLY MOOD / EXPRESSION for each state
// ═══════════════════════════════════════════════════════════════════════
void applyStateMood(RobiState state) {
    if (state == lastAppliedState) return;
    lastAppliedState = state;

    switch (state) {
        case STATE_IDLE:
            Serial.println("Mood: IDLE");
            roboEyes.setMood(DEFAULT);
            roboEyes.setPosition(DEFAULT);
            roboEyes.setAutoblinker(ON, 3, 2);
            roboEyes.setIdleMode(ON, 2, 3);
            roboEyes.setCuriosity(ON);
            roboEyes.setHFlicker(OFF, 0);
            roboEyes.setVFlicker(OFF, 0);
            roboEyes.open();
            break;

        case STATE_LISTENING:
            Serial.println("Mood: LISTENING");
            roboEyes.setMood(HAPPY);
            roboEyes.setPosition(DEFAULT);
            roboEyes.setAutoblinker(OFF, 0, 0);
            roboEyes.setIdleMode(OFF, 0, 0);
            roboEyes.setCuriosity(ON);
            roboEyes.setHFlicker(OFF, 0);
            roboEyes.setVFlicker(OFF, 0);
            roboEyes.open();
            // Trigger a single blink as "wake up" acknowledgment
            roboEyes.blink();
            break;

        case STATE_THINKING:
            Serial.println("Mood: THINKING");
            roboEyes.setMood(TIRED);
            roboEyes.setPosition(NW);
            roboEyes.setAutoblinker(OFF, 0, 0);
            roboEyes.setIdleMode(OFF, 0, 0);
            roboEyes.setCuriosity(OFF);
            roboEyes.setHFlicker(OFF, 0);
            roboEyes.setVFlicker(OFF, 0);
            // Play confused animation
            roboEyes.anim_confused();
            break;

        case STATE_HAPPY:
            Serial.println("Mood: HAPPY");
            roboEyes.setMood(HAPPY);
            roboEyes.setPosition(DEFAULT);
            roboEyes.setAutoblinker(OFF, 0, 0);
            roboEyes.setIdleMode(OFF, 0, 0);
            roboEyes.setCuriosity(OFF);
            roboEyes.setHFlicker(OFF, 0);
            roboEyes.setVFlicker(OFF, 0);
            // Laugh animation
            roboEyes.anim_laugh();
            break;

        case STATE_EXCITED:
            Serial.println("Mood: EXCITED");
            roboEyes.setMood(HAPPY);
            roboEyes.setPosition(DEFAULT);
            roboEyes.setAutoblinker(OFF, 0, 0);
            roboEyes.setIdleMode(OFF, 0, 0);
            roboEyes.setCuriosity(OFF);
            roboEyes.setHFlicker(ON, 3);
            roboEyes.setVFlicker(ON, 2);
            // Laugh animation
            roboEyes.anim_laugh();
            break;

        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  TOUCH DETECTION
// ═══════════════════════════════════════════════════════════════════════
void handleTouch() {
    uint32_t val = touchRead(TOUCH_PIN);
    bool touching = (val > TOUCH_THRESHOLD);

    if (touching && !wasTouched) {
        unsigned long now = millis();
        if (now - lastTouchTime < doubleTapWindow) {
            tapCount++;
        } else {
            tapCount = 1;
        }
        lastTouchTime = now;
        wasTouched = true;
    }
    if (!touching) wasTouched = false;

    if (tapCount > 0 && millis() - lastTouchTime > doubleTapWindow) {
        if (tapCount >= 2) {
            Serial.println("Double tap → EXCITED");
            prevAnimState = currentState;
            currentState  = STATE_EXCITED;
            animStart     = millis();
            lastAppliedState = STATE_IDLE; // force re-apply
        } else {
            Serial.println("Single tap → HAPPY");
            prevAnimState = currentState;
            currentState  = STATE_HAPPY;
            animStart     = millis();
            lastAppliedState = STATE_IDLE; // force re-apply
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

    // Check state changes
    String stateBody = httpGet(String(SERVER) + "/state");
    if (stateBody.length() > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, stateBody) == DeserializationError::Ok) {
            String s = doc["state"].as<String>();
            Serial.print("State → "); Serial.println(s);
            if (s == "listening") {
                currentState = STATE_LISTENING;
                stateStart = millis();
                lastAppliedState = STATE_IDLE; // force re-apply
            } else if (s == "thinking") {
                currentState = STATE_THINKING;
                stateStart = millis();
                lastAppliedState = STATE_IDLE;
            } else if (s == "idle") {
                if (currentState == STATE_LISTENING || currentState == STATE_THINKING) {
                    currentState = STATE_IDLE;
                    lastAppliedState = STATE_THINKING; // force re-apply
                }
            }
        }
    }

    // Check for AI reply
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
    Serial.println("\n=== Robi (RoboEyes) ===");

    Wire.begin(I2C_SDA, I2C_SCL);

    // Initialize display
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 failed!");
        while (1) delay(100);
    }
    display.clearDisplay();
    display.display();

    // Initialize RoboEyes
    roboEyes.begin(SCREEN_W, SCREEN_H, 100);

    // Configure eye shape
    roboEyes.setWidth(36, 36);
    roboEyes.setHeight(36, 36);
    roboEyes.setBorderradius(8, 8);
    roboEyes.setSpacebetween(10);
    roboEyes.setCyclops(OFF);
    roboEyes.setCuriosity(ON);
    roboEyes.setAutoblinker(ON, 3, 2);
    roboEyes.setIdleMode(ON, 2, 3);

    // Boot: eyes closed → open
    roboEyes.close();
    roboEyes.update();
    delay(500);
    roboEyes.open();

    // Show boot status
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 28);
    display.print("Connecting...");
    display.display();
    delay(300);

    connectWiFi();

    // Happy blink on connect
    roboEyes.setMood(HAPPY);
    roboEyes.open();
    for (int i = 0; i < 10; i++) {
        roboEyes.update();
        delay(30);
    }
    roboEyes.anim_laugh();
    for (int i = 0; i < 30; i++) {
        roboEyes.update();
        delay(30);
    }

    // Back to default
    roboEyes.setMood(DEFAULT);
    roboEyes.setPosition(DEFAULT);
    Serial.println("Ready!");
}

// ═══════════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════════
void loop() {
    handleTouch();
    pollServer();

    // Handle tap animation timeout
    if ((currentState == STATE_HAPPY || currentState == STATE_EXCITED) &&
        millis() - animStart > ANIM_DURATION_MS) {
        currentState = STATE_IDLE;
        lastAppliedState = STATE_HAPPY; // force re-apply idle
    }

    // Apply mood for current state
    applyStateMood(currentState);

    // Render
    if (currentState == STATE_TALKING) {
        renderTalking();
    } else {
        roboEyes.update();
    }

    delay(10);
}
