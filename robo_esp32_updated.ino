#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "driver/i2s.h"

// ===== OLED =====
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// ===== WIFI =====
const char* ssid = "Spiderman_4g";
const char* password = "akashtheboss";
const char* server = "http://192.168.29.202:5000"; // update if needed

bool wifiConnected = false;

// ===== PINS =====
#define I2S_BCLK 2
#define I2S_LRC  3
#define I2S_SD   5

// ===== EYE CONFIG =====
#define EYE_L_X 32
#define EYE_R_X 96
#define EYE_Y   27
#define EYE_W   30
#define EYE_H   26
#define PUPIL_R 6

// ===== STATE =====
float pupilX = 0, pupilY = 0;
float targetPupilX = 0, targetPupilY = 0;
unsigned long lastPupilMove = 0;

#define VOLUME_THRESHOLD 6

// ===== AI TEXT =====
String aiText = "";
bool showingText = false;
unsigned long textTime = 0;

// ===== WIFI CONNECT =====
void connectWiFi() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);

  Serial.print("Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWiFi connect timeout, retrying...");
      start = millis();
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
  }

  Serial.println("\nConnected!");
  Serial.println(WiFi.localIP());
  wifiConnected = true;
}

// ===== DISPLAY TEXT =====
void showAIText(String msg) {
  aiText = msg;
  showingText = true;
  textTime = millis();
}

// ===== DRAW EYE =====
void drawEye(int cx, int cy, int w, int h, int pdx, int pdy) {
  int hw = w / 2;
  int hh = h / 2;

  u8g2.setDrawColor(1);
  u8g2.drawRBox(cx - hw, cy - hh, w, h, 5);

  int px = constrain(cx + pdx, cx - hw + PUPIL_R + 2, cx + hw - PUPIL_R - 2);
  int py = constrain(cy + pdy, cy - hh + PUPIL_R + 2, cy + hh - PUPIL_R - 2);

  u8g2.setDrawColor(0);
  u8g2.drawDisc(px, py, PUPIL_R);

  u8g2.setDrawColor(1);
}

// ===== FACE =====
void renderFace() {

  if (showingText) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tr);

    int y = 12;
    for (int i = 0; i < aiText.length(); i += 20) {
      String line = aiText.substring(i, i + 20);
      u8g2.drawStr(0, y, line.c_str());
      y += 12;
    }

    u8g2.sendBuffer();

    if (millis() - textTime > 4000) {
      showingText = false;
    }
    return;
  }

  u8g2.clearBuffer();

  drawEye(EYE_L_X, EYE_Y, EYE_W, EYE_H, (int)pupilX, (int)pupilY);
  drawEye(EYE_R_X, EYE_Y, EYE_W, EYE_H, (int)pupilX, (int)pupilY);

  u8g2.sendBuffer();
}

// ===== MIC =====
int readVolume() {
  int32_t sample = 0;
  size_t bytes_read = 0;
  i2s_read(I2S_NUM_0, &sample, sizeof(sample), &bytes_read, 10);
  return abs(sample) / 100000;
}

// ===== NETWORK helpers =====
String httpPostJson(const String &url, const String &jsonPayload, int &outCode) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  outCode = http.POST(jsonPayload);
  String payload = "";
  if (outCode > 0) {
    payload = http.getString();
  }
  http.end();
  return payload;
}

String httpGet(const String &url, int &outCode) {
  HTTPClient http;
  http.begin(url);
  outCode = http.GET();
  String payload = "";
  if (outCode > 0) payload = http.getString();
  http.end();
  return payload;
}

// Simple JSON extractor for: {"session_id":"..."}
String extractJsonString(const String &json, const String &key) {
  String pattern = String("\"") + key + "\":\"";
  int i = json.indexOf(pattern);
  if (i < 0) return "";
  i += pattern.length();
  int j = json.indexOf('"', i);
  if (j < 0) return "";
  return json.substring(i, j);
}

// ===== TRIGGER / DISPLAY =====
String sendTrigger() {
  int code;
  String url = String(server) + "/trigger";
  String body = "{}";
  String res = httpPostJson(url, body, code);
  Serial.print("trigger code="); Serial.println(code);
  Serial.print("trigger res="); Serial.println(res);
  if (code == 200) {
    String sid = extractJsonString(res, "session_id");
    return sid;
  }
  return "";
}

String pollDisplay(const String &session_id) {
  if (session_id.length() == 0) return "";
  int code;
  String url = String(server) + "/latest_display?session_id=" + session_id;
  String res = httpGet(url, code);
  // 204 -> empty body, code will be 204 but http.getString() may be empty
  Serial.print("poll code="); Serial.println(code);
  Serial.print("poll res="); Serial.println(res);
  if (code == 200) {
    // Expect {"text":"..."}
    String text = extractJsonString(res, "text");
    return text;
  }
  return "";
}

// ===== SETUP =====
void setup() {
  Wire.begin(6, 7);
  u8g2.begin();

  connectWiFi();

  // I2S setup
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count = 8,
    .dma_buf_len = 64
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
}

// ===== LOOP =====
void loop() {

  int volume = readVolume();

  // 🎤 Trigger on voice
  if (volume > VOLUME_THRESHOLD && wifiConnected) {

    Serial.println("Voice detected");

    // 1) POST /trigger to create a session
    String session = sendTrigger();
    if (session.length() == 0) {
      Serial.println("Failed to get session id from server");
    } else {
      Serial.print("Got session: "); Serial.println(session);

      // 2) Poll for display text until timeout
      unsigned long start = millis();
      unsigned long timeout = 20000; // 20s
      String reply = "";
      while (millis() - start < timeout) {
        reply = pollDisplay(session);
        if (reply.length() > 0) break;
        delay(1000);
      }

      if (reply.length() > 0) {
        Serial.print("Reply: "); Serial.println(reply);
        showAIText(reply);
      } else {
        Serial.println("No reply received within timeout");
      }
    }

    delay(3000);
  }

  // 👀 Eye movement
  if (millis() - lastPupilMove > 2000) {
    int pos[] = {-6, 0, 6};
    targetPupilX = pos[random(0, 3)];
    targetPupilY = pos[random(0, 3)];
    lastPupilMove = millis();
  }

  pupilX += (targetPupilX - pupilX) * 0.1;
  pupilY += (targetPupilY - pupilY) * 0.1;

  renderFace();
  delay(30);
}
