#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPUpdate.h>
#include "mbedtls/base64.h"
#include <esp_task_wdt.h>

// General
#define FILESYSTEM LittleFS
#define FIRMWAREVERSION "v0.6"

// Watchdog
#define WDT_TIMEOUT 500

// OTA updates
#define MANUAL_UPDATE 0
#define TIME_UPDATE   1
const char* configFilePath = "/config.json";
const char* gitLogUrl = "https://api.github.com/repos/jareyeshurtado/esp32-ota-updates/contents/";
const char* gitKeyFilePath = "/git_key.json";
String gitToken;
int updHour, updMinute;

// IOs
#define STATUS_LED_BUILTIN 2
#define TOGGLELED 12  
#define PUSHBUTTON 13

// Subscription
#define MINUTECHECK 60000 // 60 * 1000
#define HOURCHECK 3600000 // 60 * 60 * 1000
#define DAILYCHECK 86400000 // 24 * 60 * 60 * 1000
unsigned long lastCheck = 0;
bool subscription_active = false; // Default to active
const char* google_sheets_url = "https://script.google.com/macros/s/AKfycbwjCztF96qVGNGBfEOEnzHAK9a8_3yI8h8ikaH3OGLDINDcUKJyr9K_AhQgN7VS42UN/exec";

// States Push Button Logic
#define UPDATEDURATION 10000 // 10 seconds for OTA update
#define REBOOTDURATION 5000 // 5 seconds for reboot
bool buttonPressed = false;
const unsigned long debounceDelay = 50; // Short debounce
unsigned long buttonPressTime = 0;

// Toggle Led 
bool outputState = LOW;  // Store the state of the output 
long lastToggletime = 0;

// Timestamp logic
unsigned long lastTimestampSent = 0; // Tracks last timestamp sent
long cooldownPeriod; // 5 seconds

// UDP 
WiFiUDP udp;
const char* broadcastIP = "192.168.1.255"; // UDP broadcast
int udpPort;

// Wifi & NTP Logic
WiFiUDP ntpUDP;
HTTPClient http;
WiFiClientSecure client;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -6 * 3600);
String ssid, password, boardID;

// heartbeat
int heartBeatPort;
unsigned long lastHeartbeat = 0;
unsigned long heartBeatTimeout = 5000;  // 5 seconds timeout

// more config variables
String padelName, courtNr;

void setup() {
  // Serial Begin
  Serial.begin(115200);

  // watchdog
  esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000, // Convert to milliseconds
        .idle_core_mask = 0,              // Apply to all CPU cores
        .trigger_panic = false            // Just reset ESP, no panic dump
  };
  esp_task_wdt_init(&wdt_config);  // Initialize watchdog
  esp_task_wdt_add(NULL);          // Attach watchdog to the main loop

  // Disable sleep mode
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  WiFi.setSleep(false);  // Disable Wi-Fi power-saving mode

  
  // IOs
  pinMode(STATUS_LED_BUILTIN, OUTPUT);
  digitalWrite(STATUS_LED_BUILTIN, LOW);
  pinMode(PUSHBUTTON, INPUT_PULLUP);
  pinMode(TOGGLELED, OUTPUT);

  // Start LittleFS To Read Config.json and git_key.json
  if (!FILESYSTEM.begin(true)) {
      Serial.println("Failed to mount file system");
      return;
  }

  // Load Config.json
  if (!loadConfig()) {
      Serial.println("Failed to load config file");
      return;
  }
  extractPadelNameAndCourtNr();

  // Load Git Key 
  if (!loadGitKey()) {
      Serial.println("Failed to load GitHub key");
      return;
  }

  // Wifi and NTP Client
  connectToWiFi();
  timeClient.begin();
  timeClient.update();
  validateAndSyncTime();
  client.setInsecure();
  
  // Start UDP
  udp.begin(udpPort);

  // Start heartbeat
  //udp.begin(heartBeatPort);
  //Serial.print("📡 Listening for UDP broadcast on port ");
  //Serial.println(heartBeatPort);

  // Subscription
  checkSubscription(); // Initial check
}

void loop() {

  //bool CheckedForUpdate = false;

  // watchdog reset
  esp_task_wdt_reset();

  // Wifi & NTP Client
  timeClient.update();
  validateAndSyncTime();
  checkWiFiConnection();

  // heartbeat
  /*static unsigned long lastHeartbeatCheck = 0;
  if (millis() - lastHeartbeatCheck >= 50) {  // Check every 50ms
    lastHeartbeatCheck = millis();
    HeartBeat();  // Call HeartBeat more frequently
  }*/

  // Toggle led logic
  if ((millis() - lastToggletime) > 600000 ) { // every 10 minutes
        outputState = !outputState;
        digitalWrite(TOGGLELED, outputState);
        Serial.println(outputState ? "Output ON" : "Output OFF");
        lastToggletime = millis();
    }
    
  // button handler logic
  int buttonState = digitalRead(PUSHBUTTON);
  if (buttonState == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      buttonPressTime = millis();
    }
  } else if (buttonPressed) {
    unsigned long pressDuration = millis() - buttonPressTime;
    buttonPressed = false;

    if (pressDuration >= UPDATEDURATION) {
      Serial.println("🔄 Very Long press detected: Initiating OTA update...");
      checkForFirmwareUpdate(MANUAL_UPDATE);
      //CheckedForUpdate = true;
    } else if (pressDuration >= REBOOTDURATION) {
      Serial.println("🔄 Long press detected: Rebooting ESP32...");
      esp_restart();
    } else {
      if ((millis() - lastTimestampSent >= cooldownPeriod) && !buttonPressed) {
        Serial.println("⏳ Sending timestamp...");
        sendTimestampUDP(getCurrentDateTime());
        lastTimestampSent = millis();
      } else {
        Serial.println("⚠️ Cooldown active, ignoring...");
      }
    }
  }

  // OTA Update
  if (timeClient.getHours() == updHour && timeClient.getMinutes() == updMinute) {
    Serial.println("Checking for firmware and config updates due to Time Chosen in Config...");
    checkForFirmwareUpdate(TIME_UPDATE);
	  //CheckedForUpdate = true;
  }
  //if (CheckedForUpdate) {
    //delay(60000);
    //CheckedForUpdate = false;
  //}

  // Subscription continuous Check
  if (subscription_active){
    if (millis() - lastCheck > DAILYCHECK) {
      checkSubscription();
      lastCheck = millis();
    }
  }else{
    if (millis() - lastCheck > MINUTECHECK) {
      checkSubscription();
      lastCheck = millis();
    }
  }
  
}

void checkSubscription() {
  Serial.println("Checking subscription...");

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String requestUrl = String(google_sheets_url) + "?device_id=" + boardID;
    
    //Serial.println("Request URL: " + requestUrl); // Debugging: print full URL
    
    http.begin(requestUrl);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Handle Google redirects

    int httpResponseCode = http.GET();
    
    //Serial.println("HTTP Response Code: " + String(httpResponseCode)); // Print response code

    if (httpResponseCode > 0) {
      String payload = http.getString();
      Serial.println("Response: " + payload); // Print full response
      
      if (payload.indexOf("\"active\":false") != -1) {
        subscription_active = false;
        Serial.println("❌ Subscription expired! Stopping UDP...");
        digitalWrite(STATUS_LED_BUILTIN, LOW); 
      } else {
        subscription_active = true;
        Serial.println("✅ Subscription active! Continuing UDP...");
        digitalWrite(STATUS_LED_BUILTIN, HIGH); 
      }
    } /*else {
      Serial.println("⚠️ HTTP Request Failed, Error: " + String(httpResponseCode));
    }*/

    http.end();
  } else {
    subscription_active = false;
    return;
  }
}



/** Sends the timestamp via UDP (broadcast) */
void sendTimestampUDP(String timestamp) {
  if (subscription_active) {
    // check if wifi is connected if not save the timestamp to send it when we connect again
    Serial.println("Broadcasting timestamp: " + timestamp);
    
    for (int i = 0; i < 3; i++) {
      udp.beginPacket(broadcastIP, udpPort);
      udp.print(timestamp);
      udp.endPacket();
      unsigned long start = millis();
      while (millis() - start < 350); // Non-blocking wait
    }
  }
  else
  {
    Serial.println("⛔ Subscription inactive. Not sending UDP.");
  }
}

/** Load Configuration File */
bool loadConfig() {
  File file = FILESYSTEM.open(configFilePath, "r");
  if (!file) return false;

  StaticJsonDocument<256> jsonDoc;
  if (deserializeJson(jsonDoc, file)) return false;

  ssid = jsonDoc["ssid"].as<String>();
  password = jsonDoc["password"].as<String>();
  boardID = jsonDoc["boardID"].as<String>();
  udpPort = jsonDoc["udpPort"].as<int>();
  heartBeatPort = jsonDoc["heartBeatPort"].as<int>();
  heartBeatTimeout = jsonDoc["heartBeatTimeout"].as<int>();
  updHour = jsonDoc["updHour"].as<int>();
  updMinute = jsonDoc["updMinute"].as<int>();
  cooldownPeriod = jsonDoc["cooldownPeriod"].as<long>();
  file.close();
  return true;
}

/** Connect to WiFi */
void connectToWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {  // Limit retries
      delay(1000);
      Serial.print(".");
      attempts++;
      esp_task_wdt_reset();
  }

  if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ Wi-Fi connected! IP: " + WiFi.localIP().toString());
  } else {
      Serial.println("❌ Wi-Fi failed! Restarting ESP32...");
      delay(100);
      esp_restart();  // Safe reset
  }
}


/** Fetch current date and time */
String getCurrentDateTime() {
    time_t now = timeClient.getEpochTime();
    struct tm* timeinfo = localtime(&now);
    char buffer[25];
    sprintf(buffer, "%04d-%02d-%02d_%02d-%02d-%02d", timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    return String(buffer);
}

/** OTA Update Logic */
void checkForFirmwareUpdate(int updReason) {
  String updateUrl = "https://raw.githubusercontent.com/jareyeshurtado/esp32-ota-updates/main/Padel_Button_Manager.ino.bin";
  String configUrl = "https://raw.githubusercontent.com/jareyeshurtado/esp32-ota-updates/main/" + padelName + "/config_" + courtNr + ".json";
                     
  digitalWrite(STATUS_LED_BUILTIN, LOW);
  Serial.println("Logging firmware update attempt...");

  // Log the firmware update first
  if (!logUpdateStatus("Firmware", true, updReason)) {
    Serial.println("Failed to log firmware update status. Aborting OTA update.");
    return;  // Avoid OTA if logging fails
  }

  // Handle the configuration update
  if (http.begin(client, configUrl)) {
    int httpResponseCode = http.GET();
    if (httpResponseCode == 200) {
      String newConfig = http.getString();
      File configFile = FILESYSTEM.open(configFilePath, "w");
      if (configFile) {
        configFile.print(newConfig);
        configFile.close();
        Serial.println("Config updated!");
        logUpdateStatus("Config", false, updReason);
      }
    }
    http.end();
  }

  // Perform OTA Update last
  Serial.println("Starting firmware update...");
  t_httpUpdate_return result = httpUpdate.update(client, updateUrl);
  if (result != HTTP_UPDATE_OK) {
      Serial.println("⚠️ OTA failed, rebooting in 10s...");
      delay(10000);
      esp_restart();  // Safe reboot
  }

  Serial.println("Firmware update complete. Synchronizing NTP...");
  timeClient.end();
  timeClient.begin();
  timeClient.update();
}

/** Synchronize Time */
void validateAndSyncTime() {
  time_t now = timeClient.getEpochTime();
  if (now < 946684800 || now > 1893456000) {  // Check if time is outside 2000 to 2030 range
    Serial.println("Invalid time detected, forcing NTP sync...");
    timeClient.update();  // Force sync
  }
}

bool logUpdateStatus(const String& updateType, bool logTime, int updReason) {
  String currentTime = getCurrentDateTime();
  String shaValue;
  String updReasonStr;
  String date;

  if (updReason == MANUAL_UPDATE) {
    updReasonStr = "Manual Update"; 
  } else if (updReason == TIME_UPDATE) {
    updReasonStr = "Time Based Update"; 
  } else {
    updReasonStr = "Unknown Reason"; 
  }

  String entry = logTime ? "update @ " + currentTime + " Reason: " + updReasonStr + "\n" : " ";
  entry += "   " + updateType + ": " + "Success" + "\n";

  int spaceIndex = currentTime.indexOf(' ');
  date = (spaceIndex != -1) ? currentTime.substring(0, spaceIndex) : "Unknown";

  bool fileExists = false;
  String existingContent;

  if (http.begin(client, gitLogUrl + padelName + "/upd_" + courtNr + "_" + date + ".log")) {
    http.addHeader("Authorization", "token " + String(gitToken));
    int getResponseCode = http.GET();

    if (getResponseCode == 200) {
      String response = http.getString();
      StaticJsonDocument<1024> jsonResponse;
      deserializeJson(jsonResponse, response);
      shaValue = jsonResponse["sha"].as<String>();
      String encodedContent = jsonResponse["content"].as<String>();

      size_t decodedLen = 0;
      unsigned char* decodedOutput = (unsigned char*)malloc(4096);
      if (!decodedOutput) {
          Serial.println("Memory allocation failed!");
          return false;
      }
      if (mbedtls_base64_decode(decodedOutput, 4096, &decodedLen,
                              (const unsigned char*)encodedContent.c_str(), 
                              encodedContent.length()) != 0) {
          free(decodedOutput);
          Serial.println("Base64 decoding failed!");
          return false;
      }
      free(decodedOutput); // Ensure memory is released

      existingContent = String((char*)decodedOutput, decodedLen);
      free(decodedOutput);  // Free memory
      fileExists = true;
      http.end();
    } else if (getResponseCode == 404) {
      Serial.println("Log file does not exist. Creating a new one.");
      fileExists = false;
      http.end();
    } else {
      Serial.println("Failed to get file SHA: " + String(getResponseCode));
      http.end();
      return false;
    }
  }

  existingContent.trim();
  existingContent += "\n" + entry;

  size_t encodedLen = 0;
  unsigned char* encodedOutput = (unsigned char*)malloc(8192);  // Heap allocation
  if (encodedOutput == nullptr) {
    Serial.println("Memory allocation failed!");
    return false;
  }

  mbedtls_base64_encode(encodedOutput, 8192, &encodedLen,
                        (const unsigned char*)existingContent.c_str(), existingContent.length());
  String encodedContent = String((char*)encodedOutput, encodedLen);
  free(encodedOutput);  // Free memory

  if (http.begin(client, gitLogUrl + padelName + "/upd_" + courtNr + "_" + date + ".log")) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "token " + String(gitToken));

    StaticJsonDocument<1024> updatePayload;
    updatePayload["message"] = "ESP32 update log";
    updatePayload["content"] = encodedContent;
    if (fileExists) {
      updatePayload["sha"] = shaValue;
    }

    String requestBody;
    serializeJson(updatePayload, requestBody);

    int putResponseCode = http.PUT(requestBody);

    if (putResponseCode == 200 || putResponseCode == 201) {
      Serial.println("Log updated successfully.");
      http.end();
      return true;
    } else {
      Serial.println("Failed to update log: " + String(putResponseCode));
      Serial.println("Response: " + http.getString());
      http.end();
      return false;
    }
  }

  return false;
}

/*String base64Encode(const String& input) {
  size_t encodedLen = (input.length() + 2) / 3 * 4;
  unsigned char encodedOutput[encodedLen + 1]; // Ensure space for null-terminator
  size_t writtenLen = 0;

  mbedtls_base64_encode(encodedOutput, sizeof(encodedOutput), &writtenLen,
                        (const unsigned char*)input.c_str(), input.length());

  encodedOutput[writtenLen] = '\0';  // Null-terminate the string
  return String((char*)encodedOutput);
}*/

bool loadGitKey() {
    File file = FILESYSTEM.open(gitKeyFilePath, "r");
    if (!file) return false;

    StaticJsonDocument<128> jsonDoc;
    if (deserializeJson(jsonDoc, file)) return false;

    gitToken = jsonDoc["githubToken"].as<String>();
    file.close();
    return true;
}

/** Extracts Padel Name & Court Number */
void extractPadelNameAndCourtNr() {
    int underscoreIndex = boardID.indexOf('_');
    if (underscoreIndex != -1) {
        padelName = boardID.substring(0, underscoreIndex);
        courtNr = boardID.substring(underscoreIndex + 1);
    } else {
        padelName = "default";
        courtNr = "default";
    }
}

void checkWiFiConnection() {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 5000) {  // Check every 5 sec
        lastCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("⚠️ Wi-Fi lost! Reconnecting...");
            WiFi.disconnect();
            WiFi.reconnect();
        }
    }
}
/*
void HeartBeat() {
  while (udp.parsePacket()) {  // Read ALL available packets
    char packet[10];  // Buffer for received data
    int len = udp.read(packet, sizeof(packet) - 1);
    if (len > 0) {
      packet[len] = '\0';  // Null-terminate the string
    }

    Serial.print("📩 Received: ");
    Serial.println(packet);

    if (strcmp(packet, "alive") == 0) {
      digitalWrite(STATUS_LED_BUILTIN, HIGH);  // Turn LED on
      lastHeartbeat = millis();  // Update last received time
    }
  }

  // Check if the heartbeat has timed out
  if (millis() - lastHeartbeat > heartBeatTimeout) {
    digitalWrite(STATUS_LED_BUILTIN, LOW);  // Turn LED off
  }
}*/