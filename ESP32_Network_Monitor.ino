/*
  ESP32 Network Monitor with LEDs v0.1a
  ----------------------------
  Checks up to 9 devices on your local network and shows each one on a
  two-lead (bi-directional) red/green LED, driven directly from two pins:
      GREEN = device answered
      RED   = device did not answer (after FAILS_BEFORE_RED misses in a row)
      AMBER = not checked yet (just powered on)
      ALL LEDs BLINKING RED = the ESP32 itself is not connected to WiFi

  Written for the classic ESP32 (ESP32-WROOM-32 DevKit, the common 30 or
  38 pin board). ESP32-S2/S3/C3/C6 boards use different pin numbers.

  Each LED goes between its two pins with ONE 100 ohm resistor in series.

  Needs the "ESPping" library by dvarrel
  (Arduino IDE: Sketch > Include Library > Manage Libraries > search ESPping).
*/

#include <WiFi.h>
#include <ESPping.h>
#include <Ticker.h>

// ======================= YOUR SETTINGS =======================

const char* WIFI_SSID     = "YourNetworkName";   // 2.4 GHz only
const char* WIFI_PASSWORD = "YourWiFiPassword";

// Devices to check, in LED order (first entry = LED 1). Up to 9.
// port 0  = check with ping
// port >0 = check by opening a TCP connection to that port instead.
//           Use this for devices that ignore ping (Windows PCs often do).
//           Examples: 80 = web page, 22 = SSH, 445 = Windows file sharing,
//           8096 = Jellyfin, 5000 = Synology NAS web page.
struct Target {
  IPAddress ip;
  uint16_t  port;
};

Target targets[] = {
  { IPAddress(192, 168, 1, 1),   0 },   // LED 1 - router
  { IPAddress(192, 168, 1, 10),  0 },   // LED 2
  { IPAddress(192, 168, 1, 20),  0 },   // LED 3
  { IPAddress(192, 168, 1, 30),  0 },   // LED 4
  { IPAddress(192, 168, 1, 40),  0 },   // LED 5
  { IPAddress(192, 168, 1, 50),  0 },   // LED 6
  // Add up to 3 more lines for LEDs 7, 8 and 9 (read the notes on those pins below).
};

// The two pins for each LED. "greenPin" is the pin set HIGH to show green.
// Only the pins for LEDs you actually use get touched, so unused ones are safe.
struct LedPins {
  uint8_t greenPin;
  uint8_t redPin;
};

const LedPins LEDS[] = {
  { 32, 33 },   // LED 1  - trouble-free pins
  { 25, 26 },   // LED 2  - trouble-free pins
  { 27, 13 },   // LED 3  - trouble-free pins
  { 23, 22 },   // LED 4  - trouble-free pins
  { 21, 19 },   // LED 5  - trouble-free pins
  { 18,  4 },   // LED 6  - trouble-free pins
  { 17, 16 },   // LED 7  - fine on WROOM boards; NOT usable on WROVER boards (used for extra RAM)
  {  5, 14 },   // LED 8  - boot-mode pins; LED may flicker for a moment at power-on, harmless
  { 15,  2 },   // LED 9  - boot-mode pins; on some boards this LED stops uploads from working.
                //          If an upload fails, unplug LED 9, upload, then plug it back in.
};

// If an LED shows red when it should be green, flip LED around,
// or set this to true to flip ALL of them in software.
const bool SWAP_COLORS = false;

const uint8_t  FAILS_BEFORE_RED = 2;     // misses in a row before showing red
const uint32_t CHECK_SPACING_MS = 500;   // pause between checking one device and the next
const int32_t  TCP_TIMEOUT_MS   = 1500;  // how long to wait for a TCP port to answer

// ============== NOTHING TO CHANGE ==============

enum : uint8_t { LED_OFF = 0, LED_GREEN = 1, LED_RED = 2, LED_AMBER = 3 };

const uint8_t MAX_LEDS    = sizeof(LEDS) / sizeof(LEDS[0]);
const uint8_t NUM_TARGETS = sizeof(targets) / sizeof(targets[0]);
const uint8_t NUM_LEDS    = (NUM_TARGETS < MAX_LEDS) ? NUM_TARGETS : MAX_LEDS;

volatile uint8_t ledState[MAX_LEDS];     // what each LED should show
uint8_t          failCount[MAX_LEDS];    // consecutive misses per device

Ticker ledRefresher;
bool   amberPhase = false;

// Runs every 2 ms in the background, even while a ping is waiting for a reply.
// Amber is made by switching between red and green faster than eye can see.
void refreshLeds() {
  amberPhase = !amberPhase;
  for (uint8_t k = 0; k < NUM_LEDS; k++) {
    uint8_t s = ledState[k];
    if (s == LED_AMBER) s = amberPhase ? LED_GREEN : LED_RED;
    if (SWAP_COLORS && s != LED_OFF) s = (s == LED_GREEN) ? LED_RED : LED_GREEN;

    uint8_t g = LEDS[k].greenPin;
    uint8_t r = LEDS[k].redPin;
    if (s == LED_GREEN)    { digitalWrite(r, LOW);  digitalWrite(g, HIGH); }
    else if (s == LED_RED) { digitalWrite(g, LOW);  digitalWrite(r, HIGH); }
    else                   { digitalWrite(g, LOW);  digitalWrite(r, LOW);  }
  }
}

void setAll(uint8_t state) {
  for (uint8_t k = 0; k < NUM_LEDS; k++) ledState[k] = state;
}

void printWiring() {
  Serial.println(F("\nLED wiring:"));
  for (uint8_t k = 0; k < NUM_LEDS; k++) {
    Serial.printf("  LED %u: GPIO%-2u and GPIO%-2u   checks %s",
                  k + 1, LEDS[k].greenPin, LEDS[k].redPin,
                  targets[k].ip.toString().c_str());
    if (targets[k].port) Serial.printf(" port %u", targets[k].port);
    Serial.println();
  }
  if (NUM_TARGETS > MAX_LEDS) {
    Serial.printf("WARNING: %u devices listed but only %u LEDs defined. Extras ignored.\n",
                  NUM_TARGETS, MAX_LEDS);
  }
}

bool isReachable(const Target& t) {
  if (t.port == 0) {
    return Ping.ping(t.ip, 1);
  }
  WiFiClient client;
  bool ok = client.connect(t.ip, t.port, TCP_TIMEOUT_MS);
  client.stop();
  return ok;
}

// ---------- Arduino entry points ----------

void setup() {
  Serial.begin(115200);
  delay(200);

  for (uint8_t k = 0; k < NUM_LEDS; k++) {
    pinMode(LEDS[k].greenPin, OUTPUT);
    pinMode(LEDS[k].redPin, OUTPUT);
    ledState[k]  = LED_OFF;
    failCount[k] = 0;
  }
  ledRefresher.attach_ms(2, refreshLeds);

  printWiring();

  // Startup test: all green, then all red, so can spot wiring or orientation mistakes.
  setAll(LED_GREEN); delay(1500);
  setAll(LED_RED);   delay(1500);
  setAll(LED_AMBER);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("Connecting to WiFi"));
}

void loop() {
  static uint8_t  next         = 0;
  static uint32_t lastCheck    = 0;
  static uint32_t lastBlink    = 0;
  static bool     blinkOn      = false;
  static bool     showWifiDown = false;
  static uint8_t  savedState[MAX_LEDS];

  // --- WiFi down: blink every LED red ---
  if (WiFi.status() != WL_CONNECTED) {
    if (!showWifiDown) {
      for (uint8_t k = 0; k < NUM_LEDS; k++) savedState[k] = ledState[k];
      showWifiDown = true;
    }
    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      blinkOn = !blinkOn;
      setAll(blinkOn ? LED_RED : LED_OFF);
      Serial.print('.');
    }
    delay(10);
    return;
  }

  if (showWifiDown) {
    showWifiDown = false;
    for (uint8_t k = 0; k < NUM_LEDS; k++) ledState[k] = savedState[k];
    Serial.printf("\nWiFi connected. ESP32 address: %s\n", WiFi.localIP().toString().c_str());
  }

  // --- Check one device per pass, round robin ---
  if (NUM_LEDS == 0 || millis() - lastCheck < CHECK_SPACING_MS) {
    delay(10);
    return;
  }

  bool up = isReachable(targets[next]);
  lastCheck = millis();

  if (up) {
    failCount[next] = 0;
    ledState[next]  = LED_GREEN;
  } else {
    if (failCount[next] < 255) failCount[next]++;
    if (failCount[next] >= FAILS_BEFORE_RED) ledState[next] = LED_RED;
  }

  Serial.printf("LED %u  %-15s %s\n", next + 1,
                targets[next].ip.toString().c_str(),
                up ? "UP" : (ledState[next] == LED_RED ? "DOWN" : "missed, retrying"));

  next = (next + 1) % NUM_LEDS;
}
