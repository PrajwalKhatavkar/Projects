#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <EEPROM.h>
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include <WiFiClient.h>  
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// ---------------------------------------------------------------------------
// Configuration Constants
// ---------------------------------------------------------------------------
#define EEPROM_SIZE         100
#define SSID_ADDR           0
#define PASS_ADDR           50
#define SAMPLE_RATE         8000             // in Hz
#define BITS_PER_SAMPLE     16
#define RECORD_TIME         9                // in seconds, length of each recording
#define SD_CS_PIN           5                // Chip select pin for SD card
#define SAMPLE_BUFFER_SIZE  128              // DMA buffer length for I2S
#define RECORD_INTERVAL     30000            // Delay between recordings in ms
#define FILE_DIFFERENCE 50000
#define I2S_WS              25               // I2S word select (LRCLK)
#define I2S_SCK             26               // I2S clock (BCK)
#define I2S_SD              22               // I2S data in
// ---------------------------------------------------------------------------
// Global variables and objects
// ---------------------------------------------------------------------------
char storedSSID[32];
char storedPassword[32];
unsigned long lastRecordTime = 0;
unsigned long lastUploadTime = 0;
uint32_t uploadIndex = 1;  // Start from upload index 1
uint32_t recordIndex = 1;
int upload_success = 0;
String recordfileName;
String uploadfileName;
int Order = 0;
WebServer server(80);
// ---------------------------------------------------------------------------
// WAV Header Structure
// ---------------------------------------------------------------------------
struct WAVHeader {
  char riffHeader[4] = { 'R', 'I', 'F', 'F' };                // "RIFF" header for WAV files
  uint32_t fileSize = 0;                                      // Total file size (calculated later)
  char waveHeader[4] = { 'W', 'A', 'V', 'E' };                // "WAVE" header for WAV files
  char fmtHeader[4] = { 'f', 'm', 't', ' ' };                 // "fmt " header for WAV format
  uint32_t fmtChunkSize = 16;                                 // Format chunk size
  uint16_t audioFormat = 1;                                   // Audio format: PCM
  uint16_t numChannels = 1;                                   // Number of audio channels (Mono)
  uint32_t sampleRate = SAMPLE_RATE;                          // Sample rate (e.g., 44100Hz)
  uint32_t byteRate = SAMPLE_RATE * 1 * (BITS_PER_SAMPLE / 8); // Byte rate (sample rate * channels * bytes per sample)
  uint16_t blockAlign = 1 * (BITS_PER_SAMPLE / 8);            // Block alignment (channels * bytes per sample)
  uint16_t bitsPerSample = BITS_PER_SAMPLE;                   // Bits per sample (e.g., 16)
  char dataHeader[4] = { 'd', 'a', 't', 'a' };                // "data" header for WAV file data section
  uint32_t dataSize = 0;                                      // Size of the audio data section (calculated later)
};
// ---------------------------------------------------------------------------
// Web Server: Serve Styled Webpage for Updating Wi-Fi Credentials
// ---------------------------------------------------------------------------
const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 WiFi Setup</title>
    <style>
        body { font-family: 'Roboto', sans-serif; text-align: center; padding: 20px; background: linear-gradient(to right, #6a11cb, #2575fc); color: #fff; }
        .container { max-width: 400px; margin: auto; padding: 25px; background: white; border-radius: 15px; box-shadow: 0px 4px 20px rgba(0, 0, 0, 0.1); color: #333; }
        h2 { color: #444; margin-bottom: 20px; font-size: 1.8em; }
        input { width: 100%; padding: 12px; margin: 10px 0; border: 1px solid #ddd; border-radius: 8px; font-size: 1em; background-color: #f9f9f9; }
        button { background: #5cb85c; color: #fff; padding: 12px; border: none; border-radius: 8px; font-size: 1.2em; cursor: pointer; transition: background 0.3s ease; width: 100%; }
        button:hover { background: #4cae4c; transform: translateY(-2px); box-shadow: 0 6px 15px rgba(92, 184, 92, 0.4); }
        .message { margin-top: 15px; font-weight: bold; font-size: 1.1em; }
        .message.success { color: #28a745; }
        .message.error { color: #d9534f; }
    </style>
</head>
<body>
    <div class="container">
        <h2>Connect Your ESP32 to WiFi</h2>
        <form id="wifiForm">
            <input type="text" id="ssid" name="ssid" placeholder="Enter WiFi SSID" required>
            <div style="position: relative;">
                <input type="password" id="pass" name="pass" placeholder="Enter WiFi Password" required>
                <label>
                    <input type="checkbox" id="togglePassword" style="margin-left: 10px;"> Show Password
                </label>
            </div>
            <button type="submit">Save & Connect</button>
        </form>
        <p class="message" id="status"></p>
        <script>
            document.getElementById("togglePassword").addEventListener("change", function() {
                const passwordField = document.getElementById("pass");
                passwordField.type = this.checked ? "text" : "password";
            });
            document.getElementById("wifiForm").addEventListener("submit", function(event) {
                event.preventDefault();
                const ssid = document.getElementById("ssid").value;
                const pass = document.getElementById("pass").value;
                const status = document.getElementById("status");
                const confirmation = confirm(`You entered:\\nSSID: ${ssid}\\nPassword: ${pass}\\n\\nDo you want to proceed?`);
                if (confirmation) {
                    document.getElementById("ssid").value = '';
                    document.getElementById("pass").value = '';
                    fetch("/", {
                        method: "POST",
                        headers: { "Content-Type": "application/x-www-form-urlencoded" },
                        body: `ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`
                    })
                    .then(response => {
                        if (response.ok) {
                            status.className = "message success";
                            status.innerText = "WiFi credentials saved! Restarting ESP32...";
                        } else {
                            status.className = "message error";
                            status.innerText = "Failed to save credentials.";
                        }
                    })
                    .catch(error => {
                        status.className = "message error";
                        status.innerText = "Error: " + error.message;
                    });
                } else {
                    status.className = "message error";
                    status.innerText = "Please re-enter your credentials.";
                }
            });
        </script>
    </div>
</body>
</html>
)rawliteral";
// ---------------------------------------------------------------------------
// Function Prototypes
// ---------------------------------------------------------------------------
void setupI2S();
void writeWAVHeader(File &file, uint32_t dataSize);
void Restore_File();
void connectToWiFi();
void startHotspot();
void handleConfig();
void keepDongleActive();
void keepAlivePing();
void recordAudio();
void recordTask(void *pvParameters);
void uploadTask(void *pvParameters);
// ---------------------------------------------------------------------------
// I2S Setup for Audio Recording
// ---------------------------------------------------------------------------
void setupI2S() {
  const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),  // Set I2S in master receive mode
    .sample_rate = SAMPLE_RATE,                           // Set sample rate for audio
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,         // Set audio resolution to 16 bits per sample
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,          // Set audio format to mono (only left channel)
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,      // Set I2S communication format to MSB
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,             // Interrupt level configuration
    .dma_buf_count = 8,                                   // Number of DMA buffers for data transfer
    .dma_buf_len = SAMPLE_BUFFER_SIZE,                    // Length of each DMA buffer
    .use_apll = false,                                    // Disable APLL (automatic PLL) for audio
    .tx_desc_auto_clear = false,                          // Disable automatic clearing of TX descriptors
    .fixed_mclk = 0                                       // Do not use a fixed master clock
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,              // Serial clock pin
    .ws_io_num = I2S_WS,                // Word select (LRCK) pin
    .data_out_num = I2S_PIN_NO_CHANGE,  // No data output pin (not used for input)
    .data_in_num = I2S_SD               // Serial data input pin
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);                               // Install I2S driver with the configuration
  i2s_set_pin(I2S_NUM_0, &pin_config);                                               // Set the I2S pins
  i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);  // Set the I2S clock parameters
}
// ---------------------------------------------------------------------------
// Write or Update the WAV Header with Correct Data Size
// ---------------------------------------------------------------------------
void writeWAVHeader(File& file, uint32_t dataSize) {
  WAVHeader header;                                  // Create a new WAVHeader struct
  header.fileSize = 36 + dataSize;                   // Set the total file size (36 bytes for the header + data size)
  header.dataSize = dataSize;                        // Set the size of the data section
  file.seek(0);                                      // Move the file pointer to the start of the file
  file.write((uint8_t*)&header, sizeof(WAVHeader));  // Write the header to the file
}
// ---------------------------------------------------------------------------
// Restore File Indices Based on Existing Audio Files in SD Card
// ---------------------------------------------------------------------------
void Restore_File() {
  uint32_t smallest = UINT32_MAX;  // Initialize to maximum uint32_t value
  uint32_t largest = 0;            // Initialize to minimum uint32_t
  bool foundValidFile = false;     // Flag to indicate if valid files are found

  File root = SD.open("/");  // Open the root directory
  if (!root || !root.isDirectory()) {
    Serial.println("Failed to open root directory or not a directory.");
    return;
  }

  File entry = root.openNextFile();  // Open the first file in the directory
  while (entry) {
    String filename = entry.name();
    const String prefix = "Audio_File_";

    if (filename.startsWith(prefix)) {
      // Extract the number from the filename
      uint32_t number = strtoul(filename.substring(prefix.length()).c_str(), nullptr, 10);

      if (number > 0) {  // Only consider valid numbers
        foundValidFile = true;
        smallest = min(smallest, number);
        largest = max(largest, number);
      }
    }

    entry.close();                // Close the current file
    entry = root.openNextFile();  // Open the next file
  }

  if (foundValidFile) {
    if ((largest - smallest) < FILE_DIFFERENCE) {
      recordIndex = largest + 1;
      uploadIndex = smallest;
    } else {
      Order = 1;
      recordIndex = smallest + 1;
      uploadIndex = largest;
    }

    Serial.printf("File to be Recorded: %u\n", recordIndex);
    Serial.printf("File to be Uploaded: %u\n", uploadIndex);
    Serial.printf("Smallest Number: %u\n", smallest);
    Serial.printf("Largest Number: %u\n", largest);
  } else {
    Serial.println("No valid audio files found.");
    recordIndex = 1;
    uploadIndex = 1;
  }
}
// ---------------------------------------------------------------------------
// Wi-Fi Connection: Reads Credentials from EEPROM and Attempts Connection
// ---------------------------------------------------------------------------
void connectToWiFi() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(SSID_ADDR, storedSSID);
    EEPROM.get(PASS_ADDR, storedPassword);
    if (strlen(storedSSID) == 0) {
        strcpy(storedSSID, "YOUR_WIFI_SSID");
        strcpy(storedPassword, "YOUR_WIFI_PASSWORD");
    }
    Serial.printf("Connecting to WiFi: %s\n", storedSSID);
    WiFi.begin(storedSSID, storedPassword);
    for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) {
        delay(1000);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi failed. Starting Hotspot...");
        startHotspot();
        delay(10000);
    }
}
// ---------------------------------------------------------------------------
// Start Hotspot and Launch Web Server for Wi-Fi Configuration
// ---------------------------------------------------------------------------
void startHotspot() {
    WiFi.softAP("ESP32_Hotspot", "12345678");
    Serial.printf("Hotspot Started! Connect to: http://%s\n", WiFi.softAPIP().toString().c_str());
    delay(10000);
    server.on("/", handleConfig);
    server.begin();
}
// ---------------------------------------------------------------------------
// Handle Config Function
// ---------------------------------------------------------------------------
void handleConfig() {
    if (server.hasArg("ssid") && server.hasArg("pass")) {
        String newSSID = server.arg("ssid");
        String newPass = server.arg("pass");
        newSSID.toCharArray(storedSSID, sizeof(storedSSID));
        newPass.toCharArray(storedPassword, sizeof(storedPassword));
        EEPROM.put(SSID_ADDR, storedSSID);
        EEPROM.put(PASS_ADDR, storedPassword);
        EEPROM.commit();
        server.send(200, "text/html", "<h2>WiFi credentials saved! Restarting...</h2>");
        delay(1000);
        ESP.restart();
    } else {
        server.send(200, "text/html", webpage);
    }
}
// ---------------------------------------------------------------------------
// Record Audio using I2S and Save as a WAV File on the SD Card
// ---------------------------------------------------------------------------
void recordAudio() {
  // Open the file on the SD card for writing
  File audioFile = SD.open(recordfileName, FILE_WRITE);  
  if (!audioFile) {  // Check if file opening failed
    Serial.println("Failed to open file on SD card.");
    return;  // Exit the function if file opening fails
  }

  uint32_t dataSize = 0;  // Variable to keep track of the data size
  writeWAVHeader(audioFile, dataSize);  // Write the initial WAV header (with data size 0)

  //Serial.println("Recording...");  // Print message to indicate that recording has started

  int16_t sample;  // Variable to store a single audio sample (16-bit mono)
  size_t bytesRead;  // Variable to store the number of bytes read from I2S
  const uint32_t totalSamples = SAMPLE_RATE * RECORD_TIME;  // Total number of samples to record (based on duration and sample rate)

  // Loop to record audio samples
  for (uint32_t i = 0; i < totalSamples; i++) {
    // Read audio sample from I2S, expecting mono (16-bit)
    i2s_read(I2S_NUM_0, &sample, sizeof(sample), &bytesRead, portMAX_DELAY);  
    audioFile.write((uint8_t*)&sample, sizeof(sample));  // Write the audio sample to the SD card
    dataSize += sizeof(sample);  // Update the data size (add size of the sample written)
  }

  // Write the final WAV header with the correct data size
  writeWAVHeader(audioFile, dataSize);  
  audioFile.close();  // Close the file after recording

  Serial.print("Recording audio file #");
  Serial.println(recordIndex);
  Serial.println("Recording complete.");
}
// ---------------------------------------------------------------------------
// Task: Periodically Record Audio Files
// ---------------------------------------------------------------------------
void recordTask(void* pvParameters) 
{
    while (true) 
    {
        Serial.println("Recording audio...");  // Print message indicating recording has started
    
        // Generate unique filename for each recorded audio file
        recordfileName = "/Audio_File_" + String(recordIndex) + ".wav";  
    
        // Call function to actually record the audio (not shown here)
        recordAudio();  // Make sure this function completes before proceeding
    
        // Increment recordIndex and check if it should be reset
        recordIndex++;
        if (recordIndex == 0) 
        {
            recordIndex = 1;  // Reset to 1 to avoid potential wraparound to 0
            Order = 1;        // Reset Order as well
        }
        // Allow other tasks to run by adding a delay
        vTaskDelay(100);  // 100 ms delay to yield the CPU to other tasks
    }
}
// ---------------------------------------------------------------------------
// Task: Periodically Upload Audio Files via Wi-Fi
// ---------------------------------------------------------------------------
void uploadFile(bool useWiFi) {
  uploadIndex++;
  if (uploadIndex == 0) uploadIndex++; // Ensure uploadIndex doesn't go to 0
  uploadfileName = "/Audio_File_" + String(uploadIndex) + ".wav";  // Generate unique filename
  File audioFile1 = SD.open(uploadfileName, FILE_READ);  // Check for the next file
  // Check if recording is complete or not
  if ((!audioFile1 || (recordIndex == uploadIndex)) && Order == 0) {
    uploadIndex--;
    Serial.println("Audio is still recording.");
    audioFile1.close();
    delay(2500);
    return;
  } 
  else if (!audioFile1 && Order == 1) {
    if (uploadIndex == 2) {
      Order = 0;
      uploadIndex--;
      delay(2500);
      return;
    }
  }
  uploadIndex--;
  if (uploadIndex == 0) uploadIndex--;  // Set uploadIndex correctly
  // Open file for reading
  uploadfileName = "/Audio_File_" + String(uploadIndex) + ".wav";
  File audioFile = SD.open(uploadfileName, FILE_READ);
  if (!audioFile) {
    Serial.println("Failed to open file for reading.");
    return;  // Exit if file opening fails
  }
  // Set up the client based on whether Wi-Fi or GSM is used
  WiFiClientSecure client;
  int loop = 1;
  if (useWiFi) {
    Serial.println("Connecting via Wi-Fi.");
    client.setInsecure();  // Disable SSL certificate verification
    if (client.connect("music-recognition.vendpro.tech", 443)) {
      bool uploadSuccess = false;
      do {
        Serial.println("Connected to server via Wi-Fi.");
        // Multipart Form Data construction
        String boundary = "------------------------abcd1234";
        String filename = uploadfileName.substring(uploadfileName.lastIndexOf("/") + 1);
        String startRequest = "--" + boundary + "\r\n" + "Content-Disposition: form-data; name=\"audio_file\"; filename=\"" + filename + "\"\r\n" + "Content-Type: audio/wav\r\n\r\n";
        //String startRequest = "--" + boundary + "\r\n" + "Content-Disposition: form-data; name=\"audio_File\"; filename=\"" + uploadfileName + "\"\r\n" + "Content-Type: audio/wav\r\n\r\n";
        String deviceIDField = "\r\n--" + boundary + "\r\n" + "Content-Disposition: form-data; name=\"DeviceID\"\r\n\r\n" + WiFi.macAddress() + "\r\n";
        String endRequest = "--" + boundary + "--\r\n";
        int contentLength = startRequest.length() + audioFile.size() + deviceIDField.length() + endRequest.length();
        // Sending HTTP Request
        client.print("POST /listen-file HTTP/1.1\r\n");
        client.print("Host: music-recognition.vendpro.tech\r\n");
        client.print("User-Agent: ESP32/AudioUploader\r\n");
        client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
        client.print("Content-Length: " + String(contentLength) + "\r\n");
        client.print("Connection: close\r\n\r\n");
        client.print(startRequest);
        byte buffer[1024];  // Buffer for data chunks
        while (audioFile.available()) {
          size_t bytesRead = audioFile.read(buffer, sizeof(buffer));
          client.write(buffer, bytesRead);  // Send the file in chunks
        }
        client.print(deviceIDField);
        client.print(endRequest);
        Serial.println("Server Response:");
        String response;
        bool success = false;
        while (client.connected() || client.available()) {
          if (client.available()) {
            response = client.readStringUntil('\n');
            //Serial.println(response);
            if (response.indexOf("\"status\": true") > -1) {
              success = true;
            }
          }
        }
        if (success) {
          uploadSuccess = true;
          Serial.println("File uploaded successfully.");
        } else {
          Serial.println("File upload failed. Retrying...");
          delay(1000);
          loop++;
        }
        client.stop();
        if (loop == 5) break;  // Retry logic
      } while (!uploadSuccess);  // Retry until successful
    }
  }
  audioFile.close();
  if (SD.remove(uploadfileName)) {
    Serial.println("File upladed to the Server Successfully......");
    Serial.println("File deleted from SD card: " + uploadfileName);
  }
  uploadIndex++;
  delay(5000);
}
// ---------------------------------------------------------------------------
// Task: Periodically Upload Audio Files via Wi-Fi
// ---------------------------------------------------------------------------
void uploadTask(void* pvParameters) 
{
    while (true) 
	{        
    // Generate the filename for the current upload file
    uploadfileName = "/Audio_File_" + String(uploadIndex) + ".wav";  
    //Serial.println("Uploading...");
    // Check if the device is connected to Wi-Fi
    if (WiFi.status() == WL_CONNECTED) 
		{
      uploadFile(true);  // Use WiFi to upload
    } 
	}
}
// ---------------------------------------------------------------------------
// Keep the Dongle (Wi-Fi Module) Active by Pinging a Known Server
// ---------------------------------------------------------------------------
void keepDongleActive() {
  WiFiClient client;
  if (client.connect("8.8.8.8", 53)) {
    //Serial.println("keepDongleActive: Ping successful.");
    client.stop();
  } else {
    Serial.println("keepDongleActive: Ping failed. Reconnecting Wi-Fi...");
    connectToWiFi();
  }
}
// ---------------------------------------------------------------------------
// Keep the Wi-Fi Connection Alive by Sending a GET Request
// ---------------------------------------------------------------------------
void keepAlivePing() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    if (client.connect("clients3.google.com", 80)) {
      client.print("HEAD /generate_204 HTTP/1.1\r\nHost: clients3.google.com\r\n\r\n");
      client.stop();
      //Serial.println("keepAlivePing: Ping successful.");
    } else {
      Serial.println("keepAlivePing: Ping failed. Reconnecting Wi-Fi...");
      connectToWiFi();
    }
  }
}
// ---------------------------------------------------------------------------
// Setup: Initialize Serial, SD Card, I2S, Wi-Fi, and Start Tasks
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    setupI2S();
    SPI.begin();
    while(1)
    {
    if (!SD.begin(SD_CS_PIN)) 
    {
      Serial.println("SD card initialization failed!");
      //  while (true);
    }
    else 
    {
      Serial.println("SD card initialized.");
      break ;
    }
    delay(500);
    }
    
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) 
    {
        Serial.println("Connected to WiFi");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
    } 
    Restore_File();
    delay(100);
    Serial.println("Task Started to record");
    xTaskCreatePinnedToCore(recordTask, "Record Task", 10000, NULL, 1, NULL, 0);
    Serial.println("Task Started to upload");
    xTaskCreatePinnedToCore(uploadTask, "Upload Task", 10000, NULL, 1, NULL, 1);
}
// ---------------------------------------------------------------------------
// Main Loop: Periodically Call Keep-Alive Functions
// ---------------------------------------------------------------------------
void loop() {
  server.handleClient();
  keepDongleActive();
  keepAlivePing();
  //vTaskDelay(5000 / portTICK_PERIOD_MS);
}
