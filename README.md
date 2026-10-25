# 🇨🇿 Czech Blade

**Czech Blade** is an open-source multifunctional utility tool (multitool) developed for **ESP32** development boards. It focuses on wireless network analysis, hardware diagnostics, and real-time control, built using the **PlatformIO** ecosystem.

Official Project Hub: [misbo8.cz/projects/czech-knife](https://misbo8.cz)

---

## 🛠️ How to Get Started & Install (Via VS Code)
Currently, the project is in **active development** and can be compiled and uploaded **only using the developer environment.**

1. Clone or download this repository to your computer.
2. Open the project folder inside **Visual Studio Code** (make sure you have the **PlatformIO** extension installed).
3. PlatformIO will automatically fetch and configure all required dependencies (such as `ESPAsyncWebServer`, `ArduinoJson`, and `ESP32Ping`) specified in `platformio.ini`.
4. Plug your ESP32 board into your computer using a USB data cable.
5. Click the **Upload** arrow icon in the bottom status bar of VS Code.

*Note for regular users: A simpler, one-click web installation method using binary files (.bin) is planned for future releases!*

---

## 🎛️ Key Features (v1.0 - Core Web Edition)

- 📶 **Network Mode & Connectivity (AP + STA)**
  - Broadcasts its own Access Point (`Czech Blade`, password `12345678`) at `192.168.4.1`.
  - Connects to 2.4 GHz Wi-Fi networks in client mode.
  - Live header status indicator showing online/offline status, latency, and current AP name.

- 🔍 **Wi-Fi Scanner & Quick Actions**
  - Scans nearby 2.4 GHz networks with RSSI (dBm), channel, and BSSID details.
  - Interactive network items: click any network to open the connection dialog.
  - **📡 Measure**: Instant shortcut to target and track the selected router.

- 🎯 **Geiger Signal Radar**
  - Acoustic and visual signal tracking simulating a radio Geiger counter (higher pitch for stronger signals).
  - Dual tracking: tracks distance to a selected Wi-Fi router, or measures the connected phone's signal directly to the ESP32.

- 📡 **BLE Beacon Scanner**
  - 3-second active scan discovering Bluetooth Low Energy beacons, wearables, AirTags, and sensors.
  - Displays device names, MAC addresses, and RSSI strength.

- 🌐 **Ping Connectivity Tester**
  - ICMP diagnostic test running 12 packets against targets (default: `1.1.1.1`).
  - Calculates packet loss and average latency in ms.
  - Automatic offline protection warning if not connected to an internet-enabled network.

- ⚙️ **Settings & Hardware Indication**
  - Web-based AP management: change SSID and password on the fly with automatic AP reboot.
  - Onboard status LED (GPIO 2) indicating booting, scanning, and ping activity.

---

## 🗺️ Roadmap & Upcoming Features
- 💻 **Interactive Web Terminal & Telnet** – Real-time command execution over WebSockets and Telnet.
- 🌐 **Web Serial Flasher** – One-click browser flashing with merged binaries.
- 🌐 **Localization** – Full multi-language support (English / Czech).

---

© 2026 Misbo 8 • Czech Blade Project • [misbo8.cz](https://misbo8.cz)