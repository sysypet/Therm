#include <esp_wifi.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Update.h>
#include <esp_task_wdt.h>  // WATCHDOG: Task WDT header

#define FW_VERSION "2.3.12"  // Firmware verzió – automatikus keresés 3 egymásutáni hiányzás után

#define RELAY_HEAT_PIN 5   // Fűtés relay
#define RELAY_COOL_PIN 6   // Hűtés relay
#define STATUS_LED_PIN  8   // Státusz LED (LOW=aktív, HIGH=inaktív)
#define ONE_WIRE_BUS 4

#define WATCHDOG_TIMEOUT_S 30  // Watchdog timeout 30 másodperc

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WebServer server(80);
Preferences preferences;

String ssid = "";
String password = "";
int targetTemperature = 22;
bool shouldRestart = false;
unsigned long restartTimer = 0;

bool scanRequested = false;
unsigned long scanRequestTime = 0;

float lastRoomTemp0 = -127.0;
float lastRoomTemp1 = -127.0;
float lastRoomTemp2 = -127.0;
float lastRoomTemp3 = -127.0;
unsigned long lastTempRequest = 0;
bool tempRequested = false;
int activeSensor = 0;
int sensorCount = 0;
bool isHeating = false;
float hysteresis = 0.5;
bool relayInverted = false;

// Szenzor hiányzás számláló – ha 3 mérésig nincs adat, keresés indul
int missingCount0 = 0;
int missingCount1 = 0;
int missingCount2 = 0;
int missingCount3 = 0;

String sensorName0 = "1. hőmérő";
String sensorName1 = "2. hőmérő";
String sensorName2 = "3. hőmérő";
String sensorName3 = "4. hőmérő";
String unitName = "ESP32_Thermostat";

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="hu">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Termosztát UI</title>
    <style>
        html, body {
            position: fixed; width: 100%; height: 100%; overflow: hidden; margin: 0; padding: 0;
            background: radial-gradient(ellipse at 60% 20%, #1a1a2e 0%, #121214 45%, #0d1a1a 100%);
            color: #fff; font-family: -apple-system, BlinkMacSystemFont, sans-serif;
            display: flex; flex-direction: column; align-items: center; justify-content: center;
            user-select: none; -webkit-user-select: none;
        }
        h2 { margin-bottom: 25px; color: #a0a0a5; font-weight: 500; font-size: 1.4rem; letter-spacing: 0.5px; pointer-events: none; }
        .picker-container { position: relative; width: 280px; height: 280px; display: flex; align-items: center; justify-content: center; pointer-events: auto; }
        canvas { position: absolute; top: 0; left: 0; width: 100%; height: 100%; cursor: pointer; pointer-events: auto; }
        .dial-center { position: absolute; width: 190px; height: 190px; background: #1a1a1e; border-radius: 50%; box-shadow: inset 0 0 15px rgba(0,0,0,0.5), 0 10px 20px rgba(0,0,0,0.4); display: flex; flex-direction: column; align-items: center; justify-content: center; z-index: 10; pointer-events: none; }
        .display-wrapper { display: flex; align-items: flex-start; justify-content: center; }
        .temp-display { font-size: 4.5rem; font-weight: 700; line-height: 0.85; }
        .unit { font-size: 1.6rem; color: #ff5e62; font-weight: 600; margin-left: 2px; transition: color 0.5s; }
        .label { font-size: 0.8rem; color: #707075; text-transform: uppercase; letter-spacing: 1.5px; margin-top: 12px; }
        .mode-label { font-size: 1.0rem; font-weight: 700; margin-top: 14px; letter-spacing: 1.5px; transition: color 0.4s; }
        .sensor-readings { display: flex; flex-direction: column; align-items: center; gap: 6px; margin-top: 18px; margin-bottom: 16px; width: 280px; }
        .sensor-row-ext { display: flex; align-items: center; justify-content: space-between; width: 100%; padding: 8px 16px; border-radius: 12px; box-sizing: border-box; transition: background 0.3s; }
        .sensor-row-ext.active { background: #1a7a4a; border: 1px solid #1a7a4a; }
        .sensor-row-ext.inactive { background: rgba(255,255,255,0.09); border: 1px solid rgba(255,255,255,0.15); }
        .sensor-row-name { font-size: 0.82rem; font-weight: 500; letter-spacing: 0.3px; }
        .sensor-row-ext.active .sensor-row-name { color: #ffffff; }
        .sensor-row-ext.inactive .sensor-row-name { color: #d0d0d5; }
        .sensor-row-temp { font-size: 1.05rem; font-weight: 700; }
        .sensor-row-ext.active .sensor-row-temp { color: #ffffff; }
        .sensor-row-ext.inactive .sensor-row-temp { color: #ffffff; }
        .reset-btn { padding: 10px 20px; background: #2e2e36; border: 1px solid #484850; color: #d0d0d5; border-radius: 20px; font-size: 0.85rem; cursor: pointer; transition: background 0.2s; -webkit-appearance: none; pointer-events: auto !important; }
        .reset-btn:active { background: #3a3a42; transform: translate(2px, 2px); }
        .search-btn { padding: 10px 20px; background: #00d2ff; color: #121214; border: none; border-radius: 20px; font-size: 0.85rem; font-weight: bold; cursor: pointer; transition: background 0.2s; -webkit-appearance: none; pointer-events: auto !important; margin-top: 10px; }
        .search-btn:active { background: #0098b8; transform: translate(2px, 2px); }
        .ip-info { position: fixed; bottom: 10px; left: 0; right: 0; text-align: center; font-size: 0.7rem; color: #c0c0c5; pointer-events: none; }
    </style>
</head>
<body>
    <h2>Hőmérséklet Beállítás</h2>
    <div class="picker-container" id="pickerContainer">
        <canvas id="dialCanvas" width="560" height="560"></canvas>
        <div class="dial-center">
            <div class="display-wrapper">
                <span class="temp-display" id="tempValue">--</span>
                <sup class="unit" id="unitLabel">&deg;C</sup>
            </div>
            <div class="mode-label" id="modeLabel">–</div>
        </div>
    </div>
    <div class="sensor-readings" id="sensorReadings">
        <div class="sensor-row-ext active" id="sensorRowA">
            <span class="sensor-row-name" id="sensorNameA">--</span>
            <span class="sensor-row-temp" id="sensorTempA">--.- °C</span>
        </div>
        <div class="sensor-row-ext inactive" id="sensorRowB">
            <span class="sensor-row-name" id="sensorNameB">--</span>
            <span class="sensor-row-temp" id="sensorTempB">--.- °C</span>
        </div>
        <div class="sensor-row-ext inactive" id="sensorRowC">
            <span class="sensor-row-name" id="sensorNameC">--</span>
            <span class="sensor-row-temp" id="sensorTempC">--.- °C</span>
        </div>
        <div class="sensor-row-ext inactive" id="sensorRowD">
            <span class="sensor-row-name" id="sensorNameD">--</span>
            <span class="sensor-row-temp" id="sensorTempD">--.- °C</span>
        </div>
        <button id="mainSearchBtn" class="search-btn" style="display:none; margin-top:10px;" onclick="mainSearchSensors()">Hőmérők keresése</button>
    </div>
    <button class="reset-btn" onclick="location.href='/wifi-config'">Beállítások</button>
    <div class="ip-info" id="ipInfo"></div>
    <script>
        const minTemp = 15, maxTemp = 30;
        let currentTemp = 22;
        let isDragging = false;
        let isHeating = false;
        let relayMode = 'normal';

        const canvas = document.getElementById('dialCanvas');
        const ctx = canvas.getContext('2d');
        const tempValue = document.getElementById('tempValue');
        const unitLabel = document.getElementById('unitLabel');

        async function initPage() {
            try {
                const raw = await fetch('/ip').then(r => r.text());
                const parts = raw.split('|');
                const ip = parts[0];
                const name = parts[1] || 'ESP32_Thermostat';
                const ver = parts[2] || '';
                document.getElementById('ipInfo').innerHTML = '🌐 ' + ip + ' | ' + name + '.local' + (ver ? ' | v' + ver : '');
            } catch(e) {}
            try {
                const [tempVal, heatingVal] = await Promise.all([
                    fetch('/get-current-temp').then(r => r.text()),
                    fetch('/get-heating-status').then(r => r.text())
                ]);
                let parsed = parseInt(tempVal);
                if(!isNaN(parsed)) currentTemp = parsed;
                isHeating = (heatingVal.trim() === '1');
                drawDial(currentTemp);
            } catch(e) {}
        }
        initPage();

        function fetchAllDisplayTemps() {
            Promise.all([
                fetch('/get-all-temps').then(r => r.json()),
                fetch('/get-sensor-names').then(r => r.json())
            ]).then(([temps, names]) => {
                const count = temps.count;
                const readings = document.getElementById('sensorReadings');
                const rowA = document.getElementById('sensorRowA');
                const rowB = document.getElementById('sensorRowB');
                const rowC = document.getElementById('sensorRowC');
                const rowD = document.getElementById('sensorRowD');
                if (count === 0) {
                    rowA.style.display = 'none';
                    rowB.style.display = 'none';
                    rowC.style.display = 'none';
                    rowD.style.display = 'none';
                    readings.innerHTML = '<button class="search-btn" onclick="mainSearchSensors()">Hőmérők keresése</button>';
                    return;
                }
                if (!rowA) return;
                const activeIdx = temps.active;
                const allTemps = [temps.temp0, temps.temp1, temps.temp2, temps.temp3];
                const allNames = [names.name0, names.name1, names.name2, names.name3];
                rowA.classList.remove('inactive');
                rowA.classList.add('active');
                rowA.style.display = 'flex';
                document.getElementById('sensorNameA').textContent = allNames[activeIdx];
                document.getElementById('sensorTempA').innerHTML = allTemps[activeIdx] + ' &deg;C';
                const otherIndices = [];
                for (let i = 0; i < count; i++) {
                    if (i !== activeIdx) otherIndices.push(i);
                }

                const rows = [rowB, rowC, rowD];
                for (let i = 0; i < rows.length; i++) {
                    if (i < otherIndices.length) {
                        const idx = otherIndices[i];
                        rows[i].style.display = 'flex';
                        rows[i].classList.remove('active');
                        rows[i].classList.add('inactive');
                        document.getElementById(['sensorNameB', 'sensorNameC', 'sensorNameD'][i]).textContent = allNames[idx];
                        document.getElementById(['sensorTempB', 'sensorTempC', 'sensorTempD'][i]).innerHTML = allTemps[idx] + ' &deg;C';
                    } else {
                        rows[i].style.display = 'none';
                    }
                }
            }).catch(() => {});
        }
        setInterval(fetchAllDisplayTemps, 2000);
        fetchAllDisplayTemps();

        function fetchHeatingStatus() {
            fetch('/get-heating-status').then(r => r.text()).then(val => {
                const newHeating = (val.trim() === '1');
                if (newHeating !== isHeating) { isHeating = newHeating; drawDial(currentTemp); }
            }).catch(() => {});
        }
        setInterval(fetchHeatingStatus, 1000);
        fetchHeatingStatus();

        function fetchMode() {
            fetch('/get-relay-mode').then(r => r.text()).then(mode => {
                relayMode = mode;
                const el = document.getElementById('modeLabel');
                if (mode === 'inverted') { el.textContent = 'Hűtés'; el.style.color = '#3cffa0'; }
                else { el.textContent = 'Fűtés'; el.style.color = '#ff5e62'; }
                drawDial(currentTemp);
            }).catch(() => {});
        }
        setInterval(fetchMode, 5000);
        fetchMode();

        function drawDial(temp) {
            if (!ctx) return;
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            tempValue.innerText = temp;

            let dialColor = isHeating ? '#ff5e62' : '#3cffa0';
            let unitColor = isHeating ? '#ff5e62' : '#3cffa0';
            unitLabel.style.color = unitColor;
            const cx = canvas.width / 2, cy = canvas.height / 2, radius = 220;
            const startAngle = 0.66 * Math.PI, endAngle = 0.34 * Math.PI;
            ctx.beginPath();
            ctx.arc(cx, cy, radius, startAngle, endAngle, false);
            ctx.strokeStyle = '#232329'; ctx.lineWidth = 28; ctx.lineCap = 'round'; ctx.stroke();
            if (temp > minTemp) {
                const percent = (temp - minTemp) / (maxTemp - minTemp);
                let currentAngle = startAngle + (percent * 1.68 * Math.PI);
                ctx.beginPath();
                ctx.arc(cx, cy, radius, startAngle, currentAngle, false);
                ctx.strokeStyle = dialColor;
                ctx.lineWidth = 28; ctx.lineCap = 'round'; ctx.stroke();
            }
            if (temp <= minTemp) {
                const dotAngle = startAngle;
                const dotX = cx + radius * Math.cos(dotAngle);
                const dotY = cy + radius * Math.sin(dotAngle);
                ctx.beginPath();
                ctx.arc(dotX, dotY, 14, 0, 2 * Math.PI);
                ctx.fillStyle = dialColor; ctx.fill();
            }
        }

        function calculateTemp(e) {
            e.preventDefault();
            const rect = canvas.getBoundingClientRect();
            const centerX = rect.left + rect.width / 2, centerY = rect.top + rect.height / 2;
            let clientX = e.touches ? e.touches[0].clientX : e.clientX;
            let clientY = e.touches ? e.touches[0].clientY : e.clientY;
            let angle = Math.atan2(clientY - centerY, clientX - centerX) * (180 / Math.PI);
            angle = (angle + 360) % 360;
            let relativeAngle = angle >= 120 ? angle - 120 : angle + 240;
            if (relativeAngle < 0) relativeAngle = 0;
            if (relativeAngle > 300) relativeAngle = 300;
            let target = minTemp + Math.round((relativeAngle / 300) * (maxTemp - minTemp));
            if (target < minTemp) target = minTemp;
            if (target > maxTemp) target = maxTemp;
            if (target !== currentTemp) { currentTemp = target; drawDial(currentTemp); sendTemp(currentTemp); }
        }

        let timeout;
        function sendTemp(temp) {
            clearTimeout(timeout);
            timeout = setTimeout(() => { fetch('/set-temp?value=' + temp); }, 40);
        }

        function mainSearchSensors() {
            const btn = document.getElementById('mainSearchBtn');
            btn.innerText = 'Keresés...';
            btn.disabled = true;
            fetch('/search-sensors').then(r => r.text()).then(data => {
                setTimeout(() => { location.reload(); }, 1000);
            }).catch(() => {
                btn.innerText = 'Hőmérők keresése';
                btn.disabled = false;
            });
        }

        function checkSensorSearch() {
            fetch('/get-all-temps').then(r => r.json()).then(data => {
                const btn = document.getElementById('mainSearchBtn');
                if (data.count === 0) {
                    btn.style.display = 'block';
                } else {
                    btn.style.display = 'none';
                }
            }).catch(() => {});
        }
        checkSensorSearch();
        setInterval(checkSensorSearch, 5000);
        canvas.addEventListener('touchstart', function(e) { isDragging = true; calculateTemp(e); }, { passive: false });
        canvas.addEventListener('touchmove', function(e) { if (isDragging) calculateTemp(e); }, { passive: false });
        window.addEventListener('touchend', function() { isDragging = false; });
        canvas.addEventListener('mousedown', function(e) { isDragging = true; calculateTemp(e); });
        canvas.addEventListener('mousemove', function(e) { if (isDragging) calculateTemp(e); });
        window.addEventListener('mouseup', function() { isDragging = false; });
    </script>
</body>
</html>
)rawliteral";

void handleWifiConfig() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");

  sensors.requestTemperatures();
  delay(1500);
  float t0 = sensors.getTempCByIndex(0);
  float t1 = sensors.getTempCByIndex(1);
  float t2 = sensors.getTempCByIndex(2);
  float t3 = sensors.getTempCByIndex(3);
  bool ok0 = (t0 != DEVICE_DISCONNECTED_C);
  bool ok1 = (t1 != DEVICE_DISCONNECTED_C);
  bool ok2 = (t2 != DEVICE_DISCONNECTED_C);
  bool ok3 = (t3 != DEVICE_DISCONNECTED_C);
  if (ok0) lastRoomTemp0 = t0;
  if (ok1) lastRoomTemp1 = t1;
  if (ok2) lastRoomTemp2 = t2;
  if (ok3) lastRoomTemp3 = t3;
  sensorCount = (ok0 ? 1 : 0) + (ok1 ? 1 : 0) + (ok2 ? 1 : 0) + (ok3 ? 1 : 0);
  if (activeSensor >= sensorCount && sensorCount > 0) activeSensor = 0;

  WiFi.scanNetworks(true);
  scanRequested = true;
  scanRequestTime = millis();

  preferences.begin("wifi-config", true);
  String savedSsid = preferences.getString("ssid", "");
  String savedPass = preferences.getString("password", "");
  preferences.end();

  String modeLabel = relayInverted ? "Hűtés" : "Fűtés";
  String modeBg    = relayInverted ? "#3cc8ff" : "#ff5e62";
  String html = "<!DOCTYPE html><html lang='hu'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Beállítások</title><style>";
  html += "body{font-family:-apple-system,sans-serif;background:#121214;color:#fff;padding:20px;text-align:center;}";
  html += ".container{max-width:360px;margin:40px auto;background:#1a1a1e;padding:25px;border-radius:20px;box-shadow:0 10px 30px rgba(0,0,0,0.5);}";
  html += "h2{color:#a0a0a5;margin-bottom:5px;font-size:1.4rem;}";
  html += ".info-text{font-size:0.85rem;color:#707075;margin-bottom:20px;}";
  html += ".network-list{text-align:left;margin:15px 0;max-height:160px;overflow-y:auto;background:#232329;border-radius:8px;padding:5px;border:1px solid #333;}";
  html += ".network-item{padding:12px;border-bottom:none;cursor:pointer;font-size:0.9rem;display:flex;justify-content:space-between;align-items:center;background:#2e2e36;border:1px solid #484850;border-radius:8px;margin-bottom:8px;}";
  html += ".network-item:hover{background:#393945;}.network-item.selected{background:#2d5a4a;color:#fff;}";
  html += ".saved-badge{background:#00d2ff;color:#121214;font-size:0.7rem;font-weight:bold;padding:2px 6px;border-radius:4px;}";
  html += ".scanning{padding:12px;color:#707075;font-size:0.9rem;text-align:center;}";
  html += ".input-group{text-align:left;margin-top:15px;}label{font-size:0.8rem;color:#a0a0a5;display:block;margin-bottom:5px;}";
  html += "input[type='text'],input[type='password']{width:100%;padding:12px;background:#232329;border:1px solid #444;color:#fff;border-radius:8px;box-sizing:border-box;font-size:1rem;}";
  html += "button{width:100%;padding:12px;background:#00d2ff;border:none;color:#121214;font-weight:bold;border-radius:8px;margin-top:20px;cursor:pointer;font-size:1rem;transition:all 0.15s ease;}";
  html += "button:active{transform:translate(2px,2px);box-shadow:inset 0 2px 4px rgba(0,0,0,0.3);}";
  html += ".status-text{color:#ff5e62;font-weight:bold;}";
  html += ".sensor-box{background:#232329;border-radius:12px;padding:15px;margin:20px 0;text-align:left;border:1px solid #333;}";
  html += ".sensor-box h3{color:#a0a0a5;font-size:0.9rem;margin:0 0 12px 0;text-transform:uppercase;letter-spacing:1px;}";
  html += ".sensor-row{display:flex;align-items:center;gap:8px;padding:10px 0;border-bottom:1px solid #333;flex-wrap:wrap;}";
  html += ".sensor-row:last-child{border-bottom:none;}";
  html += ".sensor-temp{font-size:1.1rem;font-weight:bold;color:#ff5e62;white-space:nowrap;}";
  html += ".sensor-name-input{flex:1;min-width:80px;padding:6px 10px;background:#121214;border:1px solid #444;color:#fff;border-radius:6px;font-size:0.9rem;}";
  html += ".sensor-name-input:focus{outline:none;border-color:#00d2ff;}";
  html += "input[type='radio']{width:20px;height:20px;accent-color:#00d2ff;cursor:pointer;flex-shrink:0;}";
  html += "</style></head><body><div class='container'><h2>Beállítások</h2>";
  html += "<div class='sensor-box'><h3 style='text-align:center;'>Eszköz neve</h3>";
  html += "<div class='input-group'>";
  html += "<input type='text' id='deviceNameInput' maxlength='24' value='" + unitName + "' placeholder='pl: Nappali' style='background:#232329;border:1px solid #444;margin-top:0;'>";
  html += "</div>";
  html += "<div style='margin-top:12px;text-align:center;'>";
  html += "<div style='font-size:0.8rem;color:#a0a0a5;margin-bottom:8px;'>Elérhető ezen a néven:</div>";
  html += "<div style='display:flex;align-items:center;gap:8px;padding:12px;justify-content:center;background:#2e2e36;border:1px solid #484850;border-radius:12px;'>";
  html += "<span style='font-size:1rem;color:#00d2ff;word-break:break-all;text-align:center;' id='deviceNameDisplay'>" + unitName + ".local</span>";
  html += "</div>";
  html += "</div>";
  html += "</div>";

  html += "<div class='sensor-box'><h3 style='display:flex;justify-content:space-between;align-items:center;'>Hőmérők (max 4)<span style='font-size:0.72rem;font-weight:400;color:#f0c040;letter-spacing:0.5px;text-transform:none;'>Dallas DS18B20</span></h3>";

  html += "<button type='button' id='searchSensorBtn' onclick='searchSensors()' style='margin-top:0;background:#00d2ff;color:#121214;width:100%;padding:12px;border:none;font-weight:bold;border-radius:8px;cursor:pointer;font-size:1rem;'>Hőmérők keresése</button>";
  html += "<div id='sensorSearchStatus' style='font-size:0.8rem;color:#707075;margin-top:8px;text-align:center;'></div>";

  if (sensorCount > 0) {
    html += "<div style='margin-top:15px; border-top:1px solid #333; padding-top:10px;'></div>";
    bool s_ok0 = (lastRoomTemp0 != -127.0) && (missingCount0 < 3);
    bool s_ok1 = (lastRoomTemp1 != -127.0) && (missingCount1 < 3);
    bool s_ok2 = (lastRoomTemp2 != -127.0) && (missingCount2 < 3);
    bool s_ok3 = (lastRoomTemp3 != -127.0) && (missingCount3 < 3);

    bool activeValid = false;
    if (activeSensor == 0 && s_ok0) activeValid = true;
    else if (activeSensor == 1 && s_ok1) activeValid = true;
    else if (activeSensor == 2 && s_ok2) activeValid = true;
    else if (activeSensor == 3 && s_ok3) activeValid = true;

    if (!activeValid) {
      if (s_ok0) activeSensor = 0;
      else if (s_ok1) activeSensor = 1;
      else if (s_ok2) activeSensor = 2;
      else if (s_ok3) activeSensor = 3;
    }

    if (s_ok0) {
      html += "<div class='sensor-row'>";
      html += "<input type='radio' name='sensorSelect' id='s0' value='0' onchange=\"selectSensor(0)\""; if (activeSensor == 0) html += " checked"; html += ">";
      html += "<input type='text' class='sensor-name-input' id='name0' value='" + sensorName0 + "' maxlength='16' onchange=\"updateSensorName(0, this.value)\">";
      html += "<span class='sensor-temp'>" + String(lastRoomTemp0, 1) + " &deg;C</span>";
      html += "</div>";
    }
    if (s_ok1) {
      html += "<div class='sensor-row'>";
      html += "<input type='radio' name='sensorSelect' id='s1' value='1' onchange=\"selectSensor(1)\""; if (activeSensor == 1) html += " checked"; html += ">";
      html += "<input type='text' class='sensor-name-input' id='name1' value='" + sensorName1 + "' maxlength='16' onchange=\"updateSensorName(1, this.value)\">";
      html += "<span class='sensor-temp'>" + String(lastRoomTemp1, 1) + " &deg;C</span>";
      html += "</div>";
    }
    if (s_ok2) {
      html += "<div class='sensor-row'>";
      html += "<input type='radio' name='sensorSelect' id='s2' value='2' onchange=\"selectSensor(2)\""; if (activeSensor == 2) html += " checked"; html += ">";
      html += "<input type='text' class='sensor-name-input' id='name2' value='" + sensorName2 + "' maxlength='16' onchange=\"updateSensorName(2, this.value)\">";
      html += "<span class='sensor-temp'>" + String(lastRoomTemp2, 1) + " &deg;C</span>";
      html += "</div>";
    }
    if (s_ok3) {
      html += "<div class='sensor-row'>";
      html += "<input type='radio' name='sensorSelect' id='s3' value='3' onchange=\"selectSensor(3)\""; if (activeSensor == 3) html += " checked"; html += ">";
      html += "<input type='text' class='sensor-name-input' id='name3' value='" + sensorName3 + "' maxlength='16' onchange=\"updateSensorName(3, this.value)\">";
      html += "<span class='sensor-temp'>" + String(lastRoomTemp3, 1) + " &deg;C</span>";
      html += "</div>";
    }
  }

  html += "</div>";

  html += "<div class='sensor-box'><h3>Üzemmód és Hiszterézis</h3>";
  html += "<div style='display:flex; gap:10px; margin-bottom:15px;'>";
  html += "<button type='button' id='modeToggleBtn' onclick='toggleMode()' style='margin-top:0; background:" + modeBg + "; color:#fff; flex:1;'>" + modeLabel + "</button>";
  html += "</div>";
  html += "<div class='input-group'><label>Hiszterézis (°C)</label>";
  html += "<input type='text' id='hysteresisInput' value='" + String(hysteresis, 1) + "' style='text-align:center;'>";
  html += "</div></div>";

  html += "<div class='sensor-box'><h3>Wi-Fi Hálózat</h3>";
  html += "<div class='info-text'>Válassz hálózatot vagy add meg kézzel.</div>";
  html += "<div class='network-list' id='networks'><div class='scanning'>Hálózatok keresése...</div></div>";
  html += "<div class='input-group'><label>Hálózat neve (SSID)</label><input type='text' id='ssid' value='" + savedSsid + "'></div>";
  html += "<div class='input-group'><label>Jelszó</label><input type='password' id='password' value='" + savedPass + "'></div>";
  html += "</div>";

  html += "<button type='button' onclick='saveConfig()' style='background:#1a7a4a;color:#fff;margin-bottom:10px;'>Mentés és Újraindítás</button>";
  html += "<button type='button' onclick=\"location.href='/'\" style='background:#2e2e36;color:#d0d0d5;margin-top:5px;'>Mégse</button>";

  html += "</div>";

  html += "<script>";
  html += "let selectedSsid='';";
  html += "function scan(){fetch('/get-networks').then(r=>r.json()).then(data=>{";
  html += "let s=document.getElementById('networks');if(data.length==0){s.innerHTML='<div class=\"scanning\">Nem található hálózat. Keresés...</div>';return;}";
  html += "let html='';data.forEach(n=>{";
  html += "let isSaved=(n.ssid==='" + savedSsid + "');";
  html += "let badge=isSaved?'<span class=\"saved-badge\">MENTETT</span>':'';";
  html += "html+='<div class=\"network-item '+(n.ssid==selectedSsid?'selected':'')+'\" onclick=\"selectNet(\''+n.ssid+'\')\"><span>'+n.ssid+' '+badge+'</span><span style=\"color:#707075;font-size:0.8rem;\">'+n.rssi+' dBm</span></div>';";
  html += "});s.innerHTML=html;}).catch(()=>{});}";
  html += "function selectNet(id){selectedSsid=id;document.getElementById('ssid').value=id;scan();}";
  html += "setInterval(scan,4000);setTimeout(scan,500);";

  html += "function selectSensor(idx){fetch('/set-active-sensor?idx='+idx).catch(()=>{});}";
  html += "function updateSensorName(idx,val){fetch('/set-sensor-name?idx='+idx+'&name='+encodeURIComponent(val)).catch(()=>{});}";

  html += "function toggleMode(){";
  html += "fetch('/toggle-relay-mode').then(r=>r.text()).then(mode=>{";
  html += "let btn=document.getElementById('modeToggleBtn');";
  html += "if(mode==='inverted'){btn.innerText='Hűtés'; btn.style.background='#3cc8ff';}";
  html += "else{btn.innerText='Fűtés'; btn.style.background='#ff5e62';}";
  html += "}).catch(()=>{});}";

  html += "function searchSensors(){";
  html += "let btn=document.getElementById('searchSensorBtn');btn.innerText='Keresés...';btn.disabled=true;";
  html += "fetch('/search-sensors').then(r=>r.text()).then(data=>{";
  html += "document.getElementById('sensorSearchStatus').innerText='Sikeres keresés! Oldal újratöltése...';";
  html += "setTimeout(()=>{location.reload();},1500);";
  html += "}).catch(()=>{btn.innerText='Hőmérők keresése';btn.disabled=false;});}";

  html += "function saveConfig(){";
  html += "let s=document.getElementById('ssid').value;";
  html += "let p=document.getElementById('password').value;";
  html += "let h=document.getElementById('hysteresisInput').value;";
  html += "let d=document.getElementById('deviceNameInput').value;";
  html += "fetch('/save-config?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)+'&hysteresis='+encodeURIComponent(h)+'&devicename='+encodeURIComponent(d))";
  html += ".then(r=>r.text()).then(data=>{";
  html += "document.body.innerHTML='<div class=\"container\"><h2>Mentve!</h2><p class=\"info-text\" style=\"color:#00d2ff;\">'+data+'</p></div>';";
  html += "}).catch(()=>{alert('Hiba a mentés során!');});}";
  html += "</script></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleSearchSensors() {
  sensors.begin();

  sensors.requestTemperatures();
  delay(1200);

  float t0 = sensors.getTempCByIndex(0);
  float t1 = sensors.getTempCByIndex(1);
  float t2 = sensors.getTempCByIndex(2);
  float t3 = sensors.getTempCByIndex(3);

  bool ok0 = (t0 != DEVICE_DISCONNECTED_C);
  bool ok1 = (t1 != DEVICE_DISCONNECTED_C);
  bool ok2 = (t2 != DEVICE_DISCONNECTED_C);
  bool ok3 = (t3 != DEVICE_DISCONNECTED_C);

  missingCount0 = ok0 ? 0 : (missingCount0 + 1);
  missingCount1 = ok1 ? 0 : (missingCount1 + 1);
  missingCount2 = ok2 ? 0 : (missingCount2 + 1);
  missingCount3 = ok3 ? 0 : (missingCount3 + 1);

  if (ok0) lastRoomTemp0 = t0; else lastRoomTemp0 = -127.0;
  if (ok1) lastRoomTemp1 = t1; else lastRoomTemp1 = -127.0;
  if (ok2) lastRoomTemp2 = t2; else lastRoomTemp2 = -127.0;
  if (ok3) lastRoomTemp3 = t3; else lastRoomTemp3 = -127.0;

  sensorCount = (ok0 ? 1 : 0) + (ok1 ? 1 : 0) + (ok2 ? 1 : 0) + (ok3 ? 1 : 0);
  if (activeSensor >= sensorCount && sensorCount > 0) activeSensor = 0;

  server.send(200, "text/plain", "OK");
}

void handleGetNetworks() {
  int n = WiFi.scanComplete();
  String json = "[";
  if (n >= 0) {
    int count = 0;
    for (int i = 0; i < n; ++i) {
      String currentSsid = WiFi.SSID(i);
      if (currentSsid.length() == 0) continue;
      if (count > 0) json += ",";
      json += "{\"ssid\":\"" + currentSsid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
      count++;
    }
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleSaveConfig() {
  String reqSsid = server.arg("ssid");
  String reqPass = server.arg("pass");
  String reqHyst = server.arg("hysteresis");
  String reqName = server.arg("devicename");

  preferences.begin("wifi-config", false);
  preferences.putString("ssid", reqSsid);
  preferences.putString("password", reqPass);
  if (reqHyst.length() > 0) {
    float h = reqHyst.toFloat();
    if (h >= 0.1 && h <= 5.0) {
      hysteresis = h;
      preferences.putFloat("hysteresis", h);
    }
  }
  if (reqName.length() > 0) {
    reqName.replace(" ", "_");
    unitName = reqName;
    preferences.putString("devicename", reqName);
  }
  preferences.end();

  server.send(200, "text/plain", "Beállítások elmentve. Az eszköz újraindul és megpróbál csatlakozni...");

  shouldRestart = true;
  restartTimer = millis();
}

void handleGetCurrentTemp() {
  int target = targetTemperature;
  preferences.begin("thermostat", true);
  target = preferences.getInt("target", 22);
  preferences.end();
  server.send(200, "text/plain", String(target));
}

void handleSetTemp() {
  if (server.hasArg("value")) {
    int val = server.arg("value").toInt();
    if (val >= 15 && val <= 30) {
      targetTemperature = val;
      preferences.begin("thermostat", false);
      preferences.putInt("target", val);
      preferences.end();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleGetHeatingStatus() {
  server.send(200, "text/plain", isHeating ? "1" : "0");
}

void handleGetRelayMode() {
  server.send(200, "text/plain", relayInverted ? "inverted" : "normal");
}

void handleToggleRelayMode() {
  relayInverted = !relayInverted;
  preferences.begin("thermostat", false);
  preferences.putBool("inverted", relayInverted);
  preferences.end();
  server.send(200, "text/plain", relayInverted ? "inverted" : "normal");
}

void handleSetActiveSensor() {
  if (server.hasArg("idx")) {
    int idx = server.arg("idx").toInt();
    if (idx >= 0 && idx < 4) {
      activeSensor = idx;
      preferences.begin("thermostat", false);
      preferences.putInt("active_sensor", idx);
      preferences.end();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSetSensorName() {
  if (server.hasArg("idx") && server.hasArg("name")) {
    int idx = server.arg("idx").toInt();
    String name = server.arg("name");
    if (name.length() > 0 && name.length() <= 16) {
      preferences.begin("sensor-names", false);
      if (idx == 0) { sensorName0 = name; preferences.putString("name0", name); }
      else if (idx == 1) { sensorName1 = name; preferences.putString("name1", name); }
      else if (idx == 2) { sensorName2 = name; preferences.putString("name2", name); }
      else if (idx == 3) { sensorName3 = name; preferences.putString("name3", name); }
      preferences.end();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleGetAllTemps() {
  String json = "{";
  json += "\"count\":" + String(sensorCount) + ",";
  json += "\"active\":" + String(activeSensor) + ",";
  json += "\"temp0\":" + String(lastRoomTemp0 != -127.0 ? String(lastRoomTemp0, 1) : "\"--\"") + ",";
  json += "\"temp1\":" + String(lastRoomTemp1 != -127.0 ? String(lastRoomTemp1, 1) : "\"--\"") + ",";
  json += "\"temp2\":" + String(lastRoomTemp2 != -127.0 ? String(lastRoomTemp2, 1) : "\"--\"") + ",";
  json += "\"temp3\":" + String(lastRoomTemp3 != -127.0 ? String(lastRoomTemp3, 1) : "\"--\"") + "";
  json += "}";
  server.send(200, "application/json", json);
}

void handleGetSensorNames() {
  String json = "{";
  json += "\"name0\":\"" + sensorName0 + "\",";
  json += "\"name1\":\"" + sensorName1 + "\",";
  json += "\"name2\":\"" + sensorName2 + "\",";
  json += "\"name3\":\"" + sensorName3 + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleIp() {
  String ipStr = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  server.send(200, "text/plain", ipStr + "|" + unitName + "|" + FW_VERSION);
}

void setup() {
  pinMode(RELAY_HEAT_PIN, OUTPUT);
  pinMode(RELAY_COOL_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(RELAY_HEAT_PIN, LOW);
  digitalWrite(RELAY_COOL_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, HIGH);

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WATCHDOG_TIMEOUT_S * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  preferences.begin("wifi-config", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  float h = preferences.getFloat("hysteresis", 0.5);
  if (h >= 0.1 && h <= 5.0) hysteresis = h;
  String dName = preferences.getString("devicename", "ESP32_Thermostat");
  unitName = dName;
  preferences.end();

  preferences.begin("thermostat", true);
  targetTemperature = preferences.getInt("target", 22);
  relayInverted = preferences.getBool("inverted", false);
  activeSensor = preferences.getInt("active_sensor", 0);
  preferences.end();

  preferences.begin("sensor-names", true);
  sensorName0 = preferences.getString("name0", "1. hőmérő");
  sensorName1 = preferences.getString("name1", "2. hőmérő");
  sensorName2 = preferences.getString("name2", "3. hőmérő");
  sensorName3 = preferences.getString("name3", "4. hőmérő");
  preferences.end();

  sensors.begin();
  sensors.setWaitForConversion(false);

  sensors.requestTemperatures();
  delay(1500);
  float t0 = sensors.getTempCByIndex(0);
  float t1 = sensors.getTempCByIndex(1);
  float t2 = sensors.getTempCByIndex(2);
  float t3 = sensors.getTempCByIndex(3);
  bool ok0 = (t0 != DEVICE_DISCONNECTED_C);
  bool ok1 = (t1 != DEVICE_DISCONNECTED_C);
  bool ok2 = (t2 != DEVICE_DISCONNECTED_C);
  bool ok3 = (t3 != DEVICE_DISCONNECTED_C);
  if (ok0) lastRoomTemp0 = t0;
  if (ok1) lastRoomTemp1 = t1;
  if (ok2) lastRoomTemp2 = t2;
  if (ok3) lastRoomTemp3 = t3;
  sensorCount = (ok0 ? 1 : 0) + (ok1 ? 1 : 0) + (ok2 ? 1 : 0) + (ok3 ? 1 : 0);
  if (activeSensor >= sensorCount && sensorCount > 0) activeSensor = 0;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(unitName.c_str());

  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), password.c_str());
  }

  if (MDNS.begin(unitName.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", index_html);
  });
  server.on("/wifi-config", HTTP_GET, handleWifiConfig);
  server.on("/get-networks", HTTP_GET, handleGetNetworks);
  server.on("/save-config", HTTP_GET, handleSaveConfig);
  server.on("/get-current-temp", HTTP_GET, handleGetCurrentTemp);
  server.on("/set-temp", HTTP_GET, handleSetTemp);
  server.on("/get-heating-status", HTTP_GET, handleGetHeatingStatus);
  server.on("/get-relay-mode", HTTP_GET, handleGetRelayMode);
  server.on("/toggle-relay-mode", HTTP_GET, handleToggleRelayMode);
  server.on("/set-active-sensor", HTTP_GET, handleSetActiveSensor);
  server.on("/set-sensor-name", HTTP_GET, handleSetSensorName);
  server.on("/get-all-temps", HTTP_GET, handleGetAllTemps);
  server.on("/get-sensor-names", HTTP_GET, handleGetSensorNames);
  server.on("/search-sensors", HTTP_GET, handleSearchSensors);
  server.on("/ip", HTTP_GET, handleIp);

  server.begin();
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  if (shouldRestart && (millis() - restartTimer > 2000)) {
    ESP.restart();
  }

  if (scanRequested && (millis() - scanRequestTime > 6000)) {
    scanRequested = false;
  }

  if (millis() - lastTempRequest > 15000) {
    lastTempRequest = millis();

    float t0 = sensors.getTempCByIndex(0);
    float t1 = sensors.getTempCByIndex(1);
    float t2 = sensors.getTempCByIndex(2);
    float t3 = sensors.getTempCByIndex(3);

    bool ok0 = (t0 != DEVICE_DISCONNECTED_C);
    bool ok1 = (t1 != DEVICE_DISCONNECTED_C);
    bool ok2 = (t2 != DEVICE_DISCONNECTED_C);
    bool ok3 = (t3 != DEVICE_DISCONNECTED_C);

    missingCount0 = ok0 ? 0 : (missingCount0 + 1);
    missingCount1 = ok1 ? 0 : (missingCount1 + 1);
    missingCount2 = ok2 ? 0 : (missingCount2 + 1);
    missingCount3 = ok3 ? 0 : (missingCount3 + 1);

    if (ok0) lastRoomTemp0 = t0; else if (missingCount0 >= 3) lastRoomTemp0 = -127.0;
    if (ok1) lastRoomTemp1 = t1; else if (missingCount1 >= 3) lastRoomTemp1 = -127.0;
    if (ok2) lastRoomTemp2 = t2; else if (missingCount2 >= 3) lastRoomTemp2 = -127.0;
    if (ok3) lastRoomTemp3 = t3; else if (missingCount3 >= 3) lastRoomTemp3 = -127.0;

    bool needRescan = (missingCount0 >= 3 && sensorCount > 0 && activeSensor == 0) ||
                      (missingCount1 >= 3 && sensorCount > 0 && activeSensor == 1) ||
                      (missingCount2 >= 3 && sensorCount > 0 && activeSensor == 2) ||
                      (missingCount3 >= 3 && sensorCount > 0 && activeSensor == 3);

    if (needRescan) {
      scanRequested = false;
      sensors.begin();
      sensors.requestTemperatures();
      delay(1500);
      t0 = sensors.getTempCByIndex(0);
      t1 = sensors.getTempCByIndex(1);
      t2 = sensors.getTempCByIndex(2);
      t3 = sensors.getTempCByIndex(3);
      ok0 = (t0 != DEVICE_DISCONNECTED_C);
      ok1 = (t1 != DEVICE_DISCONNECTED_C);
      ok2 = (t2 != DEVICE_DISCONNECTED_C);
      ok3 = (t3 != DEVICE_DISCONNECTED_C);
      missingCount0 = ok0 ? 0 : (missingCount0 + 1);
      missingCount1 = ok1 ? 0 : (missingCount1 + 1);
      missingCount2 = ok2 ? 0 : (missingCount2 + 1);
      missingCount3 = ok3 ? 0 : (missingCount3 + 1);
      if (ok0) lastRoomTemp0 = t0; else lastRoomTemp0 = -127.0;
      if (ok1) lastRoomTemp1 = t1; else lastRoomTemp1 = -127.0;
      if (ok2) lastRoomTemp2 = t2; else lastRoomTemp2 = -127.0;
      if (ok3) lastRoomTemp3 = t3; else lastRoomTemp3 = -127.0;
    }

    int newSensorCount = (ok0 ? 1 : 0) + (ok1 ? 1 : 0) + (ok2 ? 1 : 0) + (ok3 ? 1 : 0);
    if (newSensorCount > 0) {
      sensorCount = newSensorCount;
    }
    if (activeSensor >= sensorCount && sensorCount > 0) activeSensor = 0;

    sensors.requestTemperatures();
  }

  float currentActiveTemp = -127.0;
  if (activeSensor == 0) currentActiveTemp = lastRoomTemp0;
  else if (activeSensor == 1) currentActiveTemp = lastRoomTemp1;
  else if (activeSensor == 2) currentActiveTemp = lastRoomTemp2;
  else if (activeSensor == 3) currentActiveTemp = lastRoomTemp3;

  if (currentActiveTemp != -127.0) {
    if (relayInverted) {
      if (currentActiveTemp >= (targetTemperature + hysteresis)) {
        isHeating = true;
        digitalWrite(RELAY_COOL_PIN, HIGH);
        digitalWrite(RELAY_HEAT_PIN, LOW);
        digitalWrite(STATUS_LED_PIN, LOW);
      } else if (currentActiveTemp <= (targetTemperature - hysteresis)) {
        isHeating = false;
        digitalWrite(RELAY_COOL_PIN, LOW);
        digitalWrite(RELAY_HEAT_PIN, LOW);
        digitalWrite(STATUS_LED_PIN, HIGH);
      }
    } else {
      if (currentActiveTemp <= (targetTemperature - hysteresis)) {
        isHeating = true;
        digitalWrite(RELAY_HEAT_PIN, HIGH);
        digitalWrite(RELAY_COOL_PIN, LOW);
        digitalWrite(STATUS_LED_PIN, LOW);
      } else if (currentActiveTemp >= (targetTemperature + hysteresis)) {
        isHeating = false;
        digitalWrite(RELAY_HEAT_PIN, LOW);
        digitalWrite(RELAY_COOL_PIN, LOW);
        digitalWrite(STATUS_LED_PIN, HIGH);
      }
    }
  } else {
    isHeating = false;
    digitalWrite(RELAY_HEAT_PIN, LOW);
    digitalWrite(RELAY_COOL_PIN, LOW);
    digitalWrite(STATUS_LED_PIN, HIGH);
  }
}
