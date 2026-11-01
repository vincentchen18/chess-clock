
#include <Wire.h>

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

void showTimes(uint32_t leftMs, uint32_t rightMs) {
  char l[8], r[8];
  formatTime(leftMs, l);
  formatTime(rightMs, r);
  setText(0, l);
  setText(1, r);
  htFlush();
}


// test

uint32_t leftMs = 25000;
uint32_t rightMs = 3605000;
uint32_t lastTick = 0;
uint32_t lastPrint = 0;


void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  delay(500);
  htInit();
  Serial.println(htFound ? "HT16K33 found" : "not found (test)");
  setText(0, "HI  ");
  setText(1, "8.8.8.8.");
  htFlush();
  delay(1500);
  lastTick = millis();

}

void loop() {
  // put your main code here, to run repeatedly:
  uint32_t now = millis();
  uint32_t dt = now - lastTick;
  lastTick = now;
  leftMs = (leftMs > dt) ? leftMs - dt : 0;
  rightMs = (rightMs > dt*20) ? rightMs - dt*20:0;
  showTimes(leftMs, rightMs);
  if (now - lastPrint >= 250) {
    lastPrint = now;
    char l[8], r[8];
    formatTime(leftMs, l);
    formatTime(rightMs, r);
    Serial.printf("L [%s]   R [%s]\n", l, r);
  }
  delay(20);
}
