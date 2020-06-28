// Adapted from the "Get Started" project and adjusted to my use case..

#include "Arduino.h"
#include "AZ3166WiFi.h"
#include "AzureIotHub.h"
#include "DevKitMQTTClient.h"
#include "SystemTickCounter.h"

#include "config.h"
#include "utility.h"

Watchdog watchdog;
Thread serverThread;
WiFiServer server(8080);

static bool hasWifi = false;
int messageCount = 1;
int sentMessageCount = 0;
static bool messageSending = true;
static uint64_t send_interval_ms;
static uint64_t update_interval_ms;

int currentPage = 0;
static float temperature;
static float humidity;
static float pressure;
static int magAxes[3];

static void InitWifi() {
  Screen.print(2, "Connecting...");

  if (WiFi.begin() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    Screen.print(1, ip.get_address());
    hasWifi = true;
    Screen.print(2, "Running... \r\n");
  } else {
    hasWifi = false;
    Screen.print(1, "No Wi-Fi\r\n ");
    delay(WIFI_RETRY_DELAY);
    InitWifi();
  }
}

void UpdateDisplay() {
  if (currentPage == 0) {
    char messagePayload[MESSAGE_MAX_LEN];
    readMessage(messageCount, messagePayload, &temperature, &humidity, &pressure);
    UpdateFirstScreenValues();
  } else if (currentPage == 1) {
    readSecondarySensors(magAxes);
    UpdateSecondScreenValues();
  }
}

static void UpdateFirstScreenValues() {
  char line1[20];
  sprintf(line1, "M:%d/%d", sentMessageCount, messageCount);
  Screen.print(1, line1);

  char line2[20];
  sprintf(line2, "P:%.2f", pressure);
  Screen.print(2, line2);

  char line3[20];
  sprintf(line3, "T:%.2f H:%.2f", temperature, humidity);
  Screen.print(3, line3);
}

static void UpdateSecondScreenValues() {
  Screen.print(1, "> Magnetometer");

  char line1[20];
  sprintf(line1, "%d/%d/%d", magAxes[0], magAxes[1], magAxes[2]);
  Screen.print(2, line1);

  Screen.print(3, "   ");
}

static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result) {
  if (result == IOTHUB_CLIENT_CONFIRMATION_OK) {
    blinkSendConfirmation();
    sentMessageCount++;
  }

  messageCount++;
  UpdateFirstScreenValues();
}

static void MessageCallback(const char* payLoad, int size) {
  blinkLED();
  Screen.print(1, payLoad, true);
}

static void DeviceTwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char *payLoad, int size) {
  char *temp = (char *)malloc(size + 1);
  if (temp == NULL) {
    return;
  }

  memcpy(temp, payLoad, size);
  temp[size] = '\0';
  parseTwinMessage(updateState, temp);
  free(temp);
}

static int  DeviceMethodCallback(const char *methodName, const unsigned char *payload, int size, unsigned char **response, int *response_size) {
  LogInfo("Try to invoke method %s", methodName);
  const char *responseMessage = "\"Successfully invoke device method\"";
  int result = 200;

  if (strcmp(methodName, "start") == 0) {
    LogInfo("Start sending data");
    messageSending = true;
  } else if (strcmp(methodName, "stop") == 0) {
    LogInfo("Stop sending data");
    messageSending = false;
  } else {
    LogInfo("No method %s found", methodName);
    responseMessage = "\"No method found\"";
    result = 404;
  }

  *response_size = strlen(responseMessage) + 1;
  *response = (unsigned char *)strdup(responseMessage);

  return result;
}

void SwitchPage() {
  currentPage = (currentPage + 1) % TOTAL_PAGES;
  UpdateDisplay();
}

void HandleWifiClient(WiFiClient client) {
  boolean currentLineIsBlank = true;

  while (client.connected()) {
    if (client.available()) {
      char character = client.read();
      if (character == '\n' && currentLineIsBlank) {
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: application/json");
        client.println("");
        client.println("{\"temp\": " + String(temperature) + ", \"humidity\": " + String(humidity) + ", \"pressure\": " + String(pressure) + "}");
        break;
      }

      if (character == '\n') {
        currentLineIsBlank = true;
      } else if (character != '\r') {
        currentLineIsBlank = false;
      }
    }
  }

  delay(1);
  client.stop();
}

void RunServer() {
  server.begin();

  while(true) {
    WiFiClient client = server.available();
    if (client) {
      HandleWifiClient(client);
    }
  };
}


void setup() {
  watchdog.configure(WATCHDOG_CHECK);
  Screen.init();
  Screen.print(0, "Office DevKit");
  Screen.print(2, "Initializing...");

  Screen.print(3, " > Serial");
  Serial.begin(115200);

  Screen.print(3, " > WiFi");
  hasWifi = false;
  InitWifi();
  if (!hasWifi) {
    return;
  }

  Screen.print(3, " > Sensors");
  SensorInit();

  Screen.print(3, " > IoT Hub");
  DevKitMQTTClient_SetOption(OPTION_MINI_SOLUTION_NAME, "DevKit-Office");
  DevKitMQTTClient_Init(true);

  DevKitMQTTClient_SetSendConfirmationCallback(SendConfirmationCallback);
  DevKitMQTTClient_SetMessageCallback(MessageCallback);
  DevKitMQTTClient_SetDeviceTwinCallback(DeviceTwinCallback);
  DevKitMQTTClient_SetDeviceMethodCallback(DeviceMethodCallback);

  attachInterrupt(USER_BUTTON_A, SwitchPage, CHANGE);

  serverThread.start(RunServer);

  send_interval_ms = SystemTickCounterRead();
  update_interval_ms = SystemTickCounterRead();
}

void loop() {
  watchdog.resetTimer();

  if ((int)(SystemTickCounterRead() - update_interval_ms) >= getUpdateInterval()) {
    UpdateDisplay();
    update_interval_ms = SystemTickCounterRead();
  }

  if (hasWifi) {
    if (messageSending && (int)(SystemTickCounterRead() - send_interval_ms) >= getInterval()) {
      char messagePayload[MESSAGE_MAX_LEN];
      bool temperatureAlert = readMessage(messageCount, messagePayload, &temperature, &humidity, &pressure);
      EVENT_INSTANCE* message = DevKitMQTTClient_Event_Generate(messagePayload, MESSAGE);
      DevKitMQTTClient_Event_AddProp(message, "temperatureAlert", temperatureAlert ? "true" : "false");
      DevKitMQTTClient_SendEventInstance(message);

      send_interval_ms = SystemTickCounterRead();
    } else {
      DevKitMQTTClient_Check();
    }
  }

  delay(1000);
}
