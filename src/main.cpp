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
String currentLang = "cz"; // "cz" nebo "eng"

// Stav potvrzení pro shutdown a reboot
bool pendingShutdownWeb = false;
bool pendingRebootWeb = false;
bool pendingShutdownTelnet = false;
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

// Seznam příkazů pro automatické doplňování (TAB completion)
const std::vector<String> commandList = {
  "help", "wifi", "wifi scan", "wifi connect", "wifi disconnect", "wifi signal",
  "wifi-repeat", "signal", "bt", "bt scan", "ping", "set", "settings",
  "set setap", "set lang", "set lang-d", "status", "status ip", "log",
  "led", "led on", "led off", "sleep", "shutdown", "reboot", "clear"
};

// ANSI barevné a řídicí kódy pro Telnet
#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_RED    "\033[31m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE   "\033[34m"
#define ANSI_CYAN   "\033[36m"

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

void executeShutdown() {
  digitalWrite(LED_PIN, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
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

// Společný parser příkazů pro Web i Telnet
String processCommand(String cmd, bool isTelnet = false) {
  cmd.trim();
  String out = "";
  bool cz = (currentLang == "cz");

  if (cmd == "set") cmd = "settings";
  else if (cmd.startsWith("set ")) {
    cmd = "settings " + cmd.substring(4);
  }

  if (cmd == "hack" || cmd == "matrix") cmd = "log";

  auto col = [&](const char* ansiCode) -> String {
    return isTelnet ? String(ansiCode) : String("");
  };

  // HLAVNÍ NÁPOVĚDA
  if (cmd == "help") {
    if (cz) {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== KATEGORIE PRIKAZU (Czech Blade) ===" + col(ANSI_RESET) + "\n"
            " - " + col(ANSI_GREEN) + "wifi" + col(ANSI_RESET) + "         : Prikazy pro Wi-Fi (scan, connect, status, signal...)\n"
            " - " + col(ANSI_GREEN) + "signal" + col(ANSI_RESET) + "       : Zmeri silu spojeni mobilu k modulu\n"
            " - " + col(ANSI_GREEN) + "bt" + col(ANSI_RESET) + "           : Napoveda a info pro Bluetooth LE\n"
            " - " + col(ANSI_GREEN) + "ping" + col(ANSI_RESET) + "         : Napoveda k testovani odezvy a zkratkam\n"
            " - " + col(ANSI_GREEN) + "set/settings" + col(ANSI_RESET) + " : Konfigurace zarizeni (setap, lang, lang-d)\n"
            " - " + col(ANSI_GREEN) + "status" + col(ANSI_RESET) + "       : Stav celeho systemu (nebo 'status ip')\n"
            " - " + col(ANSI_GREEN) + "log" + col(ANSI_RESET) + "          : Efektni hackersky vypis / log\n"
            " - " + col(ANSI_GREEN) + "led" + col(ANSI_RESET) + "          : Prepinac LED (nebo 'led on/off')\n"
            " - " + col(ANSI_GREEN) + "sleep [sec]" + col(ANSI_RESET) + "  : Prechod do spanku (probuzeni tlacitkem BOOT/RST)\n"
            " - " + col(ANSI_GREEN) + "shutdown" + col(ANSI_RESET) + "     : Vypnuti s potvrzenim [y/N]\n"
            " - " + col(ANSI_GREEN) + "reboot" + col(ANSI_RESET) + "       : Restart modulu s potvrzenim [y/N]\n"
            " - " + col(ANSI_GREEN) + "clear" + col(ANSI_RESET) + "        : Vymaze konzoli\n"
            "Tip: Stisknete TAB pro automaticke doplnovani prikazu.";
    } else {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== COMMAND CATEGORIES (Czech Blade) ===" + col(ANSI_RESET) + "\n"
            " - " + col(ANSI_GREEN) + "wifi" + col(ANSI_RESET) + "         : Wi-Fi commands (scan, connect, status, signal...)\n"
            " - " + col(ANSI_GREEN) + "signal" + col(ANSI_RESET) + "       : Measures phone connection signal to module\n"
            " - " + col(ANSI_GREEN) + "bt" + col(ANSI_RESET) + "           : Bluetooth LE info and commands\n"
            " - " + col(ANSI_GREEN) + "ping" + col(ANSI_RESET) + "         : Latency test help and shortcuts\n"
            " - " + col(ANSI_GREEN) + "set/settings" + col(ANSI_RESET) + " : Device configuration (setap, lang, lang-d)\n"
            " - " + col(ANSI_GREEN) + "status" + col(ANSI_RESET) + "       : System status (or 'status ip')\n"
            " - " + col(ANSI_GREEN) + "log" + col(ANSI_RESET) + "          : Hollywood hacking log effect\n"
            " - " + col(ANSI_GREEN) + "led" + col(ANSI_RESET) + "          : Toggle LED (or 'led on/off')\n"
            " - " + col(ANSI_GREEN) + "sleep [sec]" + col(ANSI_RESET) + "  : Deep sleep (wake up via BOOT/RST button)\n"
            " - " + col(ANSI_GREEN) + "shutdown" + col(ANSI_RESET) + "     : Power off module with confirmation [y/N]\n"
            " - " + col(ANSI_GREEN) + "reboot" + col(ANSI_RESET) + "       : Restart module with confirmation [y/N]\n"
            " - " + col(ANSI_GREEN) + "clear" + col(ANSI_RESET) + "        : Clear console\n"
            "Tip: Press TAB for auto-completion.";
    }
  } 
  // KATEGORIE: WI-FI
  else if (cmd == "wifi") {
    if (cz) {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== WI-FI PRIKAZY ===" + col(ANSI_RESET) + "\n"
            " - wifi status                       : Rychle overi stav pripojeni k routeru\n"
            " - wifi scan                         : Ocisluje a vypise Wi-Fi site v okoli\n"
            " - wifi connect [n <cislo>|SSID]     : Pripoji se k siti (lze pouzit 'n 1')\n"
            " - wifi disconnect                   : Odpoji se od routeru\n"
            " - wifi signal                       : Zmeri aktualni pripojeny router (5s stream)\n"
            " - wifi signal[-sec] [n <cislo>|SSID]: Zmeri vybrany router na zadany pocet sekund (napr. 'wifi signal-10 n 2')\n"
            " - wifi-repeat [n <cislo>|SSID]      : Naklonuje sit jako novy AP";
    } else {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== WI-FI COMMANDS ===" + col(ANSI_RESET) + "\n"
            " - wifi status                       : Quickly checks router connection status\n"
            " - wifi scan                         : Scans and numbers nearby Wi-Fi networks\n"
            " - wifi connect [n <num>|SSID]       : Connects to target network (e.g. 'n 1')\n"
            " - wifi disconnect                   : Disconnects from router\n"
            " - wifi signal                       : Measures current connected router (5s stream)\n"
            " - wifi signal[-sec] [n <num>|SSID]  : Measures target router for X seconds (e.g. 'wifi signal-10 n 2')\n"
            " - wifi-repeat [n <num>|SSID]        : Clones target network as new AP";
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
    if (cz) {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== BLUETOOTH LE (BLE) ===" + col(ANSI_RESET) + "\n"
            "Modul pasivne zachytava BLE majaky (AirTagy, hodinky, naramky, senzory).\n"
            " - bt scan : Spusti 3sekundovy detailni sken okoli";
    } else {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== BLUETOOTH LE (BLE) ===" + col(ANSI_RESET) + "\n"
            "Passively captures BLE beacons (AirTags, watches, fitness trackers, sensors).\n"
            " - bt scan : Runs 3-second detailed environment scan";
    }
  }
  else if (cmd == "bt scan") {
    out = cz ? "Skenuji BLE v okoli (3 sekundy)...\n" : "Scanning BLE devices (3 seconds)...\n";
    setLedState(true);
    pBLEScan->stop();
    pBLEScan->clearResults();
    BLEScanResults found = pBLEScan->start(3, false);
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
  else if (cmd == "settings") {
    if (cz) {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== NASTAVENI (SETTINGS) ===" + col(ANSI_RESET) + "\n"
            " - set setap <ssid> <pwd> : Zmeni vysilane SSID a heslo AP (min. 8 znaku)\n"
            " - set lang <cz/eng>      : Docasne prepne jazyk pro tuto relaci\n"
            " - set lang-d <cz/eng>    : Ulozi trvaly vychozi jazyk do pameti modulu";
    } else {
      out = col(ANSI_BOLD) + col(ANSI_CYAN) + "=== SETTINGS ===" + col(ANSI_RESET) + "\n"
            " - set setap <ssid> <pwd> : Change broadcast SSID and AP password (min. 8 chars)\n"
            " - set lang <cz/eng>      : Temporarily switch language for this session\n"
            " - set lang-d <cz/eng>    : Save permanent default language to memory";
    }
  }
  else if (cmd.startsWith("settings setap ") || cmd.startsWith("setap ")) {
    String params = cmd.startsWith("settings setap ") ? cmd.substring(15) : cmd.substring(6);
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
      out = cz ? "Pouziti: set setap <nove_SSID> <nove_heslo>" : "Usage: set setap <new_SSID> <new_password>";
    }
  }
  else if (cmd.startsWith("settings lang ") || cmd.startsWith("lang ")) {
    String l = cmd.startsWith("settings lang ") ? cmd.substring(14) : cmd.substring(5);
    l.trim();
    l.toLowerCase();
    if (l == "cz" || l == "eng" || l == "en") {
      currentLang = (l == "cz") ? "cz" : "eng";
      out = col(ANSI_GREEN) + ((currentLang == "cz") ? "Docasny jazyk nastaven na: Cestina (CZ)" : "Temporary language set to: English (ENG)") + col(ANSI_RESET);
    } else {
      out = cz ? "Pouziti: set lang <cz/eng>" : "Usage: set lang <cz/eng>";
    }
  }
  else if (cmd.startsWith("settings lang-d ") || cmd.startsWith("lang-d ") || cmd.startsWith("set lang-d ")) {
    String l = "";
    if (cmd.startsWith("settings lang-d ")) l = cmd.substring(16);
    else if (cmd.startsWith("set lang-d ")) l = cmd.substring(11);
    else l = cmd.substring(7);
    l.trim();
    l.toLowerCase();
    if (l == "cz" || l == "eng" || l == "en") {
      currentLang = (l == "cz") ? "cz" : "eng";
      prefs.begin("config", false);
      prefs.putString("lang", currentLang);
      prefs.end();
      out = col(ANSI_GREEN) + ((currentLang == "cz") ? "Trvaly vychozi jazyk ulozen: Cestina (CZ)" : "Default permanent language saved: English (ENG)") + col(ANSI_RESET);
    } else {
      out = cz ? "Pouziti: set lang-d <cz/eng>" : "Usage: set lang-d <cz/eng>";
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
      esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
    } else {
      out = cz ? "Czech Blade usina... (probuzeni tlacitkem BOOT nebo EN/RST na desce)" 
               : "Czech Blade going to sleep... (wake up via BOOT or EN/RST button)";
    }

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    if (isTelnet) {
      telnetClient.println(out);
      telnetClient.flush();
      delay(200);
      executeShutdown();
    }
  }
  else {
    out = col(ANSI_RED) + (cz ? ("Neznamy prikaz: '" + cmd + "'. Napiste 'help'.") : ("Unknown command: '" + cmd + "'. Type 'help'.")) + col(ANSI_RESET);
  }

  return out;
}

const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="cs">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>Czech Blade</title>
  <style>
    :root {
      --bg: #090b0e;
      --card: #131720;
      --card-border: #222838;
      --text: #e2e8f0;
      --subtext: #828fa3;
      --accent: #38bdf8;
      --green: #22c55e;
      --red: #ef4444;
      --orange: #f59e0b;
    }
    * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
    body { margin: 0; font-family: -apple-system, system-ui, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: var(--bg); color: var(--text); padding-bottom: 75px; }

    header { display: flex; justify-content: space-between; align-items: center; padding: 12px 16px; background: #0f131c; border-bottom: 1px solid var(--card-border); position: sticky; top: 0; z-index: 100; }
    .status-area { display: flex; align-items: center; gap: 10px; }
    .title-box { display: flex; flex-direction: column; }
    .app-name { font-size: 1rem; font-weight: 700; color: #fff; display: flex; align-items: center; gap: 6px; }
    .status-indicators { display: flex; gap: 6px; margin-top: 3px; align-items: center; }
    .indicator-pill { display: flex; align-items: center; gap: 4px; font-size: 0.7rem; padding: 2px 7px; border-radius: 10px; background: #1a202c; border: 1px solid #2d3748; }
    .dot { width: 6px; height: 6px; border-radius: 50%; background: var(--subtext); }
    .dot.online { background: var(--green); box-shadow: 0 0 6px var(--green); }
    .dot.offline { background: var(--red); }
    .btn-icon { background: var(--card); border: 1px solid var(--card-border); color: var(--text); width: 34px; height: 34px; border-radius: 8px; font-size: 1rem; cursor: pointer; display: flex; align-items: center; justify-content: center; }

    .nav-bar { display: flex; position: fixed; bottom: 0; left: 0; right: 0; background: #0e121af5; backdrop-filter: blur(12px); border-top: 1px solid var(--card-border); z-index: 100; }
    .nav-btn { flex: 1; padding: 9px 0; border: none; background: transparent; color: var(--subtext); font-size: 0.7rem; font-weight: 600; cursor: pointer; display: flex; flex-direction: column; align-items: center; gap: 2px; }
    .nav-btn.active { color: var(--accent); }

    .container { padding: 14px; }
    .panel { display: none; }
    .panel.active { display: block; animation: fadeIn 0.2s ease-in-out; }
    @keyframes fadeIn { from { opacity: 0; transform: translateY(4px); } to { opacity: 1; transform: translateY(0); } }

    .card { background: var(--card); border: 1px solid var(--card-border); border-radius: 12px; padding: 14px; margin-bottom: 12px; }
    .btn-main { width: 100%; padding: 11px; border: none; border-radius: 8px; background: var(--accent); color: #041017; font-weight: 700; font-size: 0.9rem; cursor: pointer; }
    .btn-main:active { transform: scale(0.98); }
    .btn-secondary { background: var(--card-border); color: #fff; }

    ul { list-style: none; padding: 0; margin: 10px 0 0 0; }
    .net-item { background: #0c0f15; border: 1px solid var(--card-border); border-radius: 8px; padding: 10px 12px; margin-bottom: 8px; display: flex; justify-content: space-between; align-items: center; cursor: pointer; gap: 8px; }
    .net-item:active { background: #161b26; }
    .net-info { min-width: 0; flex: 1; }
    .net-title-row { display: flex; align-items: center; gap: 6px; }
    .net-ssid { font-weight: 600; font-size: 0.9rem; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .net-meta { font-size: 0.72rem; color: var(--subtext); margin-top: 2px; }
    .badge-rssi { font-size: 0.7rem; color: var(--accent); font-weight: bold; background: #162030; padding: 1px 5px; border-radius: 4px; white-space: nowrap; }

    .net-actions { display: flex; gap: 5px; flex-shrink: 0; }
    .action-sub-btn { border: 1px solid var(--card-border); background: #182030; color: var(--text); padding: 6px 9px; border-radius: 6px; font-size: 0.75rem; font-weight: bold; cursor: pointer; }
    .action-sub-btn:active { background: var(--accent); color: #000; }

    .radar-circle { width: 170px; height: 170px; border-radius: 50%; border: 3px solid var(--card-border); margin: 15px auto; display: flex; flex-direction: column; align-items: center; justify-content: center; background: radial-gradient(circle, #172235 0%, #0d1118 80%); }
    .signal-big { font-size: 2.3rem; font-weight: 800; color: var(--accent); }
    .signal-status { font-size: 0.8rem; color: var(--subtext); }

    /* Terminál */
    .term-window { background: #05070a; border: 1px solid #1a2230; border-radius: 8px; padding: 12px; font-family: 'Courier New', Courier, monospace; font-size: 0.8rem; height: 320px; overflow-y: auto; white-space: pre-wrap; color: #e2e8f0; line-height: 1.4; }
    .term-prompt { color: #22c55e; font-weight: bold; }
    .term-row { display: flex; gap: 8px; margin-top: 8px; }
    .term-input { flex: 1; background: #0c0f15; border: 1px solid var(--card-border); color: #fff; padding: 8px; font-family: monospace; border-radius: 6px; }

    .modal-overlay { display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.75); backdrop-filter: blur(4px); z-index: 200; align-items: center; justify-content: center; padding: 20px; }
    .modal-box { background: var(--card); border: 1px solid var(--card-border); border-radius: 12px; padding: 18px; width: 100%; max-width: 320px; }
    input, select { width: 100%; padding: 10px; margin: 6px 0 12px 0; background: #080a0e; border: 1px solid var(--card-border); border-radius: 6px; color: #fff; box-sizing: border-box; }
    
    .modal-footer { margin-top: 18px; text-align: center; font-family: monospace; font-size: 0.72rem; color: var(--accent); opacity: 0.85; letter-spacing: 0.2px; }
  </style>
</head>
<body>

  <header>
    <div class="status-area">
      <div class="title-box">
        <span class="app-name">⚡ Czech Blade</span>
        <div class="status-indicators">
          <div class="indicator-pill">
            <span class="dot offline" id="net-dot"></span>
            <span id="net-desc">Offline</span>
          </div>
          <div class="indicator-pill" id="ap-pill" style="color:var(--accent);">
            Czech Blade
          </div>
        </div>
      </div>
    </div>
    <button class="btn-icon" onclick="openSettings()">⚙️</button>
  </header>

  <div class="container">
    <!-- 1. WI-FI -->
    <div id="p-wifi" class="panel active">
      <div class="card">
        <button class="btn-main" id="btn-scan-wifi" onclick="scanWifi()">Skenovat Wi-Fi v okolí</button>
        <ul id="wifi-list"></ul>
      </div>
    </div>

    <!-- 2. GEIGER -->
    <div id="p-geiger" class="panel">
      <div class="card" style="text-align: center;">
        <h3 style="margin:0; font-size: 0.95rem;" id="geiger-target-title">Radar signálu</h3>
        <p style="font-size:0.75rem; color:var(--subtext); margin:4px 0 12px 0;" id="geiger-subtitle">Měří sílu signálu vybraného cíle</p>
        
        <div class="radar-circle">
          <span class="signal-big" id="g-rssi">--</span>
          <span class="signal-status" id="g-desc">Vypnuto</span>
        </div>
        
        <button class="btn-main" id="g-btn" onclick="toggleGeiger()">Spustit zaměřování</button>
      </div>
    </div>

    <!-- 3. BLE -->
    <div id="p-ble" class="panel">
      <div class="card">
        <button class="btn-main" id="btn-scan-ble" onclick="scanBle()">Skenovat Bluetooth majáky</button>
        <ul id="ble-list"></ul>
      </div>
    </div>

    <!-- 4. PING -->
    <div id="p-ping" class="panel">
      <div class="card">
        <h3 style="margin:0 0 8px 0; font-size: 0.95rem;" id="lbl-ping-title">Test konektivity</h3>
        <input type="text" id="ping-target" value="1.1.1.1" placeholder="IP / Host">
        <button class="btn-main" id="btn-run-ping" onclick="runPing()">Spustit Ping Test</button>
        <div id="ping-output" style="margin-top: 12px; font-size: 0.85rem;"></div>
      </div>
    </div>

    <!-- 5. TERMINÁL -->
    <div id="p-term" class="panel">
      <div class="card">
        <div class="term-window" id="term-out"></div>
        <div class="term-row">
          <input type="text" id="term-in" class="term-input" placeholder="Příkaz..." onkeydown="handleTermKey(event)">
          <button class="btn-main" style="width: auto; padding: 0 14px;" onclick="sendTermCmd()">></button>
        </div>
      </div>
    </div>
  </div>

  <div class="nav-bar">
    <button class="nav-btn active" onclick="switchTab('wifi', this)"><span id="nav-icon-wifi">📶</span><span id="nav-text-wifi">Wi-Fi</span></button>
    <button class="nav-btn" onclick="switchTab('geiger', this)"><span id="nav-icon-geiger">🎯</span><span id="nav-text-geiger">Geiger</span></button>
    <button class="nav-btn" onclick="switchTab('ble', this)"><span id="nav-icon-ble">📡</span><span id="nav-text-ble">BLE</span></button>
    <button class="nav-btn" onclick="switchTab('ping', this)"><span id="nav-icon-ping">🌐</span><span id="nav-text-ping">Ping</span></button>
    <button class="nav-btn" onclick="switchTab('term', this)"><span id="nav-icon-term">💻</span><span id="nav-text-term">Terminál</span></button>
  </div>

  <!-- MODAL: PŘIPOJENÍ -->
  <div class="modal-overlay" id="modal-connect">
    <div class="modal-box">
      <h3 style="margin-top:0; font-size:1rem;" id="modal-connect-title">Připojit k síti</h3>
      <input type="password" id="modal-connect-pass" placeholder="Heslo Wi-Fi">
      <div style="display:flex; gap:8px;">
        <button class="btn-main" id="btn-modal-connect" onclick="submitConnect()">Připojit</button>
        <button class="btn-main btn-secondary" id="btn-modal-cancel" onclick="closeModal('modal-connect')">Zrušit</button>
      </div>
    </div>
  </div>

  <!-- MODAL: OFFLINE VAROVÁNÍ -->
  <div class="modal-overlay" id="modal-offline">
    <div class="modal-box" style="text-align: center;">
      <h3 style="margin-top:0; color:var(--red);" id="modal-offline-title">Czech Blade je Offline</h3>
      <p style="font-size:0.85rem; color:var(--subtext);" id="modal-offline-desc">Modul není připojen k žádné Wi-Fi síti. Přejděte na záložku Wi-Fi a nejprve se připojte k routeru s internetem.</p>
      <button class="btn-main" id="btn-offline-goto" onclick="closeModal('modal-offline'); switchTab('wifi', document.querySelectorAll('.nav-btn')[0]);">Přejít na Wi-Fi</button>
    </div>
  </div>

  <!-- MODAL: NASTAVENÍ -->
  <div class="modal-overlay" id="modal-settings">
    <div class="modal-box">
      <h3 style="margin-top:0; font-size:1rem;" id="modal-settings-title">Nastavení</h3>
      <label style="font-size:0.75rem; color:var(--subtext);" id="lbl-lang">Výchozí jazyk / Default Language:</label>
      <select id="lang-select" onchange="changeLanguageDefault(this.value)">
        <option value="cz">Čeština (CZ)</option>
        <option value="eng">English (ENG)</option>
      </select>
      <label style="font-size:0.75rem; color:var(--subtext);" id="lbl-ap-ssid">Vysílané SSID:</label>
      <input type="text" id="ap-name-input">
      <label style="font-size:0.75rem; color:var(--subtext);" id="lbl-ap-pass">Heslo přístupového bodu:</label>
      <input type="password" id="ap-pass-input" placeholder="Min. 8 znaků">
      <div style="display:flex; gap:8px;">
        <button class="btn-main" id="btn-save-settings" onclick="submitSettings()">Uložit & Restartovat AP</button>
        <button class="btn-main btn-secondary" id="btn-close-settings" onclick="closeModal('modal-settings')">Zavřít</button>
      </div>
      <div class="modal-footer">
        &copy; 2026 Misbo 8 &bull; Czech Blade Project &bull; misbo8.cz
      </div>
    </div>
  </div>

  <script>
    let currentLang = "cz";
    let geigerActive = false;
    let geigerAbortCtrl = null;
    let audioCtx = null;
    let selectedSsid = "";
    let isConnectedToWifi = false;

    let cmdHistory = [];
    let historyIdx = -1;

    // Seznam příkazů pro TAB v prohlížeči
    const tabCommands = [
      "help", "wifi", "wifi scan", "wifi connect", "wifi disconnect", "wifi signal",
      "wifi-repeat", "signal", "bt", "bt scan", "ping", "set", "settings",
      "set setap", "set lang", "set lang-d", "status", "status ip", "log",
      "led", "led on", "led off", "sleep", "shutdown", "reboot", "clear"
    ];

    const dict = {
      cz: {
        nav_wifi: "Wi-Fi", nav_geiger: "Geiger", nav_ble: "BLE", nav_ping: "Ping", nav_term: "Terminál",
        btn_scan_wifi: "Skenovat Wi-Fi v okolí", scanning_wifi: "Skenuji pásmo...",
        geiger_title: "Radar signálu", geiger_sub: "Měří sílu signálu vybraného cíle", geiger_off: "Vypnuto",
        btn_geiger_start: "Spustit zaměřování", btn_geiger_stop: "Zastavit zaměřování",
        btn_scan_ble: "Skenovat Bluetooth majáky", scanning_ble: "Skenuji BLE majáky v okolí...",
        ping_title: "Test konektivity", btn_run_ping: "Spustit Ping Test", pinging: "Odesílám 12 paketů na ",
        ping_ok: "Hostitel odpovídá!", ping_fail: "Hostitel neodpovídá! (Ztráta paketů)",
        modal_conn_title: "Připojit k ", modal_conn_btn: "Připojit", modal_cancel_btn: "Zrušit",
        modal_offline_title: "Czech Blade je Offline", modal_offline_desc: "Modul není připojen k žádné Wi-Fi síti. Přejděte na záložku Wi-Fi a nejprve se připojte k routeru s internetem.",
        modal_offline_goto: "Přejít na Wi-Fi",
        settings_title: "Nastavení", lbl_lang: "Výchozí jazyk / Default Language:", lbl_ap_ssid: "Vysílané SSID:", lbl_ap_pass: "Heslo přístupového bodu:",
        btn_save_settings: "Uložit & Restartovat AP", btn_close_settings: "Zavřít",
        term_init: "Czech Blade Shell v3.0\nZadejte 'help' pro seznam kategorii a prikazu. (TAB = doplnovani)\n------------------------------------------------",
        btn_measure: "📡 Měřit", btn_repeat: "🔁 Opakovat"
      },
      eng: {
        nav_wifi: "Wi-Fi", nav_geiger: "Geiger", nav_ble: "BLE", nav_ping: "Ping", nav_term: "Terminal",
        btn_scan_wifi: "Scan nearby Wi-Fi", scanning_wifi: "Scanning 2.4 GHz band...",
        geiger_title: "Signal Radar", geiger_sub: "Measures signal strength of the target", geiger_off: "Off",
        btn_geiger_start: "Start Tracking", btn_geiger_stop: "Stop Tracking",
        btn_scan_ble: "Scan Bluetooth Beacons", scanning_ble: "Scanning BLE beacons nearby...",
        ping_title: "Connectivity Test", btn_run_ping: "Run Ping Test", pinging: "Sending 12 packets to ",
        ping_ok: "Host is reachable!", ping_fail: "Host unreachable! (Packet loss)",
        modal_conn_title: "Connect to ", modal_conn_btn: "Connect", modal_cancel_btn: "Cancel",
        modal_offline_title: "Czech Blade is Offline", modal_offline_desc: "The device is not connected to any Wi-Fi. Go to the Wi-Fi tab and connect to a router first.",
        modal_offline_goto: "Go to Wi-Fi",
        settings_title: "Settings", lbl_lang: "Default Language / Výchozí jazyk:", lbl_ap_ssid: "Broadcast SSID:", lbl_ap_pass: "Access Point Password:",
        btn_save_settings: "Save & Restart AP", btn_close_settings: "Close",
        term_init: "Czech Blade Shell v3.0\nType 'help' for categories and commands. (TAB = completion)\n------------------------------------------------",
        btn_measure: "📡 Measure", btn_repeat: "🔁 Repeat"
      }
    };

    function setLanguage(lang) {
      currentLang = (lang === "en" || lang === "eng") ? "eng" : "cz";
      document.getElementById('lang-select').value = currentLang;
      const t = dict[currentLang];

      document.getElementById('nav-text-wifi').innerText = t.nav_wifi;
      document.getElementById('nav-text-geiger').innerText = t.nav_geiger;
      document.getElementById('nav-text-ble').innerText = t.nav_ble;
      document.getElementById('nav-text-ping').innerText = t.nav_ping;
      document.getElementById('nav-text-term').innerText = t.nav_term;

      document.getElementById('btn-scan-wifi').innerText = t.btn_scan_wifi;
      document.getElementById('geiger-target-title').innerText = t.geiger_title;
      document.getElementById('geiger-subtitle').innerText = t.geiger_sub;
      if (!geigerActive) document.getElementById('g-btn').innerText = t.btn_geiger_start;
      document.getElementById('btn-scan-ble').innerText = t.btn_scan_ble;
      document.getElementById('lbl-ping-title').innerText = t.ping_title;
      document.getElementById('btn-run-ping').innerText = t.btn_run_ping;

      document.getElementById('btn-modal-connect').innerText = t.modal_conn_btn;
      document.getElementById('btn-modal-cancel').innerText = t.modal_cancel_btn;
      document.getElementById('modal-offline-title').innerText = t.modal_offline_title;
      document.getElementById('modal-offline-desc').innerText = t.modal_offline_desc;
      document.getElementById('btn-offline-goto').innerText = t.modal_offline_goto;

      document.getElementById('modal-settings-title').innerText = t.settings_title;
      document.getElementById('lbl-lang').innerText = t.lbl_lang;
      document.getElementById('lbl-ap-ssid').innerText = t.lbl_ap_ssid;
      document.getElementById('lbl-ap-pass').innerText = t.lbl_ap_pass;
      document.getElementById('btn-save-settings').innerText = t.btn_save_settings;
      document.getElementById('btn-close-settings').innerText = t.btn_close_settings;
    }

    async function changeLanguageDefault(lang) {
      setLanguage(lang);
      await fetch('/api/terminal?cmd=' + encodeURIComponent('set lang-d ' + lang));
    }

    function switchTab(name, btn) {
      document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
      document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
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
      osc.frequency.value = freq;
      gain.gain.value = 0.08;
      osc.start();
      osc.stop(audioCtx.currentTime + 0.04);
    }

    async function scanWifi() {
      const list = document.getElementById('wifi-list');
      list.innerHTML = `<li style="justify-content:center; color:#888;">${dict[currentLang].scanning_wifi}</li>`;
      let res = await fetch('/api/scan-wifi');
      let data = await res.json();
      list.innerHTML = '';
      data.forEach((net, idx) => {
        let li = document.createElement('li');
        li.className = 'net-item';
        li.onclick = () => openConnectModal(net.ssid);
        li.innerHTML = `
          <div class="net-info">
            <div class="net-title-row">
              <span style="color:var(--accent); font-weight:bold; font-size:0.85rem;">${idx + 1}.</span>
              <span class="net-ssid">${net.ssid || '<i>Hidden SSID</i>'}</span>
              <span class="badge-rssi">${net.rssi} dBm</span>
            </div>
            <div class="net-meta">Ch ${net.ch} &bull; ${net.bssid}</div>
          </div>
          <div class="net-actions">
            <button class="action-sub-btn" onclick="event.stopPropagation(); startTargetMeasure('${net.ssid}')">${dict[currentLang].btn_measure}</button>
            <button class="action-sub-btn" onclick="event.stopPropagation(); setupRepeater('${net.ssid}')">${dict[currentLang].btn_repeat}</button>
          </div>
        `;
        list.appendChild(li);
      });
    }

    function startTargetMeasure(ssid) {
      selectedSsid = ssid;
      document.getElementById('geiger-target-title').innerText = (currentLang === 'cz' ? "Zaměřování: " : "Tracking: ") + ssid;
      document.getElementById('geiger-subtitle').innerText = currentLang === 'cz' ? "Měří signál mezi Czech Blade a routerem" : "Measures signal between Czech Blade and router";
      switchTab('geiger', document.querySelectorAll('.nav-btn')[1]);
      if (!geigerActive) toggleGeiger();
    }

    function setupRepeater(ssid) {
      document.getElementById('modal-settings-title').innerText = (currentLang === 'cz' ? "Klonovat síť: " : "Clone network: ") + ssid;
      document.getElementById('ap-name-input').value = ssid + "_EXT";
      document.getElementById('ap-pass-input').value = "";
      document.getElementById('modal-settings').style.display = "flex";
    }

    function openConnectModal(ssid) {
      if (!ssid) return alert(currentLang === 'cz' ? "K této síti se nelze připojit bez SSID." : "Cannot connect without SSID.");
      selectedSsid = ssid;
      document.getElementById('modal-connect-title').innerText = dict[currentLang].modal_conn_title + ssid;
      document.getElementById('modal-connect-pass').value = "";
      document.getElementById('modal-connect').style.display = "flex";
    }

    async function submitConnect() {
      let pass = document.getElementById('modal-connect-pass').value;
      closeModal('modal-connect');
      checkInternetStatus(true);
      let res = await fetch(`/api/connect?ssid=${encodeURIComponent(selectedSsid)}&pass=${encodeURIComponent(pass)}`);
      let data = await res.json();
      if (data.connected) {
        checkInternetStatus();
      } else {
        alert(currentLang === 'cz' ? "Připojení k " + selectedSsid + " selhalo!" : "Connection to " + selectedSsid + " failed!");
        checkInternetStatus();
      }
    }

    function toggleGeiger() {
      const btn = document.getElementById('g-btn');
      if (geigerActive) {
        geigerActive = false;
        if (geigerAbortCtrl) geigerAbortCtrl.abort();
        btn.innerText = dict[currentLang].btn_geiger_start;
        btn.classList.remove('btn-secondary');
        document.getElementById('g-rssi').innerText = "--";
        document.getElementById('g-desc').innerText = dict[currentLang].geiger_off;
      } else {
        geigerActive = true;
        btn.innerText = dict[currentLang].btn_geiger_stop;
        btn.classList.add('btn-secondary');
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
            let pct = Math.max(0, Math.min(100, (rssi + 100) * 1.53));
            let desc = pct > 75 ? (currentLang === 'cz' ? "Silný signál" : "Strong") : (pct > 40 ? (currentLang === 'cz' ? "Střední" : "Medium") : (currentLang === 'cz' ? "Slabý dosah" : "Weak"));
            document.getElementById('g-desc').innerText = desc;
            playBeep(300 + (pct * 9));
          } else {
            document.getElementById('g-rssi').innerText = "--";
            document.getElementById('g-desc').innerText = currentLang === 'cz' ? "Mimo dosah" : "Out of range";
          }
        } catch (e) {
          if (e.name === 'AbortError') break;
        }
        await new Promise(r => setTimeout(r, 650));
      }
    }

    async function scanBle() {
      const list = document.getElementById('ble-list');
      list.innerHTML = `<li style="justify-content:center; color:#888;">${dict[currentLang].scanning_ble}</li>`;
      try {
        let res = await fetch('/api/scan-ble');
        let data = await res.json();
        list.innerHTML = '';
        data.forEach(dev => {
          let li = document.createElement('li');
          li.className = 'net-item';
          li.innerHTML = `
            <div class="net-info">
              <div class="net-title-row">
                <span class="net-ssid">${dev.name || '<i>Unknown Beacon</i>'}</span>
                <span class="badge-rssi">${dev.rssi} dBm</span>
              </div>
              <div class="net-meta">${dev.mac}</div>
            </div>
          `;
          list.appendChild(li);
        });
        if (!data.length) list.innerHTML = `<li style="justify-content:center; color:#888;">${currentLang === 'cz' ? "Žádná BLE zařízení v dosahu." : "No BLE devices in range."}</li>`;
      } catch (err) {
        list.innerHTML = `<li style="justify-content:center; color:var(--red);">${currentLang === 'cz' ? "Chyba při komunikaci s BLE." : "BLE communication error."}</li>`;
      }
    }

    async function runPing() {
      if (!isConnectedToWifi) {
        document.getElementById('modal-offline').style.display = 'flex';
        return;
      }

      let host = document.getElementById('ping-target').value.trim() || "1.1.1.1";
      let out = document.getElementById('ping-output');
      out.innerHTML = `<span style="color:#aaa;">${dict[currentLang].pinging}${host}...</span>`;
      
      try {
        let res = await fetch('/api/ping?host=' + encodeURIComponent(host));
        let data = await res.json();
        if (data.success) {
          out.innerHTML = `<span style="color:var(--green); font-weight:bold;">${dict[currentLang].ping_ok}</span><br>${currentLang === 'cz' ? "Průměrná odezva" : "Average latency"}: <strong>${data.avg_time} ms</strong> (${data.success_count}/12)`;
        } else {
          out.innerHTML = `<span style="color:var(--red); font-weight:bold;">${dict[currentLang].ping_fail}</span>`;
        }
      } catch (e) {
        out.innerHTML = `<span style="color:var(--red);">Request error.</span>`;
      }
      checkInternetStatus();
    }

    // Terminál s historií šipek a TAB autocompletion
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
          out.innerHTML += `\n<span style="color:#888;">${matches.join('   ')}</span>\n<span class="term-prompt">esp@czechblade&gt;</span> ${val}`;
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

      const promptHtml = `<span class="term-prompt">esp@czechblade&gt;</span> ${cmd}`;
      out.innerHTML += `\n${promptHtml}`;
      input.value = "";

      let loadingSpan = document.createElement("span");
      loadingSpan.style.color = "#888";
      out.appendChild(document.createTextNode("\n"));
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

        if (cmd.startsWith("settings lang ") || cmd.startsWith("lang ") || cmd.startsWith("set lang ") ||
            cmd.startsWith("settings lang-d ") || cmd.startsWith("lang-d ") || cmd.startsWith("set lang-d ")) {
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
      const dot = document.getElementById('net-dot');
      const desc = document.getElementById('net-desc');
      if (checking) {
        dot.className = "dot";
        desc.innerText = currentLang === 'cz' ? "Připojuji..." : "Connecting...";
        return;
      }
      try {
        let res = await fetch('/api/status');
        let data = await res.json();
        isConnectedToWifi = data.wifi_connected;
        if (data.internet) {
          dot.className = "dot online";
          desc.innerText = "Online (" + data.ping + " ms)";
        } else if (data.wifi_connected) {
          dot.className = "dot";
          desc.innerText = currentLang === 'cz' ? "Bez internetu" : "No internet";
        } else {
          dot.className = "dot offline";
          desc.innerText = "Offline";
        }
      } catch (e) {
        dot.className = "dot offline";
        desc.innerText = "Error";
        isConnectedToWifi = false;
      }
    }

    function openSettings() {
      document.getElementById('ap-name-input').value = document.getElementById('ap-pill').innerText;
      document.getElementById('lang-select').value = currentLang;
      document.getElementById('modal-settings').style.display = "flex";
    }

    async function submitSettings() {
      let name = document.getElementById('ap-name-input').value;
      let pass = document.getElementById('ap-pass-input').value;
      if (!name) return alert(currentLang === 'cz' ? "Zadejte název!" : "Enter SSID!");
      closeModal('modal-settings');
      await fetch(`/api/set-ap?ssid=${encodeURIComponent(name)}&pass=${encodeURIComponent(pass)}`);
      alert(currentLang === 'cz' ? "Překonfigurováno! Pokud se změnilo SSID/heslo, připojte se znovu." : "Reconfigured! Reconnect if SSID/password changed.");
      document.getElementById('ap-pill').innerText = name;
    }

    function closeModal(id) {
      document.getElementById(id).style.display = "none";
    }

    fetch('/api/terminal?cmd=status').then(r => r.text()).then(txt => {
      let initLang = txt.includes("Language: ENG") ? "eng" : "cz";
      document.getElementById('term-out').innerText = dict[initLang].term_init;
      setLanguage(initLang);
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

  // Načtení trvalého výchozího jazyka z NVS
  prefs.begin("config", true);
  currentLang = prefs.getString("lang", "cz");
  prefs.end();

  blinkLed(3, 80);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID.c_str(), apPass.c_str());

  telnetServer.begin();
  telnetServer.setNoDelay(true);

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);

  // Web endpointy
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", INDEX_HTML);
  });

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

  server.on("/api/scan-ble", HTTP_GET, [](AsyncWebServerRequest *request){
    setLedState(true);
    pBLEScan->stop();
    pBLEScan->clearResults();

    BLEScanResults foundDevices = pBLEScan->start(3, false);
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < foundDevices.getCount(); i++) {
      BLEAdvertisedDevice dev = foundDevices.getDevice(i);
      JsonObject obj = arr.createNestedObject();
      obj["name"] = dev.haveName() ? dev.getName().c_str() : "";
      obj["mac"] = dev.getAddress().toString().c_str();
      obj["rssi"] = dev.getRSSI();
    }

    pBLEScan->clearResults();
    setLedState(false);

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

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

  // Web Terminál s potvrzením shutdown a reboot
  server.on("/api/terminal", HTTP_GET, [](AsyncWebServerRequest *request){
    String cmd = request->hasParam("cmd") ? request->getParam("cmd")->value() : "";
    cmd.trim();
    String reply = "";
    bool cz = (currentLang == "cz");

    if (pendingShutdownWeb) {
      if (cmd.equalsIgnoreCase("y")) {
        pendingShutdownWeb = false;
        request->send(200, "text/plain", cz ? "Vypinam Czech Blade... (probuzeni tlacitkem BOOT nebo EN/RST)" : "Powering off Czech Blade... (wake up via BOOT or EN/RST)");
        delay(300);
        executeShutdown();
        return;
      } else {
        pendingShutdownWeb = false;
        reply = cz ? "Vypnuti zruseno." : "Shutdown aborted.";
      }
    } 
    else if (pendingRebootWeb) {
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
    else if (cmd == "shutdown") {
      pendingShutdownWeb = true;
      reply = cz ? "Opravdu chcete vypnout Czech Blade? [y/N]: " : "Do you want to shutdown? [y/N]: ";
    }
    else if (cmd == "reboot") {
      pendingRebootWeb = true;
      reply = cz ? "Opravdu chcete restartovat Czech Blade? [y/N]: " : "Do you want to reboot? [y/N]: ";
    }
    else {
      reply = processCommand(cmd, false);
      if (cmd == "sleep" || cmd.startsWith("sleep ")) {
        request->send(200, "text/plain", reply);
        delay(300);
        executeShutdown();
        return;
      }
    }

    request->send(200, "text/plain", reply);
  });

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
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) telnetClient.stop();
      telnetClient = telnetServer.available();
      telnetInputBuffer = "";
      telnetCursorPos = 0;
      telnetHistIdx = -1;
      pendingShutdownTelnet = false;
      pendingRebootTelnet = false;

      uint8_t telnetInit[] = { 255, 251, 1, 255, 251, 3, 255, 253, 34 };
      telnetClient.write(telnetInit, sizeof(telnetInit));

      telnetClient.print("\r\n\033[1;36m=== Czech Blade Telnet Shell v3.0 ===\033[0m\r\n");
      telnetClient.println("Type 'help' for available commands. (TAB for auto-completion)\r\n");
      telnetClient.print("\033[1;32mesp@czechblade>\033[0m ");
    }
  }

  if (telnetClient && telnetClient.connected()) {
    while (telnetClient.available()) {
      char c = telnetClient.read();

      // Obsluha TAB klávesy (ASCII 9) pro Telnet
      if (c == '\t') {
        if (telnetInputBuffer.length() > 0) {
          String toMatch = telnetInputBuffer;
          toMatch.toLowerCase();

          std::vector<String> matches;
          for (const auto& cmd : commandList) {
            if (cmd.startsWith(toMatch)) {
              matches.push_back(cmd);
            }
          }

          if (matches.size() == 1) {
            // Jediná shoda -> smažeme zbytek a doplníme celé slovo s mezerou
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
            // Více shod -> vypíšeme možnosti na nový řádek a znovu zobrazíme prompt
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

      // Detekce ANSI sekvence pro šipky
      if (c == 27) { // ESC
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

      // Enter (potvrzení řádku)
      if (c == '\r' || c == '\n') {
        telnetClient.print("\r\n");
        telnetInputBuffer.trim();

        // Obsluha potvrzení shutdown
        if (pendingShutdownTelnet) {
          if (telnetInputBuffer.equalsIgnoreCase("y")) {
            telnetClient.println(currentLang == "cz" ? "Vypinam Czech Blade... (probuzeni tlacitkem BOOT nebo EN/RST)" : "Powering off Czech Blade... (wake up via BOOT or EN/RST)");
            telnetClient.flush();
            delay(300);
            executeShutdown();
          } else {
            pendingShutdownTelnet = false;
            telnetClient.println(currentLang == "cz" ? "Vypnuti zruseno." : "Shutdown aborted.");
          }
          telnetInputBuffer = "";
          telnetCursorPos = 0;
          telnetClient.print("\033[1;32mesp@czechblade>\033[0m ");
          break;
        }

        // Obsluha potvrzení reboot
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
          } else if (telnetInputBuffer == "shutdown") {
            pendingShutdownTelnet = true;
            telnetClient.print(currentLang == "cz" ? "Opravdu chcete vypnout Czech Blade? [y/N]: " : "Do you want to shutdown? [y/N]: ");
            telnetInputBuffer = "";
            telnetCursorPos = 0;
            break;
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
      // Backspace / DEL
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
      // Běžný tisknutelný ASCII znak
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