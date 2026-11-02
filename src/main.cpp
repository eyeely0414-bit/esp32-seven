#include <Arduino.h>
#include <ESP_I2S.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "secrets.h"

namespace {
constexpr uint8_t kButtonPin = 32;
constexpr uint8_t kI2sBclkPin = 26;
constexpr uint8_t kI2sWsPin = 25;
constexpr uint8_t kI2sDataOutPin = 27;
constexpr uint8_t kI2sDataInPin = 34;

constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kMaxRecordSeconds = 2;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kVoiceRequestTimeoutMs = 60000;
constexpr size_t kMaxRecordedSamples = kSampleRate * kMaxRecordSeconds;
constexpr size_t kReadBufferSamples = 128;  // Interleaved left/right samples.

I2SClass i2s;
// Store one mono channel as 16-bit PCM to keep a full two-second test clip
// within the ESP32's internal RAM.
int16_t recording[kMaxRecordedSamples];
int32_t inputBuffer[kReadBufferSamples];
bool audioReady = false;

void playTone(uint16_t frequencyHz, uint16_t durationMs) {
  if (!audioReady) {
    return;
  }

  const uint32_t totalSamples = kSampleRate * durationMs / 1000;
  uint32_t halfPeriodSamples = kSampleRate / (frequencyHz * 2);
  if (halfPeriodSamples == 0) {
    halfPeriodSamples = 1;
  }
  constexpr int32_t kAmplitude = 10000000;
  int32_t sample = kAmplitude;

  for (uint32_t index = 0; index < totalSamples; ++index) {
    if (index % halfPeriodSamples == 0) {
      sample = -sample;
    }
    i2s.write(&sample, sizeof(sample));  // Left slot
    i2s.write(&sample, sizeof(sample));  // Right slot
  }
}

void playReadyJingle() {
  playTone(880, 100);
  delay(80);
  playTone(1320, 130);
}

void playSubmittedJingle() {
  playTone(880, 70);
  delay(70);
  playTone(880, 100);
}

void playErrorTone() {
  playTone(220, 350);
}

bool connectToWifi() {
  Serial.printf("Wi-Fi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < kWifiConnectTimeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi: connection failed");
    return false;
  }

  Serial.print("Wi-Fi: connected, ESP32 IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool checkBackendHealth() {
  const String endpoint = "http://" + String(BACKEND_HOST) + ":" +
                          String(BACKEND_PORT) + "/health";
  Serial.printf("Backend: checking %s\n", endpoint.c_str());

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(endpoint)) {
    Serial.println("Backend: invalid endpoint");
    return false;
  }

  const int statusCode = http.GET();
  const String response = statusCode > 0 ? http.getString() : "";
  http.end();
  Serial.printf("Backend: HTTP %d %s\n", statusCode, response.c_str());
  return statusCode == HTTP_CODE_OK;
}

void playPcm16(Stream &audioStream, int contentLength) {
  // Baidu TTS returns 16 kHz, 16-bit, mono PCM. I2S output is 32-bit stereo.
  int16_t pcm16[128];
  int32_t pcm32[128];
  int remaining = contentLength;
  uint32_t lastDataAt = millis();

  while ((remaining > 0 || contentLength < 0) &&
         millis() - lastDataAt < kVoiceRequestTimeoutMs) {
    const int available = audioStream.available();
    if (available <= 0) {
      delay(1);
      continue;
    }

    const size_t wanted = min(static_cast<size_t>(available), sizeof(pcm16));
    const size_t bytesRead =
        audioStream.readBytes(reinterpret_cast<char *>(pcm16), wanted);
    if (bytesRead == 0) {
      continue;
    }
    lastDataAt = millis();
    if (contentLength >= 0) {
      remaining -= bytesRead;
    }

    const size_t sampleCount = bytesRead / sizeof(int16_t);
    for (size_t index = 0; index < sampleCount; ++index) {
      pcm32[index] = static_cast<int32_t>(pcm16[index]) * 65536;
      i2s.write(&pcm32[index], sizeof(pcm32[index]));  // Left slot
      i2s.write(&pcm32[index], sizeof(pcm32[index]));  // Right slot
    }
  }
}

bool sendVoiceAndPlayReply(size_t sampleCount) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("VOICE: Wi-Fi disconnected");
    return false;
  }

  const String endpoint = "http://" + String(BACKEND_HOST) + ":" +
                          String(BACKEND_PORT) + "/voice";
  Serial.printf("VOICE: sending %u PCM samples\n",
                static_cast<unsigned>(sampleCount));

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(kVoiceRequestTimeoutMs);
  if (!http.begin(endpoint)) {
    Serial.println("VOICE: invalid backend endpoint");
    return false;
  }
  // begin() resets request state, so headers must be added afterwards.
  http.addHeader("Content-Type", "audio/pcm");
  http.addHeader("X-Device-Id", "rehab-mvp-001");

  const int statusCode = http.POST(reinterpret_cast<uint8_t *>(recording),
                                   sampleCount * sizeof(int16_t));
  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("VOICE: backend HTTP %d\n", statusCode);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  Serial.printf("VOICE: reply received, %d PCM bytes\n", contentLength);
  WiFiClient *replyStream = http.getStreamPtr();
  playPcm16(*replyStream, contentLength);
  http.end();
  Serial.println("VOICE: reply playback complete");
  return true;
}

size_t recordWhileButtonHeld() {
  size_t recordedSamples = 0;

  while (digitalRead(kButtonPin) == LOW && recordedSamples < kMaxRecordedSamples) {
    const size_t bytesRead =
        i2s.readBytes(reinterpret_cast<char *>(inputBuffer), sizeof(inputBuffer));
    const size_t samplesRead = bytesRead / sizeof(int32_t);

    // INMP441 L/R is wired to GND, so use data from the left slot only.
    for (size_t index = 0;
         index + 1 < samplesRead && recordedSamples < kMaxRecordedSamples;
         index += 2) {
      recording[recordedSamples++] =
          static_cast<int16_t>(inputBuffer[index] >> 16);
    }
  }

  return recordedSamples;
}

void runPushToTalkTest() {
  Serial.println("VOICE TEST: recording started");
  playTone(1040, 80);
  const size_t sampleCount = recordWhileButtonHeld();

  Serial.printf("VOICE TEST: captured %u samples\n",
                static_cast<unsigned>(sampleCount));

  if (sampleCount < kSampleRate / 5) {
    Serial.println("VOICE TEST: recording too short");
    playTone(220, 300);
    return;
  }

  Serial.println("VOICE: sending recording to AI service");
  playSubmittedJingle();
  if (sendVoiceAndPlayReply(sampleCount)) {
    playReadyJingle();
  } else {
    playErrorTone();
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nStarting screen-free voice hardware test...");

  pinMode(kButtonPin, INPUT_PULLUP);
  i2s.setPins(kI2sBclkPin, kI2sWsPin, kI2sDataOutPin, kI2sDataInPin);
  audioReady = i2s.begin(I2S_MODE_STD, kSampleRate,
                         I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO,
                         I2S_STD_SLOT_BOTH);

  if (!audioReady) {
    Serial.println("ERROR: I2S initialization failed");
    return;
  }

  Serial.println("READY: hold GPIO32 button to record, release to hear playback");
  playReadyJingle();

  if (!connectToWifi()) {
    playErrorTone();
    return;
  }

  if (!checkBackendHealth()) {
    playErrorTone();
    return;
  }

  Serial.println("BACKEND READY: local voice bridge reachable");
  playTone(1320, 80);
  delay(60);
  playTone(1760, 120);
}

void loop() {
  static bool wasPressed = false;
  const bool pressed = audioReady && digitalRead(kButtonPin) == LOW;

  if (pressed && !wasPressed) {
    wasPressed = true;
    runPushToTalkTest();
  }

  if (!pressed) {
    wasPressed = false;
  }
}
