# W-LAB Smart Controller ⚡

A fully integrated, 3D-printed, open-source smart lab automation controller based on the ESP32.

![Final Assembly](images/final_assembly.png)

This project transforms an ESP32 into a central command hub for an automation lab (or any workshop), bringing physical controls, sensor fusion, and cloud capabilities into a single beautifully 3D-printed case.

### 📱 Cloud & UI
![RainMaker UI](images/rainmaker_ui.jpg)

## 🚀 Features

*   **8-Channel Relay Control:** Independently control up to 8 high-voltage/high-current devices (lights, tools, soldering irons) via physical buttons, the RainMaker App, or MQTT.
*   **TM1638 Control Panel:** Features a retro-industrial 7-segment display that acts as a "Carousel", rotating through live sensor data (`t`, `H`, `P`, `L`) every 3 seconds. The 8 buttons on the panel act as physical overrides for the 8 relays.
*   **Sensor Fusion (BMP280 + DHT11):** Calculates a highly accurate "Official Temperature" using a weighted average (80% BMP280, 20% DHT11). It also records Relative Humidity and Atmospheric Pressure.
*   **Luminosity Tracking (LDR):** A self-calibrated LDR circuit tracks ambient light from 0% (total darkness) to 100% (bright light).
*   **Parasitic Power Architecture:** Due to extreme pin constraints (11 pins used by relays and display), the sensors are powered directly by ESP32 GPIO pins acting as fake 3.3V and GND rails.
*   **ESP RainMaker Integration:** Full integration with the ESP RainMaker cloud for remote iOS/Android control and seamless Google Home / Alexa voice commands.
*   **MQTT Telemetry:** Streams live sensor data to local MQTT topics (`home_lab/sensor/...`) for direct ingestion into Home Assistant.

## 🗜️ Hardware & CAD

The `/cad` directory contains the full SolidWorks assembly (`.SLDASM`, `.SLDPRT`) and ready-to-print `Box.STL` files. 

The custom 3D printed case features:
- Central mounting for an ESP32 DevKit V1 with screw-terminal breakout boards.
- Symmetric dual 4-channel relay mounts.
- Custom wire routing channels to keep high-voltage and low-voltage lines organized.
- Top mounting for the TM1638 display and LDR sensor.

## 🔌 Pinout & Wiring

**Warning on Boot Pins:** Do not use GPIO 0, 2, or 15 for sensors, as they are strapping pins and will cause boot failures depending on sensor states.

### Relays (8 Channels)
*   Relay 1: `GPIO 13`
*   Relay 2: `GPIO 12`
*   Relay 3: `GPIO 14`
*   Relay 4: `GPIO 27`
*   Relay 5: `GPIO 26`
*   Relay 6: `GPIO 25`
*   Relay 7: `GPIO 33`
*   Relay 8: `GPIO 32`

### TM1638 Display
*   STB: `GPIO 4`
*   CLK: `GPIO 18`
*   DIO: `GPIO 19`

### Sensors (I2C & Analog)
*   **I2C SDA (BMP280):** `GPIO 21`
*   **I2C SCL (BMP280):** `GPIO 22`
*   **LDR Signal:** `GPIO 34` (ADC1)
*   **LDR VCC:** Physical `3V3`
*   **LDR GND:** Physical `GND`

![LDR Soldering](images/ldr_soldering.png)

### Parasitic Power Rails (The "Hack")
*   **DHT11 Signal:** `GPIO 5`
*   **Shared Sensor GND:** `GPIO 16` (Driven LOW in setup)
*   **DHT11 VCC:** `GPIO 17` (Driven HIGH in setup)

## 📡 MQTT Integration (Home Assistant)

![MQTT Telemetry](images/mqtt_explorer.png)

The controller publishes telemetry data to the following topics:
*   `home_lab/sensor/temperature`
*   `home_lab/sensor/humidity`
*   `home_lab/sensor/pressure`
*   `home_lab/sensor/light`

It also subscribes to `home_lab/relay/#` for external relay control.

## 🛠️ Usage

1. Flash the `src/Rainmaker_Final/Rainmaker_Final.ino` using the Arduino IDE (Requires ESP32 core v2.0.x and ESP RainMaker libraries).
2. Scan the generated QR Code in the Serial Monitor using the ESP RainMaker app.
3. Configure your local Wi-Fi credentials via BLE.
4. Mount it on your wall and enjoy the clicking sounds of automation!
