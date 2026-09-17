/*
 * Hornet Panel v2 — ESP8266 управление Artillery Hornet (Marlin 2.0.7.2)
 *
 * GPIO1(TX)→PA10, GPIO3(RX)←PA9, GND, 115200 8N1 (3.3 В напрямую!)
 *
 * Новое во v2:
 *  - Задержка управления ~1 с вместо ~5 с (приоритетные команды, таймаут 1.2 с)
 *  - Статус + журнал в одном запросе (меньше HTTP-трафика)
 *  - Jog X/Y/Z с шагом 1 / 5 / 10 мм, кнопки с подписями
 *  - MQTT-мост (test.mosquitto.org): панель в онлайне «откуда угодно» без проброса портов
 *  - remote.html — файл на устройстве для удалённого управления через MQTT
 *  - Исправлено: отображение файлов SD, список M20, крах JS
 *  - OTA-обновления по WiFi
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <bearssl/bearssl_hash.h>

// ==================== КОНФИГ ====================
const uint32_t BAUDS[] = {115200, 250000, 57600, 9600};
const char AP_SSID[] = "Hornet-Setup";
const char AP_PASS[] = "12345678";
const char HOSTNAME[] = "hornet";
const char MQTT_HOST[] = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;
const char MQTT_PREFIX[] = "hornet/";

// ==================== СОСТОЯНИЕ ====================
String wifiSsid = "", wifiPass = "", accessPass = "";
String deviceId = "";           // 12-символьный код устройства (hex SHA256)
String tNozzle = "--", tTarget = "--", bBed = "--", bTarget = "--";
String fwInfo = "?";
String files[24]; int filesLen = 0;
bool inList = false;
bool sdMounted = true;          // по умолчанию считаем SD вставленной
bool printerSeen = false;
uint32_t rxAnyMs = 0;

struct QItem { String s, r; };
QItem queue[12]; int queueLen = 0; bool queueAuto = true;

struct PrintState { bool active, paused; uint32_t pos, total, lastGrow; String fname; };
PrintState ps = {false, false, 0, 0, 0, ""};

// журнал (merged into status)
String logLines[30]; int logHead = 0, logCount = 0;
void addLog(const String& s) {
  if (!s.length()) return;
  logLines[logHead] = s;
  logHead = (logHead + 1) % 30;
  if (logCount < 30) logCount++;
}

// ==================== ОЧЕРЕДЬ КОМАНД (hi + lo) ====================
// hi = команды пользователя (мгновенный отклик), lo = опрос каждую секунду
struct Cmd { String line; };
Cmd hiQ[8];  int hiHead = 0, hiTail = 0, hiCount = 0;
Cmd loQ[8];  int loHead = 0, loTail = 0, loCount = 0;
String inFlight = ""; uint32_t inFlightAt = 0; uint8_t inFlightTries = 0;
bool inFlightBusy = false, gotOk = false;

bool uploading = false;
uint32_t uploadBytes = 0;
String uploadShort = "", uploadReal = "";

bool hiFull() { return hiCount >= 7; }
bool loFull() { return loCount >= 7; }
void sendCmdHi(const String& c) {
  if (!c.length() || hiFull()) return;
  hiQ[hiHead].line = c; hiHead = (hiHead + 1) % 8; hiCount++;
}
void sendCmdLo(const String& c) {
  if (!c.length() || loFull()) return;
  loQ[loHead].line = c; loHead = (loHead + 1) % 8; loCount++;
}
void sendCmd(const String& c) { sendCmdHi(c); }  // alias для HTTP/высокий приоритет
void pumpCmds(uint32_t now) {
  if (uploading) return;
  if (inFlightBusy) {
    if (now - inFlightAt > 1200) {
      if (++inFlightTries >= 8) { inFlightBusy = false; addLog("! нет ответа: " + inFlight); }
      else { Serial.print(inFlight); Serial.print("\r\n"); inFlightAt = now; gotOk = false; }
    }
    return;
  }
  Cmd* next = nullptr;
  if (hiCount > 0) { next = &hiQ[hiTail]; hiTail = (hiTail + 1) % 8; hiCount--; }
  else if (loCount > 0) { next = &loQ[loTail]; loTail = (loTail + 1) % 8; loCount--; }
  if (next) {
    inFlight = next->line;
    inFlightTries = 0; inFlightAt = now; gotOk = false; inFlightBusy = true;
    Serial.print(inFlight); Serial.print("\r\n");
    addLog(">>> " + inFlight);
  }
}

// ==================== LittleFS: cfg + очередь ====================
void loadCfg() {
  File f = LittleFS.open("/cfg.txt", "r");
  if (!f) return;
  wifiSsid = f.readStringUntil('\n'); wifiSsid.trim();
  wifiPass = f.readStringUntil('\n'); wifiPass.trim();
  accessPass = f.readStringUntil('\n'); accessPass.trim();
  f.close();
}
void saveCfg() {
  File f = LittleFS.open("/cfg.txt", "w");
  f.println(wifiSsid); f.println(wifiPass); f.println(accessPass);
  f.close();
}
void loadQueue() {
  File f = LittleFS.open("/queue.txt", "r");
  if (!f) return;
  while (f.available() && queueLen < 12) {
    String ln = f.readStringUntil('\n'); ln.trim();
    int eq = ln.indexOf('=');
    if (eq > 0) { queue[queueLen].s = ln.substring(0, eq); queue[queueLen].r = ln.substring(eq + 1); queueLen++; }
  }
  f.close();
}
void saveQueue() {
  File f = LittleFS.open("/queue.txt", "w");
  for (int i = 0; i < queueLen; i++) { f.print(queue[i].s); f.print('='); f.println(queue[i].r); }
  f.close();
}
String makeShortName() {
  static const char alnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (int a = 0; a < 25; a++) {
    String s = "u";
    for (int i = 0; i < 6; i++) s += alnum[random(0, 36)];
    s += ".gco";
    bool clash = false;
    for (int i = 0; i < queueLen; i++) if (queue[i].s == s) clash = true;
    for (int i = 0; i < filesLen; i++) if (files[i] == s) clash = true;
    if (!clash) return s;
  }
  return "upload.gco";
}

void startPrint(const String& sname) {
  ps.fname = sname;
  sendCmdHi("M23 " + sname);
  sendCmdHi("M24");
}
void onPrintEnd() {
  addLog("* печать окончена: " + ps.fname);
  String done = ps.fname; done.trim(); done.toLowerCase();
  for (int i = 0; i < queueLen; i++) {
    if (queue[i].s == done) {
      for (int j = i; j < queueLen - 1; j++) queue[j] = queue[j + 1];
      queueLen--; saveQueue(); break;
    }
  }
  if (queueAuto && queueLen > 0) startPrint(queue[0].s);
}

// ==================== DEVICE ID (SHA256 MAC + pass → 12 hex) ====================
void computeDeviceId() {
  String src = WiFi.macAddress() + ":" + (accessPass.length() ? accessPass : "hornet");
  uint8_t hash[32];
  br_sha256_context ctx;
  br_sha256_init(&ctx);
  br_sha256_update(&ctx, src.c_str(), src.length());
  br_sha256_out(&ctx, hash);
  deviceId = "";
  for (int i = 0; i < 6; i++) {
    char buf[3]; sprintf(buf, "%02x", hash[i]);
    deviceId += buf;
  }
  deviceId.toUpperCase();
}

// ==================== РАЗБОР СТРОК MARLIN ====================
void parseInto(String s) {
  s.trim();
  if (!s.length()) return;

  // M20: Begin file list ... End file list
  if (s == "Begin file list") { inList = true; filesLen = 0; return; }
  if (s == "End file list") { inList = false; sdMounted = true; return; }
  if (inList) {
    if (filesLen < 24) { String n = s; n.toLowerCase(); files[filesLen++] = n; sdMounted = true; }
    return;
  }

  // SD progress: "SD printing byte <pos>/<total>"
  if (s.startsWith("SD printing byte ")) {
    String v = s.substring(17);
    int sl = v.indexOf('/');
    uint32_t pos = v.substring(0, sl).toInt();
    uint32_t total = v.substring(sl + 1).toInt();
    if (!ps.active) { ps.active = true; ps.paused = false; }
    ps.total = total;
    if (pos != ps.pos) { ps.pos = pos; ps.lastGrow = millis(); ps.paused = false; }
    else if (pos > 0 && pos < total && millis() - ps.lastGrow > 8000) ps.paused = true;
    printerSeen = true;
    return;
  }
  if (s.indexOf("Not SD printing") >= 0) {
    if (ps.active && ps.pos > 0) onPrintEnd();
    ps.active = false; ps.paused = false;
    printerSeen = true;
    return;
  }
  if (s.startsWith("Current file:")) { ps.fname = s.substring(13); ps.fname.trim(); return; }

  if (s == "ok") { gotOk = true; inFlightBusy = false; printerSeen = true; return; }
  if (s == "wait") { printerSeen = true; return; }

  if (s.startsWith("FIRMWARE_NAME:")) {
    int e = s.indexOf(" EXTERN"); if (e < 0) e = s.indexOf(" PROTOCOL"); if (e < 0) e = s.length();
    fwInfo = s.substring(14, e); printerSeen = true;
  }
  if (s.indexOf("No Media") >= 0) sdMounted = false;
  if (s.indexOf("SD card ok") >= 0 || s.indexOf("Already in use") >= 0 || s.indexOf("SD initialized") >= 0) sdMounted = true;

  // T:215.0 /240.0 B:60.0 /80.0 @:...
  int ti = s.indexOf("T:");
  if (ti >= 0 && s.indexOf("ok") != 0) {
    int sl = s.indexOf('/', ti + 2);
    int sp = s.indexOf(' ', ti);
    if (sl > ti + 2) {
      int e = (sp > ti && sp < sl) ? sp : sl;
      tNozzle = s.substring(ti + 2, e);
      int nb = s.indexOf(' ', sl);
      tTarget = s.substring(sl + 1, nb < 0 ? s.length() : nb);
    }
    int bi = s.indexOf("B:");
    if (bi >= 0) {
      sl = s.indexOf('/', bi + 2);
      sp = s.indexOf(' ', bi);
      if (sl > bi + 2) {
        int e = (sp > bi && sp < sl) ? sp : sl;
        bBed = s.substring(bi + 2, e);
        int nb = s.indexOf(' ', sl);
        bTarget = s.substring(sl + 1, nb < 0 ? s.length() : nb);
      }
    }
    return;
  }
  addLog("<<< " + s);
}

void handlePrinter() {
  static String line = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    rxAnyMs = millis();
    if (c == '\n') {
      if (line.length()) parseInto(line);
      line = "";
    } else if (line.length() < 200) line += c;
  }
}

// ==================== АВТОПОЛЛИНГ / РЕКОННЕКТ ====================
uint32_t lastPoll = 0;
uint8_t baudIdx = 0;
void autoPoll(uint32_t now) {
  if (uploading) return;
  if (hiCount || loCount || inFlightBusy) return;
  if (now - lastPoll > 1000) {           // каждую секунду вместо двух
    lastPoll = now;
    sendCmdLo("M105");
    sendCmdLo("M27");
    if (ps.active && !ps.fname.length()) sendCmdLo("M27 C");
  }
  if (now - rxAnyMs > 30000) {
    rxAnyMs = now;
    baudIdx = (baudIdx + 1) % 4;
    Serial.flush(); Serial.end(); delay(80);
    Serial.begin(BAUDS[baudIdx]);
    printerSeen = false;
    addLog("* UART-скорость → " + String(BAUDS[baudIdx]));
    sendCmdLo("M115");
  }
}

// ==================== WiFi ====================
void connectWiFi() {
  if (!wifiSsid.length()) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(250);
}

// ==================== MQTT ====================
WiFiClient mqttNet;
PubSubClient mqttClient(mqttNet);
String mqttTopicCmd, mqttTopicStat;
uint32_t lastMqttPub = 0;
bool mqttInitDone = false;

void mqttInit() {
  if (!deviceId.length()) return;
  mqttTopicCmd = String(MQTT_PREFIX) + deviceId + "/cmd";
  mqttTopicStat = String(MQTT_PREFIX) + deviceId + "/stat";
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback([](char* topic, byte* payload, unsigned int len) {
    String cmd;
    for (unsigned i = 0; i < len && i < 120; i++) cmd += (char)payload[i];
    cmd.trim();
    if (cmd.length()) {
      addLog("MQTT ← " + cmd);
      dispatchAction(cmd);
    }
  });
  mqttInitDone = true;
}

void mqttEnsure() {
  if (!mqttInitDone || WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) { mqttClient.loop(); return; }
  String clientId = "hornet-" + deviceId;
  if (mqttClient.connect(clientId.c_str())) {
    mqttClient.subscribe(mqttTopicCmd.c_str());
    addLog("* MQTT подключён: " + mqttTopicCmd);
  }
}

void mqttPublishStatus() {
  if (!mqttInitDone || !mqttClient.connected()) return;
  String j = buildStatusJson();
  mqttClient.publish(mqttTopicStat.c_str(), j.c_str(), true); // retain=true
}

// ==================== HTTP ====================
ESP8266WebServer server(80);

String jsonStr(const String& in) {
  String o;
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c < 32) {}
    else o += c;
  }
  return o;
}

String b64decode(const String& in) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  uint32_t bits = 0, nbits = 0;
  for (unsigned i = 0; i < in.length(); i++) {
    char* p = strchr(const_cast<char*>(tbl), in[i]);
    if (!p) continue;
    bits = (bits << 6) | (uint32_t)(p - tbl);
    nbits += 6;
    if (nbits >= 8) { nbits -= 8; out += (char)((bits >> nbits) & 0xFF); }
  }
  return out;
}

bool authOk() {
  if (!accessPass.length()) return true;
  String hdr = server.header("Authorization");
  if (!hdr.startsWith("Basic ")) return false;
  String dec = b64decode(hdr.substring(6));
  int col = dec.indexOf(':');
  String pass = (col < 0) ? dec : dec.substring(col + 1);
  return pass == accessPass;
}
bool gate() {
  if (authOk()) return true;
  server.sendHeader("WWW-Authenticate", "Basic realm=\"Hornet\"");
  server.send(401, "text/plain", "Нужен пароль (настройка на /wifi)");
  return false;
}
void okJson() { server.send(200, "application/json", "{\"ok\":true}"); }

// ==================== УНИВЕРСАЛЬНЫЙ ДИСПЕТЧЕР (HTTP + MQTT) ====================
void dispatchAction(String cmd) {
  // cmd вида: "preheat?m=abs"  или  "move?a=X&d=5"  или  "G28"  (raw gcode)
  cmd.trim();
  if (!cmd.length()) return;

  // raw G-код
  if (cmd[0] == 'G' || cmd[0] == 'M' || cmd[0] == 'g' || cmd[0] == 'm') {
    sendCmdHi(cmd);
    return;
  }

  int qi = cmd.indexOf('?');
  String act = (qi >= 0) ? cmd.substring(0, qi) : cmd;
  String query = (qi >= 0) ? cmd.substring(qi + 1) : "";

  // простой парсер query: "key=value&key2=value2"
  auto qp = [&](const String& key) -> String {
    int pos = 0;
    while (pos < (int)query.length()) {
      int amp = query.indexOf('&', pos);
      String pair = (amp < 0) ? query.substring(pos) : query.substring(pos, amp);
      int eq = pair.indexOf('=');
      if (eq >= 0 && pair.substring(0, eq) == key) return pair.substring(eq + 1);
      if (amp < 0) break;
      pos = amp + 1;
    }
    return "";
  };

  if (act == "pause") {
    sendCmdHi("M25");
    sendCmdHi("G91");
    sendCmdHi("G1 Z10 F600");
    sendCmdHi("G90");
    sendCmdHi("G1 X0 Y200 F4000");
  }
  else if (act == "resume") { ps.paused = false; ps.lastGrow = millis(); sendCmdHi("M24"); }
  else if (act == "stop") { queueAuto = false; sendCmdHi("M524"); }
  else if (act == "estop") { sendCmdHi("M112"); sendCmdHi("M999"); }
  else if (act == "home") sendCmdHi("G28");
  else if (act == "g29") { sendCmdHi("G28"); sendCmdHi("G29"); }
  else if (act == "motors") sendCmdHi("M84");
  else if (act == "cool") { sendCmdHi("M104 S0"); sendCmdHi("M140 S0"); sendCmdHi("M107 P0"); sendCmdHi("M107 P1"); }
  else if (act == "filload") {
    sendCmdHi("M302 S0");        // разрешить экструдер без нагрева
    sendCmdHi("G91");            // относительные координаты
    sendCmdHi("G1 E-50 F300");  // подать 50 мм (инвертированное направление)
    sendCmdHi("G90");            // абсолютные координаты
    sendCmdHi("M302 S1");        // вернуть защиту
  }
  else if (act == "filunload") {
    sendCmdHi("M302 S0");
    sendCmdHi("G91");
    sendCmdHi("G1 E50 F300");   // вытянуть 50 мм (инвертированное направление)
    sendCmdHi("G90");
    sendCmdHi("M302 S1");
  }
  else if (act == "fan0") { int p = qp("p").toInt(); sendCmdHi(p <= 0 ? "M107 P0" : "M106 P0 S" + String(constrain(p, 0, 255))); }
  else if (act == "fan1") { int p = qp("p").toInt(); sendCmdHi(p <= 0 ? "M107 P1" : "M106 P1 S" + String(constrain(p, 0, 255))); }
  else if (act == "fan2") { int p = qp("p").toInt(); sendCmdHi(p <= 0 ? "M107 P2" : "M106 P2 S" + String(constrain(p, 0, 255))); }
  else if (act == "preheat") {
    String m = qp("m"); int n = 210, b = 60;
    if (m == "petg") { n = 240; b = 80; } else if (m == "abs") { n = 250; b = 100; }
    else if (m == "tpu") { n = 225; b = 45; } else if (m == "off") { n = 0; b = 0; }
    sendCmdHi("M104 S" + String(n));
    sendCmdHi("M140 S" + String(b));
  }
  else if (act == "move") {
    String a = qp("a"); a.toUpperCase();
    float d = qp("d").toFloat();
    int f = 4000;
    if (a == "Z") f = 600;
    if (a.length() == 1 && strchr("XYZE", a[0]) && d != 0) {
      sendCmdHi("G91");
      sendCmdHi("G1 " + a + String(d, 2) + " F" + String(f));
      sendCmdHi("G90");
    }
  }
  else if (act == "fan") {
    int p = qp("p").toInt();
    sendCmdHi(p <= 0 ? "M107" : "M106 S" + String(constrain(p, 0, 255)));
  }
  else if (act == "set") {
    String n = qp("n"), b = qp("b");
    if (n.length()) sendCmdHi("M104 S" + n);
    if (b.length()) sendCmdHi("M140 S" + b);
  }
  else if (act == "print") { queueAuto = true; startPrint(qp("s")); }
  else if (act == "files") sendCmdHi("M20");
  else if (act == "queueauto") queueAuto = (qp("on") == "1");
  else if (act == "queuecont") { queueAuto = true; if (!ps.active && queueLen > 0) startPrint(queue[0].s); }
  else if (act == "queuedel") {
    int i = qp("i").toInt();
    if (i >= 0 && i < queueLen) {
      String s = queue[i].s;
      for (int j = i; j < queueLen - 1; j++) queue[j] = queue[j + 1];
      queueLen--; saveQueue();
      if (ps.fname != s) sendCmdHi("M30 " + s);
    }
  }
  else if (act == "delfile") sendCmdHi("M30 " + qp("s"));
  else if (act == "cmd") sendCmdHi(qp("c"));  // произвольный g-code через MQTT
}

// ==================== СТРОКА СТАТУСА (JSON) ====================
String buildStatusJson() {
  String j = "{";
  j += "\"online\":" + String(printerSeen ? "true" : "false");
  String state = "idle";
  if (uploading) state = "upload";
  else if (ps.active && ps.paused) state = "paused";
  else if (ps.active) state = "printing";
  j += ",\"state\":\"" + state + "\"";
  j += ",\"pct\":" + String(ps.total ? (float)((uint64_t)ps.pos * 1000 / ps.total) / 10.0f : 0.0f, 1);
  j += ",\"bytes\":" + String(ps.pos) + ",\"total\":" + String(ps.total);
  j += ",\"file\":\"" + jsonStr(ps.fname) + "\"";
  j += ",\"t\":\"" + jsonStr(tNozzle) + "\",\"th\":\"" + jsonStr(tTarget) + "\"";
  j += ",\"b\":\"" + jsonStr(bBed) + "\",\"bh\":\"" + jsonStr(bTarget) + "\"";
  j += ",\"sd\":" + String(sdMounted ? "true" : "false");
  j += ",\"info\":\"" + jsonStr(fwInfo) + "\"";
  j += ",\"uploading\":" + String(uploading ? "true" : "false");
  j += ",\"code\":\"" + deviceId + "\"";
  j += ",\"queue\":[";
  for (int i = 0; i < queueLen; i++) {
    if (i) j += ",";
    j += "{\"s\":\"" + jsonStr(queue[i].s) + "\",\"r\":\"" + jsonStr(queue[i].r) + "\"}";
  }
  j += "],\"auto\":" + String(queueAuto ? "true" : "false");
  j += ",\"files\":[";
  for (int i = 0; i < filesLen; i++) { if (i) j += ","; j += "\"" + jsonStr(files[i]) + "\""; }
  j += "]";
  j += ",\"log\":[";
  for (int i = 0; i < logCount; i++) {
    if (i) j += ",";
    j += "\"" + jsonStr(logLines[(logHead - logCount + i + 60) % 30]) + "\"";
  }
  j += "]";
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"needWifi\":" + String(WiFi.status() == WL_CONNECTED ? "false" : "true");
  j += "}";
  return j;
}

// ==================== HTTP HANDLERS ====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang=ru><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Hornet Panel</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--card2:#1c2330;--tx:#e6edf3;--mut:#8b949e;--acc:#2f81f7;--ok:#3fb950;--warn:#d29922;--err:#f85149}
*{box-sizing:border-box;margin:0;padding:0}body{background:var(--bg);color:var(--tx);font:14px/1.4 -apple-system,'Segoe UI',Roboto,sans-serif;padding-bottom:78px}
header{position:sticky;top:0;z-index:5;background:linear-gradient(135deg,#161b22,#0d1117);border-bottom:1px solid #21262d;padding:10px 14px;display:flex;align-items:center;gap:8px}
header b{font-size:16px}.dot{width:10px;height:10px;border-radius:50%;background:var(--err)}.dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
main{max-width:720px;margin:0 auto;padding:10px}.card{background:var(--card);border:1px solid #21262d;border-radius:12px;padding:12px;margin-bottom:10px}
.card h3{font-size:11px;text-transform:uppercase;letter-spacing:.6px;color:var(--mut);margin-bottom:8px}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
button{border:0;border-radius:10px;padding:9px 12px;background:var(--card2);color:var(--tx);font-size:13px;cursor:pointer;touch-action:manipulation;-webkit-tap-highlight-color:transparent;transition:.12s}
button:active{transform:scale(.95)}.pri{background:var(--acc);color:#fff}.ok{background:var(--ok);color:#fff}
.danger{background:#b62324;color:#fff}.sm{padding:6px 10px;font-size:12px}
button:disabled{opacity:.3}
.badge{display:inline-block;padding:2px 9px;border-radius:20px;font-size:11px;background:#21262d;color:var(--mut)}
.badge.printing{background:#1a4a2a;color:#7ee2a8}.badge.paused{background:#4a3c14;color:#ffd970}.badge.upload{background:#14374a;color:#70c5fe}
.ring-wrap{display:flex;gap:14px;align-items:center;flex-wrap:wrap}
.ring{width:100px;height:100px;flex:none}.ring text{fill:var(--tx);font-size:18px;font-weight:700}
.sub{color:var(--mut);font-size:12px}
.temp{display:flex;justify-content:space-between;align-items:center;padding:5px 0;border-bottom:1px dashed #21262d;gap:8px}
.temp:last-child{border:0}.temp input{width:60px;background:var(--card2);border:1px solid #30363d;color:var(--tx);border-radius:8px;padding:5px;text-align:center;font-size:13px}
.chips{display:flex;gap:6px;flex-wrap:wrap}.chip{padding:7px 12px;border-radius:20px;background:var(--card2);border:1px solid #30363d;font-size:12px;cursor:pointer}
.step-row{display:flex;gap:4px;margin-bottom:8px}
.step{padding:6px 12px;border-radius:20px;background:var(--card2);border:1px solid #30363d;font-size:13px;cursor:pointer}.step.on{border-color:var(--acc);color:#7ec0ff}
.jog{display:grid;grid-template-columns:repeat(3,56px);grid-template-rows:repeat(4,42px);gap:4px;justify-content:center;margin-bottom:8px}
.jog button{padding:0;font-size:13px}.jog .lbl{pointer-events:none;font-size:10px;display:flex;align-items:center;justify-content:center}
.list{display:flex;justify-content:space-between;align-items:center;padding:7px 4px;border-bottom:1px solid #21262d;font-size:13px;gap:6px}
.list:last-child{border:0}.list .n{word-break:break-all}.list .a{display:flex;gap:5px;flex:none}
.filebox{border:2px dashed #30363d;border-radius:12px;padding:16px;text-align:center;color:var(--mut)}
.bar{height:7px;border-radius:4px;background:#21262d;overflow:hidden;margin-top:7px;display:none}
.bar i{display:block;height:100%;width:0;background:var(--acc);transition:.15s}
nav{position:fixed;bottom:0;left:0;right:0;display:flex;background:var(--card);border-top:1px solid #21262d;z-index:6}
nav button{flex:1;border-radius:0;padding:9px 0 11px;background:transparent;color:var(--mut);font-size:10px}
nav button.on{color:var(--acc)}.page{display:none}.page.on{display:block}
pre#log{background:#0a0d12;border-radius:10px;padding:8px;font:11px/1.4 monospace;height:44vh;overflow:auto;white-space:pre-wrap}
footer{text-align:center;color:#30363d;font-size:10px;padding:8px}
</style></head><body>
<header><div class=dot id=dot></div><b>Hornet Panel</b><span class=badge id=bState>нет связи</span>
<span style="flex:1"></span><span class=sub id=bIp></span></header>
<main>

<div class="page on" id=pg1>
 <div class=card><h3>Статус печати</h3>
  <div class=ring-wrap>
   <svg class=ring viewBox="0 0 120 120"><circle cx=60 cy=60 r=52 stroke=#21262d stroke-width=9 fill=none/>
   <circle id=arc cx=60 cy=60 r=52 stroke=#2f81f7 stroke-width=9 fill=none stroke-linecap=round stroke-dasharray="326.7" stroke-dashoffset="326.7" transform="rotate(-90 60 60)"/>
   <text id=pctTxt x=60 y=66 text-anchor=middle>0%</text></svg>
   <div style="min-width:200px"><div id=fname class=sub>—</div><div id=fbytes style="font-size:12px;margin-top:3px"></div>
    <div class=row style="margin-top:8px">
     <button class=ok id=bPause onclick="act('pause')">⏸ Пауза</button>
     <button class=pri id=bResume onclick="act('resume')">▶ Продолжить</button>
     <button class=danger onclick=stopPrint()>⏹ Стоп</button>
     <button class=sm onclick="act('queuecont')">Продолжить очередь</button>
    </div></div></div></div>
 <div class=card><h3>Температура</h3>
  <div class=temp><div><b id=tN>–</b><span class=sub> / <span id=tT>–</span> °C · Сопло</span></div>
   <div class=row><input id=inN type=number min=0 max=300><button class=sm onclick=setT()>Задать</button></div></div>
  <div class=temp><div><b id=bN>–</b><span class=sub> / <span id=bT>–</span> °C · Стол</span></div>
   <div class=row><input id=inB type=number min=0 max=130><button class=sm onclick=setT()>Задать</button></div></div>
  <h3 style="margin-top:10px">Режимы</h3>
  <div class=chips><div class=chip onclick="act('preheat?m=pla')">PLA 210/60</div><div class=chip onclick="act('preheat?m=petg')">PETG 240/80</div>
   <div class=chip onclick="act('preheat?m=abs')">ABS 250/100</div><div class=chip onclick="act('preheat?m=tpu')">TPU 225/45</div>
   <div class=chip onclick="act('preheat?m=off')">Выкл</div></div></div>
 <div class=card><h3>Движение</h3>
  <div class=step-row><span class=sub style="margin-right:4px">Шаг (мм):</span>
   <div class=step id=s1 onclick="step(1)">1</div><div class="step on" id=s5 onclick="step(5)">5</div><div class=step id=s10 onclick="step(10)">10</div></div>
  <div class=row style="justify-content:space-between;gap:12px">
   <div class=jog>
    <span></span><button onclick="mv('Y',d)">Y+</button><span></span>
    <button onclick="mv('X',-d)">X−</button><button onclick="mv('Z',d)">Z+</button><button onclick="mv('X',d)">X+</button>
    <span></span><button onclick="mv('Y',-d)">Y−</button><span></span>
    <span></span><button onclick="mv('Z',-d)">Z−</button><span></span>
   </div>
   <div class=row style="flex-direction:column;align-items:stretch;flex:1;min-width:150px">
    <button onclick="act('home')">🏠 Домой (G28)</button>
    <button onclick="act('g29')">◈ Калибровка стола</button>
    <button onclick="act('motors')">Моторы выкл</button>
    <button onclick="act('cool')">❄ Остудить</button>
    <button onclick="act('filload')">⬆ Загрузить филамент</button>
    <button onclick="act('filunload')">⬇ Выгрузить филамент</button></div></div>
  <div class=row style="margin-top:10px"><span class=sub>Обдув модели (FAN0):</span>
   <button class=sm onclick="act('fan0?p=0')">0</button><button class=sm onclick="act('fan0?p=128')">50%</button><button class=sm onclick="act('fan0?p=255')">100%</button>
   <span style="flex:1"></span><button class=danger onclick=estop()>⚠ E-STOP</button></div>
  <div class=row style="margin-top:6px"><span class=sub>Вентилятор головы (FAN1):</span>
   <button class=sm onclick="act('fan1?p=0')">0</button><button class=sm onclick="act('fan1?p=128')">50%</button><button class=sm onclick="act('fan1?p=255')">100%</button></div>
  <div class=row style="margin-top:6px"><span class=sub>Вентилятор БП (FAN2):</span>
   <button class=sm onclick="act('fan2?p=0')">0</button><button class=sm onclick="act('fan2?p=128')">50%</button><button class=sm onclick="act('fan2?p=255')">100%</button></div>
</div></div>
 <div class=card><h3>Принтер</h3><div class=sub id=bInfo>–</div></div>
</div>

<div class=page id=pg2>
 <div class=card><h3>Загрузка модели на SD</h3>
  <div class=filebox><input type=file id=f accept=".gcode,.gco,.g" style="display:none" onchange=uploadStart()>
   <button class=pri onclick="document.getElementById('f').click()">📄 Выбрать G-code</button>
   <div class=sub style="margin-top:6px">файл передаётся по UART (~15 КБ/с) → SD принтера</div></div>
  <div class=bar id=ubar><i id=uarc></i></div><div class=sub id=ustat style="margin-top:4px"></div></div>
 <div class=card><h3>Очередь (<span id=qlen>0</span>)
  <span style="float:right"><label class=sub><input type=checkbox id=chkAuto onchange="act('queueauto?on='+(this.checked?1:0))"> авто</label></span></h3>
  <div id=qbox class=sub>пуста</div></div>
 <div class=card><h3>Файлы на SD <button class=sm style="float:right" onclick="act('files');setTimeout(poll,600)">⟳</button></h3>
  <div id=fbox class=sub id=fboxMsg>—</div></div>
</div>

<div class=page id=pg3>
 <div class=card><h3>Консоль G-code</h3>
  <div class=row><input id=gcmd style="flex:1;background:var(--card2);border:1px solid #30363d;color:var(--tx);border-radius:8px;padding:8px" placeholder="M115" onkeydown="if(event.key=='Enter')gSend()">
  <button class=pri onclick=gSend()>➤</button></div>
  <pre id=log></pre></div>
</div>

<div class=page id=pg4>
 <div class=card><h3>Сеть</h3><div class=sub id=netinfo>–</div>
  <div class=row style="margin-top:10px"><button onclick="location='/wifi'">⚙ WiFi и пароль</button></div></div>
 <div class=card><h3>Удалённый доступ (MQTT)</h3>
  <div class=sub>Код устройства (введите в <b>remote.html</b>): <b id=bCode style="font-size:16px;letter-spacing:2px">—</b></div>
  <div class=sub style="margin-top:6px">Без роутера: ESP сам подключается к test.mosquitto.org и ждёт команды из интернета.
   Откройте файл <b>remote.html</b> на любом устройстве с интернетом, введите код — панель работает откуда угодно.</div>
  <div class=sub style="margin-top:6px">Прямой доступ: ваш домен <b>hornetartilery.duckdns.org</b> → IP <b>188.162.204.17</b>
   — в роутере: проброс порта <b>8080</b> → <span id=ip2>IP</span>:<b>80</b>.</div></div>
 <div class=card><h3>OTA обновление</h3>
  <div class=sub>После первой прошивки по USB — следующие обновления по WiFi. Порт в Arduino IDE: hornet.local.</div></div>
</div>

<footer>Hornet Panel v2 · ESP8266 ↔ Marlin 2.0.7.2</footer>
</main>
<nav><button class=on id=n1 onclick="pg(1)">🔥<br>Статус</button><button id=n2 onclick="pg(2)">📦<br>Модели</button>
<button id=n3 onclick="pg(3)">⌨<br>Консоль</button><button id=n4 onclick="pg(4)">🌐<br>Сеть</button></nav>
<script>
var S={};var d=5;
function $(i){return document.getElementById(i)}
function pg(n){for(var i=1;i<=4;i++){$('pg'+i).className=i==n?'page on':'page';$('n'+i).className=i==n?'on':''}}
function act(u){return fetch('/api/'+u).then(function(r){return r.text()}).catch(function(){})}
function esc(s){return (s||'').replace(/</g,'&lt;').replace(/&/g,'&amp;')}
function stopPrint(){if(confirm('Остановить печать?'))act('stop')}
function estop(){if(confirm('АВАРИЙНЫЙ ОСТАНОВ?'))act('estop')}
function mv(a,v){act('move?a='+a+'&d='+v)}
function step(n){d=n;$('s1').className=n==1?'step on':'step';$('s5').className=n==5?'step on':'step';$('s10').className=n==10?'step on':'step'}
function setT(){act('set?n='+encodeURIComponent($('inN').value)+'&b='+encodeURIComponent($('inB').value))}
function gSend(){var c=$('gcmd').value.trim();if(!c)return;act('cmd?c='+encodeURIComponent(c));$('gcmd').value=''}
function render(){
 $('dot').className=S.online?'dot on':'dot';
 var s=S.state||'idle';
 $('bState').textContent={idle:'свободно',printing:'печать',paused:'пауза',upload:'загрузка'}[s]||s;
 $('bState').className='badge '+s;
 var pct=S.pct||0;
 $('pctTxt').textContent=pct.toFixed(1)+'%';
 $('arc').style.strokeDashoffset=(326.7*(1-pct/100)).toFixed(1);
 $('fname').textContent=S.file?('файл: '+S.file):(s=='idle'?'свободно':'');
 $('fbytes').textContent=S.total?((S.bytes/1024|0)+' / '+(S.total/1024|0)+' КБ'):'';
 $('bPause').disabled=s!='printing';$('bResume').disabled=s!='paused';
 $('tN').textContent=S.t;$('tT').textContent=S.th;$('bN').textContent=S.b;$('bT').textContent=S.bh;
 $('bInfo').textContent=S.info||'–';$('bCode').textContent=S.code||'—';
 $('bIp').textContent=S.ip||'';$('ip2').textContent=S.ip||'…';
 $('netinfo').innerHTML=S.needWifi?'<b style="color:#d29922">WiFi не подключён — Hornet-Setup (192.168.4.1)</b>':
  ('подключён · RSSI '+S.rssi+' дБм · IP: <b>'+(S.ip||'—')+'</b> · hornet.local');
 $('ubar').style.display=S.uploading?'block':'none';
 $('qlen').textContent=(S.queue||[]).length;$('chkAuto').checked=!!S.auto;
 var q=$('qbox');
 if(!(S.queue||[]).length)q.innerHTML='<span class=sub>пуста</span>';
 else q.innerHTML=S.queue.map(function(x,i){return '<div class=list><span class=n>'+esc(x.r)+'<br><span class=sub>'+x.s+'</span></span><span class=a>'+
  '<button class=sm onclick="act(\'print?s='+x.s+'\')">▶</button>'+
  '<button class=sm onclick="act(\'queuedel?i='+i+'\')">✖</button></span></div>'}).join('');
 var fb=$('fbox');
 if(!S.sd)fb.innerHTML='<span style="color:#d29922">SD не определена</span>';
 else if(!(S.files||[]).length)fb.textContent='нет .gco файлов';
 else fb.innerHTML=S.files.map(function(x){return '<div class=list><span class=n>'+esc(x)+'</span><span class=a>'+
  '<button class=sm onclick="act(\'print?s='+x+'\')">▶</button>'+
  '<button class=sm onclick="act(\'delfile?s='+x+'\')">🗑</button></span></div>'}).join('');
 var lg=$('log');
 if(S.log){var atB=lg.scrollTop+lg.clientHeight>=lg.scrollHeight-20;lg.textContent=S.log.join('\n');if(atB)lg.scrollTop=lg.scrollHeight}
}
function poll(){fetch('/api/status').then(function(r){return r.json()}).then(function(s){S=s;render()}).catch(function(){$('dot').className='dot'})}
function uploadStart(){
 var f=$('f').files[0];if(!f)return;
 if(f.size>25*1024*1024){alert('Файл >25 МБ');$('f').value='';return}
 var fd=new FormData();fd.append('file',f);
 var xt=new XMLHttpRequest();xt.open('POST','/upload');
 $('ubar').style.display='block';$('uarc').style.width='0%';$('ustat').textContent='передача…';
 xt.upload.onprogress=function(e){if(e.lengthComputable){$('uarc').style.width=(e.loaded/e.total*100).toFixed(0)+'%';
  $('ustat').textContent=(e.loaded/1024|0)+' / '+(e.total/1024|0)+' КБ · ≈'+Math.round(e.loaded/18/1024)+' c'}};
 xt.onload=function(){$('ustat').textContent='✔ загружено, добавлено в очередь';$('f').value='';setTimeout(poll,400)};
 xt.onerror=function(){$('ustat').textContent='✖ ошибка'};
 xt.send(fd);
}
setInterval(poll,1000);poll();
</script></body></html>)rawliteral";

const char WIFI_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang=ru><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Hornet — сеть</title><style>
body{background:#0d1117;color:#e6edf3;font:15px sans-serif;max-width:520px;margin:0 auto;padding:20px}
label{display:block;margin:14px 0 4px;color:#8b949e;font-size:13px}
input{width:100%;padding:10px;border-radius:8px;border:1px solid #30363d;background:#161b22;color:#e6edf3}
button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:10px;background:#2f81f7;color:#fff;font-size:15px}
h2{font-size:17px}small{color:#8b949e;line-height:1.6}</style></head><body>
<h2>🌐 Сеть Hornet Panel</h2>
<form method=GET action=/wifisave>
<label>Имя WiFi (SSID)</label><input name=ssid placeholder="ваша сеть">
<label>Пароль WiFi</label><input name=pass placeholder="пароль роутера">
<label>Пароль доступа к панели (пусто = без пароля)</label><input name=apass type=password placeholder="для интернета — обязательно!">
<button>Сохранить и переподключиться</button></form>
<small>Если WiFi не подключится, панель снова станет точкой <b>Hornet-Setup</b> (12345678).</small>
</body></html>)rawliteral";

void handleIndex() { if (gate()) server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }
void handleWifiPage() { server.send_P(200, "text/html; charset=utf-8", WIFI_HTML); }
void handleWifiSave() {
  wifiSsid = server.arg("ssid"); wifiSsid.trim();
  wifiPass = server.arg("pass"); wifiPass.trim();
  accessPass = server.arg("apass"); accessPass.trim();
  saveCfg(); computeDeviceId(); mqttInit();
  server.send_P(200, "text/html; charset=utf-8",
    "<meta charset=utf-8><body style='background:#111;color:#eee;font-family:sans-serif'>"
    "<p>Переподключение…</p><script>setTimeout(function(){location='/'},4000)</script>");
  WiFi.disconnect(); connectWiFi();
  if (WiFi.status() != WL_CONNECTED) { WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); }
  else { MDNS.begin(HOSTNAME); MDNS.addService("http", "tcp", 80); }
}
void handleStatus() { if (!gate()) return; server.send(200, "application/json; charset=utf-8", buildStatusJson()); }
void handleCmd() { if (!gate()) return; String c = server.arg("c"); c.replace("\r", " "); c.replace("\n", " "); c.trim();
  if (!c.length() || c.length() > 120) { server.send(400, "text/plain", "bad"); return; } sendCmd(c); okJson(); }
void handleAct() { if (!gate()) return; String path = server.uri(); String query = server.args() ? server.arg("plain") : "";
  // reconstruct: uri + "?" + args
  String cmd = path.substring(5); // remove "/api/"
  bool first = true;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "plain") continue;
    cmd += (first ? '?' : '&') + server.argName(i) + '=' + server.arg(i);
    first = false;
  }
  dispatchAction(cmd); okJson();
}
void handleFiles()  { if (!gate()) return; sendCmd("M20"); okJson(); }
void handlePrint()  { if (!gate()) return; String s = server.arg("s"); s.trim(); s.toLowerCase();
  if (!s.length() || s.length() > 12) { server.send(400, "text/plain", "bad"); return; } queueAuto = true; startPrint(s); okJson(); }
void handlePause()  { if (!gate()) return; sendCmd("M25"); okJson(); }
void handleResume() { if (!gate()) return; ps.paused = false; ps.lastGrow = millis(); sendCmd("M24"); okJson(); }
void handleStop()   { if (!gate()) return; queueAuto = false; sendCmd("M524"); okJson(); }
void handleEstop()  { if (!gate()) return; sendCmd("M112"); sendCmd("M999"); okJson(); }
void handleHome()   { if (!gate()) return; sendCmd("G28"); okJson(); }
void handleG29()    { if (!gate()) return; sendCmd("G28"); sendCmd("G29"); okJson(); }
void handleMotors() { if (!gate()) return; sendCmd("M84"); okJson(); }
void handleCool()   { if (!gate()) return; sendCmd("M104 S0"); sendCmd("M140 S0"); sendCmd("M107"); okJson(); }
void handlePreheat() { if (!gate()) return; String m = server.arg("m"); int n = 210, b = 60;
  if (m == "petg") { n = 240; b = 80; } else if (m == "abs") { n = 250; b = 100; }
  else if (m == "tpu") { n = 225; b = 45; } else if (m == "off") { n = 0; b = 0; }
  sendCmd("M104 S" + String(n)); sendCmd("M140 S" + String(b)); okJson(); }
void handleMove() { if (!gate()) return; String a = server.arg("a"); a.toUpperCase(); float d = server.arg("d").toFloat();
  if (a.length() != 1 || strchr("XYZE", a[0]) == 0 || d == 0 || fabs(d) > 100) { server.send(400, "text/plain", "bad"); return; }
  int f = 4000; if (a == "Z") f = 600; if (a == "E") f = 300;
  sendCmd("G91"); sendCmd("G1 " + a + String(d, 2) + " F" + String(f)); sendCmd("G90"); okJson(); }
void handleFan() { if (!gate()) return; int p = server.arg("p").toInt();
  sendCmd(p <= 0 ? "M107" : "M106 S" + String(constrain(p, 0, 255))); okJson(); }
void handleFan0() { if (!gate()) return; int p = server.arg("p").toInt();
  sendCmd(p <= 0 ? "M107 P0" : "M106 P0 S" + String(constrain(p, 0, 255))); okJson(); }
void handleFan1() { if (!gate()) return; int p = server.arg("p").toInt();
  sendCmd(p <= 0 ? "M107 P1" : "M106 P1 S" + String(constrain(p, 0, 255))); okJson(); }
void handleFan2() { if (!gate()) return; int p = server.arg("p").toInt();
  sendCmd(p <= 0 ? "M107 P2" : "M106 P2 S" + String(constrain(p, 0, 255))); okJson(); }
void handleFilLoad() { if (!gate()) return; sendCmd("M701"); okJson(); }
void handleFilUnload() { if (!gate()) return; sendCmd("M702"); okJson(); }
void handleSet() { if (!gate()) return; String n = server.arg("n"), b = server.arg("b");
  if (n.length() && n.toFloat() <= 300) sendCmd("M104 S" + n);
  if (b.length() && b.toFloat() <= 130) sendCmd("M140 S" + b); okJson(); }
void handleQueueAuto() { if (!gate()) return; queueAuto = server.arg("on") == "1"; okJson(); }
void handleQueueCont() { if (!gate()) return; queueAuto = true; if (!ps.active && queueLen > 0) startPrint(queue[0].s); okJson(); }
void handleQueueDel() { if (!gate()) return; int i = server.arg("i").toInt();
  if (i < 0 || i >= queueLen) { server.send(400, "text/plain", "bad"); return; }
  String s = queue[i].s; for (int j = i; j < queueLen - 1; j++) queue[j] = queue[j + 1]; queueLen--; saveQueue();
  if (ps.fname != s) sendCmd("M30 " + s); okJson(); }
void handleDelFile() { if (!gate()) return; String s = server.arg("s"); s.trim(); s.toLowerCase();
  if (!s.length() || s.length() > 12) { server.send(400, "text/plain", "bad"); return; } sendCmd("M30 " + s); okJson(); }
void handleDebug() {
  String t = "printer: " + String(printerSeen ? "есть" : "НЕТ") +
    "\nfw: " + fwInfo + "\nt=" + tNozzle + "/" + tTarget + " b=" + bBed + "/" + bTarget +
    "\nprint: " + String(ps.active) + "/" + String(ps.paused) + " " + String(ps.pos) + "/" + String(ps.total) + " " + ps.fname +
    "\nsd=" + String(sdMounted) + " files=" + String(filesLen) + " queue=" + String(queueLen) +
    "\nwifi=" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("AP:") + WiFi.softAPIP().toString()) +
    "\nmqtt=" + (mqttInitDone ? (mqttClient.connected() ? "connected" : "disconnected") : "off") +
    "\ncode=" + deviceId;
  server.send(200, "text/plain; charset=utf-8", t);
}

// ==================== ЗАГРУЗКА ФАЙЛА ====================
void handleUploadProc() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadReal = up.filename; if (!uploadReal.length()) uploadReal = "model.gcode";
    uploadShort = makeShortName(); uploadBytes = 0; uploading = true;
    Serial.print("\r\n");
    uint32_t t0 = millis(); while (millis() - t0 < 120) { handlePrinter(); delay(1); }
    gotOk = false; Serial.print("M28 " + uploadShort + "\r\n");
    t0 = millis(); while (!gotOk && millis() - t0 < 3000) { handlePrinter(); delay(1); }
    if (!gotOk) { uploading = false; addLog("! M28 не принят"); }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploading || !up.currentSize) return;
    handlePrinter();
    size_t off = 0;
    while (off < up.currentSize) {
      size_t n = up.currentSize - off > 380 ? 380 : up.currentSize - off;
      Serial.write(up.buf + off, n); off += n; uploadBytes += n; Serial.flush(); handlePrinter(); delay(10);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!uploading) return;
    delay(60); gotOk = false; Serial.print("M29\r\n");
    uint32_t t0 = millis(); while (!gotOk && millis() - t0 < 5000) { handlePrinter(); delay(1); }
    uploading = false;
    if (gotOk) { if (queueLen < 12) { queue[queueLen].s = uploadShort; queue[queueLen].r = uploadReal; queueLen++; saveQueue(); }
      addLog("* загружено: " + uploadShort + " ← " + uploadReal); }
    else addLog("! M29 без ответа");
    sendCmdHi("M20");
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  rxAnyMs = millis();
  LittleFS.begin();
  loadCfg();
  loadQueue();

  WiFi.mode(WIFI_STA);
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) { WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); }
  else { MDNS.begin(HOSTNAME); MDNS.addService("http", "tcp", 80); }

  computeDeviceId();
  mqttInit();

  ArduinoOTA.setHostname(HOSTNAME);
  if (accessPass.length()) ArduinoOTA.setPassword(accessPass.c_str());
  ArduinoOTA.begin();

  server.on("/", HTTP_GET, handleIndex);
  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifisave", HTTP_GET, handleWifiSave);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/cmd", HTTP_GET, handleCmd);
  server.on("/debug", HTTP_GET, handleDebug);
  server.on("/api/files", HTTP_GET, handleFiles);
  server.on("/api/print", HTTP_GET, handlePrint);
  server.on("/api/pause", HTTP_GET, handlePause);
  server.on("/api/resume", HTTP_GET, handleResume);
  server.on("/api/stop", HTTP_GET, handleStop);
  server.on("/api/estop", HTTP_GET, handleEstop);
  server.on("/api/home", HTTP_GET, handleHome);
  server.on("/api/g29", HTTP_GET, handleG29);
  server.on("/api/motors", HTTP_GET, handleMotors);
  server.on("/api/cool", HTTP_GET, handleCool);
  server.on("/api/preheat", HTTP_GET, handlePreheat);
  server.on("/api/move", HTTP_GET, handleMove);
  server.on("/api/fan", HTTP_GET, handleFan);
  server.on("/api/fan0", HTTP_GET, handleFan0);
  server.on("/api/fan1", HTTP_GET, handleFan1);
  server.on("/api/fan2", HTTP_GET, handleFan2);
  server.on("/api/filload", HTTP_GET, handleFilLoad);
  server.on("/api/filunload", HTTP_GET, handleFilUnload);
  server.on("/api/set", HTTP_GET, handleSet);
  server.on("/api/queueauto", HTTP_GET, handleQueueAuto);
  server.on("/api/queuecont", HTTP_GET, handleQueueCont);
  server.on("/api/queuedel", HTTP_GET, handleQueueDel);
  server.on("/api/delfile", HTTP_GET, handleDelFile);
  server.on("/upload", HTTP_POST, []() { server.send(200, "text/plain", "ok"); }, handleUploadProc);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();

  sendCmdLo("M115");
  sendCmdLo("M21");
  sendCmdLo("M20");
}

// ==================== LOOP ====================
void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  handlePrinter();
  MDNS.update();
  uint32_t now = millis();
  pumpCmds(now);
  autoPoll(now);
  mqttEnsure();
  if (now - lastMqttPub > 2000) { lastMqttPub = now; mqttPublishStatus(); }
}
