# 🧊 Smart Fridge IoT Project

An IoT-based Smart Fridge system designed to monitor food inventory, temperature, and humidity in real-time. The project helps reduce food waste, improve organization, and provide remote access through a web dashboard.

---

## 📌 Features

* 🌡️ Real-time temperature monitoring
* 💧 Real-time humidity monitoring
* ⚖️ Food quantity monitoring using load cells
* 📱 Remote monitoring through a web dashboard
* 🚪 Door status detection using Reed Switch
* 🖥️ TFT display for local information visualization
* 🌀 Automatic fan control using PWM
* ☁️ Cloud data storage and management

---

## 🏗️ System Architecture

```text
+-------------+
|   Sensors   |
+-------------+
      |
      v
+-------------+
|    ESP32    |
+-------------+
      |
   Wi-Fi
      |
      v
+-------------+
|  Firebase   |
|   Cloud DB  |
+-------------+
      |
      v
+-------------+
| Dashboard / |
|   Web App   |
+-------------+
```

---

## 🔧 Hardware Components

| Component           | Purpose                       |
| ------------------- | ----------------------------- |
| ESP32               | Main microcontroller          |
| DHT22               | Temperature & Humidity Sensor |
| HX711               | Load Cell Amplifier           |
| Load Cell           | Weight Measurement            |
| ILI9341 TFT Display | Local User Interface          |
| LED                 | Status Indicator              |
| PWM Fan             | Cooling Control               |
| Reed Switch         | Door Open/Close Detection     |

---

## 🔌 Wiring Connections

### DHT22

| DHT22 | ESP32   |
| ----- | ------- |
| VCC   | 3.3V    |
| GND   | GND     |
| DATA  | GPIO 32 |

### HX711

| HX711 | ESP32   |
| ----- | ------- |
| VCC   | 3.3V    |
| GND   | GND     |
| DT    | GPIO 16 |
| SCK   | GPIO 17 |

### ILI9341 TFT Display

| TFT Pin | ESP32   |
| ------- | ------- |
| VCC     | 3.3V    |
| GND     | GND     |
| CS      | GPIO 15 |
| RESET   | GPIO 4  |
| DC      | GPIO 2  |
| MOSI    | GPIO 23 |
| MISO    | GPIO 19 |
| SCK     | GPIO 18 |
| LED     | 3.3V    |

### LED

| LED     | ESP32   |
| ------- | ------- |
| Anode   | GPIO 26 |
| Cathode | GND     |

### PWM Fan

| Fan | ESP32               |
| --- | ------------------- |
| PWM | GPIO 25             |
| GND | GND                 |
| VCC | External 12V Supply |

### Reed Switch

| Reed Switch | ESP32   |
| ----------- | ------- |
| Pin 1       | GND     |
| Pin 2       | GPIO 27 |

---

## ⚙️ Main Functionalities

* Display temperature and humidity in real time
* Monitor food stock levels
* Detect fridge door status
* Control cooling fan speed
* Send data to Firebase Cloud
* View system status remotely through a dashboard

---

## 📊 Data Flow

1. Sensors collect environmental and inventory data.
2. ESP32 processes the information.
3. Data is transmitted via Wi-Fi.
4. Firebase stores and manages the data.
5. Users access the information through the dashboard.

---

## 🎯 Benefits

* Reduce food waste
* Better food organization
* Maintain optimal storage conditions
* Improve convenience and accessibility
* Future integration with Smart Home systems

---

## 🚀 Future Improvements

* Product recognition using ESP32-CAM + YOLOv8
* Expiration date tracking
* Automatic shopping list generation
* Mobile application support
* Automatic online grocery ordering

---

## 👨‍💻 Technologies Used

* ESP32
* Arduino Framework
* Firebase
* Wi-Fi Communication
* DHT22 Library
* HX711 Library
* TFT_eSPI / Adafruit GFX
* IoT Concepts

---

## 📄 License

This project is developed for educational and research purposes.
