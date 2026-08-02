// ESP32-S3 3-key HID macro pad (XIAO ESP32-S3)
//
// - Keys on GPIO 1/2/3 (D0/D1/D2, top/mid/bottom), active-low, internal pullup
// - LEDs on GPIO 4/5/6 (D3/D4/D5), lit while the matching key is held
// - Each key runs a configurable MACRO over USB HID (keyboard + consumer control)
// - Config mode: hold all 3 keys 3 s, or send "config" over serial.
//   While in config mode a WiFi AP "MacroPad-Config" (pass: macropad123) serves
//   a web UI at http://192.168.4.1/ ; serial commands and WebSerial also work.
// - Macros persist to NVS.
//
// Macro syntax:
//   plain text            typed as-is (ASCII only)
//   {ENTER} {TAB} {F5}    named keys
//   {CTRL+SHIFT+T}        modifier combos (CTRL/SHIFT/ALT/WIN/GUI/CMD/OPT)
//   {WAIT:500}            delay in ms
//   {ZENHAN} {HENKAN} {MUHENKAN} {HIRAGANA}   JIS IME keys (Windows)
//   {EISU} {KANA}         Mac IME keys
//   {BROWSER} {CALC} {VOL_UP} {VOL_DOWN} {MUTE} {MEDIA_PLAY} {AC_HOME} {AC_BACK}
//                         consumer-control keys
//
// USB HID requires USB Mode = "USB-OTG (TinyUSB)" (ARDUINO_USB_MODE == 0).
// With the default "Hardware CDC and JTAG" build (used by Wokwi), HID is
// compiled out and macro execution is only logged to serial.

#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include "webui_html.h"  // CONFIG_HTML[] generated from webui/index.html

#if ARDUINO_USB_MODE == 0
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
USBHIDKeyboard Keyboard;
USBHIDConsumerControl Consumer;
#define HAS_USB_HID 1
#else
#define HAS_USB_HID 0
#endif

const int NUM_KEYS = 3;
// XIAO ESP32-S3 macro pad PCB: keys on D0/D1/D2, LEDs on D3/D4/D5 (top/mid/bottom)
const int BUTTON_PINS[NUM_KEYS] = {1, 2, 3};
const int LED_PINS[NUM_KEYS] = {4, 5, 6};

const unsigned long DEBOUNCE_MS = 25;
const unsigned long COMBO_HOLD_MS = 3000;
const unsigned long CONFIG_BLINK_MS = 500;
const size_t MACRO_MAX_LEN = 500;

const char *AP_SSID = "MacroPad-Config";
const char *AP_PASS = "macropad123";

Preferences prefs;
WebServer server(80);
String keyStrings[NUM_KEYS];

enum Mode { NORMAL, CONFIG };
Mode mode = NORMAL;
bool exitRequested = false;  // set by /api/exit, handled in loop()

// Debounced button state
bool stableState[NUM_KEYS];
bool lastReading[NUM_KEYS];
unsigned long lastChangeMs[NUM_KEYS];

// All-3-buttons combo tracking
bool comboArmed = true;
unsigned long comboStartMs = 0;
bool comboActive = false;

String serialLine;
unsigned long lastBlinkMs = 0;
bool blinkOn = false;

// ---------------- NVS ----------------

void loadStrings() {
  prefs.begin("keycfg", true);
  keyStrings[0] = prefs.getString("s1", "KEY1");
  keyStrings[1] = prefs.getString("s2", "KEY2");
  keyStrings[2] = prefs.getString("s3", "KEY3");
  prefs.end();
}

void saveString(int idx, const String &value) {
  const char *keys[NUM_KEYS] = {"s1", "s2", "s3"};
  prefs.begin("keycfg", false);
  prefs.putString(keys[idx], value);
  prefs.end();
  keyStrings[idx] = value;
}

// ---------------- macro engine ----------------

#if HAS_USB_HID
struct NamedKey { const char *name; uint8_t code; };
// codes for USBHIDKeyboard::press() (arduino KEY_* constants / ASCII)
const NamedKey NAMED_KEYS[] = {
  {"ENTER", KEY_RETURN}, {"ESC", KEY_ESC}, {"TAB", KEY_TAB}, {"SPACE", ' '},
  {"BACKSPACE", KEY_BACKSPACE}, {"BS", KEY_BACKSPACE}, {"DELETE", KEY_DELETE}, {"DEL", KEY_DELETE},
  {"UP", KEY_UP_ARROW}, {"DOWN", KEY_DOWN_ARROW}, {"LEFT", KEY_LEFT_ARROW}, {"RIGHT", KEY_RIGHT_ARROW},
  {"HOME", KEY_HOME}, {"END", KEY_END}, {"PGUP", KEY_PAGE_UP}, {"PGDN", KEY_PAGE_DOWN},
  {"F1", KEY_F1}, {"F2", KEY_F2}, {"F3", KEY_F3}, {"F4", KEY_F4}, {"F5", KEY_F5}, {"F6", KEY_F6},
  {"F7", KEY_F7}, {"F8", KEY_F8}, {"F9", KEY_F9}, {"F10", KEY_F10}, {"F11", KEY_F11}, {"F12", KEY_F12},
};
// raw HID usages sent with pressRaw() — JIS / Mac IME keys
const NamedKey RAW_KEYS[] = {
  {"ZENHAN", 0x35},    // 半角/全角 (JIS)
  {"HENKAN", 0x8A}, {"MUHENKAN", 0x8B}, {"HIRAGANA", 0x88},
  {"KANA", 0x90},      // Mac かな (LANG1)
  {"EISU", 0x91},      // Mac 英数 (LANG2)
};
struct ConsumerKey { const char *name; uint16_t usage; };
const ConsumerKey CONSUMER_KEYS[] = {
  {"BROWSER", 0x196}, {"CALC", 0x192}, {"MEDIA_PLAY", 0xCD},
  {"VOL_UP", 0xE9}, {"VOL_DOWN", 0xEA}, {"MUTE", 0xE2},
  {"AC_HOME", 0x223}, {"AC_BACK", 0x224}, {"AC_SEARCH", 0x221},
};

uint8_t modifierCode(const String &s) {
  if (s == "CTRL" || s == "CONTROL") return KEY_LEFT_CTRL;
  if (s == "SHIFT") return KEY_LEFT_SHIFT;
  if (s == "ALT" || s == "OPT" || s == "OPTION") return KEY_LEFT_ALT;
  if (s == "WIN" || s == "GUI" || s == "CMD" || s == "META") return KEY_LEFT_GUI;
  return 0;
}

// executes one {TOKEN}; returns false if the token was not recognized
bool execToken(String tok) {
  tok.trim();
  if (tok.startsWith("WAIT:")) {
    long ms = tok.substring(5).toInt();
    delay(constrain(ms, 0, 10000));
    return true;
  }
  // consumer keys (no modifier combos)
  for (auto &c : CONSUMER_KEYS) {
    if (tok == c.name) {
      Consumer.press(c.usage);
      delay(10);
      Consumer.release();
      return true;
    }
  }
  // modifier combo / named key / single char — split on '+'
  int start = 0;
  bool pressedAny = false;
  while (start <= (int)tok.length()) {
    int plus = tok.indexOf('+', start);
    bool isLast = (plus < 0);
    String part = isLast ? tok.substring(start) : tok.substring(start, plus);
    part.trim();
    if (part.length() == 0) { Keyboard.releaseAll(); return false; }
    if (!isLast) {
      uint8_t m = modifierCode(part);
      if (!m) { Keyboard.releaseAll(); return false; }
      Keyboard.press(m);
      pressedAny = true;
      start = plus + 1;
      continue;
    }
    // last part: named key?
    uint8_t code = 0;
    for (auto &k : NAMED_KEYS) if (part == k.name) { code = k.code; break; }
    if (code) { Keyboard.press(code); }
    else {
      uint8_t raw = 0;
      for (auto &k : RAW_KEYS) if (part == k.name) { raw = k.code; break; }
      if (raw) { Keyboard.pressRaw(raw); }
      else if (part.length() == 1) { Keyboard.press(part[0] | 0x20); }  // letters lowercase
      else {
        uint8_t m = modifierCode(part);   // combo ending in a modifier, e.g. {WIN}
        if (!m) { Keyboard.releaseAll(); return false; }
        Keyboard.press(m);
      }
    }
    delay(20);
    Keyboard.releaseAll();
    return true;
  }
  if (pressedAny) Keyboard.releaseAll();
  return false;
}
#endif  // HAS_USB_HID

void execMacro(const String &m) {
#if HAS_USB_HID
  for (unsigned int i = 0; i < m.length(); i++) {
    char c = m[i];
    if (c == '{') {
      int close = m.indexOf('}', i);
      if (close < 0) break;
      String tok = m.substring(i + 1, close);
      if (!execToken(tok)) Serial.printf("MACRO ERR: unknown token {%s}\n", tok.c_str());
      i = close;
    } else if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
      Keyboard.write(c);
      delay(8);
    }
    // non-ASCII bytes are skipped: HID cannot type them directly
  }
#endif
}

void sendKey(int idx) {
  execMacro(keyStrings[idx]);
  Serial.printf("SENT[%d]: %s\n", idx + 1, keyStrings[idx].c_str());
}

// ---------------- config web server (WiFi AP) ----------------

String jsonEscape(const String &s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { o += ' '; }
    else o += c;
  }
  return o;
}

void handleKeys() {
  String j = "{";
  for (int i = 0; i < NUM_KEYS; i++) {
    j += "\"" + String(i + 1) + "\":\"" + jsonEscape(keyStrings[i]) + "\"";
    if (i < NUM_KEYS - 1) j += ",";
  }
  j += "}";
  server.send(200, "application/json; charset=utf-8", j);
}

void handleSet() {
  int k = server.arg("k").toInt();
  String v = server.arg("v");
  if (k < 1 || k > NUM_KEYS || v.length() == 0 || v.length() > MACRO_MAX_LEN) {
    server.send(400, "text/plain", "ERR: bad k or v");
    return;
  }
  saveString(k - 1, v);
  Serial.printf("OK: key%d = %s (via web)\n", k, v.c_str());
  server.send(200, "text/plain", "OK");
}

void startConfigServer() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", []() { server.send_P(200, "text/html; charset=utf-8", CONFIG_HTML); });
  server.on("/api/keys", HTTP_GET, handleKeys);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/exit", HTTP_POST, []() {
    server.send(200, "text/plain", "OK");
    exitRequested = true;
  });
  server.begin();
  Serial.printf("WiFi AP: %s (pass: %s)  http://%s/\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

void stopConfigServer() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ---------------- modes ----------------

void printStrings() {
  for (int i = 0; i < NUM_KEYS; i++) {
    Serial.printf("key%d = %s\n", i + 1, keyStrings[i].c_str());
  }
}

void setAllLeds(bool on) {
  for (int i = 0; i < NUM_KEYS; i++) digitalWrite(LED_PINS[i], on ? HIGH : LOW);
}

void enterConfigMode() {
  mode = CONFIG;
  exitRequested = false;
  blinkOn = false;
  lastBlinkMs = millis();
  setAllLeds(false);
  Serial.println("CONFIG MODE");
  Serial.println("Commands: 1=<macro>  2=<macro>  3=<macro>  show  test1|test2|test3  exit");
  startConfigServer();
}

void exitConfigMode() {
  mode = NORMAL;
  stopConfigServer();
  setAllLeds(false);
  Serial.println("NORMAL MODE");
}

// ---------------- serial commands ----------------

void handleConfigCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "exit") { exitConfigMode(); return; }
  if (line == "show") { printStrings(); return; }
  if (line.startsWith("test") && line.length() == 5 && line[4] >= '1' && line[4] <= '3') {
    int idx = line[4] - '1';
    Serial.printf("TEST key%d...\n", idx + 1);
    sendKey(idx);
    return;
  }
  if (line.length() >= 2 && line[0] >= '1' && line[0] <= '3' && line[1] == '=') {
    int idx = line[0] - '1';
    String value = line.substring(2);
    if (value.length() == 0 || value.length() > MACRO_MAX_LEN) {
      Serial.println("ERR: empty or too long");
      return;
    }
    saveString(idx, value);
    Serial.printf("OK: key%d = %s\n", idx + 1, value.c_str());
    return;
  }
  Serial.printf("ERR: unknown command: %s\n", line.c_str());
}

// In NORMAL mode only "config" / "show" are accepted over serial
void handleNormalCommand(String line) {
  line.trim();
  if (line == "config") enterConfigMode();
  else if (line == "show") printStrings();
}

void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (mode == CONFIG) handleConfigCommand(serialLine);
      else handleNormalCommand(serialLine);
      serialLine = "";
    } else if (c != '\r') {
      serialLine += c;
      if (serialLine.length() > MACRO_MAX_LEN + 8) serialLine = "";
    }
  }
}

// ---------------- buttons ----------------

void pollButtons(bool pressEdge[NUM_KEYS]) {
  unsigned long now = millis();
  for (int i = 0; i < NUM_KEYS; i++) {
    pressEdge[i] = false;
    bool reading = digitalRead(BUTTON_PINS[i]) == LOW;
    if (reading != lastReading[i]) {
      lastChangeMs[i] = now;
      lastReading[i] = reading;
    }
    if ((now - lastChangeMs[i]) >= DEBOUNCE_MS && reading != stableState[i]) {
      stableState[i] = reading;
      if (reading) pressEdge[i] = true;
    }
  }
}

bool pollCombo() {
  bool allPressed = stableState[0] && stableState[1] && stableState[2];
  unsigned long now = millis();

  if (!allPressed) {
    comboActive = false;
    comboArmed = true;
    return false;
  }
  if (!comboActive) {
    comboActive = true;
    comboStartMs = now;
    return false;
  }
  if (comboArmed && (now - comboStartMs) >= COMBO_HOLD_MS) {
    comboArmed = false;
    return true;
  }
  return false;
}

// ---------------- main ----------------

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_KEYS; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
    stableState[i] = false;
    lastReading[i] = false;
    lastChangeMs[i] = 0;
  }

  loadStrings();

#if HAS_USB_HID
  Keyboard.begin();
  Consumer.begin();
  USB.begin();
#endif

  delay(100);
  Serial.println("READY");
  printStrings();
}

void loop() {
  bool pressEdge[NUM_KEYS];
  pollButtons(pressEdge);
  pollSerial();

  if (pollCombo()) {
    if (mode == NORMAL) enterConfigMode();
    else exitConfigMode();
    return;
  }

  if (mode == NORMAL) {
    for (int i = 0; i < NUM_KEYS; i++) {
      digitalWrite(LED_PINS[i], stableState[i] ? HIGH : LOW);
      if (pressEdge[i]) sendKey(i);
    }
  } else {
    server.handleClient();
    if (exitRequested) { exitConfigMode(); return; }
    unsigned long now = millis();
    if (now - lastBlinkMs >= CONFIG_BLINK_MS) {
      lastBlinkMs = now;
      blinkOn = !blinkOn;
      setAllLeds(blinkOn);
    }
  }
}
