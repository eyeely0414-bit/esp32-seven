#include <Arduino.h>
#include <Wire.h>

// Standalone SSD1306 I2C diagnostic based on the user's supplied OLED example.
namespace {
constexpr uint8_t kAddress = 0x3C;
constexpr uint8_t kSdaPin = 21;
constexpr uint8_t kSclPin = 22;
constexpr uint32_t kStartupDelayMs = 5000;

bool writeByte(uint8_t value, bool data) {
  Wire.beginTransmission(kAddress);
  Wire.write(data ? 0x40 : 0x00);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void command(uint8_t value) { writeByte(value, false); }

void setPosition(uint8_t page, uint8_t column) {
  command(0xB0 | page);
  command(0x10 | (column >> 4));
  command(column & 0x0F);
}

void fill(uint8_t pixels) {
  for (uint8_t page = 0; page < 8; ++page) {
    setPosition(page, 0);
    for (uint8_t column = 0; column < 128; ++column) {
      writeByte(pixels, true);
    }
  }
}

void initialise() {
  const uint8_t commands[] = {
      0xAE, 0x00, 0x10, 0x40, 0x81, 0xCF, 0xA1, 0xC8, 0xA6, 0xA8,
      0x3F, 0xD3, 0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
      0x40, 0x20, 0x02, 0x8D, 0x14, 0xA4, 0xA6, 0xAF,
  };
  for (const uint8_t value : commands) command(value);
}

void checkerboard() {
  for (uint8_t page = 0; page < 8; ++page) {
    setPosition(page, 0);
    for (uint8_t column = 0; column < 128; ++column) {
      writeByte((column + page) & 1 ? 0xAA : 0x55, true);
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(400000);
  Wire.beginTransmission(kAddress);
  const bool detected = Wire.endTransmission() == 0;
  Serial.printf("SSD1306 0x3C: %s\n", detected ? "FOUND" : "NO_REPLY");

  // Let the OLED power rail settle before sending the supplied example's
  // SSD1306 initialization sequence or any pixel data.
  Serial.println("Waiting 5 seconds before display test...");
  delay(kStartupDelayMs);
  initialise();
}

void loop() {
  fill(0xFF);  // Entire display white.
  delay(1000);
  fill(0x00);  // Entire display black.
  delay(1000);
  checkerboard();
  delay(1000);
}
