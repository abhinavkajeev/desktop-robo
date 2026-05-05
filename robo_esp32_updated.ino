/*
 * Robi — ESP32-S3 Firmware (Updated)
 * ====================================
 * Simplified flow:
 *   1. Listens to I2S mic for voice activity.
 *   2. On voice → shows "Listening…" on OLED and polls /latest on the
 *      laptop server every second.
 *   3. When a reply arrives → displays it on the OLED (scrolling text).
 *   4. Eye animation runs continuously in the background.
 *
 * The wake-word detection and AI call are fully handled by laptop_client.py
 * running on the laptop. The ESP32 only needs to:
 *   a) Detect voice so we know *when* to start polling.
 *   b) Display whatever the laptop tells it to.
 *
 * Server endpoint used: GET /latest  → {\"text\": \"...\"} or 204 (nothing ready)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "driver/i2s.h"
#include <ArduinoJson.h>

// ===== OLED (128×64 SSD1306 over HW I2C) =====
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// ===== WIFI — update these =====
const char* WIFI_SSID     = "Spiderman_4g";
const char* WIFI_PASSWORD = "akashtheboss";
const char* SERVER        = "http://192.168.29.202:5000";

bool wifiOK = false;

// ===== I2S MIC PINS =====
#define I2S_BCLK  2
#define I2S_LRC   3
#define I2S_SD    5

// ===== EYE CONFIG =====
#define EYE_L_X  32
#define EYE_R_X  96
#define EYE_Y    27
#define EYE_W    30
#define EYE_H    26
#define PUPIL_R   6

// ===== VOLUME THRESHOLD =====
#define VOLUME_THRESHOLD 6

// ===== PUPIL STATE =====
float pupilX = 0, pupilY = 0;
float targetPX = 0, targetPY = 0;
unsigned long lastPupilMove = 0;

// ===== AI TEXT STATE =====
String aiText      = "";
bool   showingText = false;
unsigned long textStart = 0;
#define TEXT_DISPLAY_MS 6000   // how long to show AI text (ms)

// ═══════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════
void connectWiFi() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t > 20000) {
      Serial.println("\nRetrying…");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      t = millis();
    }
  }
  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());
  wifiOK = true;
}

// ═══════════════════════════════════════════════════════
//  OLED HELPERS
// ═══════════════════════════════════════════════════════
void drawEye(int cx, int cy, int w, int h, int pdx, int pdy) {
  int hw = w / 2, hh = h / 2;
  u8g2.setDrawColor(1);
  u8g2.drawRBox(cx - hw, cy - hh, w, h, 5);

  int px = constrain(cx + pdx, cx - hw + PUPIL_R + 2, cx + hw - PUPIL_R - 2);
  int py = constrain(cy + pdy, cy - hh + PUPIL_R + 2, cy + hh - PUPIL_R - 2);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(px, py, PUPIL_R);
  u8g2.setDrawColor(1);
}

// Word-wrap helper: draw `msg` in 6×10 font across the full 128px width.
void drawWrappedText(const String &msg) {
  u8g2.setFont(u8g2_font_6x10_tr);
  const int charW = 6, lineH = 12, maxCol = 128 / charW; // ~21 chars
  int y = 12;
  int start = 0;
  while (start < (int)msg.length() && y < 64) {
    int end = start + maxCol;
    if (end >= (int)msg.length()) {
      end = msg.length();
    } else {
      // break at last space
      int sp = msg.lastIndexOf(' ', end - 1);
      if (sp > start) end = sp + 1;
    }
    String line = msg.substring(start, end);
    line.trim();
    u8g2.drawStr(0, y, line.c_str());
    y += lineH;
    start = end;
  }
}

void showStatus(const String &msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 20, msg.c_str());
  u8g2.sendBuffer();
}

void renderFace() {
  if (showingText) {
    u8g2.clearBuffer();
    drawWrappedText(aiText);
    u8g2.sendBuffer();
    if (millis() - textStart > TEXT_DISPLAY_MS) {
      showingText = false;
    }
    return;
  }

  u8g2.clearBuffer();
  drawEye(EYE_L_X, EYE_Y, EYE_W, EYE_H, (int)pupilX, (int)pupilY);
  drawEye(EYE_R_X, EYE_Y, EYE_W, EYE_H, (int)pupilX, (int)pupilY);
  u8g2.sendBuffer();
}

// ═══════════════════════════════════════════════════════
//  MIC
// ═══════════════════════════════════════════════════════
int readVolume() {
  int32_t sample = 0;
  size_t  bytes  = 0;
  i2s_read(I2S_NUM_0, &sample, sizeof(sample), &bytes, 10);
  return abs(sample) / 100000;
}

// ═══════════════════════════════════════════════════════
//  NETWORK — GET /latest
// ═══════════════════════════════════════════════════════
String pollLatest() {
  if (!wifiOK) return "";

  HTTPClient http;
  String url = String(SERVER) + "/latest";
  http.begin(url);
  int code = http.GET();

  if (code == 204) {           // nothing ready
    http.end();
    return "";
  }
  if (code != 200) {
    Serial.print("GET /latest returned: "); Serial.println(code);
    http.end();
    return "";
  }

  String body = http.getString();
  http.end();

  // Parse {"text":"..."}
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("JSON parse error: "); Serial.println(err.c_str());
    return "";
  }
  return doc["text"].as<String>();
}

// ═══════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════
void setup() {
  Wire.begin(6, 7);   // SDA=6, SCL=7 — adjust for your board
  u8g2.begin();
  showStatus("Booting…");

  connectWiFi();
  showStatus("WiFi OK!");
  delay(800);

  // I2S microphone
  i2s_config_t cfg = {
    .mode             = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate      = 16000,
    .bits_per_sample  = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format   = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count    = 8,
    .dma_buf_len      = 64,
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_BCLK,
    .ws_io_num    = I2S_LRC,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD,
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);

  Serial.println("Setup complete. Listening for voice…");
}

// ═══════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════
void loop() {
  int vol = readVolume();

  // ── Voice detected → poll server for AI reply ────────────────────────────
  if (vol > VOLUME_THRESHOLD && wifiOK && !showingText) {
    Serial.println("Voice detected — polling server…");
    showStatus("Listening...");

    unsigned long pollStart = millis();
    const unsigned long POLL_TIMEOUT = 25000;  // 25 s max

    while (millis() - pollStart < POLL_TIMEOUT) {
      String reply = pollLatest();
      if (reply.length() > 0) {
        Serial.print("Reply: "); Serial.println(reply);
        aiText      = reply;
        showingText = true;
        textStart   = millis();
        break;
      }
      delay(1000);   // poll every second
    }

    if (!showingText) {
      Serial.println("No reply received within timeout.");
    }

    delay(2000);  // debounce before listening again
  }

  // ── Eye idle animation ────────────────────────────────────────────────────
  if (millis() - lastPupilMove > 2000) {
    int pos[] = {-6, 0, 6};
    targetPX = pos[random(0, 3)];
    targetPY = pos[random(0, 3)];
    lastPupilMove = millis();
  }
  pupilX += (targetPX - pupilX) * 0.1f;
  pupilY += (targetPY - pupilY) * 0.1f;

  renderFace();
  delay(30);
}
