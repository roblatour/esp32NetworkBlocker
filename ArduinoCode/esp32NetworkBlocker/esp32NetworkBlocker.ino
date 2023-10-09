// ESP32 Network Blocker (version 1)
//
// Copyright Rob Latour, 2023
// https://rlatour.com/esp32networkblocker
// https://github.com/roblatour/esp32networkblocker
// License: MIT
//
// Hardware:
// 2 x ESP32 Devkit v1 (30 pin)              https://www.aliexpress.com/item/4000090521976.html (or other such board. However, if you use another board you may need to change the pin assingments below)
// 2 x Emergency Stop Button                 https://www.aliexpress.com/item/1005001373332069.html
// 1 x 3v DPDT Relay Non Latching            https://www.digikey.ca/en/products/detail/cit-relay-and-switch/J104D2C3VDC-15S/12502634
// 2 x RJ45                                  https://s.click.aliexpress.com/e/_DkJiN9D
// 1 x 1K resistor                           (for use with relay)
// optional: type C jack power connector     https://www.aliexpress.com/item/1005004905347734.htm
// optional: LED and matching resistor       (see notes below)
// optional: 3D printed enclosures:
//           Controller box
//           Remote switch box
//
// Compile and upload using Arduino IDE (2.2.1 or greater)
//
// Board in Arduino board manager:           ESP32 Dev Module
//
// Arduino tools settings:
//
// CPU Frequency: "240MHz (WiFi/BT)"
// Core Debug Level: "None"
// Erase All Flash Before Sketch Upload: "Disabled"
// Events Run On: "Core V Flash Frequency: "80MHz"
// Flash Mode: "QIO"
// Flash Size: "4MB (32Mb)"
// JTAG Adapter: "Disabled"
// Arduino Runs On: "Core 1"
// Partition Scheme: "Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)" PSRAM: "Disabled"
// Upload Speed: "921600"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>

// User settings:
const bool useASwitchbox = true;    // set to true if you want to use the Control box and a remote Switchbox; set to false if you only want to use the Control box without a remote Switchbox
const bool useOnboardLED = false;   // if you want to use the onboard LED to signify what is going on with the program set this value to true, otherwise set it to false
const bool useExternalLED = true;   // if you want to use an external LED to signify what is going on with the program set this value to true, otherwise set it to false
const bool useLongRange = false;    // set to true to use ESP-NOW Long Range which in theory should provide for greater distances between the control box and the switch box, but have not in my testing

// *** None of the code beyond this point should need to be changed ***

// Recommended ESP32 pins to connect:

// ESP32 pins used by the Controller:
// For the Emergency Stop button on the Controller
int controllerNormallyOpenedConnectionPin = 18;
int controllerNormallyClosedConnectionPin = 19;

// For the relay
int controllerRelayConnectionPin = 23;

// ESP32 pins used by the Switchbox:
// For the Emergency Stop button on the Switchbox
int swtichboxNormallyOpenedConnectionPin = 21;
int swichboxNormallyClosedConnectionPin = 22;

// ESP32 pins used by both the Controller and the Switchbox:

// Wire the 5v and Ground pins to 5v power source

// LEDs
// The pins below are used in support of LEDs which are used to signify what is going on with the program
// when a LED is lite it means the network is blocked (data is blocked)
// when a LED is unlite it means the network is blocked (data is flowing)
// when a LED is flashing, it means there is a problem; what the potential problems are, and how frequently the LED will flash to signify them, are detailed a little further below

int onboardLEDPin = 2;  // default pin number for the onboard blue LED on the ESP32 Devkit v1 30 is 2

int externalLEDPin = 5;  // Of note: in general different colour LEDs need different resistors added in between the LED and either, but not both, the negative (ESP32 ground) pin or positive (esp32 numbered) pin
//                           // an internet search will help you find the right resistor to use for the colour of LED you want to use
//                           // having that said, in many cases, a Blue LED does not require a resistor
//                           // a red LED

const int FlashRateOfTheLEDWhenThereIsACommunicationsProblemBetweenTheControllerAndSwitch = 333;  // flash every 333 milis seconds, i.e. ~ 1/3 of a second
const int FlashRateOfTheLEDWhenThereIsAWiringProblem = 1000;                                      // flash every 1000 millis seconds, i.e. 1 second
const int FlashRateOfTheLEDWhenTheESPNOWFailedToInitialize = 3000;                                // flash every 3000 millis seconds, i.e. 3 seconds
volatile int FlashRateOfTheLED = 0;

// The controller will try to communicate with the switchbox at a frequency of either touchBaseTimePeriodWhenAllIsGood or touchBaseTimePeriodWhenThereIsAKnownProblem depending if there is a know problem or not.
//
// If when the controller tries to communicate with the Switchbox the message is sent ok then (in the OnSendData subroutine) the value for nextTouchBaseTimeTarget will be updated to reflect all is good and the nextTouchBaseTimeTarget will be recalculated.
// At the same time the Switchbox will have received the data ok (OnDataRecv) and also reflect all is good and recalculate its own nextTouchBaseTimeTarget.
//
// As needed the switchbox will also try to communicate with the Controller in the same way so as to identify problems from its perspective following the same principles as above.  However,
// the switchbox will apply a small offset to its communication times to minimize the likelihood of a situation that the controller and switchbox are both testing the communicaitons at the same time.
// Generally, this will also have the effect of allowing the Controller to drive the checking, and only when communications fail will the Switchbox start sending checks as well.
//

const unsigned long touchBaseTimePeriodWhenAllIsGood = 30000;            // Controller to re-check this every 30 seconds when all is good
const unsigned long touchBaseTimePeriodWhenThereIsAKnownProblem = 5000;  // Controller re-check this every 5 seconds when there is a known problem
const unsigned long touchbaseOffsetsForSwitchbox = 1500;                 // Switchbox re-check communications from its perspective offset by 1.5 seconds from the Controller's re-check target
unsigned long nextTouchBaseTimeTarget = millis() + touchBaseTimePeriodWhenAllIsGood;


// The variables thisIsTheController and thisIsTheSwitchbox are automatically set at startup based on the wiring of the board.
// They identify the role of the device as being either a Controller or a Switchbox
// As there are only two roles, it is perhaps redundant use two variables for this purpose.
// However, using two variables makes the code below much more readable and ultimately that is why two variables are used instead of one.

bool thisIsTheController = false;
bool thisIsTheSwitchbox = false;

// The advertised MAC address of the ESP32 will be set to one of the two values as seen below
// based on if the device has been automatically determined to be the Controller or the Switchbox
const uint8_t controllerMACAddress[] = { 0x40, 0x22, 0xD8, 0xEB, 0x3A, 0x00 };
const uint8_t remoteSwitchMACAddress[] = { 0x40, 0x22, 0xD8, 0xEB, 0x3A, 0x01 };

// the following is used for communications between the controller and the remote switchbox

enum transmission_Type {
  requestNetworkStatus,
  networkStatusReply,
  requestSwitchboxStatus,
  switchboxStatusReply,
  requestControllerBlockNetwork,
  requestControllerUnblockNetwork
};

enum device_Status {
  blocked,
  unblocked,
  unknown
};

typedef struct transmission_Structure {
  transmission_Type transmission;
  device_Status status;
} transmission_Structure;

volatile device_Status ControllerSwitchStatus = unknown;
volatile device_Status SwitchBoxSwitchStatus = unknown;
volatile device_Status NetworkStatus = unblocked;

esp_now_peer_info_t peerInfo;

volatile bool packedSentSuccesfully;

void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {

  // transmission                     Sent by      Received by   What

  // requestNetworkStatus             Switchbox    Controller    requesting the status of the network
  // networkStatusReply               Controller   Switchbox     receiving  the status of the network

  // requestSwitchboxStatus           Controller   Switchbox     requesting the status of the Switchbox's switch
  // switchboxStatusReply             Switchbox    Controller    receiving  the status of the Switchbox's switch

  // requestControllerBlockNetwork    Switchbox    Controller    requesting the network be blocked (this will be done)
  // requestControllerUnblockNetwork  Switchbox    Controller    requesting the network be unblocked (this will be done only if the Controller's switch is in the unblocked position as well)

  // as data is now being received the (LED) flash rate will be set to zero
  FlashRateOfTheLED = 0;

  if (thisIsTheController)
    nextTouchBaseTimeTarget = millis() + touchBaseTimePeriodWhenAllIsGood;
  else
    nextTouchBaseTimeTarget = millis() + touchBaseTimePeriodWhenAllIsGood + touchbaseOffsetsForSwitchbox;

  if (thisIsTheController) {

    transmission_Structure incomingMessage;

    memcpy(&incomingMessage, incomingData, sizeof(incomingMessage));

    if (incomingMessage.transmission == requestNetworkStatus) {

      Serial.println("the Switchbox has requested the Contoller provide the network status");
      provideTheSwitchboxTheNetworkStatus();

      return;
    };

    if (incomingMessage.transmission == switchboxStatusReply) {

      Serial.println("the Controller is receiving the status of the Switchbox's switch");

      if (incomingMessage.status == blocked)
        Serial.println("The Controller has been told the Switchbox's switch is set to blocked");
      else
        Serial.println("The Controller has been told the Switchbox's switch is set to unblocked");

      SwitchBoxSwitchStatus = incomingMessage.status;

      return;
    };

    if (incomingMessage.transmission == requestControllerBlockNetwork) {

      Serial.println("the Switchbox has requested the Contoller block the network");
      SwitchBoxSwitchStatus = blocked;
      BlockNetwork(true, false);
      provideTheSwitchboxTheNetworkStatus();
      return;
    };

    if (incomingMessage.transmission == requestControllerUnblockNetwork) {

      Serial.println("the Switchbox has requested the Contoller unblock the network");
      SwitchBoxSwitchStatus = unblocked;
      BlockNetwork(false, false);
      provideTheSwitchboxTheNetworkStatus();
      return;
    };

  } else {

    transmission_Structure incomingMessage;

    memcpy(&incomingMessage, incomingData, sizeof(incomingMessage));

    if (incomingMessage.transmission == networkStatusReply) {

      if (incomingMessage.status == blocked)
        Serial.println("The Switchbox has been told by the Controller the network status is blocked");
      else
        Serial.println("The Switchbox has been told by the Controller the network status is unblocked");

      NetworkStatus = incomingMessage.status;

      return;
    };

    if (incomingMessage.transmission == requestSwitchboxStatus) {

      Serial.println("The Controller is requesting the Switchbox's status");
      provideTheControllerTheSwitchboxStatus();

      return;
    };
  };

  FlashRateOfTheLED = FlashRateOfTheLEDWhenThereIsACommunicationsProblemBetweenTheControllerAndSwitch;
  Serial.print("Error: an unknown request was received");
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {

  packedSentSuccesfully = (status == ESP_NOW_SEND_SUCCESS);

  if (packedSentSuccesfully) {

    FlashRateOfTheLED = 0;

    if (thisIsTheController)
      nextTouchBaseTimeTarget = millis() + touchBaseTimePeriodWhenAllIsGood;
    else
      nextTouchBaseTimeTarget = millis() + touchBaseTimePeriodWhenAllIsGood + touchbaseOffsetsForSwitchbox;

    // Serial.println("OnDataSent delivery success");

  } else {

    FlashRateOfTheLED = FlashRateOfTheLEDWhenThereIsACommunicationsProblemBetweenTheControllerAndSwitch;

    if (thisIsTheController)
      nextTouchBaseTimeTarget = millis() + touchBaseTimePeriodWhenThereIsAKnownProblem;
    else
      nextTouchBaseTimeTarget = millis() + touchBaseTimePeriodWhenThereIsAKnownProblem + touchbaseOffsetsForSwitchbox;

    Serial.println("OnDataSent delivery failed");
  };
}

void provideTheControllerTheSwitchboxStatus() {

  if (!thisIsTheSwitchbox) {
    Serial.println("error: provideTheControllerTheSwitchboxStatus should only be invoked by the thisIsTheSwitchbox");
    return;
  };

  transmission_Structure outgoingTransmission;

  checkEmergencyStopButton();

  outgoingTransmission.transmission = switchboxStatusReply;
  outgoingTransmission.status = SwitchBoxSwitchStatus;

  Serial.print("The Switchbox is providing the Controller with a switch status of ");

  if (SwitchBoxSwitchStatus == blocked)
    Serial.println("blocked");
  else
    Serial.println("unblocked");

  if (esp_now_send(controllerMACAddress, (uint8_t *)&outgoingTransmission, sizeof(outgoingTransmission)) == ESP_OK) {

    delay(10);

    if (!packedSentSuccesfully)
      Serial.println("Switch status was not sent to the Controller");

  } else {

    FlashRateOfTheLED = FlashRateOfTheLEDWhenThereIsACommunicationsProblemBetweenTheControllerAndSwitch;
    Serial.println("Switch status was not sent to the Controller");
  };
};

void provideTheSwitchboxTheNetworkStatus() {

  if (!thisIsTheController) {
    Serial.println("error: provideTheSwitchboxTheNetworkStatus should only be invoked by the Controller");
    return;
  };

  Serial.println("Providing network status to the switchbox");

  transmission_Structure outgoingTransmission;

  outgoingTransmission.transmission = networkStatusReply;
  outgoingTransmission.status = NetworkStatus;

  if (NetworkStatus == blocked) {
    Serial.println("Controller says the network is blocked");
  } else {
    Serial.println("Controller says the network is unblocked");
  };

  if (esp_now_send(remoteSwitchMACAddress, (uint8_t *)&outgoingTransmission, sizeof(outgoingTransmission)) == ESP_OK) {

    delay(10);

    if (!packedSentSuccesfully)
      Serial.println("Network status was not sent to the Switchbox");

  } else {

    FlashRateOfTheLED = FlashRateOfTheLEDWhenThereIsACommunicationsProblemBetweenTheControllerAndSwitch;
    Serial.println("Network status was not sent to the Switchbox");
  };
}

void BlockNetwork(bool blockNow, bool RequestCameFromController) {

  if (!thisIsTheController) {
    Serial.println("error: BlockNetwork should only be invoked by the Controller");
    return;
  };

  if (blockNow)
    Serial.print("incoming block request");
  else
    Serial.print("incoming unblock request");

  if ((blockNow) && (NetworkStatus == blocked)) {
    Serial.println("a request to block network was ignored as the network is already blocked");
    return;
  };

  if ((!blockNow) && (NetworkStatus == unblocked)) {
    Serial.println("a request to unblock network was ignored as the network is already unblocked");
    return;
  };

  if (useASwitchbox) {

    if (!blockNow) {

      SwitchBoxSwitchStatus = unknown;

      if (RequestCameFromController)
        requestTheSwitchboxStatus();

      if (ControllerSwitchStatus == blocked) {
        Serial.println("a request from the Switchbox to unblock network was denied as the Controller is requesting the network be blocked");
        return;
      };

      if (SwitchBoxSwitchStatus == blocked) {
        Serial.println("a request from the Controller to unblock network was denied as the Switchbox is requesting the network be blocked");
        return;
      };
    };
  };

  if (blockNow) {
    digitalWrite(controllerRelayConnectionPin, HIGH);  // set the relay so that it is powered on so as to block the network connection
    NetworkStatus = blocked;
    Serial.println("Network blocked");
  } else {
    digitalWrite(controllerRelayConnectionPin, LOW);  // set the relay so that it is powered off so as to unblock the network connection
    NetworkStatus = unblocked;
    Serial.println("Network unblocked");
  };
};

void requestToBlockOrUnblockNetwork(bool networkShouldBeBlocked) {

  if (!thisIsTheSwitchbox) {
    Serial.println("error: requestToBlockOrUnblockNetwork should only be invoked by the Switchbox");
    return;
  };

  transmission_Structure outgoingTransmission;

  if (networkShouldBeBlocked) {
    Serial.println("The Switchbox is asking the Controller to block the network");
    outgoingTransmission.transmission = requestControllerBlockNetwork;
  } else {
    Serial.println("The Switchbox is asking the Controller to unblock the network");
    outgoingTransmission.transmission = requestControllerUnblockNetwork;
  };
  outgoingTransmission.status = unknown;

  if (esp_now_send(controllerMACAddress, (uint8_t *)&outgoingTransmission, sizeof(outgoingTransmission)) == ESP_OK) {

    delay(10);

    if (!packedSentSuccesfully)
      Serial.println("update request failed, refresh not requested");

  } else {

    FlashRateOfTheLED = FlashRateOfTheLEDWhenThereIsACommunicationsProblemBetweenTheControllerAndSwitch;
    Serial.println("update request failed, refresh not requested");
  };
}

void requestANetworkChangeAsNeeded() {

  if (!thisIsTheSwitchbox) {
    Serial.println("error: requestANetworkChangeAsNeeded should only be invoked by the Switchbox");
    return;
  };

  static device_Status lastSwitchBoxSwitchStatus = unknown;

  if (SwitchBoxSwitchStatus != lastSwitchBoxSwitchStatus) {
    lastSwitchBoxSwitchStatus = SwitchBoxSwitchStatus;
    if (SwitchBoxSwitchStatus == blocked)
      requestToBlockOrUnblockNetwork(true);
    else
      requestToBlockOrUnblockNetwork(false);
  };
};

void requestTheSwitchboxStatus() {

  if (!thisIsTheController) {
    Serial.println("error: requestTheSwitchboxStatus should only be invoked by the Controller");
    return;
  };

  transmission_Structure outgoingTransmission;

  outgoingTransmission.transmission = requestSwitchboxStatus;
  outgoingTransmission.status = unknown;

  SwitchBoxSwitchStatus = unknown;

  if (esp_now_send(remoteSwitchMACAddress, (uint8_t *)&outgoingTransmission, sizeof(outgoingTransmission)) == ESP_OK) {

    delay(10);

    if (packedSentSuccesfully) {
      unsigned long timeout = millis() + 2000;  // give the switchbox some time to reply
      while ((millis() < timeout) & (SwitchBoxSwitchStatus == unknown))
        delay(10);
    };
  };

  if (SwitchBoxSwitchStatus == blocked)
    Serial.println("Switchbox switch is blocked");
  else if (SwitchBoxSwitchStatus == unblocked)
    Serial.println("Switchbox switch is unblock");
  else if (SwitchBoxSwitchStatus == unknown) {
    Serial.println("Did not receive a reply from the Switchbox as to its status");
    FlashRateOfTheLED = FlashRateOfTheLEDWhenThereIsACommunicationsProblemBetweenTheControllerAndSwitch;
  };
};

void actionANetworkChangeAsNeeded() {

  if (!thisIsTheController) {
    Serial.println("error: actionANetworkChangeAsNeeded should only be invoked by the Controller");
    return;
  };

  static device_Status lastControllerSwitchStatus = unknown;

  if (ControllerSwitchStatus != lastControllerSwitchStatus) {

    lastControllerSwitchStatus = ControllerSwitchStatus;

    if (ControllerSwitchStatus == blocked)
      BlockNetwork(true, true);
    else
      BlockNetwork(false, true);

    provideTheSwitchboxTheNetworkStatus();
  };
};

void periodicCheckToEnsureCommunicationsAreOK() {

  // While communications are vaidate when the Controller and Switch box are being initialized and are sending data back and forth
  // this code provides a periodic check between the devices to account for situations such as one of them being powered down or the two becoming out of range of each other.

  if (millis() > nextTouchBaseTimeTarget) {

    // In order to test communication between this program and its peer, this program will send its current switch status to its peer.
    // Once done this program's subroutine OnDataSent will trigger the LED to start or stop blinking if the send fails or succeeds (respectfully).
    // Also OnDataSent will recalculate the nextTouchBaseTimeTarget as appropriate.
    // However, while the communications test will completed in less than a second it will be done in parallel with the repeated exectution of this subroutine.
    // Accordingly, before the test starts the nextTouchBaseTimeTarget is temporarily updated to be two seconds from now to prevent this subroutine from repeatedly executing while the communications test is underway.

    nextTouchBaseTimeTarget = millis() + 2000;

    // test communications
    if (thisIsTheController)
      provideTheSwitchboxTheNetworkStatus();
    else
      provideTheControllerTheSwitchboxStatus();
  };
};

void setupSerial() {

  Serial.begin(115200);
}

void showTheOpeningCredits() {

  Serial.println("");
  Serial.println("ESP32 Network Controller v1");
  Serial.println("");
  Serial.println("Copyright (c) Rob Latour, 2023");
  Serial.println("License: MIT");
  Serial.println("For more info please see: https://rlatour.com/esp32networkblocker");
  Serial.println("");
}

void setupTheESP32Pins() {

  // All pins for the controller and switch box are setup here.
  // This is done as we don't yet know the role of this device; i.e. will be used as the controller or the switchbox.
  // Also, Switchbox pins are initialized even in the case we will not be using the Switchbox, that way we can later test to enusre they are not being used

  pinMode(swtichboxNormallyOpenedConnectionPin, INPUT_PULLUP);
  pinMode(swichboxNormallyClosedConnectionPin, INPUT_PULLUP);

  pinMode(controllerNormallyOpenedConnectionPin, INPUT_PULLUP);
  pinMode(controllerNormallyClosedConnectionPin, INPUT_PULLUP);

  pinMode(controllerRelayConnectionPin, OUTPUT);
  digitalWrite(controllerRelayConnectionPin, LOW);  // ensure the relay is powered off (so that the network connection starts as being unblocked)

  pinMode(onboardLEDPin, OUTPUT);
  digitalWrite(onboardLEDPin, LOW);  // ensure the onboard LED is off to start (representing the network connection is unblocked)

  pinMode(externalLEDPin, OUTPUT);
  digitalWrite(externalLEDPin, LOW);  // ensure the external LED is off to start (representing the network connection is unblocked)
};

bool setTheRoleOfThisDeviceBasedOnItsWiring() {

  // based on wired PINs in use, the code below will determine the role of this device

  // returns true if the wiring for the Emergency Stop button on this device is correct
  // returns false if the wiring for the Emergency Stop button on this device is incorrect

  thisIsTheSwitchbox = false;
  thisIsTheController = false;

  int openSwitchboxCircuts = 0;
  int openControllerCircuts = 0;
  int totalOpenCircuts = 0;

  if (digitalRead(controllerNormallyClosedConnectionPin) == 0) {
    delay(10);  // weed out false positives caused by debounce
    if (digitalRead(controllerNormallyClosedConnectionPin) == 0)
      openControllerCircuts++;
  };

  if (digitalRead(controllerNormallyOpenedConnectionPin) == 0) {
    delay(10);  // weed out false positives caused by debounce
    if (digitalRead(controllerNormallyOpenedConnectionPin) == 0)
      openControllerCircuts++;
  };

  if (digitalRead(swichboxNormallyClosedConnectionPin) == 0) {
    delay(10);  // weed out false positives caused by debounce
    if (digitalRead(swichboxNormallyClosedConnectionPin) == 0)
      openSwitchboxCircuts++;
  };

  if (digitalRead(swtichboxNormallyOpenedConnectionPin) == 0) {
    delay(10);  // weed out false positives caused by debounce
    if (digitalRead(swtichboxNormallyOpenedConnectionPin) == 0)
      openSwitchboxCircuts++;
  };

  totalOpenCircuts = openSwitchboxCircuts + openControllerCircuts;

  if (totalOpenCircuts == 1) {

    if (openControllerCircuts == 1) {
      thisIsTheController = true;
      Serial.println("This is the Controller");
    } else {
      thisIsTheSwitchbox = true;
      Serial.println("This is the remote Switchbox");
    }

  } else {
    FlashRateOfTheLED = FlashRateOfTheLEDWhenThereIsAWiringProblem;
    Serial.println("Wiring problem detected for the Emergency Stop button");
  };

  if (thisIsTheSwitchbox)
    if (!useASwitchbox) {
      FlashRateOfTheLED = FlashRateOfTheLEDWhenThereIsAWiringProblem;
      Serial.println("Wiring problem detected for the Emergency Stop button");
      Serial.println("Either it has been configured using the Switchbox pins rather than the Controller pins, or ");
      Serial.println("the variable useASwitch has been set to false when it should be set to true");
      totalOpenCircuts = 0;
    };

  return (totalOpenCircuts == 1);
};

void setMACAddresses() {

  // based on the role of the device, set its MAC address

  Serial.println("");

  Serial.print("ESP32's board original MAC Address:  ");
  Serial.println(WiFi.macAddress());

  if (thisIsTheController)
    esp_wifi_set_mac(WIFI_IF_STA, &controllerMACAddress[0]);
  else
    esp_wifi_set_mac(WIFI_IF_STA, &remoteSwitchMACAddress[0]);

  Serial.print("ESP32's board revised MAC Address:   ");
  Serial.println(WiFi.macAddress());

  Serial.println("");
};

void initializeWiFi() {

  // for two way ESP-NOW communications both the Controller box and the Switch box must be setup in STA mode

  if (useLongRange) {
    WiFi.enableLongRange(true);  // this statement must proceed the WiFi.mode statement
    WiFi.mode(WIFI_STA);
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_LORA_250K);
    Serial.println("Using Long Range");
  } else {
    WiFi.mode(WIFI_STA);
    Serial.println("Not using Long Range");
  };

  setMACAddresses();

  // turn off modem power saving
  esp_wifi_set_ps(WIFI_PS_NONE);

}

bool initializeESPNOW() {

  bool returnValue = false;

  initializeWiFi();

  if (esp_now_init() == ESP_OK) {

    Serial.println("ESP-NOW initialize");

    esp_now_register_send_cb(OnDataSent);

    if (thisIsTheController)
      memcpy(peerInfo.peer_addr, remoteSwitchMACAddress, 6);
    else
      memcpy(peerInfo.peer_addr, controllerMACAddress, 6);

    peerInfo.channel = 0;

    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) == ESP_OK) {

      Serial.println("Peer added");
      esp_now_register_recv_cb(OnDataRecv);
      returnValue = true;

    } else {

      Serial.println("Failed to add peer");
    };

  } else {

    FlashRateOfTheLED = FlashRateOfTheLEDWhenTheESPNOWFailedToInitialize;
    Serial.println("Failed to initialize ESP-NOW");
  };

  Serial.println("");

  return returnValue;
}

void checkEmergencyStopButton() {

  int normallyClosedConnectionPin;
  int normallyOpenedConnectionPin;

  if (thisIsTheController) {

    normallyClosedConnectionPin = controllerNormallyClosedConnectionPin;
    normallyOpenedConnectionPin = controllerNormallyOpenedConnectionPin;

  } else {

    normallyClosedConnectionPin = swichboxNormallyClosedConnectionPin;
    normallyOpenedConnectionPin = swtichboxNormallyOpenedConnectionPin;
  };

  // for a blocking flag to be true
  // the normally opened connection must be closed AND the normally closed connection must be open
  // this two part test acts as a safeguard to ensure the blocking flag is only set to true when the Emergency Shutdown button is fully engaged

  bool normallyClosedConnectionIsOpened = false;
  bool normallyOpenedConnectionIsClosed = false;

  // apply the first test to see if the normally closed connection is opened

  if (digitalRead(normallyClosedConnectionPin) != 0) {

    delay(10);  // weed out false positives caused by debounce

    if (digitalRead(normallyClosedConnectionPin) != 0) {

      normallyClosedConnectionIsOpened = true;

      // apply the second test to see if the normally closed connection is opened

      // note: the second test is nested under the successful pass of the first test
      // because if the first test failed, there is no need for the second test

      if (digitalRead(normallyOpenedConnectionPin) == 0) {

        delay(10);  // weed out false positives caused by debounce

        if (digitalRead(normallyOpenedConnectionPin) == 0)
          normallyOpenedConnectionIsClosed = true;
      };
    };
  };

  if (thisIsTheController) {

    if (normallyClosedConnectionIsOpened && normallyOpenedConnectionIsClosed)
      ControllerSwitchStatus = blocked;
    else
      ControllerSwitchStatus = unblocked;

  } else {

    if (normallyClosedConnectionIsOpened && normallyOpenedConnectionIsClosed)
      SwitchBoxSwitchStatus = blocked;
    else
      SwitchBoxSwitchStatus = unblocked;
  };
}


void LEDUpdateAsRequried() {

  // if the global variable FlashRateOfTheLED > 0 then flash the LED every x milliseconds where x is determined by FlashRateOfTheLED
  // otherwise turn the LED on if the network is unblocked or turn the LED off if the network is blocked

  static bool LEDIsOn = false;
  static long nextFlashCycle = 0;

  if (FlashRateOfTheLED > 0) {

    if (millis() > nextFlashCycle) {

      nextFlashCycle = millis() + FlashRateOfTheLED;

      if (LEDIsOn) {
        if (useOnboardLED)
          digitalWrite(onboardLEDPin, LOW);  // turn off the onboard LED
        if (useExternalLED)
          digitalWrite(externalLEDPin, LOW);  // turn off the external LED

      } else {
        if (useOnboardLED)
          digitalWrite(onboardLEDPin, HIGH);  // turn on the onboard LED
        if (useExternalLED)
          digitalWrite(externalLEDPin, HIGH);  // turn on the external LED
      };

      LEDIsOn = !LEDIsOn;
    };

  } else {

    if (NetworkStatus == blocked) {

      if (!LEDIsOn) {
        LEDIsOn = true;
        if (useOnboardLED)
          digitalWrite(onboardLEDPin, HIGH);  // turn on the onboard LED
        if (useExternalLED)
          digitalWrite(externalLEDPin, HIGH);  // turn on the external LED
      };

    } else {

      if (LEDIsOn) {
        LEDIsOn = false;
        if (useOnboardLED)
          digitalWrite(onboardLEDPin, LOW);  // turn off the onboard LED
        if (useExternalLED)
          digitalWrite(externalLEDPin, LOW);  // turn off the external LED
      };
    };
  };
};

void setup() {

  setupSerial();

  showTheOpeningCredits();

  setupTheESP32Pins();

  if (setTheRoleOfThisDeviceBasedOnItsWiring()) {

    if (thisIsTheController) {

      if (useASwitchbox) {

        if (!initializeESPNOW()) {
          // there was a problem with InitializeESPNOW so flash the LED here forever
          while (true)
            LEDUpdateAsRequried();
        };

        requestTheSwitchboxStatus();
      };

      checkEmergencyStopButton();

      actionANetworkChangeAsNeeded();

      if (useASwitchbox)
        provideTheSwitchboxTheNetworkStatus();

    } else {

      if (!initializeESPNOW()) {
        //  there was a problem with InitializeESPNOW so flash the LED here forever
        while (true)
          LEDUpdateAsRequried();
      };

      checkEmergencyStopButton();

      requestANetworkChangeAsNeeded();
    };
  } else {

    // a wiring problem was detected so flash the LED here forever
    while (true)
      LEDUpdateAsRequried();
  };
}

void loop() {

  checkEmergencyStopButton();

  if (thisIsTheController)
    actionANetworkChangeAsNeeded();
  else
    requestANetworkChangeAsNeeded();

  if (((thisIsTheController) && (useASwitchbox)) || (thisIsTheSwitchbox))
    periodicCheckToEnsureCommunicationsAreOK();

  LEDUpdateAsRequried();
}