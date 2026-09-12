#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <WebSocketsServer.h>

// ================== WiFi AP Config ==================
const char *ssid = "ESP_Drone";
const char *password = "12345678";
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// ================== iBUS Config ==================
#define IBUS_SERIAL Serial1
#define IBUS_TX_PIN 21
#define CHANNELS 14

uint16_t channels[CHANNELS] = {
  1500, 1500, 1500, 1000,  // AETR: Roll, Pitch, Yaw, Throttle
  1000, 1000, 1000, 1000,  // AUX1–AUX4
  1500, 1500, 1500, 1500, 1500, 1500
};

// ================== UDP Config ==================
WiFiUDP udp;
const int udpPort = 1234;
char incomingPacket[255];

// ================== State & Watchdog ==================
bool auxState[4] = {false, false, false, false};
unsigned long lastPacketTime = 0;

void setAux(int auxNum, bool state) {
  if (auxNum >= 1 && auxNum <= 4) {
    channels[3 + auxNum] = state ? 1500 : 1000; // CH5–CH8
    auxState[auxNum - 1] = state;
    Serial.printf("[AUX] Aux%d set to %s\n", auxNum, state ? "ON" : "OFF");
  }
}

// ================== WebSocket Event Handler ==================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_TEXT && length >= 16) {
    // String payload: "1000150015001500" (Throttle, Yaw, Roll, Pitch)
    char buf[5];
    uint16_t wsChannels[4];
    for (int i = 0; i < 4; i++) {
      memcpy(buf, &payload[i * 4], 4);
      buf[4] = '\0';
      wsChannels[i] = constrain(atoi(buf), 1000, 2000);
    }

    // Map directly to AETR (Roll, Pitch, Yaw, Throttle)
    channels[0] = wsChannels[2]; // Roll (Right Stick X)
    channels[1] = wsChannels[3]; // Pitch (Right Stick Y)
    channels[2] = wsChannels[1]; // Yaw (Left Stick X)
    channels[3] = wsChannels[0]; // Throttle (Left Stick Y)

    lastPacketTime = millis();
  }
}

// ================== Web Page HTML + Gamepad JS ==================
String buildHTML() {
  String html = "<!DOCTYPE html><html><head><title>ESP Drone Modes</title><style>";
  html += "body{font-family:Arial;text-align:center;background:#1a1a1a;color:white;}";
  html += "h1{margin-top:20px;} .buttons{display:grid;grid-template-columns:repeat(2,150px);grid-gap:20px;justify-content:center;margin-top:30px;}";
  html += ".switch{position:relative;display:inline-block;width:60px;height:34px;} .switch input{display:none;}";
  html += ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;transition:.4s;border-radius:34px;}";
  html += ".slider:before{position:absolute;content:'';height:26px;width:26px;left:4px;bottom:4px;background:white;transition:.4s;border-radius:50%;}";
  html += "input:checked + .slider{background:#4CAF50;} input:checked + .slider:before{transform:translateX(26px);} ";
  html += ".status{margin-top:20px;font-size:16px;} .pad-box{margin-top:15px;color:#00e676;font-weight:bold;}</style></head><body>";
  html += "<h1>ESP Drone Modes</h1>";
  html += "<div class='pad-box' id='gamepadStatus'>Xbox Controller: Press any button to initialize</div>";
  html += "<div class='buttons'>";

  for (int i = 1; i <= 4; i++) {
    html += "<label class='switch'> <input type='checkbox' id='aux" + String(i) + "' onchange='toggle(" + String(i) + ")'> <span class='slider'></span></label> Mode " + String(i);
  }

  html += "</div><div class='status' id='status'></div>";

  // JavaScript: Handles WebSocket + Web Gamepad API natively on Mac/PC/Mobile
  html += "<script>";
  html += "let ws = new WebSocket('ws://' + window.location.hostname + ':81/');";
  html += "let activeGp = null;";
  
  html += "window.addEventListener('gamepadconnected', (e) => {";
  html += "  activeGp = e.gamepad.index;";
  html += "  document.getElementById('gamepadStatus').innerText = 'Controller Active: ' + e.gamepad.id;";
  html += "});";

  html += "window.addEventListener('gamepaddisconnected', () => {";
  html += "  activeGp = null;";
  html += "  document.getElementById('gamepadStatus').innerText = 'Controller Disconnected!';";
  html += "});";

  html += "function pollGamepad() {";
  html += "  if (activeGp !== null && ws.readyState === WebSocket.OPEN) {";
  html += "    let gp = navigator.getGamepads()[activeGp];";
  html += "    if (gp && gp.axes.length >= 4) {";
  html += "      let lx = gp.axes[0] || 0;";
  html += "      let ly = gp.axes[1] || 0;";
  html += "      let rx = gp.axes[2] || 0;";
  html += "      let ry = gp.axes[3] || 0;";
  
  // Handling unmapped Raw Gamepad fallback (on macOS with Chrome, my 8bitdo axis 0 and 1 is swapped, right stick is axes 2 and 5)
  // You can change this mapping based on your controller's behavior if needed.
  html += "      if (gp.axes.length > 4 && gp.mapping !== 'standard') {";
  html += "        lx = gp.axes[1];";
  html += "        ly = gp.axes[0];";
  html += "        rx = gp.axes[2];";
  html += "        ry = gp.axes[5] !== undefined ? gp.axes[5] : gp.axes[3];";
  html += "      }";

  // Deadzone filter (0.05)
  html += "      let dz = (v) => Math.abs(v) < 0.05 ? 0 : v;";

  // Drone Stick Mode 2:
  // Left Y  = Throttle (Inverted: Push UP -> +1.0 -> 2000, Pull DOWN -> -1.0 -> 1000)
  // Left X  = Yaw      (Right -> +1.0 -> 2000, Left -> -1.0 -> 1000)
  // Right X = Roll     (Right -> +1.0 -> 2000, Left -> -1.0 -> 1000)
  // Right Y = Pitch    (Inverted: Push UP -> +1.0 -> 2000, Pull DOWN -> -1.0 -> 1000)
  html += "      let throttle = Math.round(1500 - (dz(ly) * 500));";
  html += "      let yaw      = Math.round(1500 + (dz(lx) * 500));";
  html += "      let roll     = Math.round(1500 + (dz(rx) * 500));";
  html += "      let pitch    = Math.round(1500 - (dz(ry) * 500));";
  
  // Format to 16-character payload: "ThrottleYawRollPitch"
  html += "      let p = (v) => Math.min(2000, Math.max(1000, v)).toString().padStart(4, '0');";
  html += "      ws.send(p(throttle) + p(yaw) + p(roll) + p(pitch));";
  html += "    }";
  html += "  }";
  html += "  requestAnimationFrame(pollGamepad);";
  html += "}";
  html += "requestAnimationFrame(pollGamepad);";

  html += "function toggle(ch){fetch('/aux?ch='+ch);} ";
  html += "async function update(){let r=await fetch('/status');let j=await r.json();";
  html += "document.getElementById('status').innerHTML=";
  html += "'Throttle: '+j.throttle+'% | Roll: '+j.roll+'% | Pitch: '+j.pitch+'% | Yaw: '+j.yaw+'%';";
  html += "for(let i=1;i<=4;i++){document.getElementById('aux'+i).checked=j['aux'+i];}}";
  html += "setInterval(update,1000); update();";
  html += "</script></body></html>";
  return html;
}

void handleRoot() { server.send(200, "text/html", buildHTML()); }

void handleAux() {
  if (server.hasArg("ch")) {
    int ch = server.arg("ch").toInt();
    if (ch >= 1 && ch <= 4) {
      setAux(ch, !auxState[ch - 1]);
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleStatus() {
  String json = "{";
  json += "\"throttle\":" + String((channels[3] - 1000) * 100 / 1000) + ",";
  json += "\"roll\":" + String((channels[0] - 1000) * 100 / 1000) + ",";
  json += "\"pitch\":" + String((channels[1] - 1000) * 100 / 1000) + ",";
  json += "\"yaw\":" + String((channels[2] - 1000) * 100 / 1000);
  for (int i = 1; i <= 4; i++) {
    json += ",\"aux" + String(i) + "\":" + (auxState[i - 1] ? "true" : "false");
  }
  json += "}";
  server.send(200, "application/json", json);
}

// ================== iBUS Packet Send ==================
void sendIBUS() {
  uint8_t packet[32];
  packet[0] = 0x20; // length
  packet[1] = 0x40; // command
  for (int i = 0; i < CHANNELS; i++) {
    packet[2 + i * 2] = channels[i] & 0xFF;
    packet[2 + i * 2 + 1] = (channels[i] >> 8) & 0xFF;
  }
  uint16_t checksum = 0xFFFF;
  for (int i = 0; i < 30; i++) checksum -= packet[i];
  packet[30] = checksum & 0xFF;
  packet[31] = (checksum >> 8) & 0xFF;
  IBUS_SERIAL.write(packet, 32);
}

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  WiFi.softAP(ssid, password);
  Serial.print("[WiFi AP] IP: "); Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/aux", handleAux);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("[Web] HTTP Server started");

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("[Web] WebSocket Server started on port 81");

  udp.begin(udpPort);
  Serial.print("[UDP] Listening on port "); Serial.println(udpPort);

  IBUS_SERIAL.begin(115200, SERIAL_8N1, -1, IBUS_TX_PIN);
  Serial.println("[iBUS] Output initialized");

  lastPacketTime = millis();
}

// ================== Loop ==================
void loop() {
  // Service Web & Socket tasks
  server.handleClient();
  webSocket.loop();

  // Process legacy UDP packets if present
  int packetSize = udp.parsePacket();
  if (packetSize) {
    int len = udp.read(incomingPacket, 254);
    if (len > 0) incomingPacket[len] = '\0';

    if (strlen(incomingPacket) >= 16) {
      char buf[5];
      uint16_t udpChannels[4];
      for (int i = 0; i < 4; i++) {
        strncpy(buf, &incomingPacket[i * 4], 4);
        buf[4] = '\0';
        udpChannels[i] = constrain(atoi(buf), 1000, 2000);
      }

      channels[0] = udpChannels[2];           // Roll
      channels[1] = 3000 - udpChannels[3];    // Pitch (inverted)
      channels[2] = udpChannels[1];           // Yaw
      channels[3] = udpChannels[0];           // Throttle

      lastPacketTime = millis();
    }
  }

  // Failsafe: Reset Throttle to 1000 if connection drops for > 500ms
  if (millis() - lastPacketTime > 500) {
    channels[3] = 1000;
  }

  sendIBUS();
  delay(5);
}