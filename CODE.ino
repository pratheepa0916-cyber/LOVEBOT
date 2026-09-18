/*
  ===================================================================
  LOVEBOT v1.2: HARDWARE VOICE TO TANGLISH + WHATSAPP WEB + BUZZER
  ===================================================================
  - Push Button: GPIO 23 to GND (Push & Release to talk)
  - Buzzer: GPIO 13 to GND (Beeps for 1 sec ONLY ONCE per new message)
  - 0.96" I2C OLED (SSD1306): SDA = GPIO 21, SCL = GPIO 22
  - INMP441 Microphone: SCK = GPIO 14, WS = GPIO 25, SD = GPIO 32
  - Speech Model: gemini-3.5-flash-lite (Active & Unlimited)
  ===================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <mbedtls/base64.h>

// =======================================================
// 1. Wi-Fi & Firebase Configuration
// =======================================================
const char* WIFI_SSID     = "iPhone";              // Your Wi-Fi SSID
const char* WIFI_PASSWORD = "12345678";  // Your Wi-Fi Password

// Gemini API Configuration (Using active gemini-3.5-flash-lite)
const char* GEMINI_API_KEY = "AQ.Ab8RN6KUvvoyRqju6XzKT8LXTwo2VKowk1PxWo2v3ELSIIOj7g";
const char* GEMINI_HOST    = "generativelanguage.googleapis.com";
const int   GEMINI_PORT    = 443;
const char* GEMINI_PATH_AI = "/v1beta/models/gemini-3.5-flash-lite:generateContent";

// Firebase Realtime Database
const char* FIREBASE_HOST  = "lovebot-290bc-default-rtdb.firebaseio.com";
const int   FIREBASE_PORT  = 443;

// =======================================================
// 2. Hardware Pinout
// =======================================================
#define MIC_SCK_PIN    14
#define MIC_WS_PIN     25
#define MIC_SD_PIN     32

#define BUTTON_PIN     23  // Push button between GPIO 23 and GND
#define BUZZER_PIN     13  // Buzzer (+) to GPIO 13, Buzzer (-) to GND

#define OLED_SDA_PIN   21
#define OLED_SCL_PIN   22
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =======================================================
// 3. Audio Recording Buffer (8000 Hz Mono, 3.5 Seconds)
// =======================================================
#define MIC_SAMPLE_RATE 8000
const float RECORD_SECONDS = 3.5;

int maxRecordSamples = (int)(MIC_SAMPLE_RATE * RECORD_SECONDS);
size_t maxWavAllocSize = 44 + (maxRecordSamples * sizeof(int16_t));

uint8_t* wavBuffer = nullptr;
size_t actualWavSize = 0;
bool isSystemBusy = false;
unsigned long lastInboxCheck = 0;

// Prevents duplicate buzzing for the same incoming message
String lastSeenMessageText = "";

// =======================================================
// 4. OLED Display Helper Functions
// =======================================================
void displayStatus(const char* title, const char* subtitle = "") {
  display.clearDisplay();
  display.fillRect(0, 0, SCREEN_WIDTH, 13, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(20, 3);
  display.print("LOVEBOT VOICE");

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 22);
  display.print(title);

  if (strlen(subtitle) > 0) {
    display.setCursor(4, 40);
    display.print(subtitle);
  }
  display.display();
}

void displaySpokenText(String text) {
  display.clearDisplay();
  display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(18, 2);
  display.print("SPOKEN WORDS");

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 16);
  display.setTextWrap(true);
  display.print(text);
  display.display();
}

// =======================================================
// 5. INMP441 I2S Microphone Setup & WAV Header
// =======================================================
void setupMicrophone() {
  i2s_config_t mic_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 128,
    .use_apll = false,
    .tx_desc_auto_clear = false
  };

  i2s_pin_config_t mic_pins = {
    .bck_io_num = MIC_SCK_PIN,
    .ws_io_num = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD_PIN
  };

  i2s_driver_install(I2S_NUM_0, &mic_cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &mic_pins);
  Serial.println("I2S Mic initialized.");
}

void generateWavHeader(uint8_t* header, size_t dataSize, uint32_t sampleRate) {
  uint32_t fileSize = dataSize + 36;
  uint16_t numChannels = 1;
  uint16_t bitsPerSample = 16;
  uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
  uint16_t blockAlign = numChannels * (bitsPerSample / 8);

  memcpy(header, "RIFF", 4);
  header[4] = (uint8_t)(fileSize & 0xFF);
  header[5] = (uint8_t)((fileSize >> 8) & 0xFF);
  header[6] = (uint8_t)((fileSize >> 16) & 0xFF);
  header[7] = (uint8_t)((fileSize >> 24) & 0xFF);
  memcpy(header + 8, "WAVEfmt ", 8);

  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
  header[20] = 1;  header[21] = 0;
  header[22] = (uint8_t)(numChannels & 0xFF);
  header[23] = (uint8_t)((numChannels >> 8) & 0xFF);
  header[24] = (uint8_t)(sampleRate & 0xFF);
  header[25] = (uint8_t)((sampleRate >> 8) & 0xFF);
  header[26] = (uint8_t)((sampleRate >> 16) & 0xFF);
  header[27] = (uint8_t)((sampleRate >> 24) & 0xFF);
  header[28] = (uint8_t)(byteRate & 0xFF);
  header[29] = (uint8_t)((byteRate >> 8) & 0xFF);
  header[30] = (uint8_t)((byteRate >> 16) & 0xFF);
  header[31] = (uint8_t)((byteRate >> 24) & 0xFF);
  header[32] = (uint8_t)(blockAlign & 0xFF);
  header[33] = (uint8_t)((blockAlign >> 8) & 0xFF);
  header[34] = (uint8_t)(bitsPerSample & 0xFF);
  header[35] = (uint8_t)((bitsPerSample >> 8) & 0xFF);

  memcpy(header + 36, "data", 4);
  header[40] = (uint8_t)(dataSize & 0xFF);
  header[41] = (uint8_t)((dataSize >> 8) & 0xFF);
  header[42] = (uint8_t)((dataSize >> 16) & 0xFF);
  header[43] = (uint8_t)((dataSize >> 24) & 0xFF);
}

// =======================================================
// 6. Audio Recording (Auto-record for 3.5s)
// =======================================================
bool recordAudio() {
  i2s_zero_dma_buffer(I2S_NUM_0);
  size_t flushBytes = 0;
  int32_t flushBuf[128];
  while (i2s_read(I2S_NUM_0, flushBuf, sizeof(flushBuf), &flushBytes, 0) == ESP_OK && flushBytes > 0) {}

  displayStatus("Listening...", "Speak now! (3.5s)");
  Serial.println("\n>>> [RECORDING] Speak Tamil/Eng now for 3.5 seconds...");

  int16_t* pcmOutput = (int16_t*)(wavBuffer + 44);
  int recordedSamples = 0;
  int32_t dmaChunk[128];
  size_t bytesRead = 0;
  int32_t peak = 0;
  int32_t dcBias = 0;

  while (recordedSamples < maxRecordSamples) {
    int samplesToRead = min(64, maxRecordSamples - recordedSamples);
    i2s_read(I2S_NUM_0, dmaChunk, samplesToRead * 2 * sizeof(int32_t), &bytesRead, 30 / portTICK_PERIOD_MS);

    if (bytesRead > 0) {
      int pairCount = bytesRead / (2 * sizeof(int32_t));
      for (int i = 0; i < pairCount; i++) {
        int32_t rawRight = dmaChunk[i * 2 + 1] >> 14;
        int32_t rawLeft  = dmaChunk[i * 2] >> 14;
        // Auto-select active channel
        int32_t val = (abs(rawRight) >= 40 || abs(rawRight) >= abs(rawLeft)) ? rawRight : rawLeft;

        dcBias = (dcBias * 31 + val) / 32;
        val = val - dcBias;

        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;

        pcmOutput[recordedSamples++] = (int16_t)val;
        if (abs(val) > peak) peak = abs(val);
      }
    }
  }

  Serial.printf("Captured %d samples. Peak Amplitude: %d\n", recordedSamples, peak);
  size_t dataBytes = recordedSamples * sizeof(int16_t);
  actualWavSize = 44 + dataBytes;
  generateWavHeader(wavBuffer, dataBytes, MIC_SAMPLE_RATE);
  return true;
}

// =======================================================
// 7. Post Converted Text to Firebase
// =======================================================
bool sendToFirebase(const String& tanglishText) {
  displayStatus("Sending...", "Saving to pappa...");
  Serial.println(">>> Sending message to Firebase...");

  WiFiClientSecure fbClient;
  fbClient.setInsecure();
  fbClient.setTimeout(8000);

  if (!fbClient.connect(FIREBASE_HOST, FIREBASE_PORT)) {
    displayStatus("Firebase Err", "Connect failed");
    return false;
  }

  String safeText = tanglishText;
  safeText.replace("\"", "\\\"");

  String payload = "{\"text\":\"" + safeText + "\",\"sender\":\"lovebot\",\"timestamp\":{\".sv\":\"timestamp\"}}";

  fbClient.print(String("POST /messages.json HTTP/1.1\r\n") +
                 "Host: " + FIREBASE_HOST + "\r\n" +
                 "Content-Type: application/json\r\n" +
                 "Content-Length: " + String(payload.length()) + "\r\n" +
                 "Connection: close\r\n\r\n" +
                 payload);

  unsigned long t0 = millis();
  while (fbClient.connected() && !fbClient.available()) {
    if (millis() - t0 > 6000) break;
    delay(20);
  }

  String fbResponse = "";
  while (fbClient.available()) fbResponse += fbClient.readString();
  fbClient.stop();

  Serial.println(">>> Message saved to Firebase!");
  displayStatus("Sent to pappa!", "Stored in Firebase");
  delay(1800);
  return true;
}

// =======================================================
// 8. Check Incoming Messages & Buzz ONLY ONCE
// =======================================================
void checkForIncomingMessages() {
  if (millis() - lastInboxCheck < 2000) return;
  lastInboxCheck = millis();

  WiFiClientSecure fbClient;
  fbClient.setInsecure();
  fbClient.setTimeout(3000);

  if (!fbClient.connect(FIREBASE_HOST, FIREBASE_PORT)) return;

  fbClient.print(String("GET /bot_inbox.json HTTP/1.1\r\n") +
                 "Host: " + FIREBASE_HOST + "\r\n" +
                 "Connection: close\r\n\r\n");

  unsigned long t0 = millis();
  while (fbClient.connected() && !fbClient.available()) {
    if (millis() - t0 > 2500) break;
    delay(10);
  }

  String response = "";
  while (fbClient.available()) response += fbClient.readString();
  fbClient.stop();

  // Check if unread
  bool isUnread = (response.indexOf("\"read\":false") != -1 || response.indexOf("\"read\": false") != -1);

  if (isUnread) {
    int textIdx = response.indexOf("\"text\":");
    if (textIdx != -1) {
      int start = response.indexOf("\"", textIdx + 7) + 1;
      int end = response.indexOf("\"", start);
      if (start > 0 && end > start) {
        String msgFromPappa = response.substring(start, end);
        msgFromPappa.replace("\\\"", "\"");

        // CRITICAL: Only sound buzzer if this message was NOT buzzed before!
        if (msgFromPappa != lastSeenMessageText) {
          lastSeenMessageText = msgFromPappa; // Mark as seen locally

          Serial.println("\n💌 ============================================");
          Serial.println(">>> [NEW MESSAGE FROM PAPPA]: " + msgFromPappa);
          Serial.println(">>> BUZZER ON for 1.0 second ONLY ONCE! <<<<");
          Serial.println("==============================================");

          // 1. Buzzer sounds for exactly 1.0 second (1000ms)
          digitalWrite(BUZZER_PIN, HIGH);

          // 2. Display on OLED Screen
          display.clearDisplay();
          display.setTextSize(1);
          display.setTextColor(SSD1306_WHITE);
          display.setCursor(0, 0);
          display.println(">> FROM PAPPA <<");
          display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
          display.setCursor(0, 15);
          display.println(msgFromPappa);
          display.setCursor(0, 52);
          display.println("<3 ( * ^ _ ^ * ) <3");
          display.display();

          delay(1000); // 1-second chime
          digitalWrite(BUZZER_PIN, LOW); // Turn off buzzer

          // 3. Mark message as read in Firebase so it never triggers again
          WiFiClientSecure ackClient;
          ackClient.setInsecure();
          ackClient.setTimeout(3000);
          if (ackClient.connect(FIREBASE_HOST, FIREBASE_PORT)) {
            String ackPayload = "{\"read\":true}";
            ackClient.print(String("PATCH /bot_inbox.json HTTP/1.1\r\n") +
                            "Host: " + FIREBASE_HOST + "\r\n" +
                            "Content-Type: application/json\r\n" +
                            "Content-Length: " + String(ackPayload.length()) + "\r\n" +
                            "Connection: close\r\n\r\n" +
                            ackPayload);
            ackClient.stop();
          }

          delay(2500); // Show text on OLED for 2.5 more seconds
          displayStatus("LOVEBOT READY", "Push 23 to Talk");
        }
      }
    }
  }
}

// =======================================================
// 9. Speech to Tanglish Conversion (Gemini 3.5 Flash Lite)
// =======================================================
void processVoiceToTanglish() {
  if (WiFi.status() != WL_CONNECTED) {
    displayStatus("WiFi Error", "Disconnected!");
    return;
  }

  displayStatus("Thinking...", "Converting voice...");
  Serial.println(">>> Sending audio to Gemini 3.5 Flash Lite...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);

  if (!client.connect(GEMINI_HOST, GEMINI_PORT)) {
    displayStatus("API Error", "Connect failed");
    Serial.println("ERROR: Could not connect to Gemini Host!");
    return;
  }

  size_t base64Len = ((actualWavSize + 2) / 3) * 4;
  const char* p1 = "{\"contents\":[{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
  const char* p2 = "\"}},{\"text\":\"Transcribe this speech. If spoken in Tamil, transcribe into phonetic Tanglish (Latin alphabet, e.g. en chella kutty, epdi irukeenga, sapteengala). If English, transcribe in English. Output ONLY the raw spoken words without quotes or punctuation.\"}]}]}";

  size_t totalLen = strlen(p1) + base64Len + strlen(p2);
  String postPath = String(GEMINI_PATH_AI) + "?key=" + String(GEMINI_API_KEY);

  client.print("POST " + postPath + " HTTP/1.1\r\n");
  client.print("Host: " + String(GEMINI_HOST) + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(totalLen) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(p1);

  // Stream Base64 chunks to save RAM
  const size_t CHUNK_RAW = 48;
  unsigned char b64Chunk[68];
  for (size_t i = 0; i < actualWavSize; i += CHUNK_RAW) {
    size_t inLen = min(CHUNK_RAW, actualWavSize - i);
    size_t outLen = 0;
    mbedtls_base64_encode(b64Chunk, sizeof(b64Chunk), &outLen, wavBuffer + i, inLen);
    client.write(b64Chunk, outLen);
  }

  client.print(p2);

  unsigned long t0 = millis();
  while (client.connected() && !client.available()) {
    if (millis() - t0 > 20000) {
      Serial.println("ERROR: Gemini response timeout!");
      break;
    }
    delay(20);
  }

  String response = "";
  while (client.available()) {
    response += client.readString();
  }
  client.stop();

  Serial.println(">>> [GEMINI RAW RESPONSE RECEIVED]");

  // Parse text from Gemini JSON response
  int textIdx = response.indexOf("\"text\": \"");
  if (textIdx == -1) {
    textIdx = response.indexOf("\"text\":");
  }

  if (textIdx != -1) {
    int start = response.indexOf("\"", textIdx + 6) + 1;
    int end = start;
    while (end < response.length()) {
      if (response.charAt(end) == '\"' && response.charAt(end - 1) != '\\') break;
      end++;
    }

    if (end > start) {
      String tanglishText = response.substring(start, end);
      tanglishText.replace("\\n", " ");
      tanglishText.replace("\\\"", "\"");
      tanglishText.trim();

      if (tanglishText.length() > 0 && tanglishText != "[NO_SPEECH]") {
        Serial.println("\n🎉 >>> TRANSCRIBED TANGLISH: " + tanglishText);
        displaySpokenText(tanglishText);
        delay(1500);
        sendToFirebase(tanglishText);
        return;
      }
    }
  }

  Serial.println("Warning: No speech detected in audio.");
  displayStatus("No Words Heard", "Speak closer to mic");
  delay(1500);
}

// =======================================================
// 10. Setup & Main Loop
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Push-to-Talk button on GPIO 23
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Buzzer on GPIO 13
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Initialize I2C OLED Display
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.clearDisplay();
  displayStatus("Lovebot v1.2", "Starting...");

  // Allocate audio buffer safely
  if (psramFound()) {
    wavBuffer = (uint8_t*)ps_malloc(maxWavAllocSize);
  }
  if (!wavBuffer) {
    wavBuffer = (uint8_t*)malloc(maxWavAllocSize);
  }

  setupMicrophone();

  // Connect Wi-Fi with Auto-Reconnect (Prevents disconnection)
  displayStatus("Connecting WiFi", WIFI_SSID);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }

  displayStatus("LOVEBOT READY", "Push 23 to Talk");

  // Startup confirmation beep (60ms)
  digitalWrite(BUZZER_PIN, HIGH);
  delay(60);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println("\n==========================================================");
  Serial.println(">>> LOVEBOT v1.2 READY!                                <<<");
  Serial.println(">>> 1. PUSH & RELEASE GPIO 23 TO TALK                  <<<");
  Serial.println(">>> 2. GEMINI CONVERTS SPEECH TO TANGLISH              <<<");
  Serial.println(">>> 3. BUZZER (GPIO 13) CHIMES ONCE PER NEW MESSAGE    <<<");
  Serial.println("==========================================================");
}

void loop() {
  // Wi-Fi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(200);
    return;
  }

  if (!isSystemBusy) {
    // 1. Check for new messages from Pappa via WhatsApp Web
    checkForIncomingMessages();

    // 2. Check if GPIO 23 push button is pressed
    if (digitalRead(BUTTON_PIN) == LOW) {
      delay(40); // Debounce
      if (digitalRead(BUTTON_PIN) == LOW) {
        // Wait for release so it acts as an auto-release switch
        while (digitalRead(BUTTON_PIN) == LOW) {
          delay(10);
        }
        delay(40); // Debounce release

        isSystemBusy = true;

        // Quick 60ms beep to notify user that recording started
        digitalWrite(BUZZER_PIN, HIGH);
        delay(60);
        digitalWrite(BUZZER_PIN, LOW);

        if (recordAudio()) {
          processVoiceToTanglish();
        }

        delay(800);
        isSystemBusy = false;
        displayStatus("LOVEBOT READY", "Push 23 to Talk");
      }
    }
  }
  delay(20);
}
