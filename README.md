# 🇨🇿 Czech Blade

**Czech Blade** is an open-source multifunctional utility tool (multitool) developed for **ESP32** development boards. It focuses on wireless network analysis, hardware diagnostics, and real-time control, built using the **PlatformIO** ecosystem.

Official Project Hub: [www.misbo8.cz/projects/czech-knife](https://misbo8.cz)

---

## 🛠️ How to Get Started & Install (Via VS Code)
Currently, the project is in **active development** and can be compiled and uploaded **only using the developer environment.**

1. Clone or download this repository to your computer.
2. Open the project folder inside **Visual Studio Code** (make sure you have the **PlatformIO** extension installed).
3. PlatformIO will automatically handle everything on the background and fetch all required dependencies (such as `ESPAsyncWebServer`) specified in the `platformio.ini` file.
4. Plug your ESP32 board into your computer using a USB data cable.
5. Click the **Upload** arrow icon in the bottom status bar of VS Code.

*Note for regular users: A simpler, one-click web installation method using binary files (.bin) is planned and will be added later in future updates!*

---

## 🎛️ Key Features (Blades)
* 🔍 **Wi-Fi Scanner** – Discover nearby networks, view broadcasting channels, and measure RSSI signal strength.
* 💻 **Web Terminal** – Real-time command-line control of your ESP32 over WebSockets (*currently in development*).
* ℹ️ **System Info** – Get live hardware specifications, flash size, and available free heap memory status.
