#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "FS.h"
#include "driver/i2s.h"
#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// SD Card Pins
#define SD_CS   5                 
#define SPI_MOSI 23
#define SPI_MISO 19
#define SPI_SCK  18

// I2S Pins (MAX98357)
#define I2S_DOUT  22
#define I2S_BCLK  26
#define I2S_LRC   25

// LCD Setup (I2C Address: 0x27)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Keypad Configuration
const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
    {'1', '2', '3'},
    {'4', '5', '6'},
    {'7', '8', '9'},
    {'*', '0', '#'}
};
byte rowPins[ROWS] = {32, 33, 27, 14};
byte colPins[COLS] = {13, 12, 4};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Buffer size
#define BUFFER_SIZE 1024  

// WAV file properties
uint32_t sampleRate = 44100;
uint16_t bitsPerSample = 16;
uint8_t numChannels = 2;             

void i2sInit(uint32_t rate, uint16_t bits, uint8_t channels) {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),  
        .sample_rate = rate,  
        .bits_per_sample = (bits == 16) ? I2S_BITS_PER_SAMPLE_16BIT : I2S_BITS_PER_SAMPLE_8BIT,
        .channel_format = (channels == 1) ? I2S_CHANNEL_FMT_ONLY_LEFT : I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
        .intr_alloc_flags = 0, 
        .dma_buf_count = 8,
        .dma_buf_len = BUFFER_SIZE
    };
    
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    
    i2s_set_pin(I2S_NUM_0, &pin_config);
}

bool readWavHeader(File &file) {
    uint8_t header[44];
    if (file.read(header, 44) != 44) {
        Serial.println("Invalid WAV file");
        return false;
    }
    sampleRate = *(uint32_t*)&header[24];
    bitsPerSample = *(uint16_t*)&header[34];
    numChannels = *(uint16_t*)&header[22];

    Serial.printf("Sample Rate: %d Hz\n", sampleRate);
    Serial.printf("Bit Depth: %d-bit\n", bitsPerSample);
    Serial.printf("Channels: %d\n", numChannels);
    return true;
}

void playWav(const char *filename) {
    File file = SD.open(filename);
    if (!file) {
        Serial.println("Failed to open file");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("File Not Found!");
        return;
    }
    Serial.printf("Playing: %s\n", filename);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Playing:");
    lcd.setCursor(0, 1);
    lcd.print(filename);
    
    if (!readWavHeader(file)) {
        file.close();
        return;
    }
    i2sInit(sampleRate, bitsPerSample, numChannels);
    uint8_t buffer[BUFFER_SIZE];
    size_t bytesRead;
    while ((bytesRead = file.read(buffer, BUFFER_SIZE)) > 0) {
        size_t bytesWritten;
        i2s_write(I2S_NUM_0, buffer, bytesRead, &bytesWritten, portMAX_DELAY);
        delay(1);
    }
    file.close();
}

String input = "";
void loop() {
    char key = keypad.getKey();
    if (key) {
        Serial.print("Key Pressed: ");
        Serial.println(key);
        if (key >= '0' && key <= '9') {
            input += key;
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Input: ");
            lcd.setCursor(7, 0);
            lcd.print(input);
        } else if (key == '#') {
            if (input.length() > 0) {
                String filename = "/FRESH_" + input + "Hz.wav";
                Serial.print("Looking for file: ");
                Serial.println(filename);
                if (SD.exists(filename)) {
                    playWav(filename.c_str());
                } else {
                    Serial.println("File not found!");
                    lcd.clear();
                    lcd.setCursor(0, 0);
                    lcd.print("File Not Found!");
                }
                input = "";
            }
        } else if (key == '*') {
            input = "";
            Serial.println("Input cleared.");
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Input Cleared");
        }
    }
}

void setup() {
    Serial.begin(115200);
    SPI.begin();
    Serial.println("Initializing SD card...");
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card Error! Check wiring.");
        return;
    }
    Serial.println("SD card initialized successfully.");
    
    // Initialize LCD
    lcd.begin(16,2);
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("SD Card Ready");
}
