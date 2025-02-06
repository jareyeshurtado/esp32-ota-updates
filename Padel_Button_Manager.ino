#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPUpdate.h>
#include "mbedtls/base64.h"

#define FILESYSTEM LittleFS
#define FIRMWAREVERSION "v0.3"

const char* configFilePath = "/config.json";  // Path for the configuration file
const char* gitLogUrl = "https://api.github.com/repos/jareyeshurtado/esp32-ota-updates/contents/";
const char* gitKeyFilePath = "/git_key.json";  // Path for the GitHub key file
String gitToken;  // Store the GitHub key

#define STATUS_LED_BUILTIN 2  // Status LED Pin (using built-in LED for now)

HTTPClient http;
WiFiClientSecure client;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -6 * 3600);

const int timestampButton = 13;
const int updateButton = 12;
bool buttonPressed = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 5000;

String ssid, password, boardID, postUrl;
String padelName, courtNr;

void setup() {
  Serial.begin(115200);

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
  extractPadelNameAndCourtNr();
  timeClient.begin();
  client.setInsecure();
}

void loop() {
  static int lastDayChecked = -1;
  static bool initialSyncDone = false;
  timeClient.update();
  int currentDay = timeClient.getDay();
  bool CheckedForUpdate = false;

  int timestampButtonState = digitalRead(timestampButton);
  int updateButtonState = digitalRead(updateButton);

  if (!initialSyncDone && timeClient.getEpochTime() > 0) {
    lastDayChecked = currentDay;
    initialSyncDone = true;
    Serial.println("Initial time sync complete.");
  }

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
      checkForFirmwareUpdate();
      lastDayChecked = currentDay;
      CheckedForUpdate = true;
    }
  } else if (updateButtonState == HIGH) {
    buttonPressed = false;
  }

  if (currentDay != lastDayChecked && timeClient.getHours() == 0) {
    Serial.println("Checking for firmware and config updates...");
    checkForFirmwareUpdate();
    lastDayChecked = currentDay;
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

  Serial.println("Config loaded:");
  Serial.println("SSID: " + ssid);
  Serial.println("Board ID: " + boardID);
  Serial.println("Post URL: " + postUrl);

  file.close();
  return true;
}

void connectToWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(ssid.c_str(), password.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nConnected to Wi-Fi");
  digitalWrite(STATUS_LED_BUILTIN, HIGH);  // Indicate board is ready
}

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
}

void checkForFirmwareUpdate() {
  String updateUrl = "https://raw.githubusercontent.com/jareyeshurtado/esp32-ota-updates/main/Padel_Button_Manager.ino.bin";
  String configUrl = "https://raw.githubusercontent.com/jareyeshurtado/esp32-ota-updates/main/" + padelName + "/config_" + courtNr + ".json";
                     
  digitalWrite(STATUS_LED_BUILTIN, LOW);
  Serial.println("Logging firmware update attempt...");

  // Log the firmware update first
  if (!logUpdateStatus("Firmware", true)) {
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
        logUpdateStatus("Config", true);
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
}

bool logUpdateStatus(const String& updateType, bool success) {
  String currentTime = getCurrentDateTime();
  String logPayload;
  String shaValue;
//  String folderName, logFileName;

//  int underscoreIndex = boardID.indexOf('_');
//  if (underscoreIndex != -1) {
//    folderName = boardID.substring(0, underscoreIndex);
//    logFileName = "update_log_" + boardID.substring(underscoreIndex + 1) + ".txt";
//  } else {
//    folderName = "default";
//    logFileName = "update_log.txt";
//  }

  String entry = "update @ " + currentTime + "\n";
  entry += updateType + ": " + (success ? "Success" : "Failed") + "\n";

  bool fileExists = false;
  String existingContent;

  if (http.begin(client, gitLogUrl + padelName + "/updlog_" + courtNr + ".log")) {
    http.addHeader("Authorization", "token " + String(gitToken));
    int getResponseCode = http.GET();

    if (getResponseCode == 200) {
      String response = http.getString();
      StaticJsonDocument<512> jsonResponse;
      deserializeJson(jsonResponse, response);
      shaValue = jsonResponse["sha"].as<String>();
      String encodedContent = jsonResponse["content"].as<String>();
      size_t decodedLen;
      unsigned char decodedOutput[512];
      mbedtls_base64_decode(decodedOutput, sizeof(decodedOutput), &decodedLen, (const unsigned char*)encodedContent.c_str(), encodedContent.length());
      existingContent = String((char*)decodedOutput, decodedLen);
      fileExists = true;
      http.end();  // Close GET request
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

  if (fileExists) {
    existingContent.trim();
    existingContent += "\n" + entry;
  } else {
    existingContent = entry;
  }

  if (http.begin(client, gitLogUrl + padelName + "/updlog_" + courtNr + ".log")) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "token " + String(gitToken));

    StaticJsonDocument<512> updatePayload;
    updatePayload["message"] = "ESP32 update log";
    updatePayload["content"] = base64Encode(existingContent);
    if (fileExists) {
      updatePayload["sha"] = shaValue;
    }

    String requestBody;
    serializeJson(updatePayload, requestBody);

  //  Serial.println("JSON Payload to GitHub:");
  //  Serial.println(requestBody);
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
  sprintf(buffer, "%02d/%02d/%04d %02d:%02d:%02d", timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
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
