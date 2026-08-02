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
#include "esp32-hal-tinyusb.h"
// CONFIG_HTML[] is generated from webui/index.html by build.ps1. Without it the
// sketch still builds (Wokwi, a bare Arduino IDE copy) and serves a pointer to
// the hosted copy instead of the embedded page.
#if __has_include("webui_html.h")
#include "webui_html.h"
#else
const char CONFIG_HTML[] PROGMEM =
    "<!doctype html><meta charset=utf-8><title>MacroPad</title>"
    "<p>This build has no embedded UI. Open "
    "<a href=\"https://noxitro.github.io/esp32s3/webui/\">the hosted config page</a>"
    " and connect over USB (WebSerial).";
#endif

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
// Public config UI (GitHub Pages). Pressing key 1 while in config mode types a
// Win+R macro that opens this URL; it is also advertised as the WebUSB landing
// page (Chrome shows a "Go to ..." notification when the device is plugged in).
const char *CONFIG_URL = "https://noxitro.github.io/esp32s3/webui/";

Preferences prefs;
WebServer server(80);
String keyStrings[NUM_KEYS];
bool layoutJIS = true;  // host keyboard layout: JIS (true) or US (false); NVS "layout"

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
bool serialOverflow = false;  // drop the rest of an over-long line, not its head
unsigned long lastBlinkMs = 0;
bool blinkOn = false;
bool pageArmed = false;       // a lone key is down and may open the config page

// ---------------- NVS ----------------

void loadStrings() {
  prefs.begin("keycfg", true);
  keyStrings[0] = prefs.getString("s1", "KEY1");
  keyStrings[1] = prefs.getString("s2", "KEY2");
  keyStrings[2] = prefs.getString("s3", "KEY3");
  layoutJIS = prefs.getString("layout", "jis") == "jis";
  prefs.end();
}

void saveLayout(bool jis) {
  prefs.begin("keycfg", false);
  prefs.putString("layout", jis ? "jis" : "us");
  prefs.end();
  layoutJIS = jis;
}

// Returns false if the value could not be persisted (NVS full/corrupt), so the
// caller never reports success for a setting that will vanish on reboot.
bool saveString(int idx, const String &value) {
  const char *keys[NUM_KEYS] = {"s1", "s2", "s3"};
  prefs.begin("keycfg", false);
  size_t written = prefs.putString(keys[idx], value);
  prefs.end();
  if (written != value.length()) return false;
  keyStrings[idx] = value;
  return true;
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
      // A-Z only: "| 0x20" would turn @ [ \ ] ^ _ into a different character
      else if (part.length() == 1) {
        char pc = part[0];
        Keyboard.press((pc >= 'A' && pc <= 'Z') ? (pc | 0x20) : pc);
      }
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
// The US-layout ASCII map built into USBHIDKeyboard sends wrong keys for many
// symbols when the HOST uses a JIS (Japanese) layout — e.g. ':' arrives as '+'.
// This table sends the correct JIS keystrokes for the characters that differ.
struct JisChar { char c; uint8_t usage; bool shift; };
const JisChar JIS_CHARS[] = {
  {'"', 0x1F, true},  {'&', 0x23, true},  {'\'', 0x24, true}, {'(', 0x25, true},
  {')', 0x26, true},  {'*', 0x34, true},  {'+', 0x33, true},  {':', 0x34, false},
  {'=', 0x2D, true},  {'@', 0x2F, false}, {'[', 0x30, false}, {'\\', 0x87, false},
  {']', 0x32, false}, {'^', 0x2E, false}, {'_', 0x87, true},  {'`', 0x2F, true},
  {'{', 0x30, true},  {'|', 0x89, true},  {'}', 0x32, true},  {'~', 0x2E, true},
};

void typeChar(char c) {
  if (layoutJIS) {
    for (auto &j : JIS_CHARS) {
      if (j.c == c) {
        if (j.shift) Keyboard.press(KEY_LEFT_SHIFT);
        Keyboard.pressRaw(j.usage);
        delay(8);
        Keyboard.releaseAll();
        return;
      }
    }
  }
  Keyboard.write(c);
}
#endif  // HAS_USB_HID

void execMacro(const String &m) {
#if HAS_USB_HID
  for (unsigned int i = 0; i < m.length(); i++) {
    char c = m[i];
    if (c == '{') {
      int close = m.indexOf('}', i);
      if (close < 0) { typeChar(c); delay(8); continue; }  // unmatched: literal '{'
      String tok = m.substring(i + 1, close);
      if (!execToken(tok)) Serial.printf("MACRO ERR: unknown token {%s}\n", tok.c_str());
      i = close;
    } else if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
      typeChar(c);
      delay(8);
    }
    // non-ASCII bytes are skipped: HID cannot type them directly
  }
#endif
}

void sendKey(int idx) {
  if (keyStrings[idx].length() == 0) return;   // cleared key: do nothing
  execMacro(keyStrings[idx]);
  Serial.printf("SENT[%d]: %s\n", idx + 1, keyStrings[idx].c_str());
}

// Reboots into the ROM download mode so the firmware can be reflashed.
// Must use the core's usb_persist_restart(): besides setting the RTC download
// flag it switches the USB PHY from OTG to CDC/JTAG, without which the ROM
// never enumerates. It is NOT gated by Serial.enableReboot(false).
void rebootToBootloader() {
#if HAS_USB_HID
  Serial.println("Rebooting into download mode...");
  Serial.flush();
  delay(200);
  usb_persist_restart(RESTART_BOOTLOADER);
#else
  Serial.println("ERR: download mode requires the USB-OTG build");
#endif
}

// Types a Win+R macro on the host so it opens the hosted config UI in a browser
void openConfigPage() {
  Serial.printf("Opening config page on host: %s\n", CONFIG_URL);
  execMacro(String("{WIN+R}{WAIT:600}") + CONFIG_URL + "{WAIT:200}{ENTER}");
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
    j += "\"" + String(i + 1) + "\":\"" + jsonEscape(keyStrings[i]) + "\",";
  }
  j += "\"layout\":\"" + String(layoutJIS ? "jis" : "us") + "\"}";
  server.send(200, "application/json; charset=utf-8", j);
}

// Only same-device pages may drive the API: a page on the phone that happens to
// be open while it is joined to the AP must not be able to POST here.
bool originAllowed() {
  String o = server.header("Origin");
  if (o.length() == 0) return true;  // same-origin form posts send no Origin
  return o.indexOf(WiFi.softAPIP().toString()) >= 0;
}

void handleSet() {
  if (!originAllowed()) { server.send(403, "text/plain", "ERR: bad origin"); return; }
  int k = server.arg("k").toInt();
  String v = server.arg("v");           // empty value clears the macro
  if (k < 1 || k > NUM_KEYS || v.length() > MACRO_MAX_LEN) {
    server.send(400, "text/plain", "ERR: bad k or v");
    return;
  }
  if (!saveString(k - 1, v)) {
    Serial.printf("ERR: key%d not saved (NVS)\n", k);
    server.send(500, "text/plain", "ERR: not saved");
    return;
  }
  Serial.printf("OK: key%d = %s (via web)\n", k, v.c_str());
  server.send(200, "text/plain", "OK");
}

void handleLayout() {
  if (!originAllowed()) { server.send(403, "text/plain", "ERR: bad origin"); return; }
  String v = server.arg("v");
  if (v != "jis" && v != "us") { server.send(400, "text/plain", "ERR: bad layout"); return; }
  saveLayout(v == "jis");
  Serial.printf("OK: layout = %s (via web)\n", v.c_str());
  server.send(200, "text/plain", "OK");
}

void startConfigServer() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", []() { server.send_P(200, "text/html; charset=utf-8", CONFIG_HTML); });
  server.on("/api/keys", HTTP_GET, handleKeys);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/layout", HTTP_POST, handleLayout);
  server.on("/api/exit", HTTP_POST, []() {
    if (!originAllowed()) { server.send(403, "text/plain", "ERR: bad origin"); return; }
    server.send(200, "text/plain", "OK");
    exitRequested = true;
  });
  const char *headers[] = {"Origin"};
  server.collectHeaders(headers, 1);
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
  Serial.printf("layout = %s\n", layoutJIS ? "jis" : "us");
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
  Serial.println("Commands: 1=<macro>  2=<macro>  3=<macro>  show  test1|test2|test3  open  layout=jis|us  exit");
  Serial.printf("Press any key (or send 'open') to open the config page: %s\n", CONFIG_URL);
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
  if (line == "open") { openConfigPage(); return; }
  if (line == "config") {  // idempotent: clients send this on every connect
    Serial.println("CONFIG MODE");
    Serial.printf("Already in config mode. http://%s/\n", WiFi.softAPIP().toString().c_str());
    return;
  }
  if (line == "boot") { rebootToBootloader(); return; }
  if (line == "layout=jis" || line == "layout=us") {
    saveLayout(line.endsWith("jis"));
    Serial.printf("OK: layout = %s\n", layoutJIS ? "jis" : "us");
    return;
  }
  if (line.startsWith("test") && line.length() == 5 && line[4] >= '1' && line[4] <= '3') {
    int idx = line[4] - '1';
    Serial.printf("TEST key%d...\n", idx + 1);
    sendKey(idx);
    return;
  }
  if (line.length() >= 2 && line[0] >= '1' && line[0] <= '3' && line[1] == '=') {
    int idx = line[0] - '1';
    String value = line.substring(2);   // "n=" with no value clears the macro
    if (value.length() > MACRO_MAX_LEN) {
      Serial.println("ERR: too long");
      return;
    }
    if (!saveString(idx, value)) {
      Serial.printf("ERR: key%d not saved (NVS)\n", idx + 1);
      return;
    }
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
  else if (line == "boot") rebootToBootloader();
  else if (line.length()) Serial.printf("ERR: unknown command: %s (try 'config')\n", line.c_str());
}

void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (serialOverflow) { Serial.println("ERR: line too long"); }
      else if (mode == CONFIG) handleConfigCommand(serialLine);
      else handleNormalCommand(serialLine);
      serialLine = "";
      serialOverflow = false;
    } else if (c != '\r' && !serialOverflow) {
      serialLine += c;
      // discard the whole line, not just its head: a truncated tail could
      // otherwise be executed as its own command
      if (serialLine.length() > MACRO_MAX_LEN + 8) { serialLine = ""; serialOverflow = true; }
    }
  }
}

// ---------------- buttons ----------------

void pollButtons(bool pressEdge[NUM_KEYS], bool releaseEdge[NUM_KEYS]) {
  unsigned long now = millis();
  for (int i = 0; i < NUM_KEYS; i++) {
    pressEdge[i] = false;
    releaseEdge[i] = false;
    bool reading = digitalRead(BUTTON_PINS[i]) == LOW;
    if (reading != lastReading[i]) {
      lastChangeMs[i] = now;
      lastReading[i] = reading;
    }
    if ((now - lastChangeMs[i]) >= DEBOUNCE_MS && reading != stableState[i]) {
      stableState[i] = reading;
      if (reading) pressEdge[i] = true;
      else releaseEdge[i] = true;
    }
  }
}

int keysDown() {
  int n = 0;
  for (int i = 0; i < NUM_KEYS; i++) if (stableState[i]) n++;
  return n;
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
  // Escape hatch: holding all three keys while powering up enters the ROM
  // download mode, so the board can always be reflashed without the tiny BOOT
  // button and without a working serial link.
  delay(50);
  if (digitalRead(BUTTON_PINS[0]) == LOW && digitalRead(BUTTON_PINS[1]) == LOW &&
      digitalRead(BUTTON_PINS[2]) == LOW) {
    setAllLeds(true);
    delay(300);
    usb_persist_restart(RESTART_BOOTLOADER);
  }
#endif

#if HAS_USB_HID
  // Opening/closing the CDC port (Arduino IDE, WebSerial, esptool) otherwise
  // toggles DTR/RTS and drops the device off USB. Reflashing uses the "boot"
  // serial command (or the BOOT button) instead.
  Serial.enableReboot(false);
  USB.webUSB(true);
  USB.webUSBURL(CONFIG_URL);
  Keyboard.begin();
  Consumer.begin();
  USB.begin();
#endif

  delay(100);
  Serial.println("READY");
  printStrings();
}

void loop() {
  bool pressEdge[NUM_KEYS], releaseEdge[NUM_KEYS];
  pollButtons(pressEdge, releaseEdge);
  pollSerial();

  if (pollCombo()) {
    pageArmed = false;  // the 3-key hold is a mode toggle, not a page request
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
    // A single tap opens the web config page on the host. It fires on release
    // and only for a lone key, so the 3-key hold used to LEAVE config mode
    // never types Win+R into whatever the host has focused.
    for (int i = 0; i < NUM_KEYS; i++) {
      if (pressEdge[i]) pageArmed = (keysDown() == 1);
      if (releaseEdge[i] && pageArmed && keysDown() == 0) {
        pageArmed = false;
        openConfigPage();
      }
    }
    unsigned long now = millis();
    if (now - lastBlinkMs >= CONFIG_BLINK_MS) {
      lastBlinkMs = now;
      blinkOn = !blinkOn;
      setAllLeds(blinkOn);
    }
  }
}
