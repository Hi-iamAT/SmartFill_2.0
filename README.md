# SmartFill 2.0 — smart water monitoring and pump control

SmartFill uses an ESP32, an HC-SR04 ultrasonic sensor, an OLED, indicator LEDs, a buzzer, and a relay to monitor a tank and control its pump. Level measurement and pump safety run on the ESP32. Wi-Fi is used to send readings to the SmartFill server and receive remote commands; automatic control continues locally when the network is unavailable.

> **Wiring note:** The supplied `connection.png` is a component-level reference, but some of its GPIO routes differ from the current firmware. Use the pin table below and in the [connection guide](https://hi-iamat.github.io/SmartFill_2.0/connection.html) as the source of truth when wiring this sketch. Do not copy a diagram route where it conflicts with the table.

## Project files

- `water_meter/water_meter.ino` — ESP32 firmware.
- `water_meter/secrets.h.example` — configuration template; copy it to `water_meter/secrets.h` and enter credentials.
- `connection.png` — supplied wiring illustration.
- `connection.html` — detailed connection instructions and diagram.
- `assets/image-1.jpeg` … `assets/image-5.jpeg` — project build photos.

## Build photos

These photos document the prototype assembly and powered operation. They are visual references; the firmware pin map and connection guide define the current wiring.

| Prototype assembly | Prototype assembly | Powered indicators |
|---|---|---|
| ![SmartFill prototype assembly, photo 1](assets/image-1.jpeg) | ![SmartFill prototype assembly, photo 2](assets/image-2.jpeg) | ![SmartFill powered prototype](assets/image-5.jpeg) |

| Sensor and controller wiring | Prototype wiring detail |
|---|---|
| ![SmartFill wiring detail, photo 3](assets/image-3.jpeg) | ![SmartFill wiring detail, photo 4](assets/image-4.jpeg) |

## What it does

### Water measurement

The firmware takes five HC-SR04 readings, discards readings outside the 2–400 cm range, and uses the median of the valid readings. It calculates the water level from the measured distance and a saved empty-tank reference. The sensor is mounted above the tank, pointing down at the water; a 6 cm rim offset is reserved as unusable space to keep the water below the sensor’s blind zone.

The OLED shows calibration status, level percentage, water height, free space, Wi-Fi status, control mode, and actual pump state. The green, blue, and red LEDs turn on at the configured 25%, 50%, and 75% thresholds; the buzzer pulses at the configured buzzer threshold (default 90%).

### Pump operation

- **AUTO (default):** starts the pump when the measured level is at or below 10% and stops at 50%. This hysteresis is hard-coded in the sketch, so automatic control does not depend on Wi-Fi.
- **MANUAL:** applies the server’s ON/OFF command. A manual ON command expires if no successful command poll arrives for 30 seconds.
- **Local safeguards:** pump output is set OFF at boot and is stopped when the sensor reading is invalid, calibration is missing, the level reaches 50%, or continuous runtime reaches 15 minutes.
- **Buttons:** a short press of OK begins empty-tank calibration. Hold RESET for 2.5 seconds to erase calibration. A short RESET press while in MANUAL cancels the current ON request locally.

The current implementation has one 50% local stop threshold for both AUTO and MANUAL. The remote settings adjust the display/alarm thresholds and upload interval; they do not change the firmware’s 10% AUTO start, 50% stop, or 15-minute runtime safety limits.

### Server communication

When online, the ESP32 posts readings to `/api/readings` (default interval 30 seconds) and polls `/api/device-command` every 5 seconds. The reading includes sensor measurements, Wi-Fi signal strength, firmware version, control mode, and pump state. Remote commands are requested by the device and still pass through local safety checks before the relay changes.

## Wiring and connections

Open the [SmartFill connection guide](connection.html) for the supplied diagram, pin-by-pin table, power guidance, and first power-up sequence.

| Device | ESP32 connection | Notes |
|---|---|---|
| HC-SR04 VCC / GND | 5V / GND | Use a regulated 5 V supply appropriate to the sensor. |
| HC-SR04 TRIG | GPIO 5 | 3.3 V ESP32 output is used for the trigger. |
| HC-SR04 ECHO | GPIO 18 through divider | HC-SR04 ECHO can be 5 V. Use a divider; see the connection guide. |
| SSD1306 OLED SDA / SCL | GPIO 21 / GPIO 22 | I²C, address `0x3C`; connect VCC and GND per module rating. |
| Green / blue / red LEDs | GPIO 15 / GPIO 16 / GPIO 17 | Each LED needs its own series resistor (220–330 Ω). Confirm LED polarity. |
| Active buzzer module | GPIO 23 via driver | Firmware drives HIGH for ON. Use the NPN driver arrangement described in the guide. |
| OK / RESET buttons | GPIO 32 / GPIO 33 | Each momentary switch connects its GPIO to GND; firmware uses `INPUT_PULLUP`. |
| Relay module IN | GPIO 19 | Firmware currently defines `RELAY_ACTIVE_LOW = false`; verify the actual relay board before connecting the pump. |

The sketch’s exact pin definitions are at the top of `water_meter/water_meter.ino`. The supplied diagram includes a battery/charger/buck-converter arrangement and routing that may not match this sketch; follow the guide’s current map. Never connect a pump directly to an ESP32 pin. Use a correctly rated relay and a separate supply sized for the pump’s voltage and startup current.

## Setup

1. In Arduino IDE, select an ESP32 Dev Module-compatible board and install **ArduinoJson 7**, **Adafruit GFX Library**, and **Adafruit SSD1306**.
2. Copy `water_meter/secrets.h.example` to `water_meter/secrets.h`.
3. Set `WIFI_SSID`, `WIFI_PASSWORD`, `SERVER_URL`, `DEVICE_CODE`, and `DEVICE_API_KEY`. Register the device in the SmartFill service and copy its API key when shown. Do not commit `secrets.h`.
4. Check the pin map and relay polarity, then flash `water_meter/water_meter.ino`.
5. Open Serial Monitor at **115200 baud**. Empty the tank and position the sensor. Press OK to begin calibration, then press OK again when the OLED/Serial prompt asks to save the empty reference. Calibration is stored in ESP32 preferences and survives reboot.
6. Test sensor display and relay behavior with the pump disconnected before connecting the pump supply.

## Calibration and measurement notes

- Mount the sensor level and securely above the maximum water surface, aimed away from tank walls, braces, and inlet streams.
- `SENSOR_TO_RIM_OFFSET_CM` is currently 6 cm. Adjust it to the measured sensor-face-to-safe-fill distance if the installation differs; keep the water surface outside the HC-SR04 blind zone.
- Calibrate with the tank empty and the sensor in its final position. A changed sensor mount or tank geometry requires recalibration.
- The `free_space_cm` value subtracts the 6 cm safety offset from measured distance. Level percentage uses the empty reference minus that offset as usable tank height.
- Firmware currently calls `tls.setInsecure()`. This skips HTTPS certificate verification and is suitable only for a controlled prototype; production deployment should configure trusted certificate validation.

## Troubleshooting

| Symptom | Check |
|---|---|
| OLED not found | SDA/SCL on GPIO 21/22, power, ground, and I²C address `0x3C`. |
| Sensor error or unstable level | Common ground, TRIG/ECHO orientation, ECHO divider, sensor mounting, and clear reflections from the water. |
| Pump acts inverted | Check relay module active level and change `RELAY_ACTIVE_LOW` only after testing with pump disconnected. |
| Pump remains off | Confirm calibration, valid sensor reading, level below the stop point, relay wiring, and that the 15-minute limit has not been reached. |
| No server updates | Check 2.4 GHz Wi-Fi availability, credentials, server URL, device code/API key, and Serial Monitor HTTP messages. Local AUTO control can still work offline. |

## Safety

Mains wiring and pump circuits can cause electric shock, fire, or equipment damage. Keep mains-side work isolated and have it performed by a qualified person. Use a relay module and power supply rated for the pump’s voltage, running current, and startup current; include suitable fusing and enclosure/strain relief. Keep low-voltage ESP32 wiring separated from mains. Test relay logic with the pump disconnected first.
