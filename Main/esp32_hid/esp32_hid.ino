/*
 * Victrl ESP32 BLE HID Bridge
 * ──────────────────────────────────────────────────────────────────
 * Uses the ESP32 core BLE library only
 *
 * Protocol (one command per line, '\n' terminated):
 *   M <x> <y>         absolute mouse move (pixels)
 *   D <button>        mouse down  (left | right | middle)
 *   U <button>        mouse up
 *   C <button>        mouse click
 *   S <dx> <dy>       scroll (int, int)
 *   K <combo>         key press  (e.g. "ctrl+c", "enter")
 *   T <b64>           type string (base64-encoded)
 *   R                 release all
 *   Z <w> <h>         set screen resolution (pixels)
 *   W <ms>            wait milliseconds
 */

#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// ── Hardware config ─────────────────────────────────────────────────────
#define SERIAL_BAUD    115200
#define DEVICE_NAME    "Victrl HID"
#define MANUFACTURER   "Victrl"
#define MAX_LINE_LEN   512

int screenW = 1920;   // default, overridden by Z command from host
int screenH = 1080;

// ── BLE globals ─────────────────────────────────────────────────────────
BLEServer*         bleServer    = nullptr;
BLEHIDDevice*      bleHID       = nullptr;
BLECharacteristic* kbdReport    = nullptr;  // keyboard input report (ID 1)
BLECharacteristic* kbdBootIn    = nullptr;  // keyboard boot input
BLECharacteristic* kbdBootOut   = nullptr;  // keyboard boot output (LEDs)
BLECharacteristic* mouseReport  = nullptr;  // mouse input report (ID 2)
BLEAdvertising*    advertising  = nullptr;
bool bleConnected = false;

// ── Virtual cursor tracking ─────────────────────────────────────────────
int curX = screenW / 2;
int curY = screenH / 2;

// ── Serial line buffer ──────────────────────────────────────────────────
char lineBuf[MAX_LINE_LEN];
int  linePos = 0;

// ── Key-map structures ──────────────────────────────────────────────────
struct KeyEntry {
  const char* name;
  uint8_t     code;          // USB HID usage ID (or ASCII for printable)
};

// Modifier sentinels
#define _K_CTRL   0xE0
#define _K_SHIFT  0xE1
#define _K_ALT    0xE2
#define _K_GUI    0xE3

// ── USB HID Keyboard usage IDs (raw hex) ────────────────────────────────
#define HID_ENTER          0x28
#define HID_ESC            0x29
#define HID_BACKSPACE      0x2A
#define HID_TAB            0x2B
#define HID_SPACE          0x2C
#define HID_CAPSLOCK       0x39
#define HID_F1             0x3A
#define HID_F2             0x3B
#define HID_F3             0x3C
#define HID_F4             0x3D
#define HID_F5             0x3E
#define HID_F6             0x3F
#define HID_F7             0x40
#define HID_F8             0x41
#define HID_F9             0x42
#define HID_F10            0x43
#define HID_F11            0x44
#define HID_F12            0x45
#define HID_PRINTSCREEN    0x46
#define HID_SCROLLLOCK     0x47
#define HID_PAUSE          0x48
#define HID_INSERT         0x49
#define HID_HOME           0x4A
#define HID_PAGEUP         0x4B
#define HID_DELETE         0x4C
#define HID_END            0x4D
#define HID_PAGEDOWN       0x4E
#define HID_RIGHT          0x4F
#define HID_LEFT           0x50
#define HID_DOWN           0x51
#define HID_UP             0x52
#define HID_NUMLOCK        0x53
#define HID_KPSLASH        0x54
#define HID_KPASTERISK     0x55
#define HID_KPMINUS        0x56
#define HID_KPPLUS         0x57
#define HID_KPENTER        0x58
#define HID_KP1            0x59
#define HID_KP2            0x5A
#define HID_KP3            0x5B
#define HID_KP4            0x5C
#define HID_KP5            0x5D
#define HID_KP6            0x5E
#define HID_KP7            0x5F
#define HID_KP8            0x60
#define HID_KP9            0x61
#define HID_KP0            0x62
#define HID_KPDOT          0x63

const KeyEntry keyMap[] = {
  {"a", 'a'}, {"b", 'b'}, {"c", 'c'}, {"d", 'd'}, {"e", 'e'},
  {"f", 'f'}, {"g", 'g'}, {"h", 'h'}, {"i", 'i'}, {"j", 'j'},
  {"k", 'k'}, {"l", 'l'}, {"m", 'm'}, {"n", 'n'}, {"o", 'o'},
  {"p", 'p'}, {"q", 'q'}, {"r", 'r'}, {"s", 's'}, {"t", 't'},
  {"u", 'u'}, {"v", 'v'}, {"w", 'w'}, {"x", 'x'}, {"y", 'y'}, {"z", 'z'},
  {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
  {"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},
  {"f1", HID_F1},  {"f2", HID_F2},  {"f3", HID_F3},  {"f4", HID_F4},
  {"f5", HID_F5},  {"f6", HID_F6},  {"f7", HID_F7},  {"f8", HID_F8},
  {"f9", HID_F9},  {"f10",HID_F10}, {"f11",HID_F11}, {"f12",HID_F12},
  {"space",       HID_SPACE},
  {"enter",       HID_ENTER},
  {"tab",         HID_TAB},
  {"backspace",   HID_BACKSPACE},
  {"escape",      HID_ESC},
  {"esc",         HID_ESC},
  {"delete",      HID_DELETE},
  {"insert",      HID_INSERT},
  {"home",        HID_HOME},
  {"end",         HID_END},
  {"pageup",      HID_PAGEUP},
  {"pagedown",    HID_PAGEDOWN},
  {"up",          HID_UP},
  {"down",        HID_DOWN},
  {"left",        HID_LEFT},
  {"right",       HID_RIGHT},
  {"capslock",    HID_CAPSLOCK},
  {"numlock",     HID_NUMLOCK},
  {"printscreen", HID_PRINTSCREEN},
  {"pause",       HID_PAUSE},
  {"-", '-'}, {"=", '='}, {"[", '['}, {"]", ']'},
  {";", ';'}, {"'",'\''}, {"`", '`'}, {"\\",'\\'},
  {",", ','}, {".", '.'}, {"/", '/'},
  {"kp1",HID_KP1},{"kp2",HID_KP2},{"kp3",HID_KP3},
  {"kp4",HID_KP4},{"kp5",HID_KP5},{"kp6",HID_KP6},
  {"kp7",HID_KP7},{"kp8",HID_KP8},{"kp9",HID_KP9},
  {"kp0",HID_KP0},
  {"ctrl", _K_CTRL}, {"shift", _K_SHIFT},
  {"alt",  _K_ALT},  {"super", _K_GUI},
  {"win",  _K_GUI},  {"command", _K_GUI},
};

const int keyMapSize = sizeof(keyMap) / sizeof(keyMap[0]);

// ── HID Report descriptors ──────────────────────────────────────────────

// Keyboard boot report descriptor (8 bytes: mod + reserved + 6 keys)
static const uint8_t kbdReportDesc[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID 1
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0,        //   Usage Minimum (Left Control)
  0x29, 0xE7,        //   Usage Maximum (Right GUI)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x08,        //   Report Count (8)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)  — modifier byte
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x08,        //   Report Size (8)
  0x81, 0x01,        //   Input (Constant)                   — reserved byte
  0x95, 0x06,        //   Report Count (6)
  0x75, 0x08,        //   Report Size (8)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x65,        //   Logical Maximum (101)
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0x00,        //   Usage Minimum (0)
  0x29, 0x65,        //   Usage Maximum (101)
  0x81, 0x00,        //   Input (Data, Array)                — 6 key slots
  0xC0               // End Collection
};

// Mouse boot report descriptor (4 bytes: buttons + X + Y + wheel)
static const uint8_t mouseReportDesc[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x02,        //   Report ID 2
  0x09, 0x01,        //   Usage (Pointer)
  0xA1, 0x00,        //   Collection (Physical)
  0x05, 0x09,        //     Usage Page (Buttons)
  0x19, 0x01,        //     Usage Minimum (Button 1)
  0x29, 0x03,        //     Usage Maximum (Button 3)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x75, 0x01,        //     Report Size (1)
  0x95, 0x03,        //     Report Count (3)
  0x81, 0x02,        //     Input (Data, Var, Abs)  — 3 button bits
  0x95, 0x01,        //     Report Count (1)
  0x75, 0x05,        //     Report Size (5)
  0x81, 0x01,        //     Input (Constant)        — 5-bit padding
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x30,        //     Usage (X)
  0x09, 0x31,        //     Usage (Y)
  0x09, 0x38,        //     Usage (Wheel)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x03,        //     Report Count (3)
  0x81, 0x06,        //     Input (Data, Var, Rel)  — X, Y, Wheel
  0xC0,              //   End Collection
  0xC0               // End Collection
};

// ── Server callbacks ────────────────────────────────────────────────────
class VictrlServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    { bleConnected = true;  Serial.println("EVT BLE_connected"); }
  void onDisconnect(BLEServer*) { bleConnected = false; Serial.println("EVT BLE_disconnected");
                                  bleServer->startAdvertising(); }
};

// ── Forward declarations ────────────────────────────────────────────────
uint8_t lookupKey(const char* name);
void kbdSend(uint8_t modifiers, const uint8_t keys[6]);
void kbdReleaseAll();
void mouseSend(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void mouseReleaseAll();
void handleLine(const char* line);

// ── Setup ───────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.setTimeout(50);
  Serial.println("INIT Victrl ESP32 BLE HID Bridge");

  // ── 1. BLE stack ──────────────────────────────────────────────────
  BLEDevice::init(DEVICE_NAME);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new VictrlServerCB());

  bleHID = new BLEHIDDevice(bleServer);

  // ── 2. Build combined Report Map (keyboard ID1 + mouse ID2) ─────
  uint8_t combinedDesc[sizeof(kbdReportDesc) + sizeof(mouseReportDesc)];
  memcpy(combinedDesc, kbdReportDesc, sizeof(kbdReportDesc));
  memcpy(combinedDesc + sizeof(kbdReportDesc), mouseReportDesc, sizeof(mouseReportDesc));
  bleHID->reportMap(combinedDesc, sizeof(combinedDesc));

  // ── 3. Report characteristics (create AFTER reportMap) ───────────
  kbdReport   = bleHID->inputReport(1);       // regular keyboard input
  mouseReport = bleHID->inputReport(2);       // regular mouse input
  bleHID->outputReport(1);                    // keyboard output (LEDs, etc.)

  // ── 4. Boot protocol characteristics (REQUIRED by Windows) ───────
  kbdBootIn  = bleHID->bootInput();
  kbdBootOut = bleHID->bootOutput();

  // ── 5. Device metadata ───────────────────────────────────────────
  bleHID->manufacturer()->setValue(MANUFACTURER);
  bleHID->pnp(0x02, 0x1234, 0x5678, 0x0110);
  bleHID->hidInfo(0x00, 0x01);

  // ── 6. Start services ────────────────────────────────────────────
  bleHID->startServices();

  // ── 7. Advertising ───────────────────────────────────────────────
  advertising = bleServer->getAdvertising();
  advertising->setAppearance(0x03C0);           // Generic HID
  advertising->addServiceUUID(bleHID->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->start();

  Serial.println("OK READY");
}

// ── Loop ────────────────────────────────────────────────────────────────
void loop() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (linePos > 0) {
        lineBuf[linePos] = '\0';
        handleLine(lineBuf);
        linePos = 0;
      }
    } else if (linePos < MAX_LINE_LEN - 1) {
      lineBuf[linePos++] = c;
    }
  }
}

// ── Keyboard report sender ──────────────────────────────────────────────
void kbdSend(uint8_t modifiers, const uint8_t keys[6]) {
  if (!bleConnected) return;
  uint8_t report[8] = { modifiers, 0x00, keys[0], keys[1], keys[2],
                        keys[3],   keys[4], keys[5] };
  // Regular input report (Report Protocol)
  if (kbdReport) { kbdReport->setValue(report, 8); kbdReport->notify(); }
  // Boot input report (Boot Protocol — BIOS / older Windows)
  if (kbdBootIn) { kbdBootIn->setValue(report, 8); kbdBootIn->notify(); }
  delay(2);
}

void kbdReleaseAll() {
  uint8_t empty[6] = {0};
  kbdSend(0, empty);
}

// ── Mouse report sender ─────────────────────────────────────────────────
void mouseSend(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
  if (!bleConnected || !mouseReport) return;
  uint8_t report[4] = { buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
  mouseReport->setValue(report, 4);
  mouseReport->notify();
}

void mouseReleaseAll() {
  mouseSend(0, 0, 0, 0);
}

// ── Command dispatcher ──────────────────────────────────────────────────
void handleLine(const char* line) {
  if (line[0] == '\0') return;
  char cmd = line[0];
  const char* args = line + 1;
  while (*args == ' ') args++;

  switch (cmd) {
    case 'M': {  // ── Mouse move absolute ──
      int x, y;
      if (sscanf(args, "%d %d", &x, &y) != 2) { Serial.println("ERR M"); return; }
      if (x < 0) x = 0; if (x > screenW) x = screenW;
      if (y < 0) y = 0; if (y > screenH) y = screenH;
      int dx = x - curX, dy = y - curY;
      if (dx < -127) dx = -127; if (dx > 127) dx = 127;
      if (dy < -127) dy = -127; if (dy > 127) dy = 127;
      if (dx || dy) { mouseSend(0, (int8_t)dx, (int8_t)dy, 0); curX += dx; curY += dy; }
      Serial.printf("OK M %d %d\n", curX, curY);
      break;
    }
    case 'D': {  // ── Mouse down ──
      uint8_t btn = 0;
      if      (!strcmp(args, "left"))   btn = 0x01;
      else if (!strcmp(args, "right"))  btn = 0x02;
      else if (!strcmp(args, "middle")) btn = 0x04;
      else { Serial.printf("ERR D %s\n", args); return; }
      mouseSend(btn, 0, 0, 0);
      Serial.printf("OK D %s\n", args);
      break;
    }
    case 'U': {  // ── Mouse up ──
      mouseSend(0, 0, 0, 0);
      Serial.printf("OK U %s\n", args);
      break;
    }
    case 'C': {  // ── Mouse click ──
      uint8_t btn = 0;
      if      (!strcmp(args, "left"))   btn = 0x01;
      else if (!strcmp(args, "right"))  btn = 0x02;
      else if (!strcmp(args, "middle")) btn = 0x04;
      else { Serial.printf("ERR C %s\n", args); return; }
      mouseSend(btn, 0, 0, 0);  delay(15);
      mouseSend(0,   0, 0, 0);
      Serial.printf("OK C %s\n", args);
      break;
    }
    case 'S': {  // ── Scroll ──
      int dx = 0, dy = 0;
      sscanf(args, "%d %d", &dx, &dy);
      if (dx < -127) dx = -127; if (dx > 127) dx = 127;
      if (dy < -127) dy = -127; if (dy > 127) dy = 127;
      mouseSend(0, 0, 0, (int8_t)dy);
      Serial.printf("OK S %d %d\n", dx, dy);
      break;
    }
    case 'K': {  // ── Key press (combo) ──
      char buf[128]; strncpy(buf, args, sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
      const char* tokens[5]; int nTok = 0;
      char* saveptr;
      char* tok = strtok_r(buf, "+", &saveptr);
      while (tok && nTok < 5) {
        while (*tok == ' ') tok++;
        char* e = tok + strlen(tok) - 1;
        while (e > tok && *e == ' ') { *e = '\0'; e--; }
        tokens[nTok++] = tok;
        tok = strtok_r(nullptr, "+", &saveptr);
      }
      if (!nTok) { Serial.println("ERR K empty"); return; }

      uint8_t mods = 0, keys[6] = {0}; int ki = 0;

      for (int i = 0; i < nTok; i++) {
        uint8_t code = lookupKey(tokens[i]);
        if      (code == _K_CTRL)  mods |= 0x01;
        else if (code == _K_SHIFT) mods |= 0x02;
        else if (code == _K_ALT)   mods |= 0x04;
        else if (code == _K_GUI)   mods |= 0x08;
        else if (ki < 6) {           // non-modifier key
          if (code >= 0x20 && code <= 0x7E) {
            // ASCII — convert to USB HID code
            keys[ki++] = asciiToHid(code);
          } else {
            keys[ki++] = code;       // already a HID code
          }
        }
      }

      // Send press
      kbdSend(mods, keys);
      delay(30);  // hold key for OS to register (was 10ms, too short)
      // Release
      uint8_t zero[6] = {0};
      kbdSend(0, zero);
      Serial.printf("OK K %s\n", args);
      break;
    }
    case 'T': {  // ── Type string (base64) ──
      int inLen = strlen(args);
      if (!inLen) { Serial.println("OK T (empty)"); return; }
      char outBuf[256]; int outLen = 0, bits = 0, val = 0;
      for (int i = 0; i < inLen; i++) {
        char c = args[i]; if (c == '=') break;
        int v;
        if (c>='A'&&c<='Z') v=c-'A'; else if (c>='a'&&c<='z') v=c-'a'+26;
        else if (c>='0'&&c<='9') v=c-'0'+52; else if (c=='+') v=62;
        else if (c=='/') v=63; else continue;
        val = (val<<6)|v; bits+=6;
        if (bits>=8) { bits-=8; if (outLen<(int)sizeof(outBuf)-1) outBuf[outLen++]=(val>>bits)&0xFF; }
      }
      outBuf[outLen] = '\0';
      for (int i = 0; i < outLen; i++) {
        uint8_t code = asciiToHid((uint8_t)outBuf[i]);
        uint8_t keys[6] = {code, 0,0,0,0,0};
        uint8_t mods = keyModifier((uint8_t)outBuf[i]);  // shift handling
        kbdSend(mods, keys);
        delay(15);  // BLE needs time between notifications (was 5ms, too fast)
      }
      kbdReleaseAll();
      Serial.printf("OK T %d\n", outLen);
      break;
    }
    case 'R':  // ── Release all ──
      kbdReleaseAll();
      mouseReleaseAll();
      Serial.println("OK R");
      break;
    case 'Z': {  // ── Set screen resolution ──
      int w, h;
      if (sscanf(args, "%d %d", &w, &h) != 2) { Serial.println("ERR Z"); return; }
      if (w > 0 && h > 0) {
        screenW = w; screenH = h;
        curX = screenW / 2; curY = screenH / 2;  // re-center cursor
        Serial.printf("OK Z %d %d\n", screenW, screenH);
      } else {
        Serial.println("ERR Z invalid");
      }
      break;
    }
    case 'W': { int ms = atoi(args); if (ms>0) delay(ms);
                Serial.printf("OK W %d\n", ms); break; }
    default:
      Serial.printf("ERR unknown cmd: %c\n", cmd);
  }
}

// ── ASCII → USB HID keycode ─────────────────────────────────────────────
// ASCII values that map 1:1 to HID (after removing 0x60 offset)
uint8_t asciiToHid(uint8_t ascii) {
       if (ascii >= 'a' && ascii <= 'z') return ascii - 'a' + 0x04;
  else if (ascii >= 'A' && ascii <= 'Z') return ascii - 'A' + 0x04;
  else if (ascii >= '1' && ascii <= '9') return ascii - '1' + 0x1E;
  else if (ascii == '0')                 return 0x27;
  else if (ascii == ' ')                return 0x2C;
  else if (ascii == '\n' || ascii=='\r')return 0x28;
  else if (ascii == '\t')               return 0x2B;
  else if (ascii == '\b')               return 0x2A;
  // Symbols — HID code lookup
  else switch (ascii) {
    case '-': return 0x2D; case '=': return 0x2E; case '[': return 0x2F;
    case ']': return 0x30; case '\\':return 0x31; case ';': return 0x33;
    case '\'':return 0x34; case '`': return 0x35; case ',': return 0x36;
    case '.': return 0x37; case '/': return 0x38;
    default:  return 0x00;  // unmapped
  }
}

// ── Which characters need Left-Shift modifier ───────────────────────────
uint8_t keyModifier(uint8_t ascii) {
  uint8_t mod = 0;
  if (ascii >= 'A' && ascii <= 'Z') mod |= 0x02;
  else switch (ascii) {
    case '!': case '@': case '#': case '$': case '%': case '^': case '&':
    case '*': case '(': case ')': case '_': case '+': case '{': case '}':
    case '|': case ':': case '"': case '<': case '>': case '?': case '~':
      mod |= 0x02; break;
  }
  return mod;
}

// ── Key name lookup ─────────────────────────────────────────────────────
uint8_t lookupKey(const char* name) {
  for (int i = 0; i < keyMapSize; i++) {
    if (strcmp(name, keyMap[i].name) == 0)
      return keyMap[i].code;
  }
  if (strlen(name) == 1) return (uint8_t)name[0];
  return 0;
}
