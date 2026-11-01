
#include <Wire.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

//pins
#define PIN_BTN_PAUSE D0
#define PIN_VBAT      D1
#define PIN_BTN_LEFT  D2
#define PIN_BTN_RIGHT D3
#define PIN_SDA       D4
#define PIN_SCL       D5
#define PIN_BTN_PLUS  D6
#define PIN_BTN_MINUS D7
#define PIN_BTN_MODE  D8
#define PIN_BUZZER    D10

#define HT_ADDR 0x70
struct Button {
  uint8_t pin;
  bool stable;
  bool lastRead;
  uint32_t lastChange;
  uint32_t pressStart; //not a gd reference
  bool pressEvent;
  bool longEvent;
  bool longFired;
};



enum IncType { INC_NONE, INC_FISCHER, INC_BRONSTEIN, INC_DELAY };
struct Preset {
  uint16_t baseMin;
  uint8_t incSec;
  IncType type;
};
enum State {READY, RUNNING, PAUSED, FLAGGED, SETUP};
enum Field {F_PRESET, F_BASE, F_INC, F_TYPE};

uint32_t lastActivity = 0;



//digit segment bits
#define SEG_DP 0x80
const uint8_t DIGITS[10] {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

uint8_t segChar(char c) {
  if (c >= '0' && c <= '9') return DIGITS[c-'0'];
  if (c >=  'a' && c <= 'z') c-= 32;
  switch (c) {
    case 'A': return 0x77; 
    case 'B': return 0x7C;
    case 'C': return 0x39;
    case 'D': return 0x5E;
    case 'E': return 0x79;
    case 'F': return 0x71;
    case 'G': return 0x3D;
    case 'H': return 0x76;
    case 'I': return 0x30;
    case 'J': return 0x1E;
    // k is undrawable
    case 'L': return 0x38;
    // m also is undrawable :CC
    case 'N': return 0x54;
    case 'O': return 0x5C;
    case 'P': return 0x73;
    case 'Q': return 0x67;
    case 'R': return 0x50;
    case 'S': return 0x6D;
    case 'T': return 0x78;
    case 'U': return 0x3E;
    // v & w undrawable :(
    case 'Y': return 0x6E;
    case 'Z': return 0x5B;
    case '-': return 0x40;
    case '_': return 0x08;
    default: return 0x00;
  }
}

uint8_t frame[8];
bool htFound = false;

void htCmd(uint8_t c) {
  Wire.beginTransmission(HT_ADDR);
  Wire.write(c);
  Wire.endTransmission();
}

void htInit() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.beginTransmission(HT_ADDR);
  htFound = (Wire.endTransmission() == 0);
  htCmd(0x21);
  htCmd(0x81);
  htCmd(0xE0 | 15); // turn on oscillator, display, and brgithnes
}

void htBrightness(uint8_t b) {
  htCmd(0xE0 | (b & 0x0F));
}

void htFlush() {
  Wire.beginTransmission(HT_ADDR);
  Wire.write(0x00);
  for (int com=0; com < 8; com++) {
    Wire.write(frame[com]);
    Wire.write(0x00);
  }
  Wire.endTransmission();
}

void setText(int side, const char* text) {
  int d=0;
  for (int i=0; text[i] && d < 4; i++) {
    uint8_t s=segChar(text[i]);
    if (text[i+1] == '.') {
      s |= SEG_DP; 
      i++; 
    }
    frame[side*4 + d++] = s;
  }
  while (d<4) frame[side*4 + d++] = 0;
}

void formatTime(uint32_t ms, char* out) {
  uint32_t totalSec = ms / 1000;
  uint32_t h = totalSec / 3600;
  uint32_t m = (totalSec / 60) % 60;
  uint32_t s = totalSec % 60;
  uint32_t t = (ms/100) % 10;
  if (h>0) {
    sprintf(out, " %lu.%02lu", (unsigned long)(h%10), (unsigned long)m); // if theres hours, show hours and minutes
  } else if (totalSec >= 20) {
    if (m >= 10) sprintf(out, "%02lu.%02lu", (unsigned long)m, (unsigned long)s);
    else sprintf(out, " %lu.%02lu", (unsigned long)m, (unsigned long)s);
  } else {
    //time scramble :))))
    if (s>=10) sprintf(out, " %02lu.%lu", (unsigned long)s, (unsigned long)t);
    else sprintf(out, "  %lu.%lu", (unsigned long)s, (unsigned long)t);
  }
}
// buzzer >:D
bool soundOn = true;

void beep(uint16_t freq, uint16_t ms) {
  if (soundOn) tone(PIN_BUZZER, freq, ms);
}

//messages
char msgL[8], msgR[8];
uint32_t msgUntil = 0;

void showMsg(const char* l, const char* r, uint32_t ms) {
  strncpy(msgL, l, 7); msgL[7] = 0;
  strncpy(msgR, r, 7); msgR[7] = 0;
  msgUntil = millis() + ms;
}

//buttons!! :DD
#define DEBOUNCE_MS 25 
#define LONG_MS 1000



Button btnLeft = {PIN_BTN_LEFT};
Button btnRight = {PIN_BTN_RIGHT};
Button btnPause = {PIN_BTN_PAUSE};
Button btnMode = {PIN_BTN_MODE};
Button btnPlus = {PIN_BTN_PLUS};
Button btnMinus = {PIN_BTN_MINUS};

void btnInit(Button& b) {
  pinMode(b.pin, INPUT_PULLUP);
}

void btnUpdate(Button& b, uint32_t now) {
  bool r = (digitalRead(b.pin) == LOW);
  if (r != b.lastRead) {
    b.lastRead = r;
    b.lastChange = now;
  }
  if (now - b.lastChange >= DEBOUNCE_MS && r != b.stable) {
    b.stable = r;
    if (r) {
      b.pressEvent = true;  
      b.pressStart = now;
      b.longFired = false;
      lastActivity = now;
    }
  }
  if (b.stable && !b.longFired && now - b.pressStart >= LONG_MS) {
    b.longFired = true;
    b.longEvent = true;
  }
}

bool took(bool& e) {
  if (e) {
    e = false;
    return true;
  }
  return false;
}

// time controls

const Preset PRESETS[] = {
  {1, 0, INC_NONE}, // 1|0 bullet
  {1, 1, INC_FISCHER}, // 1|1 bullet
  {2, 1, INC_FISCHER}, // 2|1 bullet
  {3, 0, INC_NONE}, // 3min blitz
  {3, 2, INC_FISCHER}, // 3|2 blitz
  {5, 0, INC_NONE}, // 5min blitz
  {10, 0, INC_NONE}, // 10min rapid
  {15, 10, INC_FISCHER}, // 15|10 rapid
  {30, 0, INC_NONE}, // 30min rapid
  {90, 30, INC_FISCHER}, // 90|30 classical
};

const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

int presetIdx = 4; // 3min blitz default
Preset tc = PRESETS[4];
int field = F_PRESET;
Preferences prefs;

void saveSettings() {
  prefs.putInt("preset", presetIdx);
  prefs.putUShort("base", tc.baseMin);
  prefs.putUChar("inc", tc.incSec);
  prefs.putUChar("type", (uint8_t)tc.type);
  prefs.putBool("sound", soundOn);
}

void loadSettings() {
  presetIdx = prefs.getInt("preset", 4);
  tc.baseMin = prefs.getUShort("base", 3);
  tc.incSec = prefs.getUChar("inc", 2);
  tc.type = (IncType)prefs.getUChar("type", INC_FISCHER);
  soundOn = prefs.getBool("sound", true);
}

// clock stuffs

State state = READY;
int active = -1;
int flagged = -1;
uint32_t timeMs[2];
uint32_t turnUsed = 0; // ms used in this turn
uint16_t moves[2];
bool warned[2];
uint32_t lastTick = 0;
uint32_t lastPrint = 0;


const char* stateName[] = {"READY", "RUNNING", "PAUSED", "FLAGGED", "SETUP"};

void resetClock() {
  timeMs[0] = timeMs[1] = tc.baseMin * 60000UL;
  moves[0] = moves[1] = 0;
  warned[0] = warned[1] = false;
  turnUsed = 0;
  state = READY;
  active = -1;
  flagged = -1;
  Serial.println("RESET");
}

void endTurn(int side) { // stop my clock and start the opponent's clock
  if (state == READY || (state == RUNNING && active == side)) {
    if (state == RUNNING) {
      uint32_t inc = tc.incSec * 1000UL;
      if (tc.type == INC_FISCHER) timeMs[side] += inc;
      else if (tc.type == INC_BRONSTEIN) timeMs[side] += min(inc, turnUsed); // basically a ripoff of delay
      moves[side]++;
      if (timeMs[side] > 10000) warned[side] = false; // let the clock rewarn player if their time goes back up above 10s
    }
    active= 1-side;
    turnUsed = 0;
    state = RUNNING;
    beep(2200, 15);
    Serial.printf("%s pressed -> %s running\n", side==0 ? "LEFT" : "RIGHT", active == 0 ? "LEFT":"RIGHT");
  }
}

void tickClock(uint32_t now) {
  uint32_t dt = now - lastTick;
  lastTick = now;
  if (state != RUNNING) return;
  turnUsed += dt;
  if (tc.type == INC_DELAY) {
    uint32_t delayMs = tc.incSec * 1000UL;
    if (turnUsed <= delayMs) return; // doesnt lose time if turn is shorter than the delay
    uint32_t over = turnUsed - delayMs;
    if (over < dt) dt = over; // only subtract the part after the delay
  }
  if (timeMs[active] > dt) {
    timeMs[active] -= dt;
    if (!warned[active] && timeMs[active] <= 10000) {
      warned[active] = true; // 10s time warning
      beep(1500, 120);
    }
  } else {
    timeMs[active] = 0;
    flagged = active;
    state = FLAGGED;
    beep(800, 800);
    Serial.printf("%s FLAGGED \n", flagged==0 ? "LEFT":"RIGHT");
  }
}

void handleSetup() {
  int dir=0;
  if (took(btnPlus.pressEvent)) dir = 1;
  if (took(btnMinus.pressEvent)) dir = -1;
  if (dir != 0) {
    beep(2200, 10);
    if (field == F_PRESET) {
      presetIdx = (presetIdx + dir + NUM_PRESETS) % NUM_PRESETS;
      tc = PRESETS[presetIdx];
    } else if (field == F_BASE) {
      tc.baseMin = constrain((int)tc.baseMin + dir, 1, 180); //time range: 1-180min
    } else if (field == F_INC) {
      tc.incSec = constrain((int)tc.incSec + dir, 0, 60); // incrememnt range: 0-60s
    } else {
      tc.type = (IncType)(((int)tc.type + dir + 4) % 4);
    }
  }
  if (took(btnMode.pressEvent)) {
    field = (field+1) % 4;
    beep(1800, 20);
  }
  if (took(btnPause.pressEvent)) {
    saveSettings();
    resetClock();
    beep(2500, 60);
    Serial.printf("SET %u min +%u s type %d\n", tc.baseMin, tc.incSec, tc.type);
  }
  took(btnLeft.pressEvent);
  took(btnRight.pressEvent);
  took(btnPause.longEvent);
}

void handleButtons() {
  if (state == SETUP) { handleSetup(); return; }
  if (took(btnLeft.pressEvent)) endTurn(0);
  if (took(btnRight.pressEvent)) endTurn(1);
  if (took(btnPause.pressEvent)) {
    if (state == RUNNING) { state = PAUSED; Serial.println("PAUSED"); }
    else if (state == PAUSED) { state = RUNNING; Serial.println("RESUMED"); }
  }
  if (took(btnPause.longEvent)) resetClock(); // reset clock if longhold pause
  if (took(btnMode.pressEvent) && (state == READY || state == FLAGGED)) {
    state = SETUP;
    field = F_PRESET;
    beep(1800, 20);
  }
  if (took(btnMinus.longEvent) && state == READY) {
    soundOn = !soundOn;
    saveSettings();
    showMsg("SND ", soundOn ? " ON " : " OFF", 1000);
    beep(2200, 40);
  }
  took(btnPlus.pressEvent);
  took(btnMinus.pressEvent);
}

char typeChar(int t) {
  if (t == INC_FISCHER) return 'F';
  if (t == INC_BRONSTEIN) return 'B';
  if (t == INC_DELAY) return 'D';
  return '-';
}

void renderSetup(bool blinkOn) {
  char l[8], r[8];
  sprintf(l, "%4u", tc.baseMin);
  sprintf(r, "%c%3u", typeChar(tc.type), tc.incSec);
  if (!blinkOn) {
    if (field == F_PRESET) { strcpy(l, "    "); strcpy(r, "    "); }
    else if (field == F_BASE) strcpy(l, "    ");
    else if (field == F_INC) { r[1] = r[2] = r[3] = ' '; }
    else r[0] = ' ';
  }
  setText(0, l);
  setText(1, r);
}

void render(uint32_t now) {
  bool blinkOn = (now / 400) % 2;
  char buf[8]; 
  if (now < msgUntil) {
    setText(0, msgL);
    setText(1, msgR);
    htFlush();
    return;
  }
  if (state == SETUP) {
    renderSetup(blinkOn);
    htFlush();
    return;
  }
  for (int side=0; side<2; side++) {
    if (state == FLAGGED && side == flagged) {
      setText(side, blinkOn ? "FLAG" : "    ");
      continue;
    }
    if (state == PAUSED && !blinkOn) {
      sprintf(buf, "N%3u", moves[side]);
      setText(side, buf);
      continue;
    }
    formatTime(timeMs[side], buf);
    setText(side, buf);
    if (state == RUNNING && side == active) frame[side*4+3] |= SEG_DP; // indicator dot
  }
  htFlush();
}

// battery section 

#define VBAT_LOW_MV 3500
#define VBAT_CRIT_MV 3300
uint32_t lastBattCheck = 0;
uint32_t lastBattWarn = 0;

uint32_t readVbatMv() {
  uint32_t sum = 0;
  for (int i=0; i<8; i++) sum += analogReadMilliVolts(PIN_VBAT);
  return (sum/8)*2; // 220k/220k divider
}

#define IDLE_SLEEP_MS (5UL * 60 * 1000) // automatically sleep when 5 minutes of inactivity and not playing.
#define PAUSE_SLEEP_MS (30UL * 60 * 1000) // 30 minutes if the clock is paused

void goSleep() {
  Serial.println("SLEEP");
  memset(frame, 0, sizeof(frame));
  htFlush();
  htCmd(0x80);
  htCmd(0x20);
  gpio_pullup_en((gpio_num_t)PIN_BTN_LEFT);
  gpio_pullup_en((gpio_num_t)PIN_BTN_RIGHT);
  uint64_t mask = (1ULL << PIN_BTN_LEFT) | (1ULL << PIN_BTN_RIGHT); // wake up if either player button is clicked
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void checkPower(uint32_t now) {
  if (now - lastBattCheck >= 5000) {
    lastBattCheck = now;
    uint32_t mv = readVbatMv();
    if (mv > 1000 && mv < VBAT_CRIT_MV && state != RUNNING) {
      showMsg(" LO ", "BATT", 2000);
      uint32_t t0 = millis();
      while (millis() - t0 < 2000) { render(millis()); delay(20); }
      goSleep();
    }
    if (mv > 1000 && mv < VBAT_LOW_MV && state != RUNNING && now - lastBattWarn >= 60000) {
      lastBattWarn = now;
      showMsg("BATT", " LO ", 1500);
      beep(1000, 100);
    }
  }
  uint32_t idle = now - lastActivity;
  if ((state == READY || state == FLAGGED || state == SETUP) && idle >= IDLE_SLEEP_MS) goSleep();
  if (state == PAUSED && idle >= PAUSE_SLEEP_MS) goSleep();
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  delay(500);
  htInit();
  Serial.println(htFound ? "HT16K33 found" : "not found (test)");
  btnInit(btnLeft);
  btnInit(btnRight);
  btnInit(btnPause);
  btnInit(btnMode);
  btnInit(btnPlus);
  btnInit(btnMinus);
  pinMode(PIN_BUZZER, OUTPUT);
  prefs.begin("clock", false);
  loadSettings();
  setText(0, "HI  ");
  setText(1, "8.8.8.8.");
  htFlush();
  delay(1000);
  uint32_t mv = readVbatMv();
  char v[8];
  sprintf(v, "%lu.%02lu", (unsigned long)(mv/1000), (unsigned long)((mv % 1000)/ 10));
  setText(0, "BATT");
  setText(1, v);
  htFlush();
  Serial.printf("battery %lu mV\n", (unsigned long)mv);
  delay(1200);
  resetClock();
  lastTick = millis();
  lastActivity = millis();

}

void loop() {
  // put your main code here, to run repeatedly:
  uint32_t now = millis();
  btnUpdate(btnLeft, now);
  btnUpdate(btnRight, now);
  btnUpdate(btnPause, now);
  btnUpdate(btnMode, now);
  btnUpdate(btnPlus, now);
  btnUpdate(btnMinus, now);
  handleButtons();
  tickClock(now);
  render(now);
  checkPower(now);
  if (now - lastPrint >= 500) {
    lastPrint = now;
    char l[8], r[8];
    formatTime(timeMs[0], l);
    formatTime(timeMs[1], r);
    Serial.printf("%-8s L [%s]  R [%s]\n", stateName[state], l, r);
  }
  delay(5);
}
