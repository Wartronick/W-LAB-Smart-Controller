#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include <PubSubClient.h>
#include <TM1638plus.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <esp_system.h>
#include <esp_rmaker_factory.h>
#include <esp_partition.h>

// BLE Provisioning Configuration (RainMaker)
const char *service_name = "Wartronick_esp";
const char *pop = "Engenharia";

// App provisioning is disabled due to BLE Payload limits with 10+ devices.
// The board connects directly to the network.

// Local MQTT Broker Configuration
const char* mqtt_server = "192.168.X.XXX"; // CHANGE THIS: Your Raspberry Pi / Home Assistant IP
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient mqtt_client(espClient);
uint32_t lastReconnectAttempt = 0;

// TM1638 Initialization (STB, CLK, DIO, true to prevent read desync on Core 2.x)
TM1638plus tm(18, 19, 21, true);
uint8_t lastButtons = 0;

// 7-segment display control variables
unsigned long displayTimer = 0;
bool showingStatus = false;

// Sequential definition of the 8 relay pins
uint8_t channelPins[] = {13, 12, 14, 27, 26, 25, 33, 32};
bool channelStates[8] = {false};

static Switch *my_switches[8];
String channelNames[8]; // Array to persistently store channel names

// --- PIN DEFINITIONS ---
#define I2C_SDA 4
#define I2C_SCL 22

// Sensores
#define DHT_VCC_PIN 17
#define DHT_PIN 5
#define DHT_GND_PIN 16
#define LDR_PIN 34 // ADC1
// DHT Type
Adafruit_BMP280 bmp;
bool bmp_status = false;

#define DHTPIN 5
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

unsigned long lastSensorRead = 0;

// Sensor Data Cache for TM1638 Display
float cache_bmp_t = 0, cache_bmp_p = 0;
float cache_dht_t = 0, cache_dht_h = 0;
int cache_ldr = 0;
float official_temp = 0; // Sensor Fusion (Weighted Average)

static TemperatureSensor *temp_sensor; // Primary for Google Home
static Device *hum_sensor;
static Device *press_sensor;
static Device *light_sensor;

// --- CENTRAL UPDATE AND SYNCHRONIZATION FUNCTION ---
void updateChannel(int id, bool state, bool updateApp, bool updateMQTT) {
  if (id < 0 || id >= 8) return;

  channelStates[id] = state;
  digitalWrite(channelPins[id], state ? HIGH : LOW);

  // Update the corresponding physical LED on the TM1638 panel
  tm.setLED(id, state ? 1 : 0);

  // Update the 7-segment display text dynamically (String avoids buffer overflow crash)
  String text = "CH " + String(id + 1) + (state ? " ON" : " OFF");
  while (text.length() < 8) text += " ";
  tm.displayText(text.c_str());

  displayTimer = millis();
  showingStatus = true;

  Serial.printf("[RELAY] Channel %d changed to %s\n", id + 1, state ? "ON" : "OFF");

  // Synchronize and update the RainMaker mobile app
  if (updateApp) {
    my_switches[id]->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, state);
  }

  // Synchronize and publish the new state to the local MQTT Broker
  if (updateMQTT && mqtt_client.connected()) {
    String topic = "home_lab/ch" + String(id + 1) + "/status";
    if (!mqtt_client.publish(topic.c_str(), state ? "ON" : "OFF")) {
       Serial.println("[MQTT] ERROR: Failed to publish relay state!");
    }
  }
}

// --- MQTT RECEIVED COMMANDS CALLBACK ---
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];

  String topicStr(topic);
  for (int i = 0; i < 8; i++) {
    String expectedTopic = "home_lab/ch" + String(i + 1) + "/set";
    if (topicStr == expectedTopic) {
      if (msg == "ON") updateChannel(i, true, true, false);
      else if (msg == "OFF") updateChannel(i, false, true, false);
      else if (msg == "TOGGLE") updateChannel(i, !channelStates[i], true, true);
    }
  }
}

// --- AUTOMATIC AND NON-BLOCKING MQTT RECONNECTION ---
void reconnectMQTT() {
  if (WiFi.status() == WL_CONNECTED && (millis() - lastReconnectAttempt > 5000)) {
    lastReconnectAttempt = millis();
    String clientId = "ESP32_Lab";
    
    if (mqtt_client.connect(clientId.c_str())) {
      mqtt_client.subscribe("home_lab/#"); // O curinga '#' é permitido. O '+' junto com texto (ch+) é proibido pelo MQTT!
      Serial.println("[MQTT] Connected to Local Broker");
    }
  }
}

// --- RAINMAKER APP RECEIVED COMMANDS CALLBACK ---
void write_callback(Device *device, Param *param, const param_val_t val, void *priv_data, write_ctx_t *ctx) {
  const char *device_name = device->getDeviceName();
  if (strcmp(param->getParamName(), "Power") == 0) {
    for (int i = 0; i < 8; i++) {
      if (strcmp(device_name, channelNames[i].c_str()) == 0) {
        updateChannel(i, val.val.b, false, true);
        param->updateAndReport(val);
      }
    }
  }
}

void setup() {
  // Spoof MAC Address (Bypass Espressif cloud chip block)
  uint8_t fake_mac[6] = {0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33};
  esp_base_mac_addr_set(fake_mac);

  Serial.begin(115200);
  delay(1000); // Additional delay to stabilize voltage

  Serial.println("\n[SYSTEM] Booting ESP32 Control Hub (Spoofed MAC)...");

  // Relay output pins configuration BEFORE any consumption
  for (int i = 0; i < 8; i++) {
    pinMode(channelPins[i], OUTPUT);
    digitalWrite(channelPins[i], LOW); // Starts off
  }
  
  pinMode(LDR_PIN, INPUT);

  // TM1638 display initialization and configuration
  tm.displayBegin();
  tm.brightness(3);
  tm.reset();
  
  // --- BOOT ANIMATION (Knight Rider style) ---
  tm.displayText("BOOTING ");
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < 8; i++) {
      tm.setLED(i, 1);
      delay(20);
      tm.setLED(i, 0);
    }
    for (int i = 6; i >= 1; i--) {
      tm.setLED(i, 1);
      delay(20);
      tm.setLED(i, 0);
    }
  }
  for (int flash = 0; flash < 2; flash++) {
    for (int i = 0; i < 8; i++) tm.setLED(i, 1);
    delay(150);
    for (int i = 0; i < 8; i++) tm.setLED(i, 0);
    delay(150);
  }

  // --- SEQUENTIAL PHYSICAL RELAY TEST ---
  tm.displayText("TEST Ch");
  Serial.println("[SYSTEM] Running physical relay test...");
  for (int i = 0; i < 8; i++) {
    digitalWrite(channelPins[i], HIGH); // Turn relay ON
    tm.setLED(i, 1);
    delay(150); // Keep ON for 150ms
    digitalWrite(channelPins[i], LOW);  // Turn relay OFF
    tm.setLED(i, 0);
  }
  
  tm.displayText("LAB CTRL"); // Final default screen

  // --- SENSOR INIT ---
  // Turn on parasitic power plant FIRST! (BMP280 relies on GND from pin 16)
  pinMode(DHT_VCC_PIN, OUTPUT);
  pinMode(DHT_GND_PIN, OUTPUT);
  
  digitalWrite(DHT_GND_PIN, LOW);  // Create shared Ground (DHT and BMP)
  digitalWrite(DHT_VCC_PIN, HIGH); // Create 3.3V for DHT
  delay(1500); // Give chips time to wake up before I2C/OneWire communication
  
  Wire.begin(I2C_SDA, I2C_SCL);
  bmp_status = bmp.begin(0x76); 
  if (!bmp_status) bmp_status = bmp.begin(0x77); // Try alternative default address
  Serial.println(bmp_status ? "[SENSOR] BMP280 initialized successfully." : "[SENSOR] Failed to find BMP280 sensor!");
  
  dht.begin();
  Serial.println("[SENSOR] DHT11 initialized.");

  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  mqtt_client.setBufferSize(1024); // Aumentado para evitar que mensagens retidas do broker derrubem a conexão

  // Physically verify where the secret partition is located on the board
  const esp_partition_t *fctry_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "fctry");
  if (fctry_part != NULL) {
      Serial.printf("\n[DEBUG] fctry partition FOUND at address: 0x%x\n", fctry_part->address);
  } else {
      Serial.println("\n[DEBUG] WARNING: fctry partition NOT FOUND!\n");
  }

  // Initialize the "fctry" partition
  esp_err_t err = esp_rmaker_factory_init();
  if (err != ESP_OK) {
      Serial.printf("[DEBUG] Failed to load certificates from fctry partition! Error: %d\n", err);
  } else {
      Serial.println("[DEBUG] Certificates loaded from fctry partition successfully!");
  }

  // Node and Devices initialization in the RainMaker ecosystem
  Node my_node = RMaker.initNode("Wartronick Hub");
  for (int i = 0; i < 8; i++) {
    channelNames[i] = "Channel " + String(i + 1);
    my_switches[i] = new Switch(channelNames[i].c_str(), &channelPins[i]);
    my_switches[i]->addCb(write_callback);
    my_node.addDevice(*my_switches[i]);
    delay(50);
  }
  
  // --- RAINMAKER SENSOR DEVICES ---
  // 1. Temperature Device (Standard type for Google Home compatibility)
  temp_sensor = new TemperatureSensor("Temperature");
  my_node.addDevice(*temp_sensor);
  
  // 2. Humidity Device
  hum_sensor = new Device("Humidity", "esp.device.other");
  Param hum_param("Humidity", "custom.param.humidity", value(0.0f), PROP_FLAG_READ);
  hum_param.addUIType(ESP_RMAKER_UI_TEXT);
  hum_sensor->addParam(hum_param);
  hum_sensor->assignPrimaryParam((param_handle_t *)hum_param.getParamHandle());
  my_node.addDevice(*hum_sensor);
  
  // 3. Pressure Device
  press_sensor = new Device("Pressure", "esp.device.other");
  Param press_param("Pressure", "custom.param.pressure", value(0.0f), PROP_FLAG_READ);
  press_param.addUIType(ESP_RMAKER_UI_TEXT);
  press_sensor->addParam(press_param);
  press_sensor->assignPrimaryParam((param_handle_t *)press_param.getParamHandle());
  my_node.addDevice(*press_sensor);
  
  // 4. Light Device
  light_sensor = new Device("Light", "esp.device.other");
  Param light_param("Brightness", "custom.param.light", value(0), PROP_FLAG_READ);
  light_param.addUIType(ESP_RMAKER_UI_TEXT);
  light_sensor->addParam(light_param);
  light_sensor->assignPrimaryParam((param_handle_t *)light_param.getParamHandle());
  my_node.addDevice(*light_sensor);

  // Secondary services disabled to save memory and avoid node registration crashes
  // RMaker.enableOTA(OTA_USING_PARAMS);
  // RMaker.enableTZService();
  
  // Direct Wi-Fi Connection (Bypassing BLE Payload Bug)
  Serial.println("\n[SYSTEM] Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD"); // CHANGE THIS
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\n[SYSTEM] Wi-Fi Connected Successfully!");
  
  // Start RainMaker
  RMaker.start();
}

void loop() {
  // 1. Network processing and maintenance (Local MQTT)
  if (!mqtt_client.connected()) {
    int currentState = mqtt_client.state();
    if (millis() - lastReconnectAttempt > 4900) {
       // Print the error right before reconnecting to avoid flooding the serial
       Serial.printf("\n[MQTT] Connection Dropped! PubSubClient Error Code: %d\n", currentState);
    }
    reconnectMQTT();
  } else {
    mqtt_client.loop();
  }

  // 2. Panel buttons scan
  static unsigned long lastScan = 0;
  if (millis() - lastScan > 50) { 
    uint8_t raw_buttons = tm.readButtons();
    
    // Ignore only the error noise (255)
    if (raw_buttons != 255 && raw_buttons != lastButtons) {
      for (int i = 0; i < 8; i++) {
        if ((raw_buttons & (1 << i)) && !(lastButtons & (1 << i))) {
           updateChannel(i, !channelStates[i], true, true);
        }
      }
      lastButtons = raw_buttons;
    }
    lastScan = millis();
  }

  // 3. Display Rotation Logic (Carousel)
  if (showingStatus) {
    if (millis() - displayTimer > 2000) {
      showingStatus = false;
      displayTimer = millis(); // Reset timer for the carousel
    }
  } else {
    // Rotate sensor data every 3 seconds
    static int displayState = 0;
    if (millis() - displayTimer > 3000) {
      displayTimer = millis();
      char buffer[9]; // 8 characters + null terminator
      
      if (displayState == 0) {
        snprintf(buffer, sizeof(buffer), "t   %-4.1f", official_temp); // "t   24.5"
      } else if (displayState == 1) {
        snprintf(buffer, sizeof(buffer), "H   %-4d", (int)cache_dht_h); // "H   45  "
      } else if (displayState == 2) {
        snprintf(buffer, sizeof(buffer), "P   %-4d", (int)cache_bmp_p); // "P   913 "
      } else if (displayState == 3) {
        snprintf(buffer, sizeof(buffer), "L   %-4d", cache_ldr); // "L   100 "
      }
      
      tm.displayText(buffer);
      displayState = (displayState + 1) % 4; // Move to next sensor
    }
  }

  // 4. Sensor Reading (Staggered to avoid RainMaker MQTT Budget limits)
  static unsigned long lastSensorRead = 0;
  static int sensorState = 0; // 0=BMP, 1=DHT, 2=LDR

  if (millis() - lastSensorRead > 3000) { // Every 3 seconds, reads one sensor
    lastSensorRead = millis();
    
    if (sensorState == 0) {
      // BMP280 Reading
      if (bmp_status) {
        float t_bmp = bmp.readTemperature();
        float p_bmp = bmp.readPressure() / 100.0F;
        cache_bmp_t = t_bmp;
        cache_bmp_p = p_bmp;
        Serial.printf("[SENSOR] BMP280 -> Temp: %.1f C | Press: %.1f hPa\n", t_bmp, p_bmp);
        
        press_sensor->updateAndReportParam("Pressure", p_bmp);
        
        if (mqtt_client.connected()) {
          if (!mqtt_client.publish("home_lab/sensor/pressure", String(p_bmp).c_str())) Serial.println("[MQTT] ERROR: Publish pressure failed!");
        }
      }
      sensorState = 1;
    } 
    else if (sensorState == 1) {
      // DHT11 Reading
      float h_dht = dht.readHumidity();
      float t_dht = dht.readTemperature();
      if (!isnan(h_dht) && !isnan(t_dht)) {
        cache_dht_h = h_dht;
        cache_dht_t = t_dht;
        Serial.printf("[SENSOR] DHT11 -> Temp: %.1f C | Humidity: %.1f %%\n", t_dht, h_dht);
        
        // Sensor Fusion (Weighted Average: 80% BMP, 20% DHT)
        if (cache_bmp_t > 0) {
          official_temp = (cache_bmp_t * 0.8) + (t_dht * 0.2);
        } else {
          official_temp = t_dht; // Fallback if BMP fails
        }
        Serial.printf("[SENSOR] FUSION -> Official Temp: %.1f C\n", official_temp);
        
        temp_sensor->updateAndReportParam("Temperature", official_temp);
        hum_sensor->updateAndReportParam("Humidity", h_dht);
        
        if (mqtt_client.connected()) {
          if (!mqtt_client.publish("home_lab/sensor/temperature", String(official_temp).c_str())) Serial.println("[MQTT] ERROR: Publish temperature failed!");
          if (!mqtt_client.publish("home_lab/sensor/humidity", String(h_dht).c_str())) Serial.println("[MQTT] ERROR: Publish humidity failed!");
        }
      } else {
        Serial.println("[SENSOR] DHT11 -> Read failed! Wiring or warm-up issue.");
      }
      sensorState = 2;
    } 
    else if (sensorState == 2) {
      // LDR Reading (Mapped to 0-100% with Calibration)
      int raw_ldr = analogRead(LDR_PIN);
      
      // LDR Calibration: Total darkness is approx 800 raw value, bright light is 0
      int raw_constrained = raw_ldr;
      if (raw_constrained > 800) raw_constrained = 800;
      
      int ldr_percent = map(raw_constrained, 800, 0, 0, 100);
      if (ldr_percent < 0) ldr_percent = 0;
      if (ldr_percent > 100) ldr_percent = 100;
      
      cache_ldr = ldr_percent;
      Serial.printf("[SENSOR] LDR -> Raw: %d | Brightness: %d %%\n", raw_ldr, ldr_percent);
      
      light_sensor->updateAndReportParam("Brightness", ldr_percent);
      
      if (mqtt_client.connected()) {
        if (!mqtt_client.publish("home_lab/sensor/light", String(ldr_percent).c_str())) Serial.println("[MQTT] ERROR: Publish light failed!");
      }
      sensorState = 0;
    }
  }

  // 5. Factory Reset safety logic (BOOT Button / GPIO 0)
  if (digitalRead(0) == LOW) {
    delay(100);
    int startTime = millis();
    while (digitalRead(0) == LOW) {
      delay(50);
      mqtt_client.loop();
    }
    if ((millis() - startTime) > 5000) {
      Serial.println("[SYSTEM] Factory Reset Triggered!");
      RMakerFactoryReset(2);
    }
  }
}
