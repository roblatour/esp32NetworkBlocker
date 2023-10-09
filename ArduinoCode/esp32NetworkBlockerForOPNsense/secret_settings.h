
#define SECRET_SETTINGS_WIFI_SSID             "yourWiFiSSID"   
#define SECRET_SETTINGS_WIFI_PASSWORD         "yourWiFiPassword"   

#define SECRET_SETTINGS_OTA_DEVICE_NAME       "ESP32NetworkBlockerForOPNsense"
#define SECRET_SETTINGS_OTA_PASSWORD          "ESP32NetworkBlockerForOPNsense"

#define SECRET_OPNSENSE_HOST_IP               "192.168.1.1" // the ip address of your OPNsense firewall

// In OPNsence an api secret and key are set via the OPNsense GUI - System - Access - Users window for a particualur user
// for more information, please see: https://docs.opnsense.org/development/how-tos/api.html

#define SECRET_SETTINGS_OPNSENSE_API_SECRET   "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
#define SECRET_SETTINGS_OPNSENSE_API_KEY      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";

// In OPNsense rules controlable via the APi are set In the OPNsense GUI, via the Firewall - Automation - Filter window
// These rules are evaluated before the floating rules are evaluated
// At the top right of the Firewall - Automation - Filter window, click on the symbol with three dot and dashes (".__") and select the option to View Id to see the rule's uuid numbers
//
// alternatively, you can use the following curl command to list all rules and their uuids:
// curl -d '' -k -u "secret":"apiKey" https://OPNsenseHostIP/api/firewall/filter/searchRule
//
// this program uses two rules; one to block IPv4 traffic on the selected interfaces, and another to blcok IPv4 traffic on the selected interfaces
//

// As many OPNsense Firewall Automation Filter rules as needed may be included below
// When the Emergency Stop button is down the Firewall Automation Filter rules include below will be disabled
// When the Emergency Stop button is up the Firewall Automation Filter rules include below will be enabled

const char* API_RULES[] = { "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",  // IPv4 BLOCK RULE UUID
                            "ffffffff-gggg-hhhh-iiii-jjjjjjjjjjjj"}; // IPv6 BLOCK RULE UUID

