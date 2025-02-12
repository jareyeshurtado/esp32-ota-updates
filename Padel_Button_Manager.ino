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

#define FILESYSTEM LittleFS
#define FIRMWAREVERSION "v0.5"

#define MANUAL_UPDATE 0
#define TIME_UPDATE   1

const char* configFilePath = "/config.json";  // Path for the configuration file
const char* gitLogUrl = "https://api.github.com/repos/jareyeshurtado/esp32-ota-updates/contents/";
const char* gitKeyFilePath = "/git_key.json";  // Path for the GitHub key file
String gitToken;  // Store the GitHub key

// Static IP configuration
// TODO: Add the IP Address into the config.json file
IPAddress local_IP(192, 168, 100, 50); // Set your desired IP 
IPAddress gateway(192, 168, 100, 1);    // Router IP (usually .1)
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);  // Google DNS
IPAddress secondaryDNS(8, 8, 4, 4); // Google DNS

#define STATUS_LED_BUILTIN 2  // Status LED Pin (using built-in LED for now)

WebServer server(80);
HTTPClient http;
WiFiClientSecure client;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -6 * 3600);

const int timestampButton = 13;
const int updateButton = 12;
const unsigned long debounceDelay = 5000; // REPLACE HERE FOR THE DELAY TO HAVE BETWEEN POINTS SAVED
bool buttonPressed = false;
unsigned long lastDebounceTime = 0;


String logData = "";  // Stores all timestamps

String ssid, password, boardID, postUrl;
int updHour, updMinute;
String padelName, courtNr;

void setup() {
  Serial.begin(115200);
  Serial.println(String("..... Starting Software : ") + FIRMWAREVERSION);
  pinMode(STATUS_LED_BUILTIN, OUTPUT);  // Set status LED as output
  digitalWrite(STATUS_LED_BUILTIN, LOW);  // Default to LOW (not ready)
  pinMode(timestampButton, INPUT_PULLUP);
  pinMode(updateButton, INPUT_PULLUP);

  if (!FILESYSTEM.begin(true)) {
    Serial.println("Failed to mount file system");
    return;
  }

  if (!loadConfig()) {
    Serial.println("Failed to load config file");
    return;
  }

  if (!loadGitKey()) {
    Serial.println("Failed to load GitHub key");
    return;
  }

  connectToWiFi();
  timeClient.begin();
  timeClient.update();  // Initial sync

  // Define endpoints
  server.on("/timestamps.log", handleFileRequest);  // Get timestamps
  server.on("/clear", handleClearLog);        // Clear log
  server.begin();

  validateAndSyncTime();  // Ensure valid time at startup
  extractPadelNameAndCourtNr();
  client.setInsecure();
}

void loop() {
  timeClient.update();
  validateAndSyncTime();  // Check and correct invalid time
  server.handleClient();
  bool CheckedForUpdate = false;

  int timestampButtonState = digitalRead(timestampButton);
  int updateButtonState = digitalRead(updateButton);


  if (timestampButtonState == LOW && (millis() - lastDebounceTime) > debounceDelay) {
    lastDebounceTime = millis();
    if (!buttonPressed) {
      buttonPressed = true;
      sendPostRequest();
    }
  } else if (timestampButtonState == HIGH) {
    buttonPressed = false;
  }

  if (updateButtonState == LOW && (millis() - lastDebounceTime) > debounceDelay) {
    lastDebounceTime = millis();
    if (!buttonPressed) {
      buttonPressed = true;
      Serial.println("Checking for firmware and config updates Due To Button Pressed...");
      checkForFirmwareUpdate(MANUAL_UPDATE);
      CheckedForUpdate = true;
    }
  } else if (updateButtonState == HIGH) {
    buttonPressed = false;
  }

  if (timeClient.getHours() == updHour && timeClient.getMinutes() == updMinute) {
    Serial.println("Checking for firmware and config updates due to Time Chosen in Config...");
    checkForFirmwareUpdate(TIME_UPDATE);
    CheckedForUpdate = true;
  }
  if (CheckedForUpdate) {
    delay(60000);
    CheckedForUpdate = false;
  }
}

bool loadConfig() {
  File file = FILESYSTEM.open(configFilePath, "r");
  if (!file) {
    Serial.println("Failed to open config file");
    return false;
  }

  StaticJsonDocument<256> jsonDoc;
  DeserializationError error = deserializeJson(jsonDoc, file);
  if (error) {
    Serial.println("Failed to parse config file");
    return false;
  }

  ssid = jsonDoc["ssid"].as<String>();
  password = jsonDoc["password"].as<String>();
  boardID = jsonDoc["boardID"].as<String>();
  postUrl = jsonDoc["postUrl"].as<String>();
  updHour = jsonDoc["updHour"].as<int>();
  updMinute = jsonDoc["updMinute"].as<int>();

  Serial.println("Config loaded:");
  Serial.println("SSID: " + ssid);
  Serial.println("Board ID: " + boardID);
  Serial.println("Post URL: " + postUrl);
  Serial.print("update daily time ");
  Serial.print(updHour);
  Serial.print(":");
  Serial.println(updMinute);

  file.close();
  return true;
}

void connectToWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(ssid.c_str(), password.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
  digitalWrite(STATUS_LED_BUILTIN, HIGH);  // Indicate board is ready
}

// TODO: Check if the SendPostRequest is still needed or not
void sendPostRequest() {
  String currentTime = getCurrentDateTime();
  if (WiFi.status() == WL_CONNECTED) {
    http.begin(client, postUrl);
    http.addHeader("Content-Type", "application/json");

    String jsonPayload = "{\"boardID\":\"" + boardID + "\", \"tmp\":\"" + currentTime + "\", \"Version\":\"" + FIRMWAREVERSION + "\"}";
    Serial.println("Sending POST request with payload: " + jsonPayload);

    int httpResponseCode = http.POST(jsonPayload);
    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.println("Response Code: " + String(httpResponseCode));
      Serial.println("Response Body: " + response);
    } else {
      Serial.println("POST error: " + String(httpResponseCode));
    }

    http.end();
  } else {
    Serial.println("Wi-Fi not connected");
  }
  String formattedTime = reformatDateTime(currentTime);
  // Log timestamp locally
  logTimestamp(formattedTime);
}

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
    Serial.println("Firmware update failed: " + String(httpUpdate.getLastErrorString()));
  }

  Serial.println("Firmware update complete. Synchronizing NTP...");
  timeClient.end();  // Stop any previous NTP activity
  timeClient.begin();  // Start fresh
  timeClient.update();  // Force immediate sync
}

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
      unsigned char* decodedOutput = (unsigned char*)malloc(4096);  // Heap allocation
      if (decodedOutput == nullptr) {
        Serial.println("Memory allocation failed!");
        return false;
      }

      mbedtls_base64_decode(decodedOutput, 4096, &decodedLen,
                            (const unsigned char*)encodedContent.c_str(), encodedContent.length());
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

String base64Encode(const String& input) {
  size_t encodedLen = (input.length() + 2) / 3 * 4;
  unsigned char encodedOutput[encodedLen + 1]; // Ensure space for null-terminator
  size_t writtenLen = 0;

  mbedtls_base64_encode(encodedOutput, sizeof(encodedOutput), &writtenLen,
                        (const unsigned char*)input.c_str(), input.length());

  encodedOutput[writtenLen] = '\0';  // Null-terminate the string
  return String((char*)encodedOutput);
}

String getCurrentDateTime() {
  time_t now = timeClient.getEpochTime();
  struct tm* timeinfo = localtime(&now);
  char buffer[25];
  sprintf(buffer, "%02d-%02d-%04d %02d:%02d:%02d", timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  return String(buffer);
}

bool loadGitKey() {
  File file = FILESYSTEM.open(gitKeyFilePath, "r");
  if (!file) {
    Serial.println("Failed to open GitHub key file");
    return false;
  }

  StaticJsonDocument<128> jsonDoc;
  DeserializationError error = deserializeJson(jsonDoc, file);
  if (error) {
    Serial.println("Failed to parse GitHub key file");
    return false;
  }

  gitToken = jsonDoc["githubToken"].as<String>();
  Serial.println("GitHub token loaded successfully");

  file.close();
  return true;
}

void extractPadelNameAndCourtNr(){
  // Extract Padel Name and Court Nr from boardID file
  int underscoreIndex = boardID.indexOf('_');
  if (underscoreIndex != -1) {
    padelName = boardID.substring(0, underscoreIndex);
    courtNr = boardID.substring(underscoreIndex + 1);
  } else {
    padelName = "default";
    courtNr = "default";
  }
}

// CODE FOR SENDING LOG DATA 
void handleFileRequest() {
    server.send(200, "text/plain", logData);  // Send all logs
}

void handleClearLog() {
    logData = "";  // Clear the log
    server.send(200, "text/plain", "Log cleared!");
}

void logTimestamp(const String& timestamp) {
    //String timestamp = getCurrentDateTime();
    logData += timestamp + "\n";  // Append timestamp to log
    Serial.println("Logged: " + timestamp);
}

String reformatDateTime(String dt) {
    // Extract day, month, year, hour, minutes, and seconds
    String day = dt.substring(0, 2);
    String month = dt.substring(3, 5);
    String year = dt.substring(6, 10);
    String hour = dt.substring(11, 13);
    String minute = dt.substring(14, 16);
    String second = dt.substring(17, 19);

    // Construct the new format: Year-Month-Day_Hour-Minutes-Seconds
    String formattedTime = year + "-" + month + "-" + day + "_" + hour + "-" + minute + "-" + second;
    
    return formattedTime;
}