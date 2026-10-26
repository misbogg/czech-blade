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

#define LED_PIN 2

Preferences prefs;
String apSSID = "Czech Blade";
String apPass = "12345678";
String currentLang = "eng";

AsyncWebServer server(80);
BLEScan* pBLEScan = nullptr;

// Globální mezipaměť pro BLE výsledky
struct BleDeviceItem {
  String name;
  String mac;
  int rssi;
};
std::vector<BleDeviceItem> bleResults;
volatile bool isBleScanning = false;

void blinkLed(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    delay(delayMs);
  }
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
    input:focus, select:focus {
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

  </div>

  <div class="nav-bar">
    <button class="nav-tab active" onclick="switchTab('wifi', this)"><span id="nav-text-wifi">WIFI</span></button>
    <button class="nav-tab" onclick="switchTab('geiger', this)"><span id="nav-text-geiger">TRACK</span></button>
    <button class="nav-tab" onclick="switchTab('ble', this)"><span id="nav-text-ble">BLE</span></button>
    <button class="nav-tab" onclick="switchTab('ping', this)"><span id="nav-text-ping">PING</span></button>
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

    const dict = {
      cz: {
        nav_wifi: "WIFI", nav_geiger: "RADAR", nav_ble: "BLE", nav_ping: "PING",
        hdr_wifi: "WLAN KONTROLÉR (2.4 GHz)", btn_scan_wifi: "SKENOVAT PÁSMO", scanning_wifi: "PROBÍHÁ SKENOVÁNÍ...",
        geiger_title: "ZAMĚŘOVAČ SIGNÁLU", geiger_standby: "POHOTOVOST",
        btn_geiger_start: "SPUSTIT ZAMĚŘOVÁNÍ", btn_geiger_stop: "ZASTAVIT", btn_measure: "ZAMĚŘIT",
        hdr_ble: "BLE MONITOR", btn_scan_ble: "ZACHYTIT MAJÁKY (2s)", scanning_ble: "SKENUJI PÁSMO...",
        hdr_ping: "ICMP DIAGNOSTIKA", lbl_ping: "CÍL TESTU / HOST:", btn_run_ping: "ODESLAT 12 PAKETŮ",
        ping_exec: "Odesílám pakety na ", ping_ok: "HOSTITEL AKTIVNÍ", ping_fail: "HOSTITEL NEDOSTUPNÝ",
        modal_conn_title: "PŘIPOJENÍ K SÍTI", modal_conn_btn: "PŘIPOJIT", modal_cancel_btn: "ZRUŠIT",
        settings_title: "KONFIGURACE SYSTÉMU", lbl_lang: "JAZYK ROZHRANÍ:", lbl_ap_ssid: "VYSÍLANÉ SSID:", lbl_ap_pass: "HESLO K AP:",
        btn_save_settings: "ULOŽIT A RESTART", btn_close_settings: "ZAVŘÍT"
      },
      eng: {
        nav_wifi: "WIFI", nav_geiger: "TRACK", nav_ble: "BLE", nav_ping: "PING",
        hdr_wifi: "WLAN CONTROLLER (2.4 GHz)", btn_scan_wifi: "SCAN ACTIVE FREQUENCIES", scanning_wifi: "SCANNING AIRWAVES...",
        geiger_title: "SIGNAL DETECTOR", geiger_standby: "STANDBY",
        btn_geiger_start: "ENGAGE RECEIVER", btn_geiger_stop: "DISENGAGE", btn_measure: "TRACK",
        hdr_ble: "BLUETOOTH LE SNIFFER", btn_scan_ble: "CAPTURE BEACONS (2s)", scanning_ble: "SCANNING PROXIMITY...",
        hdr_ping: "ICMP DIAGNOSTICS", lbl_ping: "TARGET IP / HOST:", btn_run_ping: "EXECUTE PING x12",
        ping_exec: "Pinging target ", ping_ok: "HOST REACHABLE", ping_fail: "HOST UNREACHABLE",
        modal_conn_title: "CONNECT TO NETWORK", modal_conn_btn: "CONNECT", modal_cancel_btn: "CANCEL",
        settings_title: "SYSTEM CONFIGURATION", lbl_lang: "INTERFACE LANGUAGE:", lbl_ap_ssid: "BROADCAST SSID:", lbl_ap_pass: "ACCESS KEY:",
        btn_save_settings: "SAVE & REBOOT", btn_close_settings: "CLOSE"
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

    // Bezpečný neblokující BLE dotaz
    async function scanBle() {
      const list = document.getElementById('ble-list');
      list.innerHTML = `<li style="color:var(--muted); text-align:center; padding:10px;">${dict[currentLang].scanning_ble}</li>`;
      
      try {
        // 1. Zahájení skenu
        await fetch('/api/scan-ble-start');

        // 2. Počkáme 2,2 sekundy v JavaScriptu (server tak nezamrzne a odpovídá dál)
        await new Promise(r => setTimeout(r, 2200));

        // 3. Vyzvednutí výsledků
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

  prefs.begin("blade", false);
  currentLang = prefs.getString("lang", "eng");
  prefs.end();

  blinkLed(3, 80);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID.c_str(), apPass.c_str());

  // Inicializace BLE s koexistencí pro Wi-Fi
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
    digitalWrite(LED_PIN, HIGH);
    int n = WiFi.scanNetworks(false, true);
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < n; ++i) {
      JsonObject net = arr.createNestedObject();
      net["ssid"] = WiFi.SSID(i);
      net["rssi"] = WiFi.RSSI(i);
      net["bssid"] = WiFi.BSSIDstr(i);
      net["ch"] = WiFi.channel(i);
    }
    WiFi.scanDelete();
    digitalWrite(LED_PIN, LOW);

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 3. Connect to Router
  server.on("/api/connect", HTTP_GET, [](AsyncWebServerRequest *request){
    String ssid = request->hasParam("ssid") ? request->getParam("ssid")->value() : "";
    String pass = request->hasParam("pass") ? request->getParam("pass")->value() : "";

    digitalWrite(LED_PIN, HIGH);
    WiFi.begin(ssid.c_str(), pass.length() > 0 ? pass.c_str() : NULL);

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 25) {
      delay(200);
      timeout++;
    }
    digitalWrite(LED_PIN, LOW);

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
      digitalWrite(LED_PIN, HIGH);
      int n = WiFi.scanNetworks(false, true);
      for (int i = 0; i < n; ++i) {
        if (WiFi.SSID(i) == target) {
          if (WiFi.RSSI(i) > rssi) {
            rssi = WiFi.RSSI(i);
          }
        }
      }
      WiFi.scanDelete();
      digitalWrite(LED_PIN, LOW);
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
      digitalWrite(LED_PIN, HIGH);
      pBLEScan->stop();
      pBLEScan->clearResults();
      // Asynchronní start na 2 sekundy s callbackem po dokončení
      pBLEScan->start(2, onBleScanComplete, false);
    }
    request->send(200, "text/plain", "SCANNING");
  });

  // 6B. BLE Scanner - Results (Vyzvednutí hotových výsledků)
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

    digitalWrite(LED_PIN, HIGH);

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

    digitalWrite(LED_PIN, LOW);

    bool ok = (successCount > 0);
    doc["success"] = ok;
    doc["avg_time"] = ok ? (int)(totalTime / successCount) : 0;
    doc["success_count"] = successCount;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 8. Status
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

  // 9. NVS Language API
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

  // 10. AP Config
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
  delay(100);
}