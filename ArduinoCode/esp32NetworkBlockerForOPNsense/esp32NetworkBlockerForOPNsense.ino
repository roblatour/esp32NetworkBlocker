// Emergency Network Shutdown using OPNsense (version 1)
//
// Copyright Rob Latour, 2023
// https://raltour.com/esp32networkblocker
// https://github.com/roblatour/esp32networkblocker
// License: MIT
//
// Compile and upload using Arduino IDE (2.2.1 or greater)
//
// Physical board:                 LILYGO T-Display-S3
// Board in Arduino board manager: ESP32S3 Dev Module
//
// Arduino Tools settings:
// USB CDC On Boot:                Enabled
// CPU Frequency:                  240MHz
// USB DFU On Boot:                Enabled
// Core Debug Level:               None
// Erase All Flash Before Upload:  Disabled
// Events Run On:                  Core 1
// Flash Mode:                     QIO 80Mhz
// Flash Size:                     16MB (128MB)
// Arduino Runs On:                Core 1
// USB Firmware MSC On Boot:       Disabled
// PSRAM:                          OPI PSRAM
// Partition Scheme:               16 M Flash (3MB APP/9.9MB FATFS)
// USB Mode:                       Hardware CDC and JTAG
// Upload Mode:                    UART0 / Hardware CDC
// Upload Speed:                   921600
// Programmer                      ESPTool

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <TFT_eSPI.h>  // https://github.com/Xinyuan-LilyGO/T-Display-S3/tree/main/lib
#include "general_settings.h"
#include "secret_settings.h"

const char* WiFiSSID = SECRET_SETTINGS_WIFI_SSID;
const char* WiFiPassword = SECRET_SETTINGS_WIFI_PASSWORD;

const char* OPNsenseHostIP = SECRET_OPNSENSE_HOST_IP;

const char* secret = SECRET_SETTINGS_OPNSENSE_API_SECRET;
const char* apiKey = SECRET_SETTINGS_OPNSENSE_API_KEY;

const int disabled = 0;
const int enabled = 1;

// ESP32 LilyGo T-Display S3 Power On Pin (used to ensure the display is on when the device is running on battery)
#define PIN_POWER_ON 15

TFT_eSPI tft = TFT_eSPI();

int displayWidth = 320;

bool USBOnTheRight = GENERAL_SETTINGS_USB_ON_THE_RIGHT;

WiFiClientSecure client;

String DevicesIPAddress;
unsigned long LastTimeWiFiWasConnected;
const unsigned long DeviceResetThreshold = GENERAL_SETTINGS_DEVICE_WILL_RESET_AFTER_THIS_MANY_SECONDS_WITH_NO_WIFI_CONNECTION * 1000;

String productVersion;
String productLatestVersion;
bool productUpdateAvailable;
bool UpdateAvailable;
String systemStatus;
String servicesStatus;
String firewallStatus;
volatile bool networkBlockingIsActive = false;
volatile bool everyThingIsAsItShouldBe = true;

// OPNsense API call to enable (or disable) a rule
bool enableRule(const char* ruleUUID, bool ruleShouldBeEnabled) {

  // return values:
  //  true  : the rule was enabled/disabled as requested
  //  false : the rule was not enabled/disabled as requested

  bool returnValue = false;

  const char* toggleRuleRequest = "/api/firewall/filter/toggleRule/";

  HTTPClient httpClient;

  String url = "https://" + String(OPNsenseHostIP) + String(toggleRuleRequest) + String(ruleUUID) + "/";

  if (ruleShouldBeEnabled)
    url.concat(String(enabled));
  else
    url.concat(String(disabled));

  httpClient.begin(url);
  httpClient.setAuthorization(secret, apiKey);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");
  httpClient.addHeader("Content-Length", "0");

  int statusCode = httpClient.POST("");

  if (statusCode > 0) {

    if (statusCode == HTTP_CODE_OK) {

      String payload = httpClient.getString();
      // Serial.println(payload);

      if (ruleShouldBeEnabled)
        returnValue = (payload.indexOf("\"result\":\"Enabled\"") > 0);
      else
        returnValue = (payload.indexOf("\"result\":\"Disabled\"") > 0);
    };
  };

  httpClient.end();

  return returnValue;
}

// OPNsense API call to apply all rule changes
bool applyAllRules() {

  // return values:
  //  true : rules were applied
  //  false: rules were not applied

   Serial.print("Applying changes ... ");

  bool returnValue = false;
  const char* applyRequest = "/api/firewall/filter/apply";

  HTTPClient httpClient;

  String url = "https://" + String(OPNsenseHostIP) + String(applyRequest);

  httpClient.begin(url);
  httpClient.setAuthorization(secret, apiKey);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");
  httpClient.addHeader("Content-Length", "0");

  int statusCode = httpClient.POST("");

  if (statusCode > 0) {

    if (statusCode == HTTP_CODE_OK) {

      String payload = httpClient.getString();
      // Serial.println(payload);
      if (payload == "{\"status\":\"OK\\n\\n\"}")
        returnValue = true;
    };
  };

  httpClient.end();

  if (returnValue)
    Serial.println("changes applied");
  else
    Serial.println("applying changes failed");

  return returnValue;
}

// OPNsense API call check the status (enabled/disabled) of a rule
int isRuleEnabled(const char* ruleUUID) {

  // return values:
  // -1: could not determine
  //  0: rule is disabled
  //  1: rule is enabled

  int returnValue = -1;

  const char* getRuleRequest = "/api/firewall/filter/getRule/";

  HTTPClient httpClient;

  String url = "https://" + String(OPNsenseHostIP) + String(getRuleRequest) + String(ruleUUID);

  httpClient.begin(url);
  httpClient.setAuthorization(secret, apiKey);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int statusCode = httpClient.GET();

  if (statusCode > 0) {

    if (statusCode == HTTP_CODE_OK) {

      String payload = httpClient.getString();
      // Serial.println(payload);

      if (payload.indexOf("\"rule\":{\"enabled\":\"0\"") > 0)
        returnValue = 0;

      else if (payload.indexOf("\"rule\":{\"enabled\":\"1\"") > 0)
        returnValue = 1;
    };
  };

  httpClient.end();

  return returnValue;
};

// OPNsense API call align all OPNsense Rulse (enabled/disabled) with the status of the Emergency Stop button

bool alignAllOPNsenseRules() {

  // return value
  // true:  all values aligned
  // false: problem allinging values

  bool returnValue = true;
  bool applyRequired = false;

  const char* getAllRulesRequest = "/api/firewall/filter/searchRule/";

  HTTPClient httpClient;

  String url = "https://" + String(OPNsenseHostIP) + String(getAllRulesRequest);

  httpClient.begin(url);
  httpClient.setAuthorization(secret, apiKey);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int statusCode = httpClient.GET();

  if (statusCode == HTTP_CODE_OK) {

    //String payload = httpClient.getString();
    //Serial.println(payload);

    // Parse response
    DynamicJsonDocument doc(32768);
    deserializeJson(doc, httpClient.getStream());

    int numberOfOPNsenseAutomationRules = doc["rowCount"].as<int>();

    int numberOfAPIRules = sizeof(API_RULES) / sizeof(API_RULES[0]);

    Serial.println("");

    if (networkBlockingIsActive)
      Serial.println("Emergency shutdown is enabled");
    else
      Serial.println("Emergency shutdown is disabled");

    Serial.println("Aligning rules");

    Serial.print("Number of OPNsense - Firewall - Filter - Automation rules: ");
    Serial.println(numberOfOPNsenseAutomationRules);

    Serial.print("Number of API rules defined in the secret_settings.h file: ");
    Serial.println(numberOfAPIRules);

    // look up each API rule in the retrieved OPNSense API rules, and if any need to be changed to align to the networkBlockingIsActive flag then do that now
    bool matchFound;

    for (int APIRuleToCheck = 0; APIRuleToCheck < numberOfAPIRules; APIRuleToCheck++) {

      matchFound = false;

      for (int OPNsenseRulesToCheck = 0; OPNsenseRulesToCheck < numberOfOPNsenseAutomationRules; OPNsenseRulesToCheck++) {

        String OPNsenseRuleUUID = doc["rows"][OPNsenseRulesToCheck]["uuid"].as<String>();

        if (String(API_RULES[APIRuleToCheck]) == OPNsenseRuleUUID) {

          Serial.print("rule id: ");
          Serial.print(API_RULES[APIRuleToCheck]);
          Serial.print(" matched, its current status is ");

          matchFound = true;

          if (doc["rows"][OPNsenseRulesToCheck]["enabled"].as<int>() == enabled) {

            Serial.print("enabled; ");

            if (networkBlockingIsActive) {

              Serial.print("no change required");

            } else {

              bool changeSucceeded = enableRule(API_RULES[APIRuleToCheck], false);

              if (changeSucceeded) {
                Serial.print("its status was changed to disabled");
                applyRequired = true;
              } else
                Serial.print("failed to change its status to disabled");

              returnValue = returnValue && changeSucceeded;
            };

          } else {

            Serial.print("disabled; ");

            if (networkBlockingIsActive) {

              bool changeSucceeded = enableRule(API_RULES[APIRuleToCheck], true);

              if (changeSucceeded) {
                Serial.print("its status was changed to enabled");
                applyRequired = true;
              } else
                Serial.print("failed to change its status to enabled");

              returnValue = returnValue && changeSucceeded;

            } else {

              Serial.print("no change required");
            };
          };

          Serial.println("");
          break;
        };
      };

      if (!matchFound) {
        Serial.print("rule id: ");
        Serial.print(String(API_RULES[APIRuleToCheck]));
        Serial.println(" was defined in the secret_settings.h file but not within the OPNsense - Firewall - Filter - Automation rules");
        returnValue = false;  // a rule defined in this program could not be matched to an existing OPNsense rule
      };
    }
  } else {

    Serial.print("Status code: ");
    Serial.print(statusCode);
    Serial.print(" returned from httpClient.GET()");
    returnValue = false;
  };

  httpClient.end();

  if (applyRequired)
    applyAllRules();

  return returnValue;
};


// OPNsense API call to get product info
void getProductInfo() {

  const char* getOPNsenseProductInfoRequest = "/api/core/firmware/status/";

  Serial.println("Getting product info");

  HTTPClient httpClient;

  String url = "https://" + String(OPNsenseHostIP) + String(getOPNsenseProductInfoRequest);

  httpClient.useHTTP10(true);
  httpClient.begin(url);
  httpClient.setAuthorization(secret, apiKey);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int statusCode = httpClient.GET();

  if (statusCode == HTTP_CODE_OK) {

    // Parse response
    DynamicJsonDocument doc(32768);
    deserializeJson(doc, httpClient.getStream());

    productVersion = doc["product"]["product_version"].as<String>();
    productLatestVersion = doc["product"]["product_latest"].as<String>();
    productUpdateAvailable = (doc["status"].as<String>() != "none");

  } else {

    Serial.print("OPNsense API call failed; status code = ");
    Serial.println(statusCode);

    productVersion = "Unavailable";
    productLatestVersion = "Unavailable";
    productUpdateAvailable = false;
  };

  httpClient.end();
}

// OPNsense API call to get product info
void getUpgradeInfo() {

  const char* getOPNsenseProductInfoRequest = "/api/core/firmware/upgradestatus/";

  Serial.println("Getting upgrade status info");

  HTTPClient httpClient;

  String url = "https://" + String(OPNsenseHostIP) + String(getOPNsenseProductInfoRequest);

  String logDetails;

  httpClient.useHTTP10(true);
  httpClient.begin(url);
  httpClient.setAuthorization(secret, apiKey);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int statusCode = httpClient.GET();

  if (statusCode == HTTP_CODE_OK) {

    // Parse response
    DynamicJsonDocument doc(32768);
    deserializeJson(doc, httpClient.getStream());

    logDetails = doc["log"].as<String>();
    UpdateAvailable = ((logDetails.indexOf("Number of packages to be upgraded:") > 0) || (logDetails.indexOf("Number of packages to be reinstalled:") > 0));

  } else {

    Serial.print("OPNsense API call failed; status code = ");
    Serial.println(statusCode);

    UpdateAvailable = false;
  };

  httpClient.end();
}

// OPNsense API call to get System and Firewall status info
void getSystemAndFirewallStatus() {

  const char* getSystemStatusRequest = "/api/core/system/status/";

  Serial.println("Getting system and firwall info");

  HTTPClient httpClient;

  String url = "https://" + String(OPNsenseHostIP) + String(getSystemStatusRequest);

  httpClient.useHTTP10(true);
  httpClient.begin(url);
  httpClient.setAuthorization(secret, apiKey);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int statusCode = httpClient.GET();

  if (statusCode == HTTP_CODE_OK) {

    // Parse response
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, httpClient.getStream());

    systemStatus = doc["System"]["status"].as<String>();

    firewallStatus = doc["Firewall"]["status"].as<String>();

  } else {

    Serial.print("OPNsense API call failed; status code = ");
    Serial.println(statusCode);

    systemStatus = "Unavailable";
    firewallStatus = "Unavailable";
  };

  httpClient.end();
}

// OPNsense API call to get Services status info
void getServicesStatus() {

  const char* getServicesStatusRequest = "/api/core/service/search/";

  Serial.println("Getting service info");

  HTTPClient httpClient;

  String url = "https://" + String(OPNsenseHostIP) + String(getServicesStatusRequest);

  httpClient.useHTTP10(true);
  httpClient.begin(url);
  httpClient.setAuthorization(secret, apiKey);
  httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int statusCode = httpClient.GET();

  if (statusCode == HTTP_CODE_OK) {

    // Parse response
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, httpClient.getStream());

    int runningServices = 0;
    int stoppedServices = 0;
    int totalServices = doc["rowCount"].as<int>();

    for (int i = 0; i <= totalServices; i++) {
      if (doc["rows"][i]["running"].as<int>() == 1)
        runningServices++;
    };
    stoppedServices = totalServices - runningServices;

    servicesStatus = String(runningServices) + " up";
    if (stoppedServices > 0) {
      servicesStatus.concat(" ");
      servicesStatus.concat(String(stoppedServices));
      servicesStatus.concat(" down");
    };

  } else {

    Serial.print("OPNsense API call failed; status code = ");
    Serial.println(statusCode);

    servicesStatus = "Unavailable";
  };

  httpClient.end();
}

void WiFiEvent(WiFiEvent_t event, arduino_event_info_t info) {

  switch (event) {

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:

      Serial.println("");
      Serial.println("Station connected");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:

      Serial.println("");
      Serial.println("Got IP address: ");
      Serial.println(WiFi.localIP());
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:

      Serial.println("");
      Serial.println("Disconnected from station, attempting reconnection");
      WiFi.reconnect();
      break;

    default:
      break;
  }
}

void setupWiFi() {

  bool notyetconnected = true;
  int waitThisManySecondsForAConnection = 5;

  String message;

  Serial.println("Setting up WiFi");

  const int leftBoarder = 0;
  const int horizontalOffset = 20;
  const int thirdHeight = (TFT_WIDTH / 3);

  int attempts = 0;

  tft.fillScreen(TFT_BLACK);

  message = "Connecting to ";
  message.concat(WiFiSSID);

  tft.setTextColor(TFT_WHITE);
  tft.setCursor(leftBoarder, horizontalOffset);
  tft.println(message);

  tft.setTextColor(TFT_LIGHTGREY);

  while (notyetconnected) {

    WiFi.mode(WIFI_STA);
    WiFi.onEvent(WiFiEvent);
    WiFi.begin(SECRET_SETTINGS_WIFI_SSID, SECRET_SETTINGS_WIFI_PASSWORD);
    delay(1000);

    unsigned long startedWaiting = millis();
    unsigned long waitUntil = startedWaiting + (waitThisManySecondsForAConnection * 1000);

    while ((WiFi.status() != WL_CONNECTED) && (millis() < waitUntil)) {
      tft.print(".");
      delay(500);
    };

    if (WiFi.status() == WL_CONNECTED) {

      notyetconnected = false;
      LastTimeWiFiWasConnected = millis();

    } else {

      // make  the next connection attempt cycle to wait five seconds longer than we did the previous attempt
      waitThisManySecondsForAConnection += 5;

      WiFi.disconnect(true);
      delay(1000);

      attempts++;

      // if still not connecting after 15 attempts, restart the esp32
      if (attempts > 5)
        ESP.restart();
    };
  };

  // Setup OTA

  Serial.println("Setting up OTA");

  // Port defaults to 3232
  ArduinoOTA.setPort(3232);

  //Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname(SECRET_SETTINGS_OTA_DEVICE_NAME);

  // No authentication by default
  ArduinoOTA.setPassword(SECRET_SETTINGS_OTA_PASSWORD);

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else  // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();

  Serial.println("Ready");

  String DevicesIPAddress = WiFi.localIP().toString().c_str();

  Serial.print("IP address: ");
  Serial.println(DevicesIPAddress);

  tft.setTextColor(TFT_GREEN);
  tft.setCursor(leftBoarder, thirdHeight + horizontalOffset);
  tft.println("Connected!");

  message = "IP address: ";
  message.concat(DevicesIPAddress);
  tft.setTextColor(TFT_LIGHTGREY);
  tft.setCursor(leftBoarder, thirdHeight * 2 + horizontalOffset);
  tft.setTextColor(TFT_LIGHTGREY);
  tft.println(message);
};

void checkWiFiConnection() {

  // if connection has been out for > Device reset threshold restart

  if (WiFi.status() == WL_CONNECTED) {
    LastTimeWiFiWasConnected = millis();
  } else {
    if ((millis() - LastTimeWiFiWasConnected) > DeviceResetThreshold) {
      ESP.restart();
    }
  };
}

bool getNewOPNsenseStatus() {

  // returns true if any of the OPNsense data has changed

  // to avoid excessive api calls
  // the product information (which is unlikely to change often) is only queried once every 30 mintues,
  // while the system, server, and firewall data is queried every minute

  const int confirmTheProductInformationOnceEveryThisManyMinutes = 30;
  const int confirmTheSystemServiceAndFirewallDataEveryThisManyMinutes = 1;

  static long nextTimeToConfirmTheProductInformationData = 0;
  static long nextTimeToConfirmTheSystemServiceAndFirewallData = 0;

  bool returnValue = false;

  // get udpated product and upgrade info every half hour

  if (millis() > nextTimeToConfirmTheProductInformationData) {

    nextTimeToConfirmTheProductInformationData = millis() + confirmTheProductInformationOnceEveryThisManyMinutes * 60 * 1000;

    String lastProductVersion = productVersion;
    String lastProductLatestVersion = productLatestVersion;
    bool lastProductUpdateAvailable = productUpdateAvailable;
    bool lastUpdateAvailable = UpdateAvailable;

    getProductInfo();
    getUpgradeInfo();

    if ((productVersion != lastProductVersion) || (productLatestVersion != lastProductLatestVersion) || (productUpdateAvailable != lastProductUpdateAvailable) || (UpdateAvailable != lastUpdateAvailable))
      returnValue = true;
  };

  if (millis() > nextTimeToConfirmTheSystemServiceAndFirewallData) {

    // get udpated system, service and firewall data every minute
    nextTimeToConfirmTheSystemServiceAndFirewallData = millis() + confirmTheSystemServiceAndFirewallDataEveryThisManyMinutes * 60 * 1000;

    String lastSystemStatus = systemStatus;
    String lastServicesStatus = servicesStatus;
    String lastFirewallStatus = firewallStatus;

    getSystemAndFirewallStatus();
    getServicesStatus();

    returnValue = (returnValue || (systemStatus != lastSystemStatus) || (servicesStatus != lastServicesStatus) || (firewallStatus != lastFirewallStatus));
  };

  return returnValue;
};

void refreshDispalyWithOpenSenseData() {

  const int leftBoarder = 0;
  const int leftIndent = 116;
  const int horizontalOffset = 8;
  const int lineHeight = (TFT_WIDTH / 5);

  String message;

  // clear the screen
  tft.fillScreen(TFT_BLACK);

  // display product and version info
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(leftBoarder, horizontalOffset);
  tft.println("OPNsense");

  tft.setCursor(leftBoarder + leftIndent, horizontalOffset);

  if (productVersion == "Unavailable")
    tft.setTextColor(TFT_RED);
  else if (productUpdateAvailable || UpdateAvailable)
    tft.setTextColor(TFT_YELLOW);
  else
    tft.setTextColor(TFT_GREEN);

  if (productVersion == "Unavailable")
    message = productVersion;
  else if (productUpdateAvailable)
    message = productVersion + " > " + productLatestVersion;
  else
    message = productVersion;

  tft.println(message);

  // display system status
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(leftBoarder, horizontalOffset + lineHeight);
  tft.println("System");
  if (systemStatus.indexOf("OK") == 0)
    tft.setTextColor(TFT_GREEN);
  else
    tft.setTextColor(TFT_RED);
  tft.setCursor(leftBoarder + leftIndent, horizontalOffset + lineHeight);
  tft.println(systemStatus);

  // display services status
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(leftBoarder, horizontalOffset + lineHeight * 2);
  tft.println("Services");

  if ((servicesStatus.indexOf("down") == -1) && (servicesStatus != "Unavailable"))
    tft.setTextColor(TFT_GREEN);
  else
    tft.setTextColor(TFT_RED);
  tft.setCursor(leftBoarder + leftIndent, horizontalOffset + lineHeight * 2);
  tft.println(servicesStatus);

  // display firewall status
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(leftBoarder, horizontalOffset + lineHeight * 3);
  tft.println("Firewall");
  if (firewallStatus.indexOf("OK") == 0)
    tft.setTextColor(TFT_GREEN);
  else
    tft.setTextColor(TFT_RED);
  tft.setCursor(leftBoarder + leftIndent, horizontalOffset + lineHeight * 3);
  tft.println(firewallStatus);

  // write results to the serial monitor as well
  Serial.println("");
  Serial.println(productVersion);
  Serial.print("Update available: ");
  Serial.println(productUpdateAvailable);
  Serial.println(systemStatus);
  Serial.println(firewallStatus);
  Serial.println(servicesStatus);
}

void refreshNetworkLockDisplay() {

  static long nextTimeToShowNetworkLock = 0;

  static bool flashText = true;

  static int xxx = 0;

  if (millis() > nextTimeToShowNetworkLock) {

    // refresh Network lock display every second
    nextTimeToShowNetworkLock = millis() + 1000;

    const int leftBoarder = 0;
    const int horizontalOffset = 10;
    const int lineHeight = (TFT_WIDTH / 5);

    String blockMessage1;
    String blockMessage2;

    if (everyThingIsAsItShouldBe) {
      blockMessage1 = "   NETWORK BLOCKING ON   ";
      blockMessage2 = " * NETWORK BLOCKING ON * ";
    } else {
      blockMessage1 = "         PROBLEM         ";
      blockMessage2 = "       * PROBLEM *       ";
    };

    int tw1 = (displayWidth - (int)tft.textWidth(blockMessage1)) / 2;
    int tw2 = (displayWidth - (int)tft.textWidth(blockMessage2)) / 2;
    int th = horizontalOffset + lineHeight * 4;

    if (networkBlockingIsActive) {

      if (flashText) {
        tft.setTextColor(TFT_BLACK);  // clear block message 2
        tft.setCursor(tw2, th);
        tft.println(blockMessage2);
        tft.setTextColor(TFT_RED);  // print block message 1
        tft.setCursor(tw1, th);
        tft.println(blockMessage1);
      } else {
        tft.setTextColor(TFT_BLACK);  // clear block message 1
        tft.setCursor(tw1, th);
        tft.println(blockMessage1);
        tft.setTextColor(TFT_RED);  // print block message 2
        tft.setCursor(tw2, th);
        tft.println(blockMessage2);
      };

      flashText = !flashText;

    } else {

      flashText = true;
      tft.setCursor(tw2, th);
      tft.setTextColor(TFT_BLACK);
      tft.println(blockMessage2);
    };
  };
};

void setupDisplay() {

  Serial.println("Setting up display");

  // turn on the T-Display screen when in battery mode
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  tft.init();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);

  // text size = 2 represents a medium text size; it is set here once as the text sized to be use through out the program
  tft.setTextSize(2);

  // Important Note: as the TFT will be rotated either 90 or 270 degrees the TFT Width will in used for
  // height calculations and the TFT Height will in used for width calculations through out this sketch

  int currentRotation;

  if (USBOnTheRight)
    tft.setRotation(1);  // rotate 90 degrees
  else
    tft.setRotation(3);  // rotate 270 degrees
}

void activateNetworkBlockingAsRequired() {

  // Ensure the network blocking rules in OPNsense are correctly enabled/disabled based on if the Emergency Stop button's state (pushed down or not)

  // if the Emergency Stop button state has not changed since the last time this routine was run then assume the actual network blocking status remains good
  // but only for a maximum of 30 seconds.  After 30 seconds re-confirm the actual network blocking status from OPNsense to be sure it remains correct.

  // If a actual state is incorrect then correct it.

  const int confirmTheNetworkBlockingStatusOnceEveryThisManySeconds = 30;

  static bool lastnetworkBlockingIsActive = false;
  static long nextScheduledReconfirmation = 0;

  if ((networkBlockingIsActive != lastnetworkBlockingIsActive) || (millis() > nextScheduledReconfirmation)) {

    // a re-check and/or update of the actual network system status is needed, proceed to the balance of the code in this routine below
    lastnetworkBlockingIsActive = networkBlockingIsActive;
    nextScheduledReconfirmation = millis() + confirmTheNetworkBlockingStatusOnceEveryThisManySeconds * 1000;

    // ensure all the API rules are currently enabled/disabled as needed

    everyThingIsAsItShouldBe = alignAllOPNsenseRules();

    if (!everyThingIsAsItShouldBe)
      Serial.println("Changes needed in support of the emergency shutdown status failed");
  };
}

void checkEmergencyStopButton() {

  // for the networkBlockingIsActive flag to be true
  // the normally opened connection must be closed and the normally closed connection must be open
  // this double test acts as a safeguard to ensure the networkBlockingIsActive flag is only set to true when the Emergency Shutdown button is fully engaged

  bool normallyClosedConnectionIsOpened = false;
  bool normallyOpenedConnectionIsClosed = false;

  // apply the first test to see if the normally closed connection is opened

  if (digitalRead(GENERAL_SETTINGS_NORMALLY_CLOSED_CONNECTION_PIN) != 0) {

    delay(10);  // weed out false positives caused by debounce

    if (digitalRead(GENERAL_SETTINGS_NORMALLY_CLOSED_CONNECTION_PIN) != 0) {

      normallyClosedConnectionIsOpened = true;

      // apply the second test to see if the normally closed connection is opened

      // note: the second test is nested under the successful pass of the first test
      // because if the first test failed, there is no need for the second test

      if (digitalRead(GENERAL_SETTINGS_NORMALLY_OPENED_CONNECTION_PIN) == 0) {

        delay(10);  // weed out false positives caused by debounce

        if (digitalRead(GENERAL_SETTINGS_NORMALLY_OPENED_CONNECTION_PIN) == 0)
          normallyOpenedConnectionIsClosed = true;
      };
    };
  };

  networkBlockingIsActive = (normallyClosedConnectionIsOpened && normallyOpenedConnectionIsClosed);

  activateNetworkBlockingAsRequired();
}

void setupEmergencyStopButton() {

  Serial.println("Setting up button");

  // this code is taylored for the use of an Emergency Stop Switch Push Button with 1NO1NC
  // here is an eample: https://s.click.aliexpress.com/e/_DDque9V
  // for more information as to why, please see the subroutine checkEmergencyStopButton above

  pinMode(GENERAL_SETTINGS_NORMALLY_OPENED_CONNECTION_PIN, INPUT_PULLUP);
  pinMode(GENERAL_SETTINGS_NORMALLY_CLOSED_CONNECTION_PIN, INPUT_PULLUP);
}

void roleTheOpeningCredits() {

  Serial.println("");
  Serial.println("ESP32 Network Blocker for OPNsense v1");
  Serial.println("Copyright (c) Rob Latour, 2023");
  Serial.println("License: MIT");
  Serial.println("For more info please see: https://rlatour.com/esp32networkblocker");
  Serial.println("");
}

void setupSerial() {

  Serial.begin(115200);
}

void setup() {

  setupSerial();

  roleTheOpeningCredits();

  setupDisplay();

  setupEmergencyStopButton();

  setupWiFi();
}

void loop() {

  checkEmergencyStopButton();

  if (getNewOPNsenseStatus())
    refreshDispalyWithOpenSenseData();

  refreshNetworkLockDisplay();

  checkWiFiConnection();

  ArduinoOTA.handle();
}
