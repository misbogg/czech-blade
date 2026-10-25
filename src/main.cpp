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

#define LED_PIN 2

String apSSID = "Czech Blade";
String apPass = "12345678";

AsyncWebServer server(80);
BLEScan* pBLEScan = nullptr;

void blinkLed(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    delay(delayMs);
  }
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
    .nav-btn { flex: 1; padding: 10px 0; border: none; background: transparent; color: var(--subtext); font-size: 0.72rem; font-weight: 600; cursor: pointer; display: flex; flex-direction: column; align-items: center; gap: 3px; }
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

    .modal-overlay { display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.75); backdrop-filter: blur(4px); z-index: 200; align-items: center; justify-content: center; padding: 20px; }
    .modal-box { background: var(--card); border: 1px solid var(--card-border); border-radius: 12px; padding: 18px; width: 100%; max-width: 320px; }
    input { width: 100%; padding: 10px; margin: 6px 0 12px 0; background: #080a0e; border: 1px solid var(--card-border); border-radius: 6px; color: #fff; box-sizing: border-box; }
    
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
        <button class="btn-main" onclick="scanWifi()">Scan nearby Wi-Fi</button>
        <ul id="wifi-list"></ul>
      </div>
    </div>

    <!-- 2. GEIGER -->
    <div id="p-geiger" class="panel">
      <div class="card" style="text-align: center;">
        <h3 style="margin:0; font-size: 0.95rem;" id="geiger-target-title">Signal Radar</h3>
        <p style="font-size:0.75rem; color:var(--subtext); margin:4px 0 12px 0;" id="geiger-subtitle">Measures signal strength of the target</p>
        
        <div class="radar-circle">
          <span class="signal-big" id="g-rssi">--</span>
          <span class="signal-status" id="g-desc">Off</span>
        </div>
        
        <button class="btn-main" id="g-btn" onclick="toggleGeiger()">Start Tracking</button>
      </div>
    </div>

    <!-- 3. BLE -->
    <div id="p-ble" class="panel">
      <div class="card">
        <button class="btn-main" onclick="scanBle()">Scan Bluetooth Beacons</button>
        <ul id="ble-list"></ul>
      </div>
    </div>

    <!-- 4. PING -->
    <div id="p-ping" class="panel">
      <div class="card">
        <h3 style="margin:0 0 8px 0; font-size: 0.95rem;">Connectivity Test</h3>
        <input type="text" id="ping-target" value="1.1.1.1" placeholder="IP / Host (e.g. 1.1.1.1)">
        <button class="btn-main" onclick="runPing()">Run Ping Test</button>
        <div id="ping-output" style="margin-top: 12px; font-size: 0.85rem;"></div>
      </div>
    </div>
  </div>

  <div class="nav-bar">
    <button class="nav-btn active" onclick="switchTab('wifi', this)"><span>📶</span>Wi-Fi</button>
    <button class="nav-btn" onclick="switchTab('geiger', this)"><span>🎯</span>Geiger</button>
    <button class="nav-btn" onclick="switchTab('ble', this)"><span>📡</span>BLE</button>
    <button class="nav-btn" onclick="switchTab('ping', this)"><span>🌐</span>Ping</button>
  </div>

  <!-- MODAL: CONNECT -->
  <div class="modal-overlay" id="modal-connect">
    <div class="modal-box">
      <h3 style="margin-top:0; font-size:1rem;" id="modal-connect-title">Connect to network</h3>
      <input type="password" id="modal-connect-pass" placeholder="Password (empty for open)">
      <div style="display:flex; gap:8px;">
        <button class="btn-main" onclick="submitConnect()">Connect</button>
        <button class="btn-main btn-secondary" onclick="closeModal('modal-connect')">Cancel</button>
      </div>
    </div>
  </div>

  <!-- MODAL: OFFLINE ALERT -->
  <div class="modal-overlay" id="modal-offline">
    <div class="modal-box" style="text-align: center;">
      <h3 style="margin-top:0; color:var(--red);">ESP32 is Offline</h3>
      <p style="font-size:0.85rem; color:var(--subtext);">The device is not connected to any router. Go to the Wi-Fi tab and connect to a network first.</p>
      <button class="btn-main" onclick="closeModal('modal-offline'); switchTab('wifi', document.querySelectorAll('.nav-btn')[0]);">Go to Wi-Fi</button>
    </div>
  </div>

  <!-- MODAL: AP SETTINGS -->
  <div class="modal-overlay" id="modal-settings">
    <div class="modal-box">
      <h3 style="margin-top:0; font-size:1rem;">Access Point Settings</h3>
      <label style="font-size:0.75rem; color:var(--subtext);">Broadcast SSID:</label>
      <input type="text" id="ap-name-input">
      <label style="font-size:0.75rem; color:var(--subtext);">Access Point Password:</label>
      <input type="password" id="ap-pass-input" placeholder="Min. 8 characters">
      <div style="display:flex; gap:8px;">
        <button class="btn-main" onclick="submitSettings()">Save & Restart AP</button>
        <button class="btn-main btn-secondary" onclick="closeModal('modal-settings')">Close</button>
      </div>
      <div class="modal-footer">
        &copy; 2026 Misbo 8 &bull; Czech Blade Project &bull; misbo8.cz
      </div>
    </div>
  </div>

  <script>
    let geigerActive = false;
    let geigerAbortCtrl = null;
    let audioCtx = null;
    let selectedSsid = "";
    let isConnectedToWifi = false;

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
      list.innerHTML = '<li style="justify-content:center; color:#888;">Scanning 2.4 GHz band...</li>';
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
            <button class="action-sub-btn" onclick="event.stopPropagation(); startTargetMeasure('${net.ssid}')">📡 Measure</button>
          </div>
        `;
        list.appendChild(li);
      });
    }

    function startTargetMeasure(ssid) {
      selectedSsid = ssid;
      document.getElementById('geiger-target-title').innerText = "Tracking: " + ssid;
      document.getElementById('geiger-subtitle').innerText = "Measures signal between ESP32 and router";
      switchTab('geiger', document.querySelectorAll('.nav-btn')[1]);
      if (!geigerActive) toggleGeiger();
    }

    function openConnectModal(ssid) {
      if (!ssid) return alert("Cannot connect without SSID.");
      selectedSsid = ssid;
      document.getElementById('modal-connect-title').innerText = "Connect to " + ssid;
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
        alert("Connection to " + selectedSsid + " failed!");
        checkInternetStatus();
      }
    }

    function toggleGeiger() {
      const btn = document.getElementById('g-btn');
      if (geigerActive) {
        geigerActive = false;
        if (geigerAbortCtrl) geigerAbortCtrl.abort();
        btn.innerText = "Start Tracking";
        btn.classList.remove('btn-secondary');
        document.getElementById('g-rssi').innerText = "--";
        document.getElementById('g-desc').innerText = "Off";
      } else {
        geigerActive = true;
        btn.innerText = "Stop Tracking";
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
            let desc = pct > 75 ? "Strong" : (pct > 40 ? "Medium" : "Weak");
            document.getElementById('g-desc').innerText = desc;
            playBeep(300 + (pct * 9));
          } else {
            document.getElementById('g-rssi').innerText = "--";
            document.getElementById('g-desc').innerText = "Out of range";
          }
        } catch (e) {
          if (e.name === 'AbortError') break;
        }
        await new Promise(r => setTimeout(r, 650));
      }
    }

    async function scanBle() {
      const list = document.getElementById('ble-list');
      list.innerHTML = '<li style="justify-content:center; color:#888;">Scanning BLE beacons nearby...</li>';
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
        if (!data.length) list.innerHTML = '<li style="justify-content:center; color:#888;">No BLE devices in range.</li>';
      } catch (err) {
        list.innerHTML = '<li style="justify-content:center; color:var(--red);">BLE communication error.</li>';
      }
    }

    async function runPing() {
      if (!isConnectedToWifi) {
        document.getElementById('modal-offline').style.display = 'flex';
        return;
      }

      let host = document.getElementById('ping-target').value.trim() || "1.1.1.1";
      let out = document.getElementById('ping-output');
      out.innerHTML = `<span style="color:#aaa;">Sending 12 packets to ${host}...</span>`;
      
      try {
        let res = await fetch('/api/ping?host=' + encodeURIComponent(host));
        let data = await res.json();
        if (data.success) {
          out.innerHTML = `<span style="color:var(--green); font-weight:bold;">Host is reachable!</span><br>Average latency: <strong>${data.avg_time} ms</strong> (${data.success_count}/12)`;
        } else {
          out.innerHTML = `<span style="color:var(--red); font-weight:bold;">Host unreachable!</span> (Packet loss)`;
        }
      } catch (e) {
        out.innerHTML = `<span style="color:var(--red);">Request error.</span>`;
      }
      checkInternetStatus();
    }

    async function checkInternetStatus(checking = false) {
      const dot = document.getElementById('net-dot');
      const desc = document.getElementById('net-desc');
      if (checking) {
        dot.className = "dot";
        desc.innerText = "Connecting...";
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
          desc.innerText = "No internet";
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
      document.getElementById('modal-settings').style.display = "flex";
    }

    async function submitSettings() {
      let name = document.getElementById('ap-name-input').value;
      let pass = document.getElementById('ap-pass-input').value;
      if (!name) return alert("Enter SSID!");
      closeModal('modal-settings');
      await fetch(`/api/set-ap?ssid=${encodeURIComponent(name)}&pass=${encodeURIComponent(pass)}`);
      alert("Reconfigured! Reconnect if SSID/password changed.");
      document.getElementById('ap-pill').innerText = name;
    }

    function closeModal(id) {
      document.getElementById(id).style.display = "none";
    }

    setInterval(checkInternetStatus, 8000);
    checkInternetStatus();
  </script>
</body>
</html>
)rawhtml";

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  blinkLed(3, 80);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID.c_str(), apPass.c_str());

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);

  // 1. Web Root
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

  // 6. BLE Scanner
  server.on("/api/scan-ble", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(LED_PIN, HIGH);
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
    digitalWrite(LED_PIN, LOW);

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // 7. Ping Tester
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

  // 9. Configure AP
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