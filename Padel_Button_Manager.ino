#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPUpdate.h>
#include "mbedtls/base64.h"
#include <esp_task_wdt.h>

// Comment or Uncomment this for production or testing
//#define DEBUG
// Comment or Uncomment this for production or testing

#ifdef DEBUG
  #define DEBUG_BEGIN(x) Serial.begin(x);
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
#else
  #define DEBUG_BEGIN(x)
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

// General
#define FILESYSTEM LittleFS

// Watchdog
#define WDT_TIMEOUT 30

// OTA updates
#define MANUAL_UPDATE 0
#define TIME_UPDATE   1
const char* configFilePath = "/config.json";
const char* gitLogUrl = "https://api.github.com/repos/jareyeshurtado/esp32-ota-updates/contents/";
const char* gitKeyFilePath = "/git_key.json";
String gitToken;

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
const char* broadcastDemoIP = "192.168.100.255"; // UDP broadcast
int udpPort;

// Wifi & NTP Logic
WiFiUDP ntpUDP;
HTTPClient http;
WiFiClientSecure client;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -6 * 3600);
String ssid, password, boardID;

// more config variables
String padelName, courtNr;

// Demo Variables
int isDemo = 0;

void setup() {
  // Serial Begin
  DEBUG_BEGIN(115200);

  // Kill any watchdog previously active
  esp_task_wdt_delete(NULL);   // Remove current task from watchdog
  esp_task_wdt_deinit();       // Completely stop watchdog
  
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
      DEBUG_PRINTLN("Failed to mount file system");
      return;
  }

  // Load Config.json
  if (!loadConfig()) {
      DEBUG_PRINTLN("Failed to load config file");
      return;
  }
  extractPadelNameAndCourtNr();

  // Load Git Key 
  if (!loadGitKey()) {
      DEBUG_PRINTLN("Failed to load GitHub key");
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

  DEBUG_PRINTLN(ESP.getSdkVersion());
  DEBUG_PRINTLN(ESP.getCoreVersion());

  // Subscription
  checkSubscription(); // Initial check
}

void loop() {

  //bool CheckedForUpdate = false;

  // watchdog reset
  esp_task_wdt_reset();  // Feed the dog!

  // Wifi & NTP Client
  timeClient.update();
  validateAndSyncTime();
  checkWiFiConnection();

  // Toggle led logic
  if ((millis() - lastToggletime) > 10*MINUTECHECK ) { // every 10 minutes
        outputState = !outputState;
        digitalWrite(TOGGLELED, outputState);
        DEBUG_PRINTLN(outputState ? "Output ON" : "Output OFF");
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
      DEBUG_PRINTLN("🔄 Very Long press detected: Initiating OTA update...");
      checkForFirmwareUpdate(MANUAL_UPDATE);
      //CheckedForUpdate = true;
    } else if (pressDuration >= REBOOTDURATION) {
      DEBUG_PRINTLN("🔄 Long press detected: Rebooting ESP32...");
      esp_restart();
    } else {
      if ((millis() - lastTimestampSent >= cooldownPeriod) && !buttonPressed) {
        DEBUG_PRINTLN("⏳ Sending timestamp...");
        sendTimestampUDP(getCurrentDateTime());
        lastTimestampSent = millis();
      } else {
        DEBUG_PRINTLN("⚠️ Cooldown active, ignoring...");
      }
    }
  }

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
  DEBUG_PRINTLN("Checking subscription...");

  if (WiFi.status() == WL_CONNECTED) {
    const String requestUrl = String(google_sheets_url) + "?device_id=" + boardID;
    
    http.begin(requestUrl);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Handle Google redirects

    esp_task_wdt_reset();

    int httpResponseCode = http.GET();
    if (httpResponseCode > 0) {
      String payload = http.getString();
      DEBUG_PRINTLN("Response: " + payload); // Print full response
      
      if (payload.indexOf("\"active\":false") != -1) {
        subscription_active = false;
        DEBUG_PRINTLN("❌ Subscription expired! Stopping UDP...");
        digitalWrite(STATUS_LED_BUILTIN, LOW); 
      } else {
        subscription_active = true;
        DEBUG_PRINTLN("✅ Subscription active! Continuing UDP...");
        digitalWrite(STATUS_LED_BUILTIN, HIGH); 
      }
    } else {
      DEBUG_PRINTLN("⚠️ HTTP Request Failed, Error: " + String(httpResponseCode));
    }

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
    DEBUG_PRINTLN("Broadcasting timestamp: " + timestamp);
    
    for (int i = 0; i < 3; i++) {
      isDemo ? udp.beginPacket(broadcastDemoIP, udpPort) :udp.beginPacket(broadcastIP, udpPort);
      udp.print(timestamp);
      udp.endPacket();
      unsigned long start = millis();
      while (millis() - start < 350); // Non-blocking wait
    }
  }
  else
  {
    DEBUG_PRINTLN("⛔ Subscription inactive. Not sending UDP.");
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
  cooldownPeriod = jsonDoc["cooldownPeriod"].as<long>();
  isDemo = jsonDoc["isDemo"].as<int>();
  file.close();
  return true;
}

/** Connect to WiFi */
void connectToWiFi() {
  DEBUG_PRINT("Connecting to Wi-Fi");
  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {  // Limit retries
      delay(1000);
      DEBUG_PRINT(".");
      attempts++;
      esp_task_wdt_reset();
  }

  if (WiFi.status() == WL_CONNECTED) {
      DEBUG_PRINTLN("\n✅ Wi-Fi connected! IP: " + WiFi.localIP().toString());
  } else {
      DEBUG_PRINTLN("❌ Wi-Fi failed! Restarting ESP32...");
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
  const String updateUrl = "https://raw.githubusercontent.com/jareyeshurtado/esp32-ota-updates/main/Padel_Button_Manager.ino.bin?t=" + String(millis()); // adding millis to prevent caching
  const String configUrl = "https://raw.githubusercontent.com/jareyeshurtado/esp32-ota-updates/main/" + padelName + "/config_" + courtNr + ".json";
                     
  digitalWrite(STATUS_LED_BUILTIN, LOW);
  DEBUG_PRINTLN("Logging firmware update attempt...");

  // Log the firmware update first
  if (!logUpdateStatus("Firmware", true, updReason)) {
    DEBUG_PRINTLN("Failed to log firmware update status. Aborting OTA update.");
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
        DEBUG_PRINTLN("Config updated!");
        logUpdateStatus("Config", false, updReason);
      }
    }
    http.end();
  }

  // Perform OTA Update last
  DEBUG_PRINTLN("Starting firmware update...");
  esp_task_wdt_reset();  // Feed the dog!
  httpUpdate.onStart([]() {
    DEBUG_PRINTLN("OTA update started...");
  });
  httpUpdate.onEnd([]() {
    DEBUG_PRINTLN("OTA update finished!");
  });
  httpUpdate.onProgress([](int cur, int total) {
    Serial.printf("Progress: %d / %d bytes\n", cur, total);
  });
  httpUpdate.onError([](int err) {
    Serial.printf("OTA Error[%d]: %s\n", err, httpUpdate.getLastErrorString().c_str());
  });
  t_httpUpdate_return result = httpUpdate.update(client, updateUrl);
  if (result != HTTP_UPDATE_OK) {
      DEBUG_PRINTLN("⚠️ OTA failed, rebooting in 10s...");
      Serial.printf("HTTP Update Error: %s\n", http.errorToString(result).c_str());
      delay(10000);
      esp_restart();  // Safe reboot
  }

  DEBUG_PRINTLN("Firmware update complete. Synchronizing NTP...");
  timeClient.end();
  timeClient.begin();
  timeClient.update();
}

/** Synchronize Time */
void validateAndSyncTime() {
  time_t now = timeClient.getEpochTime();
  if (now < 946684800 || now > 1893456000) {  // Check if time is outside 2000 to 2030 range
    DEBUG_PRINTLN("Invalid time detected, forcing NTP sync...");
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

  int spaceIndex = currentTime.indexOf('_');
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
          DEBUG_PRINTLN("Memory allocation failed!");
          return false;
      }
      if (mbedtls_base64_decode(decodedOutput, 4096, &decodedLen,
                              (const unsigned char*)encodedContent.c_str(), 
                              encodedContent.length()) != 0) {
          free(decodedOutput);
          DEBUG_PRINTLN("Base64 decoding failed!");
          return false;
      }

      existingContent = String((char*)decodedOutput, decodedLen);
      free(decodedOutput);  // Free memory
      fileExists = true;
      http.end();
    } else if (getResponseCode == 404) {
      DEBUG_PRINTLN("Log file does not exist. Creating a new one.");
      fileExists = false;
      http.end();
    } else {
      DEBUG_PRINTLN("Failed to get file SHA: " + String(getResponseCode));
      http.end();
      return false;
    }
  }

  existingContent.trim();
  existingContent += "\n" + entry;

  size_t encodedLen = 0;
  unsigned char* encodedOutput = (unsigned char*)malloc(8192);  // Heap allocation
  if (encodedOutput == nullptr) {
    DEBUG_PRINTLN("Memory allocation failed!");
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
      DEBUG_PRINTLN("Log updated successfully.");
      http.end();
      return true;
    } else {
      DEBUG_PRINTLN("Failed to update log: " + String(putResponseCode));
      DEBUG_PRINTLN("Response: " + http.getString());
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
            DEBUG_PRINTLN("⚠️ Wi-Fi lost! Reconnecting...");
            WiFi.disconnect();
            WiFi.reconnect();
        }
    }
}