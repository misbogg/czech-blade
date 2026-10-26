#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ESP32Ping.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <vector>

#define LED_PIN 2

Preferences prefs;
String apSSID = "Czech Blade";
String apPass = "12345678";
bool ledEnabled = true;
String currentLang = "eng"; // Výchozí jazyk: English

// Stavy pro potvrzení příkazů
bool pendingRebootWeb = false;
bool pendingRebootTelnet = false;

AsyncWebServer server(80);
WiFiServer telnetServer(23);
WiFiClient telnetClient;

// Telnet buffer, kurzor a historie
String telnetInputBuffer = "";
int telnetCursorPos = 0;
const int TELNET_HIST_SIZE = 10;
String telnetHistory[TELNET_HIST_SIZE];
int telnetHistCount = 0;
int telnetHistIdx = -1;

BLEScan* pBLEScan = nullptr;

// Paměť pro očíslované Wi-Fi sítě
struct ScannedNet {
  String ssid;
  int rssi;
  int ch;
  String bssid;
};
std::vector<ScannedNet> lastScannedNets;

// Paměť pro neblokující BLE výsledky
struct BleDeviceItem {
  String name;
  String mac;
  int rssi;
};
std::vector<BleDeviceItem> bleResults;
volatile bool isBleScanning = false;

// Seznam příkazů pro automatické doplňování (TAB completion)
const std::vector<String> commandList = {
  "help", "wifi help", "wifi scan", "wifi connect", "wifi disconnect", "wifi status", "wifi signal",
  "wifi-repeat", "signal", "bt help", "bt scan", "ping", "set help",
  "set setap", "set lang", "set lang-d", "status", "status ip", "log",
  "led", "led on", "led off", "sleep", "reboot", "clear"
};

// ANSI barvy pro Telnet
#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_RED    "\033[31m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE   "\033[34m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_ORANGE "\033[38;5;208m"

void blinkLed(int times, int delayMs) {
  if (!ledEnabled) return;
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    delay(delayMs);
  }
}

void setLedState(bool state) {
  if (ledEnabled && state) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }
}

// Signalizace startu verze 3.0: 3x dlouhé bliknutí
void blinkVersion3_0() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(1000);
    digitalWrite(LED_PIN, LOW);
    delay(400);
  }
}

// Sjednocený přechod do spánku
void executeSleep(int sleepSec = 0) {
  digitalWrite(LED_PIN, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();

  if (sleepSec > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  }
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Probuzení tlačítkem BOOT (GPIO 0) nebo EN/RST
  esp_deep_sleep_start();
}

String resolveSSID(String param) {
  param.trim();
  if (param.startsWith("n ") || param.startsWith("N ")) {
    int num = param.substring(2).toInt();
    if (num >= 1 && num <= (int)lastScannedNets.size()) {
      return lastScannedNets[num - 1].ssid;
    }
  }
  return param;
}

// Callback po dokončení BLE skenu
void onBleScanComplete(BLEScanResults results) {
  bleResults.clear();
  for (int i = 0; i < results.getCount(); i++) {
    BLEAdvertisedDevice dev = results.getDevice(i);
    BleDeviceItem item;
    item.name = dev.haveName() ? dev.getName().c_str() : "";
    item.mac = dev.getAddress().toString().c_str();
    item.rssi = dev.getRSSI();
    bleResults.push_back(item);
  }
  pBLEScan->clearResults();
  digitalWrite(LED_PIN, LOW);
  isBleScanning = false;
}

// Jádro parseru příkazů pro Web Shell i Telnet
String processCommand(String cmd, bool isTelnet = false) {
  cmd.trim();
  String out = "";
  bool cz = (currentLang == "cz");

  if (cmd == "settings") cmd = "set";
  else if (cmd.startsWith("settings ")) {
    cmd = "set " + cmd.substring(9);
  }

  if (cmd == "hack" || cmd == "matrix") cmd = "log";

  auto col = [&](const char* ansiCode) -> String {
    return isTelnet ? String(ansiCode) : String("");
  };

  // HLAVNÍ NÁPOVĚDA
  if (cmd == "help") {
    if (cz) {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== CzechBlade prikazy ===" + col(ANSI_RESET) + "\n"
            " - " + col(ANSI_GREEN) + "wifi" + col(ANSI_RESET) + "         : Wi-Fi prikazy (viz 'wifi help')\n"
            " - " + col(ANSI_GREEN) + "signal" + col(ANSI_RESET) + "       : Signal telefonu k modulu\n"
            " - " + col(ANSI_GREEN) + "bt" + col(ANSI_RESET) + "           : Bluetooth LE prikazy (viz 'bt help')\n"
            " - " + col(ANSI_GREEN) + "ping" + col(ANSI_RESET) + "         : Test odezvy (samotny pingne 1.1.1.1)\n"
            " - " + col(ANSI_GREEN) + "set" + col(ANSI_RESET) + "          : Nastaveni zarizeni (viz 'set help')\n"
            " - " + col(ANSI_GREEN) + "status" + col(ANSI_RESET) + "       : Stav systemu (nebo 'status ip')\n"
            " - " + col(ANSI_GREEN) + "log" + col(ANSI_RESET) + "          : Hackersky log\n"
            " - " + col(ANSI_GREEN) + "led" + col(ANSI_RESET) + "          : Prepinac LED (nebo 'led on/off')\n"
            " - " + col(ANSI_GREEN) + "sleep [sec]" + col(ANSI_RESET) + "  : Uspani modulu (probuzeni BOOT/RST)\n"
            " - " + col(ANSI_GREEN) + "reboot" + col(ANSI_RESET) + "       : Restart modulu s potvrzenim [y/N]\n"
            " - " + col(ANSI_GREEN) + "clear" + col(ANSI_RESET) + "        : Vymaze konzoli\n"
            "Tip: Stisknete TAB pro automaticke doplneni prikazu.";
    } else {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== CzechBlade commands ===" + col(ANSI_RESET) + "\n"
            " - " + col(ANSI_GREEN) + "wifi" + col(ANSI_RESET) + "         : Wi-Fi commands (see 'wifi help')\n"
            " - " + col(ANSI_GREEN) + "signal" + col(ANSI_RESET) + "       : Phone signal to module\n"
            " - " + col(ANSI_GREEN) + "bt" + col(ANSI_RESET) + "           : Bluetooth LE commands (see 'bt help')\n"
            " - " + col(ANSI_GREEN) + "ping" + col(ANSI_RESET) + "         : Latency test (defaults to 1.1.1.1)\n"
            " - " + col(ANSI_GREEN) + "set" + col(ANSI_RESET) + "          : Device settings (see 'set help')\n"
            " - " + col(ANSI_GREEN) + "status" + col(ANSI_RESET) + "       : System status (or 'status ip')\n"
            " - " + col(ANSI_GREEN) + "log" + col(ANSI_RESET) + "          : Hacking log\n"
            " - " + col(ANSI_GREEN) + "led" + col(ANSI_RESET) + "          : Toggle LED (or 'led on/off')\n"
            " - " + col(ANSI_GREEN) + "sleep [sec]" + col(ANSI_RESET) + "  : Sleep module (wake up via BOOT/RST)\n"
            " - " + col(ANSI_GREEN) + "reboot" + col(ANSI_RESET) + "       : Restart module with confirmation [y/N]\n"
            " - " + col(ANSI_GREEN) + "clear" + col(ANSI_RESET) + "        : Clear console\n"
            "Tip: Press TAB for auto-completion.";
    }
  } 
  else if (cmd == "wifi") {
    out = cz ? "Neznamy prikaz. Pouzijte 'wifi help' pro prehled Wi-Fi prikazu." 
             : "Unknown command. Use 'wifi help' for Wi-Fi commands list.";
  }
  else if (cmd == "wifi help") {
    if (cz) {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== Wi-Fi prikazy ===" + col(ANSI_RESET) + "\n"
            " - wifi scan                  : Vyhleda a ocisluje site v okoli (1, 2, 3...)\n"
            " - wifi connect <SSID|n X>    : Pripojeni k siti podle nazvu nebo cisla (napr. 'wifi connect n 1')\n"
            " - wifi disconnect            : Odpojeni od routeru\n"
            " - wifi status                : Stav aktualniho pripojeni k siti\n"
            " - wifi signal [SSID|n X]     : Mereni signalu pripojene site (nebo vybrane ze scanu)\n"
            " - wifi signal-<sec> [cil]    : Mereni signalu na X sekund (napr. 'wifi signal-10 n 1')\n"
            " - wifi-repeat <SSID|n X>     : Klonovani site jako novy AP (heslo 12345678)";
    } else {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== Wi-Fi commands ===" + col(ANSI_RESET) + "\n"
            " - wifi scan                  : Scans and numbers nearby networks (1, 2, 3...)\n"
            " - wifi connect <SSID|n X>    : Connect by SSID or scan number (e.g. 'wifi connect n 1')\n"
            " - wifi disconnect            : Disconnect from network\n"
            " - wifi status                : Current Wi-Fi connection status\n"
            " - wifi signal [SSID|n X]     : Measure signal of connected or scanned network\n"
            " - wifi signal-<sec> [target] : Measure signal for X seconds (e.g. 'wifi signal-10 n 1')\n"
            " - wifi-repeat <SSID|n X>     : Clone network as new AP (password 12345678)";
    }
  }
  else if (cmd == "wifi status") {
    if (WiFi.status() == WL_CONNECTED) {
      out = col(ANSI_GREEN) + (cz ? "Wi-Fi: PRIPOJENO\n" : "Wi-Fi: CONNECTED\n") + col(ANSI_RESET);
      out += "SSID: " + col(ANSI_YELLOW) + WiFi.SSID() + col(ANSI_RESET) + "\n";
      out += "IP: " + WiFi.localIP().toString() + "\n";
      out += "Signal: " + String(WiFi.RSSI()) + " dBm";
    } else {
      out = col(ANSI_RED) + (cz ? "Wi-Fi: NEPRIPOJENO (Offline)" : "Wi-Fi: DISCONNECTED (Offline)") + col(ANSI_RESET);
    }
  }
  else if (cmd == "wifi scan") {
    setLedState(true);
    int n = WiFi.scanNetworks(false, true);
    lastScannedNets.clear();

    out = cz ? ("Nalezeno " + String(n) + " Wi-Fi siti:\n") : ("Found " + String(n) + " Wi-Fi networks:\n");
    for (int i = 0; i < n; i++) {
      ScannedNet item;
      item.ssid = WiFi.SSID(i);
      item.rssi = WiFi.RSSI(i);
      item.ch = WiFi.channel(i);
      item.bssid = WiFi.BSSIDstr(i);
      lastScannedNets.push_back(item);

      out += col(ANSI_CYAN) + String(i + 1) + ". " + col(ANSI_RESET);
      out += "[" + col(ANSI_YELLOW) + String(item.rssi) + " dBm" + col(ANSI_RESET) + "] ";
      out += col(ANSI_BOLD) + (item.ssid.length() > 0 ? item.ssid : "(Hidden)") + col(ANSI_RESET);
      out += " (k." + String(item.ch) + ", " + item.bssid + ")\n";
    }
    WiFi.scanDelete();
    setLedState(false);
  }
  else if (cmd.startsWith("wifi connect")) {
    String params = cmd.substring(12);
    params.trim();
    String sSsid = "";
    String sPass = "";

    if (params.startsWith("n ") || params.startsWith("N ")) {
      String sub = params.substring(2);
      sub.trim();
      int spaceIdx = sub.indexOf(' ');
      if (spaceIdx > 0) {
        int idx = sub.substring(0, spaceIdx).toInt();
        sPass = sub.substring(spaceIdx + 1);
        sPass.trim();
        if (idx >= 1 && idx <= (int)lastScannedNets.size()) {
          sSsid = lastScannedNets[idx - 1].ssid;
        }
      } else {
        int idx = sub.toInt();
        if (idx >= 1 && idx <= (int)lastScannedNets.size()) {
          sSsid = lastScannedNets[idx - 1].ssid;
        }
      }
    } else {
      int spaceIdx = params.indexOf(' ');
      if (spaceIdx > 0) {
        sSsid = params.substring(0, spaceIdx);
        sPass = params.substring(spaceIdx + 1);
        sPass.trim();
      } else {
        sSsid = params;
      }
    }

    if (sSsid.length() == 0) {
      return col(ANSI_RED) + (cz ? "Chyba: Zadejte platne SSID nebo 'n <cislo>'!" : "Error: Specify valid SSID or 'n <number>'!") + col(ANSI_RESET);
    }

    out = cz ? ("Pripojuji k '" + sSsid + "'...") : ("Connecting to '" + sSsid + "'...");
    setLedState(true);
    WiFi.begin(sSsid.c_str(), sPass.length() > 0 ? sPass.c_str() : NULL);
    int t = 0;
    while (WiFi.status() != WL_CONNECTED && t < 25) {
      delay(200);
      t++;
    }
    setLedState(false);

    if (WiFi.status() == WL_CONNECTED) {
      out += col(ANSI_GREEN) + (cz ? ("\nPripojeno! IP adresa: " + WiFi.localIP().toString()) : ("\nConnected! IP address: " + WiFi.localIP().toString())) + col(ANSI_RESET);
    } else {
      out += col(ANSI_RED) + (cz ? "\nChyba: Pripojeni selhalo." : "\nError: Connection failed.") + col(ANSI_RESET);
    }
  }
  else if (cmd == "wifi disconnect") {
    WiFi.disconnect(false, true);
    out = cz ? "Rozhrani STA odpojeno od routeru." : "STA interface disconnected from router.";
  }
  else if (cmd.startsWith("wifi signal")) {
    int durationSec = 5;
    String targetParam = "";

    String sigPart = cmd.substring(5);
    sigPart.trim();

    if (sigPart.startsWith("signal-")) {
      int spaceIdx = sigPart.indexOf(' ');
      if (spaceIdx > 0) {
        durationSec = sigPart.substring(7, spaceIdx).toInt();
        targetParam = sigPart.substring(spaceIdx + 1);
      } else {
        durationSec = sigPart.substring(7).toInt();
      }
    } else if (sigPart.startsWith("signal ")) {
      targetParam = sigPart.substring(7);
    }

    if (durationSec <= 0) durationSec = 5;
    targetParam.trim();

    String target = "";
    if (targetParam.length() > 0) {
      target = resolveSSID(targetParam);
    } else {
      if (WiFi.status() == WL_CONNECTED) {
        target = WiFi.SSID();
      } else {
        return col(ANSI_RED) + (cz ? "Chyba: Nejste pripojeni k zadne Wi-Fi siti!" : "Error: Not connected to any Wi-Fi!") + col(ANSI_RESET);
      }
    }

    out = (cz ? "Merim signal pro '" : "Measuring signal for '") + target + "' (" + String(durationSec) + "s stream):\n";
    for (int sec = 1; sec <= durationSec; sec++) {
      int curRssi = -105;
      if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == target) {
        curRssi = WiFi.RSSI();
      } else {
        setLedState(true);
        int n = WiFi.scanNetworks(false, true);
        for (int i = 0; i < n; i++) {
          if (WiFi.SSID(i) == target && WiFi.RSSI(i) > curRssi) {
            curRssi = WiFi.RSSI(i);
          }
        }
        WiFi.scanDelete();
        setLedState(false);
      }

      if (curRssi > -105) {
        out += " [T+" + String(sec) + "s] " + col(ANSI_YELLOW) + String(curRssi) + " dBm" + col(ANSI_RESET) + "\n";
      } else {
        out += " [T+" + String(sec) + "s] " + col(ANSI_RED) + (cz ? "Mimo dosah" : "Out of range") + col(ANSI_RESET) + "\n";
      }

      if (sec < durationSec) delay(1000);
    }
    out.trim();
  }
  else if (cmd.startsWith("wifi-repeat")) {
    String param = cmd.length() > 11 ? cmd.substring(11) : "";
    param.trim();
    String target = resolveSSID(param);

    if (target.length() == 0 && WiFi.status() == WL_CONNECTED) {
      target = WiFi.SSID();
    }

    if (target.length() == 0) {
      return col(ANSI_RED) + (cz ? "Chyba: Zadejte SSID nebo cislo site 'n <cislo>'!" : "Error: Specify SSID or number 'n <number>'!") + col(ANSI_RESET);
    }

    apSSID = target + "_EXT";
    apPass = "12345678";
    out = cz ? ("Klonuji sit na SSID: '" + apSSID + "' s heslem '12345678'. Restartuji AP...") : ("Cloning network to SSID: '" + apSSID + "' with pass '12345678'. Restarting AP...");
    blinkLed(4, 50);
    WiFi.softAPdisconnect(true);
    WiFi.softAP(apSSID.c_str(), apPass.c_str());
  }
  else if (cmd == "signal") {
    wifi_sta_list_t sta_list;
    esp_wifi_ap_get_sta_list(&sta_list);
    if (sta_list.num > 0) {
      out = (cz ? "Signal pripojeneho telefonu: " : "Connected phone signal: ") + col(ANSI_YELLOW) + String(sta_list.sta[0].rssi) + " dBm" + col(ANSI_RESET);
    } else {
      out = col(ANSI_RED) + (cz ? "K Access Pointu Czech Blade neni aktualne pripojen zadny klient." : "No clients currently connected to Czech Blade Access Point.") + col(ANSI_RESET);
    }
  }
  else if (cmd == "bt") {
    out = cz ? "Pouziti: 'bt scan' pro sken majaku nebo 'bt help' pro napovedu."
             : "Usage: 'bt scan' to scan beacons or 'bt help' for help.";
  }
  else if (cmd == "bt help") {
    if (cz) {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== Bluetooth LE prikazy ===" + col(ANSI_RESET) + "\n"
            " - bt scan : 2sekundovy sken BLE majaku v okoli (AirTagy, hodinky, senzory)";
    } else {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== Bluetooth LE commands ===" + col(ANSI_RESET) + "\n"
            " - bt scan : 2-second scan of nearby BLE beacons (AirTags, watches, sensors)";
    }
  }
  else if (cmd == "bt scan") {
    out = cz ? "Skenuji BLE v okoli (2 sekundy)...\n" : "Scanning BLE devices (2 seconds)...\n";
    setLedState(true);
    pBLEScan->stop();
    pBLEScan->clearResults();
    BLEScanResults found = pBLEScan->start(2, false);
    out += (cz ? "Nalezeno " : "Found ") + String(found.getCount()) + " BLE devices:\n";
    for (int i = 0; i < found.getCount(); i++) {
      BLEAdvertisedDevice d = found.getDevice(i);
      out += " - [" + col(ANSI_YELLOW) + String(d.getRSSI()) + " dBm" + col(ANSI_RESET) + "] " + 
             col(ANSI_BOLD) + (d.haveName() ? d.getName().c_str() : "(Unknown)") + col(ANSI_RESET) + 
             " (" + d.getAddress().toString().c_str() + ")\n";
    }
    pBLEScan->clearResults();
    setLedState(false);
  }
  else if (cmd == "ping" || cmd.startsWith("ping ")) {
    String target = "1.1.1.1";
    if (cmd.startsWith("ping ")) {
      target = cmd.substring(5);
      target.trim();
      if (target == "1") target = "1.1.1.1";
    }

    if (WiFi.status() != WL_CONNECTED) {
      out = col(ANSI_RED) + (cz ? "Chyba: Czech Blade neni pripojen k Wi-Fi (offline)." : "Error: Czech Blade is not connected to Wi-Fi (offline).") + col(ANSI_RESET);
    } else {
      setLedState(true);
      int success = 0;
      float total = 0;
      for (int i = 0; i < 4; i++) {
        if (Ping.ping(target.c_str(), 1)) {
          success++;
          total += Ping.averageTime();
        }
        delay(20);
      }
      setLedState(false);

      if (success > 0) {
        out = col(ANSI_GREEN) + "Ping -> " + target + ": " + String(success) + "/4 " + (cz ? "paketu OK | Prumer: " : "packets OK | Avg: ") + String((int)(total / success)) + " ms" + col(ANSI_RESET);
      } else {
        out = col(ANSI_RED) + "Ping -> " + target + (cz ? ": Hostitel neodpovida." : ": Host unreachable.") + col(ANSI_RESET);
      }
    }
  }
  else if (cmd == "set") {
    out = cz ? "Pouziti: 'set help' pro napovedu k nastaveni."
             : "Usage: 'set help' for settings help.";
  }
  else if (cmd == "set help") {
    if (cz) {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== Nastaveni ===" + col(ANSI_RESET) + "\n"
            " - set setap <SSID> <heslo> : Zmena nazvu a hesla AP site (min. 8 znaku)\n"
            " - set lang <cz|eng>        : Docasna zmena jazyka pro tuto relaci\n"
            " - set lang-d <cz|eng>      : Trvala zmena vychoziho jazyka do pameti";
    } else {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== Settings ===" + col(ANSI_RESET) + "\n"
            " - set setap <SSID> <pass>  : Change AP network SSID and password (min. 8 chars)\n"
            " - set lang <cz|eng>        : Temporarily change language for this session\n"
            " - set lang-d <cz|eng>      : Permanently save default language to memory";
    }
  }
  else if (cmd.startsWith("set setap ")) {
    String params = cmd.substring(10);
    params.trim();
    int spaceIdx = params.indexOf(' ');
    if (spaceIdx > 0) {
      String nSsid = params.substring(0, spaceIdx);
      String nPass = params.substring(spaceIdx + 1);
      nPass.trim();
      if (nPass.length() < 8) {
        out = col(ANSI_RED) + (cz ? "Chyba: Heslo musi mit alespon 8 znaku!" : "Error: Password must be at least 8 characters long!") + col(ANSI_RESET);
      } else {
        apSSID = nSsid;
        apPass = nPass;
        out = col(ANSI_GREEN) + (cz ? ("AP prenastaveno na '" + apSSID + "'. Restartuji Wi-Fi AP...") : ("AP reconfigured to '" + apSSID + "'. Restarting AP...")) + col(ANSI_RESET);
        blinkLed(4, 50);
        WiFi.softAPdisconnect(true);
        WiFi.softAP(apSSID.c_str(), apPass.c_str());
      }
    } else {
      out = cz ? "Pouziti: set setap <SSID> <heslo>" : "Usage: set setap <SSID> <password>";
    }
  }
  else if (cmd.startsWith("set lang ")) {
    String l = cmd.substring(9);
    l.trim();
    l.toLowerCase();
    if (l == "cz" || l == "eng" || l == "en") {
      currentLang = (l == "cz") ? "cz" : "eng";
      out = col(ANSI_GREEN) + ((currentLang == "cz") ? "Docasny jazyk nastaven na: Cestina (CZ)" : "Temporary language set to: English (ENG)") + col(ANSI_RESET);
    } else {
      out = cz ? "Pouziti: set lang <cz|eng>" : "Usage: set lang <cz|eng>";
    }
  }
  else if (cmd.startsWith("set lang-d ")) {
    String l = cmd.substring(11);
    l.trim();
    l.toLowerCase();
    if (l == "cz" || l == "eng" || l == "en") {
      currentLang = (l == "cz") ? "cz" : "eng";
      prefs.begin("blade", false);
      prefs.putString("lang", currentLang);
      prefs.end();
      out = col(ANSI_GREEN) + ((currentLang == "cz") ? "Trvaly vychozi jazyk ulozen: Cestina (CZ)" : "Default permanent language saved: English (ENG)") + col(ANSI_RESET);
    } else {
      out = cz ? "Pouziti: set lang-d <cz|eng>" : "Usage: set lang-d <cz|eng>";
    }
  }
  else if (cmd == "status ip") {
    out = col(ANSI_BOLD) + "AP  IP: " + col(ANSI_RESET) + WiFi.softAPIP().toString() + "\n";
    out += col(ANSI_BOLD) + "STA IP: " + col(ANSI_RESET) + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Offline"));
  }
  else if (cmd == "status" || cmd == "info") {
    out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== CZECH BLADE SYSTEM INFO ===" + col(ANSI_RESET) + "\n";
    out += "AP SSID: " + col(ANSI_YELLOW) + apSSID + col(ANSI_RESET) + " (IP: " + WiFi.softAPIP().toString() + ")\n";
    out += "STA Status: " + String(WiFi.status() == WL_CONNECTED ? ((cz ? "Pripojeno k " : "Connected to ") + col(ANSI_YELLOW) + WiFi.SSID() + col(ANSI_RESET) + " (IP: " + WiFi.localIP().toString() + ")") : (cz ? "Nepripojeno" : "Disconnected")) + "\n";
    out += "MAC AP: " + WiFi.softAPmacAddress() + "\n";
    out += "MAC STA: " + WiFi.macAddress() + "\n";
    out += (cz ? "Volna RAM: " : "Free RAM: ") + String(ESP.getFreeHeap() / 1024) + " KB\n";
    out += (cz ? "LED indikace: " : "LED indicator: ") + String(ledEnabled ? (cz ? "ZAPNUTA" : "ENABLED") : (cz ? "VYPNUTA" : "DISABLED")) + "\n";
    out += (cz ? "Jazyk: " : "Language: ") + (currentLang == "cz" ? String("CZ") : String("ENG"));
  }
  else if (cmd == "log") {
    out = col(ANSI_GREEN) +
          "[*] INITIATING PENETRATION SEQUENCE (v4.82-sec)...\n"
          "[*] Target host: 192.168.1.1 (subnet probe active)\n"
          "[*] Port scanning [21, 22, 23, 80, 443, 8080]... open: [22, 80]\n"
          "[*] Probing memory stack offset: 0x7FFF" + String(random(1000, 9999), HEX) + "\n"
          "[+] Heap corruption detected. Injecting NOP sled (128 bytes)...\n"
          "[+] Return pointer hijacked: [EIP -> 0x0040" + String(random(10, 99), HEX) + "]\n"
          "[*] Evading signature IDS/IPS filters... [BYPASSED]\n"
          "[*] Dumping shadow credential storage (SHA512):\n"
          "    > [UID 0] root: $6$z8F9..." + String(random(100000, 999999)) + " -> Hash MATCH!\n"
          "    > [UID 1000] operator: $6$dK31..." + String(random(100000, 999999)) + " -> Hash MATCH!\n"
          "[+] Cryptographic handshake established via port 1337\n"
          "[*] Deploying daemon backdoor to /tmp/.sys_kernel_hook\n"
          "[!] ELEVATING USER PRIVILEGES: [UID=0(root) GID=0(root)]\n"
          "[*] SESSION ACTIVE. ENCRYPTED SHELL READY." + col(ANSI_RESET);
  }
  else if (cmd == "led") {
    ledEnabled = !ledEnabled;
    if (ledEnabled) {
      blinkLed(3, 80);
      out = cz ? "Indikacni LED zapnuta (3x problikla)." : "Indicator LED enabled (blinked 3x).";
    } else {
      digitalWrite(LED_PIN, LOW);
      out = cz ? "Indikacni LED vypnuta." : "Indicator LED disabled.";
    }
  }
  else if (cmd == "led on") {
    ledEnabled = true;
    blinkLed(3, 80);
    out = cz ? "Indikacni LED zapnuta (3x problikla)." : "Indicator LED enabled (blinked 3x).";
  }
  else if (cmd == "led off") {
    ledEnabled = false;
    digitalWrite(LED_PIN, LOW);
    out = cz ? "Indikacni LED vypnuta (bude trvale zhasnuta)." : "Indicator LED disabled (permanently off).";
  }
  else if (cmd == "sleep" || cmd.startsWith("sleep ")) {
    int sleepSec = 0;
    if (cmd.startsWith("sleep ")) {
      sleepSec = cmd.substring(6).toInt();
    }

    if (sleepSec > 0) {
      out = cz ? ("Czech Blade usina na " + String(sleepSec) + " sekund... (probuzeni tlacitkem BOOT/RST nebo casovacem)") 
               : ("Czech Blade going to sleep for " + String(sleepSec) + " seconds... (wake up via BOOT/RST or timer)");
    } else {
      out = cz ? "Czech Blade usina... (probuzeni tlacitkem BOOT nebo EN/RST na desce)" 
               : "Czech Blade going to sleep... (wake up via BOOT or EN/RST button)";
    }

    if (isTelnet) {
      telnetClient.println(out);
      telnetClient.flush();
      delay(200);
      executeSleep(sleepSec);
    }
  }
  else {
    out = col(ANSI_RED) + (cz ? ("Neznamy prikaz: '" + cmd + "'. Napiste 'help'.") : ("Unknown command: '" + cmd + "'. Type 'help'.")) + col(ANSI_RESET);
  }

  return out;
}

const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>Czech Blade</title>
  <style>
    :root {
      --bg: #0d0f12;
      --surface: #15181f;
      --surface-sub: #1c212a;
      --border: #28303d;
      --border-bright: #3c485c;
      --text: #e7ebf2;
      --muted: #798599;
      --orange: #f97316;
      --orange-bg: rgba(249, 115, 22, 0.12);
      --green: #10b981;
      --red: #ef4444;
      --mono: ui-monospace, "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace;
    }

    * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
    body {
      margin: 0;
      background: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      font-size: 13px;
      padding-bottom: 75px;
    }

    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 10px 14px;
      background: var(--surface);
      border-bottom: 1px solid var(--border);
      position: sticky;
      top: 0;
      z-index: 100;
    }
    .brand-box {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .brand-id {
      font-weight: 800;
      letter-spacing: 0.5px;
      color: var(--orange);
      font-size: 14px;
      display: flex;
      align-items: center;
      gap: 6px;
    }
    .status-tags {
      display: flex;
      gap: 6px;
    }
    .tag {
      font-family: var(--mono);
      font-size: 10px;
      padding: 3px 8px;
      border: 1px solid var(--border);
      background: var(--surface-sub);
      color: var(--muted);
      border-radius: 6px;
      display: flex;
      align-items: center;
      gap: 5px;
    }
    .tag.online { color: var(--green); border-color: rgba(16, 185, 129, 0.4); }
    .tag.offline { color: var(--red); border-color: rgba(239, 68, 68, 0.4); }

    .btn-cfg {
      background: var(--surface-sub);
      border: 1px solid var(--border);
      color: var(--text);
      padding: 6px 10px;
      border-radius: 8px;
      cursor: pointer;
      font-family: var(--mono);
      font-size: 12px;
      font-weight: bold;
    }
    .btn-cfg:active { background: var(--border-bright); }

    .nav-bar {
      display: flex;
      position: fixed;
      bottom: 0;
      left: 0;
      right: 0;
      background: var(--surface);
      border-top: 1px solid var(--border);
      z-index: 100;
    }
    .nav-tab {
      flex: 1;
      padding: 10px 0;
      border: none;
      background: transparent;
      color: var(--muted);
      font-size: 11px;
      font-weight: 700;
      cursor: pointer;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 3px;
      border-top: 2px solid transparent;
      margin-top: -1px;
    }
    .nav-tab.active {
      color: var(--orange);
      background: var(--orange-bg);
      border-top-color: var(--orange);
    }

    .container {
      padding: 12px;
      max-width: 600px;
      margin: 0 auto;
    }
    .panel { display: none; }
    .panel.active { display: block; }

    .block {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 14px;
      margin-bottom: 12px;
    }
    .block-header {
      font-family: var(--mono);
      font-size: 11px;
      font-weight: bold;
      color: var(--muted);
      letter-spacing: 0.5px;
      border-bottom: 1px solid var(--border);
      padding-bottom: 8px;
      margin-bottom: 12px;
      text-transform: uppercase;
    }

    .btn-exec {
      width: 100%;
      padding: 11px;
      border: 1px solid var(--orange);
      background: var(--orange-bg);
      color: var(--orange);
      font-family: var(--mono);
      font-size: 12px;
      font-weight: 700;
      letter-spacing: 0.5px;
      cursor: pointer;
      border-radius: 8px;
      transition: background 0.15s, color 0.15s;
    }
    .btn-exec:active {
      background: var(--orange);
      color: #0d0f12;
    }
    .btn-exec.stop-mode {
      border-color: var(--red);
      background: rgba(239, 68, 68, 0.12);
      color: var(--red);
    }

    ul { list-style: none; padding: 0; margin: 10px 0 0 0; }
    .row {
      background: var(--surface-sub);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 9px 12px;
      margin-bottom: 6px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      cursor: pointer;
      gap: 8px;
    }
    .row:active { border-color: var(--orange); }
    .row-meta {
      font-family: var(--mono);
      font-size: 10px;
      color: var(--muted);
      margin-top: 3px;
    }
    .rssi-box {
      font-family: var(--mono);
      font-size: 11px;
      font-weight: 700;
      padding: 3px 7px;
      border: 1px solid var(--border);
      background: var(--bg);
      border-radius: 6px;
      white-space: nowrap;
    }
    .rssi-box.good { color: var(--green); border-color: rgba(16, 185, 129, 0.35); }
    .rssi-box.mid { color: var(--orange); border-color: rgba(249, 115, 22, 0.35); }
    .rssi-box.poor { color: var(--red); border-color: rgba(239, 68, 68, 0.35); }

    .sub-btn {
      font-family: var(--mono);
      font-size: 11px;
      font-weight: 700;
      padding: 5px 9px;
      border: 1px solid var(--border-bright);
      background: var(--surface);
      color: var(--text);
      cursor: pointer;
      border-radius: 6px;
    }
    .sub-btn:active { background: var(--orange); color: #0d0f12; border-color: var(--orange); }

    .meter-unit {
      text-align: center;
      padding: 14px 0;
      border: 1px solid var(--border);
      background: #0d0f13;
      margin-bottom: 12px;
      border-radius: 10px;
    }
    .meter-leds {
      display: flex;
      justify-content: center;
      gap: 4px;
      margin-bottom: 12px;
    }
    .led {
      width: 13px;
      height: 26px;
      background: #181b22;
      border: 1px solid #262b35;
      border-radius: 3px;
      transition: background 0.15s;
    }
    .led.on-orange { background: var(--orange); border-color: var(--orange); }
    .led.on-green { background: var(--green); border-color: var(--green); }
    .led.on-red { background: var(--red); border-color: var(--red); }

    .meter-value {
      font-family: var(--mono);
      font-size: 40px;
      font-weight: 800;
      color: #fff;
      line-height: 1;
    }
    .meter-status {
      font-family: var(--mono);
      font-size: 11px;
      color: var(--muted);
      margin-top: 6px;
      letter-spacing: 0.5px;
    }

    /* Terminál */
    .term-window {
      background: #080a0d;
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 12px;
      font-family: var(--mono);
      font-size: 12px;
      height: 300px;
      overflow-y: auto;
      white-space: pre-wrap;
      color: var(--text);
      line-height: 1.4;
    }
    .term-prompt { color: var(--green); font-weight: bold; }
    .term-row { display: flex; gap: 8px; margin-top: 8px; }
    .term-input {
      flex: 1;
      background: var(--surface-sub);
      border: 1px solid var(--border);
      color: #fff;
      padding: 9px 10px;
      font-family: var(--mono);
      font-size: 12px;
      border-radius: 8px;
    }

    input, select {
      width: 100%;
      padding: 9px 10px;
      margin: 6px 0 12px 0;
      background: var(--bg);
      border: 1px solid var(--border);
      color: #fff;
      font-family: var(--mono);
      font-size: 12px;
      border-radius: 8px;
    }
    input:focus, select:focus, .term-input:focus {
      outline: none;
      border-color: var(--orange);
    }
    .modal-overlay {
      display: none;
      position: fixed;
      inset: 0;
      background: rgba(0, 0, 0, 0.75);
      z-index: 200;
      align-items: center;
      justify-content: center;
      padding: 16px;
    }
    .modal-card {
      background: var(--surface);
      border: 1px solid var(--border-bright);
      padding: 16px;
      width: 100%;
      max-width: 320px;
      border-radius: 12px;
    }
    .modal-card h4 {
      margin: 0 0 12px 0;
      font-size: 13px;
      color: var(--orange);
      letter-spacing: 0.5px;
    }
    .copy-foot {
      margin-top: 14px;
      padding-top: 10px;
      border-top: 1px solid var(--border);
      text-align: center;
      font-family: var(--mono);
      font-size: 10px;
      color: var(--muted);
      letter-spacing: 0.2px;
    }
  </style>
</head>
<body>

  <header>
    <div class="brand-box">
      <span class="brand-id">⚡ CZECH_BLADE</span>
      <div class="status-tags">
        <div class="tag" id="tag-net">
          <span id="net-txt">STA: OFF</span>
        </div>
        <div class="tag" id="tag-ap" style="color:var(--orange);">AP: READY</div>
      </div>
    </div>
    <button class="btn-cfg" onclick="openSettings()">[CFG]</button>
  </header>

  <div class="container">

    <!-- 1. WI-FI PANEL -->
    <div id="p-wifi" class="panel active">
      <div class="block">
        <div class="block-header" id="hdr-wifi">WLAN CONTROLLER (2.4 GHz)</div>
        <button class="btn-exec" id="btn-scan-wifi" onclick="scanWifi()">SCAN ACTIVE FREQUENCIES</button>
        <ul id="wifi-list"></ul>
      </div>
    </div>

    <!-- 2. GEIGER TRACKER -->
    <div id="p-geiger" class="panel">
      <div class="block">
        <div class="block-header" id="geiger-target-title">SIGNAL DETECTOR</div>
        <div class="meter-unit">
          <div class="meter-leds">
            <div class="led" id="l1"></div><div class="led" id="l2"></div>
            <div class="led" id="l3"></div><div class="led" id="l4"></div>
            <div class="led" id="l5"></div><div class="led" id="l6"></div>
            <div class="led" id="l7"></div><div class="led" id="l8"></div>
            <div class="led" id="l9"></div><div class="led" id="l10"></div>
          </div>
          <div>
            <span class="meter-value" id="g-rssi">--</span>
            <span style="font-size:12px; color:var(--muted);">dBm</span>
          </div>
          <div class="meter-status" id="g-desc">STANDBY</div>
        </div>
        <button class="btn-exec" id="g-btn" onclick="toggleGeiger()">ENGAGE RECEIVER</button>
      </div>
    </div>

    <!-- 3. BLE MONITOR -->
    <div id="p-ble" class="panel">
      <div class="block">
        <div class="block-header" id="hdr-ble">BLUETOOTH LE SNIFFER</div>
        <button class="btn-exec" id="btn-scan-ble" onclick="scanBle()">CAPTURE BEACONS (2s)</button>
        <ul id="ble-list"></ul>
      </div>
    </div>

    <!-- 4. PING DIAGNOSTICS -->
    <div id="p-ping" class="panel">
      <div class="block">
        <div class="block-header" id="hdr-ping">ICMP DIAGNOSTICS</div>
        <label style="font-size:10px; color:var(--muted);" id="lbl-ping">TARGET IP / HOST:</label>
        <input type="text" id="ping-target" value="1.1.1.1">
        <button class="btn-exec" id="btn-run-ping" onclick="runPing()">EXECUTE PING x12</button>
        <div id="ping-output" style="margin-top: 10px; font-size:11px;"></div>
      </div>
    </div>

    <!-- 5. TERMINAL -->
    <div id="p-term" class="panel">
      <div class="block">
        <div class="term-window" id="term-out"></div>
        <div class="term-row">
          <input type="text" id="term-in" class="term-input" placeholder="Type a command... (TAB to complete)" onkeydown="handleTermKey(event)">
          <button class="sub-btn" style="padding: 0 16px;" onclick="sendTermCmd()">&gt;</button>
        </div>
      </div>
    </div>

  </div>

  <div class="nav-bar">
    <button class="nav-tab active" onclick="switchTab('wifi', this)"><span id="nav-text-wifi">WIFI</span></button>
    <button class="nav-tab" onclick="switchTab('geiger', this)"><span id="nav-text-geiger">TRACK</span></button>
    <button class="nav-tab" onclick="switchTab('ble', this)"><span id="nav-text-ble">BLE</span></button>
    <button class="nav-tab" onclick="switchTab('ping', this)"><span id="nav-text-ping">PING</span></button>
    <button class="nav-tab" onclick="switchTab('term', this)"><span id="nav-text-term">SHELL</span></button>
  </div>

  <!-- MODAL: AUTH / CONNECT -->
  <div class="modal-overlay" id="modal-connect">
    <div class="modal-card">
      <h4 id="modal-connect-title">CONNECT TO NETWORK</h4>
      <input type="password" id="modal-connect-pass" placeholder="Password (blank if open)">
      <div style="display:flex; gap:6px;">
        <button class="btn-exec" id="btn-modal-connect" onclick="submitConnect()">CONNECT</button>
        <button class="sub-btn" style="flex:1;" id="btn-modal-cancel" onclick="closeModal('modal-connect')">CANCEL</button>
      </div>
    </div>
  </div>

  <!-- MODAL: SYSTEM SETTINGS -->
  <div class="modal-overlay" id="modal-settings">
    <div class="modal-card">
      <h4 id="modal-settings-title">SYSTEM CONFIGURATION</h4>
      
      <label style="font-size:10px; color:var(--muted);" id="lbl-lang">INTERFACE LANGUAGE:</label>
      <select id="lang-select" onchange="changeLanguage(this.value)">
        <option value="eng">English (ENG)</option>
        <option value="cz">Čeština (CZ)</option>
      </select>

      <label style="font-size:10px; color:var(--muted);" id="lbl-ap-ssid">BROADCAST SSID:</label>
      <input type="text" id="ap-name-input">

      <label style="font-size:10px; color:var(--muted);" id="lbl-ap-pass">ACCESS KEY (min. 8 chars):</label>
      <input type="password" id="ap-pass-input">

      <div style="display:flex; gap:6px;">
        <button class="btn-exec" id="btn-save-settings" onclick="submitSettings()">SAVE & REBOOT</button>
        <button class="sub-btn" style="flex:1;" id="btn-close-settings" onclick="closeModal('modal-settings')">CLOSE</button>
      </div>

      <div class="copy-foot">
        &copy; 2026 Misbo 8 &bull; Czech Blade Project &bull; misbo8.cz
      </div>
    </div>
  </div>

  <script>
    let currentLang = "eng";
    let geigerActive = false;
    let geigerAbortCtrl = null;
    let audioCtx = null;
    let selectedSsid = "";
    let isConnectedToWifi = false;

    let cmdHistory = [];
    let historyIdx = -1;

    const tabCommands = [
      "help", "wifi help", "wifi scan", "wifi connect", "wifi disconnect", "wifi status", "wifi signal",
      "wifi-repeat", "signal", "bt help", "bt scan", "ping", "set help",
      "set setap", "set lang", "set lang-d", "status", "status ip", "log",
      "led", "led on", "led off", "sleep", "reboot", "clear"
    ];

    const dict = {
      cz: {
        nav_wifi: "WIFI", nav_geiger: "RADAR", nav_ble: "BLE", nav_ping: "PING", nav_term: "SHELL",
        hdr_wifi: "WLAN KONTROLÉR (2.4 GHz)", btn_scan_wifi: "SKENOVAT PÁSMO", scanning_wifi: "PROBÍHÁ SKENOVÁNÍ...",
        geiger_title: "ZAMĚŘOVAČ SIGNÁLU", geiger_standby: "POHOTOVOST",
        btn_geiger_start: "SPUSTIT ZAMĚŘOVÁNÍ", btn_geiger_stop: "ZASTAVIT", btn_measure: "ZAMĚŘIT",
        hdr_ble: "BLE MONITOR", btn_scan_ble: "ZACHYTIT MAJÁKY (2s)", scanning_ble: "SKENUJI PÁSMO...",
        hdr_ping: "ICMP DIAGNOSTIKA", lbl_ping: "CÍL TESTU / HOST:", btn_run_ping: "ODESLAT 12 PAKETŮ",
        ping_exec: "Odesílám pakety na ", ping_ok: "HOSTITEL AKTIVNÍ", ping_fail: "HOSTITEL NEDOSTUPNÝ",
        modal_conn_title: "PŘIPOJENÍ K SÍTI", modal_conn_btn: "PŘIPOJIT", modal_cancel_btn: "ZRUŠIT",
        settings_title: "KONFIGURACE SYSTÉMU", lbl_lang: "JAZYK ROZHRANÍ:", lbl_ap_ssid: "VYSÍLANÉ SSID:", lbl_ap_pass: "HESLO K AP:",
        btn_save_settings: "ULOŽIT A RESTART", btn_close_settings: "ZAVŘÍT",
        term_init: "Czech Blade Shell v3.0\nZadejte 'help' pro seznam prikazu. (TAB = doplnovani)\n------------------------------------------------"
      },
      eng: {
        nav_wifi: "WIFI", nav_geiger: "TRACK", nav_ble: "BLE", nav_ping: "PING", nav_term: "SHELL",
        hdr_wifi: "WLAN CONTROLLER (2.4 GHz)", btn_scan_wifi: "SCAN ACTIVE FREQUENCIES", scanning_wifi: "SCANNING AIRWAVES...",
        geiger_title: "SIGNAL DETECTOR", geiger_standby: "STANDBY",
        btn_geiger_start: "ENGAGE RECEIVER", btn_geiger_stop: "DISENGAGE", btn_measure: "TRACK",
        hdr_ble: "BLUETOOTH LE SNIFFER", btn_scan_ble: "CAPTURE BEACONS (2s)", scanning_ble: "SCANNING PROXIMITY...",
        hdr_ping: "ICMP DIAGNOSTICS", lbl_ping: "TARGET IP / HOST:", btn_run_ping: "EXECUTE PING x12",
        ping_exec: "Pinging target ", ping_ok: "HOST REACHABLE", ping_fail: "HOST UNREACHABLE",
        modal_conn_title: "CONNECT TO NETWORK", modal_conn_btn: "CONNECT", modal_cancel_btn: "CANCEL",
        settings_title: "SYSTEM CONFIGURATION", lbl_lang: "INTERFACE LANGUAGE:", lbl_ap_ssid: "BROADCAST SSID:", lbl_ap_pass: "ACCESS KEY:",
        btn_save_settings: "SAVE & REBOOT", btn_close_settings: "CLOSE",
        term_init: "Czech Blade Shell v3.0\nType 'help' for commands. (TAB = completion)\n------------------------------------------------"
      }
    };

    function setLanguage(lang) {
      currentLang = (lang === "cz") ? "cz" : "eng";
      document.getElementById('lang-select').value = currentLang;
      const t = dict[currentLang];

      document.getElementById('nav-text-wifi').innerText = t.nav_wifi;
      document.getElementById('nav-text-geiger').innerText = t.nav_geiger;
      document.getElementById('nav-text-ble').innerText = t.nav_ble;
      document.getElementById('nav-text-ping').innerText = t.nav_ping;
      document.getElementById('nav-text-term').innerText = t.nav_term;

      document.getElementById('hdr-wifi').innerText = t.hdr_wifi;
      document.getElementById('btn-scan-wifi').innerText = t.btn_scan_wifi;
      document.getElementById('geiger-target-title').innerText = t.geiger_title;
      if (!geigerActive) document.getElementById('g-btn').innerText = t.btn_geiger_start;
      
      document.getElementById('hdr-ble').innerText = t.hdr_ble;
      document.getElementById('btn-scan-ble').innerText = t.btn_scan_ble;
      document.getElementById('hdr-ping').innerText = t.hdr_ping;
      document.getElementById('lbl-ping').innerText = t.lbl_ping;
      document.getElementById('btn-run-ping').innerText = t.btn_run_ping;

      document.getElementById('modal-connect-title').innerText = t.modal_conn_title;
      document.getElementById('btn-modal-connect').innerText = t.modal_conn_btn;
      document.getElementById('btn-modal-cancel').innerText = t.modal_cancel_btn;
      document.getElementById('modal-settings-title').innerText = t.settings_title;
      document.getElementById('lbl-lang').innerText = t.lbl_lang;
      document.getElementById('lbl-ap-ssid').innerText = t.lbl_ap_ssid;
      document.getElementById('lbl-ap-pass').innerText = t.lbl_ap_pass;
      document.getElementById('btn-save-settings').innerText = t.btn_save_settings;
      document.getElementById('btn-close-settings').innerText = t.btn_close_settings;
    }

    async function changeLanguage(lang) {
      setLanguage(lang);
      await fetch('/api/set-lang?lang=' + encodeURIComponent(lang));
    }

    function switchTab(name, btn) {
      document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
      document.querySelectorAll('.nav-tab').forEach(b => b.classList.remove('active'));
      document.getElementById('p-' + name).classList.add('active');
      btn.classList.add('active');
      if (name !== 'geiger' && geigerActive) toggleGeiger();
    }

    function playBeep(freq) {
      if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      if (audioCtx.state === 'suspended') audioCtx.resume();
      let osc = audioCtx.createOscillator();
      let gain = audioCtx.createGain();
      osc.connect(gain);
      gain.connect(audioCtx.destination);
      osc.type = "sawtooth";
      osc.frequency.value = freq;
      gain.gain.value = 0.03;
      osc.start();
      osc.stop(audioCtx.currentTime + 0.025);
    }

    function updateSegments(rssi) {
      for (let i = 1; i <= 10; i++) {
        document.getElementById('l' + i).className = 'led';
      }
      if (rssi <= -105 || rssi === 0) return;
      
      let count = Math.max(1, Math.min(10, Math.round((rssi + 100) / 6)));
      for (let i = 1; i <= count; i++) {
        let el = document.getElementById('l' + i);
        if (i <= 3) el.classList.add('on-red');
        else if (i <= 7) el.classList.add('on-orange');
        else el.classList.add('on-green');
      }
    }

    async function scanWifi() {
      const list = document.getElementById('wifi-list');
      list.innerHTML = `<li style="color:var(--muted); text-align:center; padding:10px;">${dict[currentLang].scanning_wifi}</li>`;
      let res = await fetch('/api/scan-wifi');
      let data = await res.json();
      list.innerHTML = '';
      data.forEach((net, idx) => {
        let li = document.createElement('li');
        li.className = 'row';
        li.onclick = () => openConnectModal(net.ssid);

        let badgeClass = net.rssi >= -65 ? "good" : (net.rssi >= -78 ? "mid" : "poor");

        li.innerHTML = `
          <div>
            <div>
              <span style="color:var(--muted);">${idx + 1}.</span>
              <strong style="color:#fff;">${net.ssid || '[HIDDEN_SSID]'}</strong>
            </div>
            <div class="row-meta">CH ${net.ch} | ${net.bssid}</div>
          </div>
          <div style="display:flex; align-items:center; gap:8px;">
            <span class="rssi-box ${badgeClass}">${net.rssi} dBm</span>
            <button class="sub-btn" onclick="event.stopPropagation(); startTargetMeasure('${net.ssid}')">${dict[currentLang].btn_measure}</button>
          </div>
        `;
        list.appendChild(li);
      });
    }

    function startTargetMeasure(ssid) {
      selectedSsid = ssid;
      document.getElementById('geiger-target-title').innerText = "LOCK: " + ssid;
      switchTab('geiger', document.querySelectorAll('.nav-tab')[1]);
      if (!geigerActive) toggleGeiger();
    }

    function openConnectModal(ssid) {
      if (!ssid) return;
      selectedSsid = ssid;
      document.getElementById('modal-connect-pass').value = "";
      document.getElementById('modal-connect').style.display = "flex";
    }

    async function submitConnect() {
      let pass = document.getElementById('modal-connect-pass').value;
      closeModal('modal-connect');
      checkInternetStatus(true);
      let res = await fetch(`/api/connect?ssid=${encodeURIComponent(selectedSsid)}&pass=${encodeURIComponent(pass)}`);
      let data = await res.json();
      if (!data.connected) {
        alert("FAIL: Connection refused by target");
      }
      checkInternetStatus();
    }

    function toggleGeiger() {
      const btn = document.getElementById('g-btn');
      if (geigerActive) {
        geigerActive = false;
        if (geigerAbortCtrl) geigerAbortCtrl.abort();
        btn.innerText = dict[currentLang].btn_geiger_start;
        btn.classList.remove('stop-mode');
        document.getElementById('g-rssi').innerText = "--";
        document.getElementById('g-desc').innerText = dict[currentLang].geiger_standby;
        updateSegments(-105);
      } else {
        geigerActive = true;
        btn.innerText = dict[currentLang].btn_geiger_stop;
        btn.classList.add('stop-mode');
        runGeigerLoop();
      }
    }

    async function runGeigerLoop() {
      while (geigerActive) {
        try {
          geigerAbortCtrl = new AbortController();
          let url = selectedSsid.length > 0 
            ? `/api/track-ap?ssid=${encodeURIComponent(selectedSsid)}` 
            : '/api/client-rssi';

          let res = await fetch(url, { signal: geigerAbortCtrl.signal });
          let data = await res.json();
          let rssi = data.rssi;
          if (rssi && rssi > -105 && rssi !== 0) {
            document.getElementById('g-rssi').innerText = rssi;
            updateSegments(rssi);
            let desc = rssi >= -65 ? "OPTIMAL LINK" : (rssi >= -78 ? "DEGRADED" : "CRITICAL ATTENUATION");
            document.getElementById('g-desc').innerText = desc;
            playBeep(350 + Math.max(0, (rssi + 100) * 10));
          } else {
            document.getElementById('g-rssi').innerText = "--";
            document.getElementById('g-desc').innerText = "SIGNAL LOSS";
            updateSegments(-105);
          }
        } catch (e) {
          if (e.name === 'AbortError') break;
        }
        await new Promise(r => setTimeout(r, 600));
      }
    }

    async function scanBle() {
      const list = document.getElementById('ble-list');
      list.innerHTML = `<li style="color:var(--muted); text-align:center; padding:10px;">${dict[currentLang].scanning_ble}</li>`;
      
      try {
        await fetch('/api/scan-ble-start');
        await new Promise(r => setTimeout(r, 2200));

        let res = await fetch('/api/scan-ble-results');
        let data = await res.json();

        list.innerHTML = '';
        if (data.length === 0) {
          list.innerHTML = `<li style="color:var(--muted); text-align:center; padding:10px;">No BLE beacons detected in proximity.</li>`;
          return;
        }

        data.forEach(dev => {
          let li = document.createElement('li');
          li.className = 'row';
          let badgeClass = dev.rssi >= -70 ? "good" : "poor";
          li.innerHTML = `
            <div>
              <strong style="color:#fff;">${dev.name || '[ANONYMOUS_BEACON]'}</strong>
              <div class="row-meta">${dev.mac}</div>
            </div>
            <span class="rssi-box ${badgeClass}">${dev.rssi} dBm</span>
          `;
          list.appendChild(li);
        });
      } catch (err) {
        list.innerHTML = `<li style="color:var(--red); text-align:center; padding:10px;">[ERR] Scan I/O Fault</li>`;
      }
    }

    async function runPing() {
      let host = document.getElementById('ping-target').value.trim() || "1.1.1.1";
      let out = document.getElementById('ping-output');
      out.innerHTML = `<span style="color:var(--muted);">${dict[currentLang].ping_exec}${host}...</span>`;
      
      try {
        let res = await fetch('/api/ping?host=' + encodeURIComponent(host));
        let data = await res.json();
        if (data.success) {
          out.innerHTML = `<span style="color:var(--green); font-weight:bold;">${dict[currentLang].ping_ok}</span> | AVG: <strong>${data.avg_time} ms</strong> (${data.success_count}/12)`;
        } else {
          out.innerHTML = `<span style="color:var(--red); font-weight:bold;">${dict[currentLang].ping_fail}</span>`;
        }
      } catch (e) {
        out.innerHTML = `<span style="color:var(--red);">[ERR] Network Down</span>`;
      }
      checkInternetStatus();
    }

    // Web Shell: šipky Nahoru/Dolů + doplňování přes TAB
    function handleTermKey(e) {
      const input = document.getElementById('term-in');

      if (e.key === 'Tab') {
        e.preventDefault();
        let val = input.value;
        if (!val) return;

        let matches = tabCommands.filter(c => c.startsWith(val.toLowerCase()));
        if (matches.length === 1) {
          input.value = matches[0] + " ";
        } else if (matches.length > 1) {
          const out = document.getElementById('term-out');
          out.innerHTML += `\n<span style="color:var(--muted);">${matches.join('   ')}</span>\n<span class="term-prompt">esp@czechblade&gt;</span> ${val}`;
          out.scrollTop = out.scrollHeight;
        }
        return;
      }

      if (e.key === 'Enter') {
        sendTermCmd();
      } else if (e.key === 'ArrowUp') {
        if (cmdHistory.length > 0 && historyIdx < cmdHistory.length - 1) {
          historyIdx++;
          input.value = cmdHistory[cmdHistory.length - 1 - historyIdx];
        }
        e.preventDefault();
      } else if (e.key === 'ArrowDown') {
        if (historyIdx > 0) {
          historyIdx--;
          input.value = cmdHistory[cmdHistory.length - 1 - historyIdx];
        } else if (historyIdx === 0) {
          historyIdx = -1;
          input.value = "";
        }
        e.preventDefault();
      }
    }

    async function sendTermCmd() {
      const input = document.getElementById('term-in');
      const out = document.getElementById('term-out');
      const cmd = input.value.trim();
      if (!cmd) return;

      cmdHistory.push(cmd);
      historyIdx = -1;

      if (cmd.toLowerCase() === 'clear') {
        out.innerHTML = "";
        input.value = "";
        return;
      }

      out.innerHTML += `\n<span class="term-prompt">esp@czechblade&gt;</span> ${cmd}\n`;
      input.value = "";

      let loadingSpan = document.createElement("span");
      loadingSpan.style.color = "var(--muted)";
      out.appendChild(loadingSpan);

      let dots = 1;
      let loadTimer = setInterval(() => {
        dots = (dots % 4) + 1;
        loadingSpan.innerText = ".".repeat(dots);
        out.scrollTop = out.scrollHeight;
      }, 250);

      try {
        let res = await fetch('/api/terminal?cmd=' + encodeURIComponent(cmd));
        let reply = await res.text();
        clearInterval(loadTimer);
        loadingSpan.remove();
        out.innerHTML += `${reply}`;

        if (cmd.startsWith("set lang ") || cmd.startsWith("set lang-d ")) {
          let parts = cmd.split(" ");
          let l = parts[parts.length - 1].toLowerCase();
          if (l === "cz" || l === "eng" || l === "en") {
            setLanguage(l);
          }
        }
      } catch (e) {
        clearInterval(loadTimer);
        loadingSpan.remove();
        out.innerHTML += `[Connection Error]`;
      }
      out.scrollTop = out.scrollHeight;
    }

    async function checkInternetStatus(checking = false) {
      const tag = document.getElementById('tag-net');
      const txt = document.getElementById('net-txt');
      if (checking) {
        txt.innerText = "STA: SYNC...";
        return;
      }
      try {
        let res = await fetch('/api/status');
        let data = await res.json();
        isConnectedToWifi = data.wifi_connected;
        if (data.internet) {
          tag.className = "tag online";
          txt.innerText = "INET: " + data.ping + "ms";
        } else if (data.wifi_connected) {
          tag.className = "tag";
          txt.innerText = "LAN: NO_INET";
        } else {
          tag.className = "tag offline";
          txt.innerText = "STA: OFF";
        }
      } catch (e) {
        tag.className = "tag offline";
        txt.innerText = "STA: ERR";
      }
    }

    function openSettings() {
      document.getElementById('ap-name-input').value = "Czech Blade";
      document.getElementById('lang-select').value = currentLang;
      document.getElementById('modal-settings').style.display = "flex";
    }

    async function submitSettings() {
      let name = document.getElementById('ap-name-input').value;
      let pass = document.getElementById('ap-pass-input').value;
      if (!name) return;
      closeModal('modal-settings');
      await fetch(`/api/set-ap?ssid=${encodeURIComponent(name)}&pass=${encodeURIComponent(pass)}`);
    }

    function closeModal(id) {
      document.getElementById(id).style.display = "none";
    }

    fetch('/api/get-lang').then(r => r.text()).then(l => {
      setLanguage(l);
      document.getElementById('term-out').innerText = dict[currentLang].term_init;
    });

    setInterval(checkInternetStatus, 8000);
    checkInternetStatus();
  </script>
</body>
</html>
)rawhtml";

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  // Hardwarová indikace startu verze 3.0 (3x dlouhé bliknutí)
  blinkVersion3_0();

  prefs.begin("blade", false);
  currentLang = prefs.getString("lang", "eng");
  prefs.end();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID.c_str(), apPass.c_str());

  // Spuštění Telnet serveru na portu 23
  telnetServer.begin();
  telnetServer.setNoDelay(true);

  // Inicializace BLE
  BLEDevice::init("CzechBlade");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);

  // 1. Root
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", INDEX_HTML);
  });

  // 2. Wi-Fi Scanner
  server.on("/api/scan-wifi", HTTP_GET, [](AsyncWebServerRequest *request){
    setLedState(true);
    int n = WiFi.scanNetworks(false, true);
    lastScannedNets.clear();
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < n; ++i) {
      ScannedNet item;
      item.ssid = WiFi.SSID(i);
      item.rssi = WiFi.RSSI(i);
      item.ch = WiFi.channel(i);
      item.bssid = WiFi.BSSIDstr(i);
      lastScannedNets.push_back(item);

      JsonObject net = arr.createNestedObject();
      net["ssid"] = item.ssid;
      net["rssi"] = item.rssi;
      net["bssid"] = item.bssid;
      net["ch"] = item.ch;
    }
    WiFi.scanDelete();
    setLedState(false);

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 3. Connect to Router
  server.on("/api/connect", HTTP_GET, [](AsyncWebServerRequest *request){
    String ssid = request->hasParam("ssid") ? request->getParam("ssid")->value() : "";
    String pass = request->hasParam("pass") ? request->getParam("pass")->value() : "";

    setLedState(true);
    WiFi.begin(ssid.c_str(), pass.length() > 0 ? pass.c_str() : NULL);

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 25) {
      delay(200);
      timeout++;
    }
    setLedState(false);

    bool connected = (WiFi.status() == WL_CONNECTED);

    DynamicJsonDocument doc(128);
    doc["connected"] = connected;
    doc["ip"] = connected ? WiFi.localIP().toString() : "";

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 4. Measure AP Signal (Geiger)
  server.on("/api/track-ap", HTTP_GET, [](AsyncWebServerRequest *request){
    String target = request->hasParam("ssid") ? request->getParam("ssid")->value() : "";
    int rssi = -105;

    if (target.length() > 0) {
      setLedState(true);
      int n = WiFi.scanNetworks(false, true);
      for (int i = 0; i < n; ++i) {
        if (WiFi.SSID(i) == target) {
          if (WiFi.RSSI(i) > rssi) {
            rssi = WiFi.RSSI(i);
          }
        }
      }
      WiFi.scanDelete();
      setLedState(false);
    }

    DynamicJsonDocument doc(64);
    doc["rssi"] = rssi;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 5. Client Signal
  server.on("/api/client-rssi", HTTP_GET, [](AsyncWebServerRequest *request){
    wifi_sta_list_t sta_list;
    esp_wifi_ap_get_sta_list(&sta_list);

    int clientRssi = -105;
    if (sta_list.num > 0) {
      clientRssi = sta_list.sta[0].rssi;
    }

    DynamicJsonDocument doc(64);
    doc["rssi"] = clientRssi;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 6A. BLE Scanner - Start (Neblokující start)
  server.on("/api/scan-ble-start", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isBleScanning) {
      isBleScanning = true;
      setLedState(true);
      pBLEScan->stop();
      pBLEScan->clearResults();
      pBLEScan->start(2, onBleScanComplete, false);
    }
    request->send(200, "text/plain", "SCANNING");
  });

  // 6B. BLE Scanner - Results
  server.on("/api/scan-ble-results", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& dev : bleResults) {
      JsonObject obj = arr.createNestedObject();
      obj["name"] = dev.name;
      obj["mac"] = dev.mac;
      obj["rssi"] = dev.rssi;
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 7. Ping Diagnostics
  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request){
    String host = request->hasParam("host") ? request->getParam("host")->value() : "1.1.1.1";
    DynamicJsonDocument doc(128);

    if (WiFi.status() != WL_CONNECTED) {
      doc["success"] = false;
      doc["error"] = "Not connected";
      String response;
      serializeJson(doc, response);
      request->send(200, "application/json", response);
      return;
    }

    setLedState(true);

    int successCount = 0;
    float totalTime = 0.0;

    for (int i = 0; i < 12; i++) {
      if (WiFi.status() != WL_CONNECTED) break;
      if (Ping.ping(host.c_str(), 1)) {
        successCount++;
        totalTime += Ping.averageTime();
      }
      delay(20);
    }

    setLedState(false);

    bool ok = (successCount > 0);
    doc["success"] = ok;
    doc["avg_time"] = ok ? (int)(totalTime / successCount) : 0;
    doc["success_count"] = successCount;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 8. Web Terminal Endpoint
  server.on("/api/terminal", HTTP_GET, [](AsyncWebServerRequest *request){
    String cmd = request->hasParam("cmd") ? request->getParam("cmd")->value() : "";
    cmd.trim();
    String reply = "";
    bool cz = (currentLang == "cz");

    if (pendingRebootWeb) {
      if (cmd.equalsIgnoreCase("y")) {
        pendingRebootWeb = false;
        request->send(200, "text/plain", cz ? "Restartuji Czech Blade..." : "Restarting Czech Blade...");
        delay(400);
        ESP.restart();
        return;
      } else {
        pendingRebootWeb = false;
        reply = cz ? "Restart zrusen." : "Reboot aborted.";
      }
    }
    else if (cmd == "reboot") {
      pendingRebootWeb = true;
      reply = cz ? "Opravdu chcete restartovat Czech Blade? [y/N]: " : "Do you want to reboot? [y/N]: ";
    }
    else {
      reply = processCommand(cmd, false);
      if (cmd == "sleep" || cmd.startsWith("sleep ")) {
        int sec = 0;
        if (cmd.startsWith("sleep ")) sec = cmd.substring(6).toInt();
        request->send(200, "text/plain", reply);
        delay(300);
        executeSleep(sec);
        return;
      }
    }

    request->send(200, "text/plain", reply);
  });

  // 9. Status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(128);
    bool wifiConn = (WiFi.status() == WL_CONNECTED);
    bool internet = false;
    int pingTime = 0;

    if (wifiConn) {
      if (Ping.ping(IPAddress(1, 1, 1, 1), 1)) {
        internet = true;
        pingTime = (int)Ping.averageTime();
      }
    }

    doc["wifi_connected"] = wifiConn;
    doc["internet"] = internet;
    doc["ping"] = pingTime;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 10. Jazyk NVS
  server.on("/api/get-lang", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", currentLang);
  });

  server.on("/api/set-lang", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("lang")) {
      currentLang = request->getParam("lang")->value();
      prefs.begin("blade", false);
      prefs.putString("lang", currentLang);
      prefs.end();
      request->send(200, "text/plain", "OK");
      return;
    }
    request->send(400, "text/plain", "Error");
  });

  // 11. AP Config
  server.on("/api/set-ap", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("ssid")) {
      apSSID = request->getParam("ssid")->value();
      apPass = request->hasParam("pass") ? request->getParam("pass")->value() : "";

      request->send(200, "text/plain", "OK");
      blinkLed(4, 60);
      WiFi.softAPdisconnect(true);
      WiFi.softAP(apSSID.c_str(), apPass.length() >= 8 ? apPass.c_str() : NULL);
      return;
    }
    request->send(400, "text/plain", "Error");
  });

  server.begin();
}

void loop() {
  // Telnet klient
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) telnetClient.stop();
      telnetClient = telnetServer.available();
      telnetInputBuffer = "";
      telnetCursorPos = 0;
      telnetHistIdx = -1;
      pendingRebootTelnet = false;

      uint8_t telnetInit[] = { 255, 251, 1, 255, 251, 3, 255, 253, 34 };
      telnetClient.write(telnetInit, sizeof(telnetInit));

      telnetClient.print("\r\n\033[1;38;5;208m=== Czech Blade Telnet Shell v3.0 ===\033[0m\r\n");
      telnetClient.println("Type 'help' for available commands. (TAB for auto-completion)\r\n");
      telnetClient.print("\033[1;32mesp@czechblade>\033[0m ");
    }
  }

  if (telnetClient && telnetClient.connected()) {
    while (telnetClient.available()) {
      char c = telnetClient.read();

      // Klávesa TAB pro Telnet
      if (c == '\t') {
        if (telnetInputBuffer.length() > 0) {
          String toMatch = telnetInputBuffer;
          toMatch.toLowerCase();

          std::vector<String> matches;
          for (const auto& cmdItem : commandList) {
            if (cmdItem.startsWith(toMatch)) {
              matches.push_back(cmdItem);
            }
          }

          if (matches.size() == 1) {
            while (telnetCursorPos < (int)telnetInputBuffer.length()) {
              telnetClient.print("\033[C");
              telnetCursorPos++;
            }
            while (telnetInputBuffer.length() > 0) {
              telnetClient.print("\b \b");
              telnetInputBuffer.remove(telnetInputBuffer.length() - 1);
            }
            telnetInputBuffer = matches[0] + " ";
            telnetCursorPos = telnetInputBuffer.length();
            telnetClient.print(telnetInputBuffer);
          } else if (matches.size() > 1) {
            telnetClient.print("\r\n\033[33m");
            for (const auto& m : matches) {
              telnetClient.print(m + "   ");
            }
            telnetClient.print("\033[0m\r\n\033[1;32mesp@czechblade>\033[0m ");
            telnetClient.print(telnetInputBuffer);
          }
        }
        continue;
      }

      // Detekce ANSI šipek
      if (c == 27) {
        if (telnetClient.available() >= 2) {
          char c1 = telnetClient.read();
          char c2 = telnetClient.read();
          if (c1 == '[') {
            // Šipka NAHORU
            if (c2 == 'A') {
              if (telnetHistCount > 0 && telnetHistIdx < telnetHistCount - 1) {
                telnetHistIdx++;
                while (telnetCursorPos < (int)telnetInputBuffer.length()) {
                  telnetClient.print("\033[C");
                  telnetCursorPos++;
                }
                while (telnetInputBuffer.length() > 0) {
                  telnetClient.print("\b \b");
                  telnetInputBuffer.remove(telnetInputBuffer.length() - 1);
                }
                telnetInputBuffer = telnetHistory[telnetHistCount - 1 - telnetHistIdx];
                telnetCursorPos = telnetInputBuffer.length();
                telnetClient.print(telnetInputBuffer);
              }
            }
            // Šipka DOLŮ
            else if (c2 == 'B') {
              if (telnetHistIdx > 0) {
                telnetHistIdx--;
                while (telnetCursorPos < (int)telnetInputBuffer.length()) {
                  telnetClient.print("\033[C");
                  telnetCursorPos++;
                }
                while (telnetInputBuffer.length() > 0) {
                  telnetClient.print("\b \b");
                  telnetInputBuffer.remove(telnetInputBuffer.length() - 1);
                }
                telnetInputBuffer = telnetHistory[telnetHistCount - 1 - telnetHistIdx];
                telnetCursorPos = telnetInputBuffer.length();
                telnetClient.print(telnetInputBuffer);
              } else if (telnetHistIdx == 0) {
                telnetHistIdx = -1;
                while (telnetCursorPos < (int)telnetInputBuffer.length()) {
                  telnetClient.print("\033[C");
                  telnetCursorPos++;
                }
                while (telnetInputBuffer.length() > 0) {
                  telnetClient.print("\b \b");
                  telnetInputBuffer.remove(telnetInputBuffer.length() - 1);
                }
                telnetCursorPos = 0;
              }
            }
            // Šipka VPRAVO
            else if (c2 == 'C') {
              if (telnetCursorPos < (int)telnetInputBuffer.length()) {
                telnetCursorPos++;
                telnetClient.print("\033[C");
              }
            }
            // Šipka VLEVO
            else if (c2 == 'D') {
              if (telnetCursorPos > 0) {
                telnetCursorPos--;
                telnetClient.print("\033[D");
              }
            }
          }
        }
        continue;
      }

      // Enter
      if (c == '\r' || c == '\n') {
        telnetClient.print("\r\n");
        telnetInputBuffer.trim();

        if (pendingRebootTelnet) {
          if (telnetInputBuffer.equalsIgnoreCase("y")) {
            telnetClient.println(currentLang == "cz" ? "Restartuji Czech Blade..." : "Restarting Czech Blade...");
            telnetClient.flush();
            delay(400);
            ESP.restart();
          } else {
            pendingRebootTelnet = false;
            telnetClient.println(currentLang == "cz" ? "Restart zrusen." : "Reboot aborted.");
          }
          telnetInputBuffer = "";
          telnetCursorPos = 0;
          telnetClient.print("\033[1;32mesp@czechblade>\033[0m ");
          break;
        }

        if (telnetInputBuffer.length() > 0) {
          if (telnetHistCount < TELNET_HIST_SIZE) {
            telnetHistory[telnetHistCount++] = telnetInputBuffer;
          } else {
            for (int i = 0; i < TELNET_HIST_SIZE - 1; i++) {
              telnetHistory[i] = telnetHistory[i + 1];
            }
            telnetHistory[TELNET_HIST_SIZE - 1] = telnetInputBuffer;
          }
          telnetHistIdx = -1;

          if (telnetInputBuffer == "clear") {
            telnetClient.print("\033[2J\033[H");
          } else if (telnetInputBuffer == "reboot") {
            pendingRebootTelnet = true;
            telnetClient.print(currentLang == "cz" ? "Opravdu chcete restartovat Czech Blade? [y/N]: " : "Do you want to reboot? [y/N]: ");
            telnetInputBuffer = "";
            telnetCursorPos = 0;
            break;
          } else {
            bool hasLoading = telnetInputBuffer.startsWith("wifi scan") || 
                              telnetInputBuffer.startsWith("bt scan") || 
                              telnetInputBuffer.startsWith("wifi connect") ||
                              telnetInputBuffer.startsWith("wifi-repeat") ||
                              telnetInputBuffer.startsWith("wifi signal") ||
                              telnetInputBuffer.startsWith("ping");

            if (hasLoading) {
              telnetClient.print(currentLang == "cz" ? "Zpracovavam... " : "Processing... ");
            }

            String reply = processCommand(telnetInputBuffer, true);

            if (hasLoading) {
              telnetClient.print("\r\033[K");
            }

            reply.replace("\n", "\r\n");
            telnetClient.print(reply);
            telnetClient.print("\r\n");
          }
          telnetInputBuffer = "";
          telnetCursorPos = 0;
        }
        telnetClient.print("\033[1;32mesp@czechblade>\033[0m ");
        break;
      }
      // Backspace
      else if (c == 8 || c == 127) {
        if (telnetCursorPos > 0) {
          telnetInputBuffer.remove(telnetCursorPos - 1, 1);
          telnetCursorPos--;
          telnetClient.print("\b\033[K");
          if (telnetCursorPos < (int)telnetInputBuffer.length()) {
            String rest = telnetInputBuffer.substring(telnetCursorPos);
            telnetClient.print(rest);
            for (int i = 0; i < (int)rest.length(); i++) {
              telnetClient.print("\033[D");
            }
          }
        }
      }
      // Běžný ASCII znak
      else if (c >= 32 && c <= 126) {
        if (telnetCursorPos == (int)telnetInputBuffer.length()) {
          telnetInputBuffer += c;
          telnetCursorPos++;
          telnetClient.print(c);
        } else {
          telnetInputBuffer = telnetInputBuffer.substring(0, telnetCursorPos) + c + telnetInputBuffer.substring(telnetCursorPos);
          telnetCursorPos++;
          String rest = telnetInputBuffer.substring(telnetCursorPos - 1);
          telnetClient.print(rest);
          for (int i = 0; i < (int)rest.length() - 1; i++) {
            telnetClient.print("\033[D");
          }
        }
      }
    }
  }

  delay(15);
}