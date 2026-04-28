#include <Arduino.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <EEPROM.h>

// ─── Constants ────────────────────────────────────────────────────────────────
#define MAX_FILES    10
#define NAME_LEN     12
#define CONTENT_LEN  32
#define PATH_LEN     16
#define DMESG_LINES   6
#define DMESG_LEN    40

// EEPROM layout
// [0]        = magic byte (0xAB = valid save)
// [1..N]     = packed RAMFile structs
#define EEPROM_MAGIC      0xAB
#define EEPROM_MAGIC_ADDR 0

// ─── Structs ──────────────────────────────────────────────────────────────────
typedef struct {
  char name[NAME_LEN];
  char content[CONTENT_LEN];
  char parentDir[PATH_LEN];
  int  isDirectory;
  int  active;
} RAMFile;

typedef struct {
  unsigned long timestamp;
  char message[DMESG_LEN];
} DmesgEntry;

// ─── Globals ──────────────────────────────────────────────────────────────────
RAMFile    fs[MAX_FILES];
char       currentPath[PATH_LEN] = "/";
char       inputBuffer[32]       = "";
int        inputLen               = 0;
DmesgEntry dmesg[DMESG_LINES];
int        dmesgIndex             = 0;

// ─── Forward declaration ──────────────────────────────────────────────────────
void runScript(const char* content);
void executeCommand(char* line);

// ─── Utilities ────────────────────────────────────────────────────────────────
int freeMemory() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

void(* resetFunc)(void) = 0;

void addDmesg(const __FlashStringHelper* msg) {
  if (dmesgIndex >= DMESG_LINES) dmesgIndex = 0;
  dmesg[dmesgIndex].timestamp = millis() / 1000;
  strncpy_P(dmesg[dmesgIndex].message, (PGM_P)msg, DMESG_LEN - 1);
  dmesg[dmesgIndex].message[DMESG_LEN - 1] = '\0';
  dmesgIndex++;
}

void addDmesgRam(const char* msg) {
  if (dmesgIndex >= DMESG_LINES) dmesgIndex = 0;
  dmesg[dmesgIndex].timestamp = millis() / 1000;
  strncpy(dmesg[dmesgIndex].message, msg, DMESG_LEN - 1);
  dmesg[dmesgIndex].message[DMESG_LEN - 1] = '\0';
  dmesgIndex++;
}

int indexOf(const char* str, const char* substr) {
  int i, j, slen = strlen(str), sublen = strlen(substr);
  for (i = 0; i <= slen - sublen; i++) {
    int match = 1;
    for (j = 0; j < sublen; j++) {
      if (str[i + j] != substr[j]) { match = 0; break; }
    }
    if (match) return i;
  }
  return -1;
}

int atoi_safe(const char* str) {
  int num = 0;
  while (*str >= '0' && *str <= '9') { num = num * 10 + (*str - '0'); str++; }
  return num;
}

void toLowercase(char* str) {
  int i;
  for (i = 0; str[i] != '\0'; i++)
    if (str[i] >= 'A' && str[i] <= 'Z') str[i] = str[i] - 'A' + 'a';
}

int safeConcatPath(char* dest, const char* add) {
  int destLen = strlen(dest);
  int addLen  = strlen(add);
  if (destLen + addLen + 2 >= PATH_LEN) return 0;
  strncat(dest, add,  PATH_LEN - destLen - 1);
  strncat(dest, "/",  PATH_LEN - strlen(dest) - 1);
  return 1;
}

// Resolve "A0"–"A5" string to actual Arduino analog pin number
// Returns -1 if not a valid analog pin name
int resolveAnalogPin(const char* str) {
  if ((str[0] == 'A' || str[0] == 'a') && str[1] >= '0' && str[1] <= '5' && str[2] == '\0') {
    const int analogPins[] = {A0, A1, A2, A3, A4, A5};
    return analogPins[str[1] - '0'];
  }
  return -1;
}

bool isPWMPin(int pin) {
  // UNO PWM-capable pins: 3, 5, 6, 9, 10, 11
  const int pwmPins[] = {3, 5, 6, 9, 10, 11};
  for (int k = 0; k < 6; k++) if (pin == pwmPins[k]) return true;
  return false;
}

// ─── EEPROM Persistence ───────────────────────────────────────────────────────
void saveFS() {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  int addr = 1;
  for (int i = 0; i < MAX_FILES; i++) {
    // Write each byte of the struct
    const byte* p = (const byte*)&fs[i];
    for (unsigned int b = 0; b < sizeof(RAMFile); b++) {
      EEPROM.write(addr++, p[b]);
    }
    // Guard: stop if we're near the 1KB limit
    if (addr + (int)sizeof(RAMFile) > 1024) break;
  }
  addDmesg(F("FS saved to EEPROM"));
  Serial.println(F("Filesystem saved to EEPROM."));
}

void loadFS() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC) {
    Serial.println(F("No EEPROM save found."));
    return;
  }
  int addr = 1;
  for (int i = 0; i < MAX_FILES; i++) {
    byte* p = (byte*)&fs[i];
    for (unsigned int b = 0; b < sizeof(RAMFile); b++) {
      p[b] = EEPROM.read(addr++);
    }
    if (addr + (int)sizeof(RAMFile) > 1024) break;
  }
  addDmesg(F("FS loaded from EEPROM"));
  Serial.println(F("Filesystem loaded from EEPROM."));
}

void clearEEPROM() {
  EEPROM.write(EEPROM_MAGIC_ADDR, 0x00);
  Serial.println(F("EEPROM cleared."));
  addDmesg(F("EEPROM cleared"));
}

// ─── Filesystem Init ──────────────────────────────────────────────────────────
void initFS() {
  int d, i;
  const char* dirs[] = {"home", "dev"};
  for (d = 0; d < 2; d++) {
    for (i = 0; i < MAX_FILES; i++) {
      if (!fs[i].active) {
        strncpy(fs[i].name, dirs[d], NAME_LEN - 1);
        fs[i].name[NAME_LEN - 1] = '\0';
        strncpy(fs[i].parentDir, "/", PATH_LEN - 1);
        fs[i].parentDir[PATH_LEN - 1] = '\0';
        fs[i].isDirectory = 1;
        fs[i].active      = 1;
        break;
      }
    }
  }

  char devPath[PATH_LEN] = "/dev/";
  const char* pins[] = {"pin2", "pin3", "pin4"};
  for (d = 0; d < 3; d++) {
    for (i = 0; i < MAX_FILES; i++) {
      if (!fs[i].active) {
        strncpy(fs[i].name, pins[d], NAME_LEN - 1);
        fs[i].name[NAME_LEN - 1] = '\0';
        strncpy(fs[i].parentDir, devPath, PATH_LEN - 1);
        fs[i].parentDir[PATH_LEN - 1] = '\0';
        fs[i].isDirectory  = 0;
        fs[i].content[0]   = '\0';
        fs[i].active        = 1;
        break;
      }
    }
  }

  addDmesg(F("Kernel initialized"));
  addDmesg(F("Filesystem mounted"));
  addDmesg(F("Ready for commands"));
}

// ─── Prompt ───────────────────────────────────────────────────────────────────
void printPrompt() {
  Serial.print(F("root@arduino:"));
  Serial.print(currentPath);
  Serial.print(F("# "));
}

// ─── Setup & Loop ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  initFS();
  delay(500);
  Serial.println(F("\n--- KernelUNO v2.0 ---"));
  Serial.println(F("Type 'help' for commands"));
  printPrompt();
}

void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (inputLen > 0) {
        inputBuffer[inputLen] = '\0';
        Serial.println();
        executeCommand(inputBuffer);
        inputLen = 0;
        memset(inputBuffer, 0, 32);
        printPrompt();
      } else {
        Serial.println();
        printPrompt();
      }
    }
    else if (c == 8 || c == 127) {
      if (inputLen > 0) {
        inputLen--;
        inputBuffer[inputLen] = '\0';
        Serial.print(F("\b \b"));
      }
    }
    else if (inputLen < 31) {
      Serial.print(c);
      inputBuffer[inputLen] = c;
      inputLen++;
    }
  }
}

// ─── Command Executor ─────────────────────────────────────────────────────────
void executeCommand(char* line) {
  char cmd[32]  = "";
  char args[32] = "";
  int  space1   = -1;
  int  i, sp, pin;
  char buf[40];

  strncpy(cmd, line, 31);
  cmd[31] = '\0';

  for (i = 0; cmd[i] != '\0'; i++) {
    if (cmd[i] == ' ') {
      space1 = i;
      strncpy(args, cmd + i + 1, 31);
      args[31] = '\0';
      cmd[i]   = '\0';
      break;
    }
  }

  toLowercase(cmd);

  // ── pinmode ────────────────────────────────────────────────────────────────
  if (strcmp_P(cmd, PSTR("pinmode")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) { Serial.println(F("Usage: pinmode [pin] [in/out]")); return; }
    pin = atoi_safe(args);
    char mode[8] = "";
    strncpy(mode, args + sp + 1, 7);
    mode[7] = '\0';
    toLowercase(mode);
    if (strcmp_P(mode, PSTR("out")) == 0) {
      pinMode(pin, OUTPUT);
      snprintf_P(buf, sizeof(buf), PSTR("Pin %d -> OUTPUT"), pin);
      addDmesgRam(buf);
      Serial.println(F("Pin set to OUTPUT"));
    } else if (strcmp_P(mode, PSTR("in")) == 0) {
      pinMode(pin, INPUT_PULLUP);
      snprintf_P(buf, sizeof(buf), PSTR("Pin %d -> INPUT_PULLUP"), pin);
      addDmesgRam(buf);
      Serial.println(F("Pin set to INPUT_PULLUP"));
    } else {
      Serial.println(F("Mode must be 'in' or 'out'"));
    }
  }

  // ── write ──────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("write")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) { Serial.println(F("Usage: write [pin] [high/low]")); return; }
    pin = atoi_safe(args);
    char val[8] = "";
    strncpy(val, args + sp + 1, 7);
    val[7] = '\0';
    toLowercase(val);
    bool isHigh = (strcmp_P(val, PSTR("high")) == 0);
    digitalWrite(pin, isHigh ? HIGH : LOW);
    snprintf_P(buf, sizeof(buf), PSTR("Pin %d -> %s"), pin, isHigh ? "HIGH" : "LOW");
    addDmesgRam(buf);
    Serial.println(F("Write OK."));
  }

  // ── read ───────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("read")) == 0) {
    pin = atoi_safe(args);
    int value = digitalRead(pin);
    Serial.print(F("Pin ")); Serial.print(pin);
    Serial.print(F(" = ")); Serial.println(value);
    snprintf_P(buf, sizeof(buf), PSTR("Pin %d read: %d"), pin, value);
    addDmesgRam(buf);
  }

  // ── NEW: aread [A0-A5] ─────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("aread")) == 0) {
    if (args[0] == '\0') { Serial.println(F("Usage: aread [A0-A5]")); return; }
    int apin = resolveAnalogPin(args);
    if (apin == -1) { Serial.println(F("Invalid pin. Use A0-A5.")); return; }
    int aval = analogRead(apin);
    float voltage = aval * (5.0 / 1023.0);
    Serial.print(args);
    Serial.print(F(" = "));
    Serial.print(aval);
    Serial.print(F(" ("));
    // Print voltage as X.XX manually (no float printf on AVR)
    int vInt  = (int)voltage;
    int vFrac = (int)((voltage - vInt) * 100);
    Serial.print(vInt); Serial.print(F(".")); 
    if (vFrac < 10) Serial.print(F("0"));
    Serial.print(vFrac);
    Serial.println(F("V)"));
    snprintf_P(buf, sizeof(buf), PSTR("aread %s: %d"), args, aval);
    addDmesgRam(buf);
  }

  // ── gpio ───────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("gpio")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) { Serial.println(F("Usage: gpio [pin] [on/off/toggle] | gpio vixa [n]")); return; }
    char pinStr[8]  = "";
    char action[8]  = "";
    strncpy(pinStr, args, sp);
    pinStr[sp] = '\0';
    strncpy(action, args + sp + 1, 7);
    action[7] = '\0';
    toLowercase(action);

    if (strcmp_P(pinStr, PSTR("vixa")) == 0) {
      int count = atoi_safe(action);
      if (count <= 0) count = 10;
      addDmesg(F("LED disco mode"));
      Serial.println(F("LED DISCO MODE!"));
      for (int cycle = 0; cycle < count; cycle++) {
        for (int p = 2; p <= 13; p++) {
          pinMode(p, OUTPUT);
          digitalWrite(p, HIGH); delay(50);
          digitalWrite(p, LOW);
        }
      }
      Serial.println(F("Disco finished!"));
      addDmesg(F("Disco complete"));
    } else {
      pin = atoi_safe(pinStr);
      if (strcmp_P(action, PSTR("on")) == 0) {
        pinMode(pin, OUTPUT); digitalWrite(pin, HIGH);
        snprintf_P(buf, sizeof(buf), PSTR("GPIO %d ON"), pin);
        addDmesgRam(buf);
        Serial.print(F("GPIO ")); Serial.print(pin); Serial.println(F(" ON"));
      } else if (strcmp_P(action, PSTR("off")) == 0) {
        pinMode(pin, OUTPUT); digitalWrite(pin, LOW);
        snprintf_P(buf, sizeof(buf), PSTR("GPIO %d OFF"), pin);
        addDmesgRam(buf);
        Serial.print(F("GPIO ")); Serial.print(pin); Serial.println(F(" OFF"));
      } else if (strcmp_P(action, PSTR("toggle")) == 0) {
        pinMode(pin, OUTPUT); digitalWrite(pin, !digitalRead(pin));
        snprintf_P(buf, sizeof(buf), PSTR("GPIO %d toggled"), pin);
        addDmesgRam(buf);
        Serial.print(F("GPIO ")); Serial.print(pin); Serial.println(F(" toggled"));
      } else {
        Serial.println(F("Action: on/off/toggle"));
      }
    }
  }

  // ── pwm ───────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("pwm")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) { Serial.println(F("Usage: pwm [pin] [0-255]")); return; }
    pin = atoi_safe(args);
    if (!isPWMPin(pin)) { Serial.println(F("Not a PWM pin. Use 3,5,6,9,10,11")); return; }
    char valStr[8] = "";
    strncpy(valStr, args + sp + 1, 7);
    valStr[7] = '\0';
    int pwmVal = atoi_safe(valStr);
    if (pwmVal < 0)   pwmVal = 0;
    if (pwmVal > 255) pwmVal = 255;
    pinMode(pin, OUTPUT);
    analogWrite(pin, pwmVal);
    snprintf_P(buf, sizeof(buf), PSTR("PWM pin %d = %d"), pin, pwmVal);
    addDmesgRam(buf);
    Serial.print(F("PWM pin ")); Serial.print(pin);
    Serial.print(F(" = ")); Serial.println(pwmVal);
  }

  // ── NEW: tone [pin] [freq] ─────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("tone")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) { Serial.println(F("Usage: tone [pin] [freq_Hz] OR tone [pin] [freq] [dur_ms]")); return; }
    pin = atoi_safe(args);
    char rest[24] = "";
    strncpy(rest, args + sp + 1, 23);
    rest[23] = '\0';
    int sp2 = indexOf(rest, " ");
    unsigned int freq;
    unsigned long dur = 0;
    if (sp2 != -1) {
      char freqStr[12] = "";
      char durStr[12]  = "";
      strncpy(freqStr, rest, sp2);
      freqStr[sp2] = '\0';
      strncpy(durStr, rest + sp2 + 1, 11);
      freq = (unsigned int)atoi_safe(freqStr);
      dur  = (unsigned long)atoi_safe(durStr);
    } else {
      freq = (unsigned int)atoi_safe(rest);
    }
    if (freq == 0) { Serial.println(F("Freq must be > 0 Hz")); return; }
    if (dur > 0) {
      tone(pin, freq, dur);
      Serial.print(F("Tone pin ")); Serial.print(pin);
      Serial.print(F(" freq ")); Serial.print(freq);
      Serial.print(F("Hz dur ")); Serial.print(dur); Serial.println(F("ms"));
    } else {
      tone(pin, freq);
      Serial.print(F("Tone pin ")); Serial.print(pin);
      Serial.print(F(" freq ")); Serial.print(freq); Serial.println(F("Hz (continuous)"));
    }
    snprintf_P(buf, sizeof(buf), PSTR("tone pin%d %uHz"), pin, freq);
    addDmesgRam(buf);
  }

  // ── NEW: notone [pin] ──────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("notone")) == 0) {
    pin = atoi_safe(args);
    noTone(pin);
    Serial.print(F("Tone stopped on pin ")); Serial.println(pin);
  }

  // ── NEW: delay [ms] ───────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("delay")) == 0) {
    if (args[0] == '\0') { Serial.println(F("Usage: delay [ms]")); return; }
    unsigned long ms = (unsigned long)atoi_safe(args);
    if (ms > 30000) { Serial.println(F("Max delay is 30000ms")); return; }
    Serial.print(F("Waiting ")); Serial.print(ms); Serial.println(F("ms..."));
    delay(ms);
    Serial.println(F("Done."));
  }

  // ── ls ────────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("ls")) == 0) {
    int empty = 1, j;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && strcmp(fs[j].parentDir, currentPath) == 0) {
        Serial.print(fs[j].name);
        if (fs[j].isDirectory) Serial.print(F("/"));
        Serial.print(F("  "));
        empty = 0;
      }
    }
    if (empty) Serial.print(F("(empty)"));
    Serial.println();
  }

  // ── mkdir / touch ─────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("mkdir")) == 0 || strcmp_P(cmd, PSTR("touch")) == 0) {
    if (args[0] == '\0') { Serial.println(F("Usage: mkdir/touch [name]")); return; }
    int foundSlot = -1, j;
    // Check for duplicate
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && strcmp(fs[j].name, args) == 0 && strcmp(fs[j].parentDir, currentPath) == 0) {
        Serial.println(F("Already exists.")); return;
      }
    }
    for (j = 0; j < MAX_FILES; j++) { if (!fs[j].active) { foundSlot = j; break; } }
    if (foundSlot == -1) { Serial.println(F("No space.")); return; }
    strncpy(fs[foundSlot].name, args, NAME_LEN - 1);
    fs[foundSlot].name[NAME_LEN - 1] = '\0';
    strncpy(fs[foundSlot].parentDir, currentPath, PATH_LEN - 1);
    fs[foundSlot].parentDir[PATH_LEN - 1] = '\0';
    fs[foundSlot].isDirectory = (strcmp_P(cmd, PSTR("mkdir")) == 0);
    fs[foundSlot].content[0]  = '\0';
    fs[foundSlot].active       = 1;
    Serial.println(F("OK."));
  }

  // ── cd ────────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("cd")) == 0) {
    if (strcmp_P(args, PSTR("/")) == 0) {
      strncpy(currentPath, "/", PATH_LEN - 1);
    } else if (strcmp_P(args, PSTR("..")) == 0) {
      // Proper cd .. — strip last path segment
      if (strcmp(currentPath, "/") != 0) {
        int len = strlen(currentPath);
        // Remove trailing slash
        if (len > 1 && currentPath[len - 1] == '/') currentPath[--len] = '\0';
        // Find and cut at the last slash
        char* lastSlash = strrchr(currentPath, '/');
        if (lastSlash != NULL) *(lastSlash + 1) = '\0';
      }
    } else {
      int j, found = 0;
      for (j = 0; j < MAX_FILES; j++) {
        if (fs[j].active && fs[j].isDirectory &&
            strcmp(args, fs[j].name) == 0 &&
            strcmp(fs[j].parentDir, currentPath) == 0) {
          if (!safeConcatPath(currentPath, fs[j].name)) {
            strncpy(currentPath, "/", PATH_LEN - 1);
            Serial.println(F("Path too long."));
            return;
          }
          found = 1;
          break;
        }
      }
      if (!found) Serial.println(F("No such directory."));
    }
  }

  // ── pwd ───────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("pwd")) == 0) {
    Serial.println(currentPath);
  }

  // ── echo (with > and >> support) ──────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("echo")) == 0) {
    // Check for append ">>"
    int appendArrow = indexOf(args, " >> ");
    int overArrow   = indexOf(args, " > ");

    if (appendArrow != -1) {
      // ── APPEND mode ────────────────────────────────────────────────────────
      char text[40]     = "";
      char filename[12] = "";
      strncpy(text, args, appendArrow);
      text[appendArrow] = '\0';
      strncpy(filename, args + appendArrow + 4, NAME_LEN - 1);
      filename[NAME_LEN - 1] = '\0';

      int j, found = 0;
      for (j = 0; j < MAX_FILES; j++) {
        if (fs[j].active && !fs[j].isDirectory &&
            strcmp(filename, fs[j].name) == 0 &&
            strcmp(fs[j].parentDir, currentPath) == 0) {
          int existLen  = strlen(fs[j].content);
          int appendLen = strlen(text);
          if (existLen + appendLen + 1 >= CONTENT_LEN) {
            Serial.println(F("File full. Cannot append."));
          } else {
            strncat(fs[j].content, text, CONTENT_LEN - existLen - 1);
            Serial.println(F("Appended."));
          }
          found = 1;
          break;
        }
      }
      if (!found) Serial.println(F("File not found. Use 'touch' first."));

    } else if (overArrow != -1) {
      // ── OVERWRITE mode ─────────────────────────────────────────────────────
      char text[40]     = "";
      char filename[12] = "";
      strncpy(text, args, overArrow);
      text[overArrow] = '\0';
      strncpy(filename, args + overArrow + 3, NAME_LEN - 1);
      filename[NAME_LEN - 1] = '\0';

      int j, found = 0;
      for (j = 0; j < MAX_FILES; j++) {
        if (fs[j].active && !fs[j].isDirectory &&
            strcmp(filename, fs[j].name) == 0 &&
            strcmp(fs[j].parentDir, currentPath) == 0) {
          strncpy(fs[j].content, text, CONTENT_LEN - 1);
          fs[j].content[CONTENT_LEN - 1] = '\0';
          Serial.println(F("Saved."));
          // devfs: if /dev/pinX, apply to GPIO
          if (strcmp_P(fs[j].parentDir, PSTR("/dev/")) == 0 && strncmp_P(fs[j].name, PSTR("pin"), 3) == 0) {
            int devPin = atoi_safe(fs[j].name + 3);
            if (devPin > 0) {
              pinMode(devPin, OUTPUT);
              digitalWrite(devPin, (text[0] == '1') ? HIGH : LOW);
              snprintf_P(buf, sizeof(buf), PSTR("GPIO %d %s via echo"), devPin, (text[0] == '1') ? "HIGH" : "LOW");
              addDmesgRam(buf);
            }
          }
          found = 1;
          break;
        }
      }
      if (!found) Serial.println(F("File not found. Use 'touch' first."));

    } else {
      // ── plain echo ─────────────────────────────────────────────────────────
      Serial.println(args);
    }
  }

  // ── cat ───────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("cat")) == 0) {
    int j, found = 0;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && !fs[j].isDirectory &&
          strcmp(args, fs[j].name) == 0 &&
          strcmp(fs[j].parentDir, currentPath) == 0) {
        Serial.println(fs[j].content);
        found = 1;
        break;
      }
    }
    if (!found) Serial.println(F("File not found."));
  }

  // ── NEW: cp [src] [dst] ───────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("cp")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) { Serial.println(F("Usage: cp [src] [dst]")); return; }
    char src[NAME_LEN] = "";
    char dst[NAME_LEN] = "";
    strncpy(src, args, sp);
    src[sp] = '\0';
    strncpy(dst, args + sp + 1, NAME_LEN - 1);
    dst[NAME_LEN - 1] = '\0';

    // Find source
    int srcIdx = -1, j;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && !fs[j].isDirectory &&
          strcmp(src, fs[j].name) == 0 &&
          strcmp(fs[j].parentDir, currentPath) == 0) {
        srcIdx = j; break;
      }
    }
    if (srcIdx == -1) { Serial.println(F("Source not found.")); return; }

    // Find or create destination
    int dstIdx = -1;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && !fs[j].isDirectory &&
          strcmp(dst, fs[j].name) == 0 &&
          strcmp(fs[j].parentDir, currentPath) == 0) {
        dstIdx = j; break;
      }
    }
    if (dstIdx == -1) {
      // Create new file
      for (j = 0; j < MAX_FILES; j++) { if (!fs[j].active) { dstIdx = j; break; } }
      if (dstIdx == -1) { Serial.println(F("No space for destination.")); return; }
      strncpy(fs[dstIdx].name, dst, NAME_LEN - 1);
      fs[dstIdx].name[NAME_LEN - 1] = '\0';
      strncpy(fs[dstIdx].parentDir, currentPath, PATH_LEN - 1);
      fs[dstIdx].parentDir[PATH_LEN - 1] = '\0';
      fs[dstIdx].isDirectory = 0;
      fs[dstIdx].active       = 1;
    }
    strncpy(fs[dstIdx].content, fs[srcIdx].content, CONTENT_LEN - 1);
    fs[dstIdx].content[CONTENT_LEN - 1] = '\0';
    Serial.print(src); Serial.print(F(" -> ")); Serial.println(dst);
    snprintf_P(buf, sizeof(buf), PSTR("cp %s->%s"), src, dst);
    addDmesgRam(buf);
  }

  // ── info ──────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("info")) == 0) {
    int j, found = 0;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && strcmp(args, fs[j].name) == 0 && strcmp(fs[j].parentDir, currentPath) == 0) {
        Serial.print(F("Name: "));    Serial.println(fs[j].name);
        Serial.print(F("Type: "));    Serial.println(fs[j].isDirectory ? F("Directory") : F("File"));
        Serial.print(F("Parent: "));  Serial.println(fs[j].parentDir);
        Serial.print(F("Size: "));    Serial.print(strlen(fs[j].content)); Serial.println(F(" bytes"));
        found = 1;
        break;
      }
    }
    if (!found) Serial.println(F("Not found."));
  }

  // ── rm ────────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("rm")) == 0) {
    int j, found = 0;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && strcmp(args, fs[j].name) == 0 && strcmp(fs[j].parentDir, currentPath) == 0) {
        if (fs[j].isDirectory) {
          char dirPath[PATH_LEN];
          snprintf_P(dirPath, PATH_LEN, PSTR("%s%s/"), currentPath, args);
          for (int k = 0; k < MAX_FILES; k++) {
            if (fs[k].active && strncmp(fs[k].parentDir, dirPath, strlen(dirPath)) == 0)
              fs[k].active = 0;
          }
        }
        fs[j].active = 0;
        Serial.println(F("Removed."));
        found = 1;
        break;
      }
    }
    if (!found) Serial.println(F("Not found."));
  }

  // ── dmesg ─────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("dmesg")) == 0) {
    Serial.println(F("=== KERNEL LOG ==="));
    // Print in chronological order
    for (int j = dmesgIndex; j < DMESG_LINES; j++) {
      if (dmesg[j].message[0] != '\0') {
        Serial.print(F("[")); Serial.print(dmesg[j].timestamp); Serial.print(F("] "));
        Serial.println(dmesg[j].message);
      }
    }
    for (int j = 0; j < dmesgIndex; j++) {
      if (dmesg[j].message[0] != '\0') {
        Serial.print(F("[")); Serial.print(dmesg[j].timestamp); Serial.print(F("] "));
        Serial.println(dmesg[j].message);
      }
    }
  }

  // ── uptime ────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("uptime")) == 0) {
    unsigned long s   = millis() / 1000;
    unsigned long h   = s / 3600;
    unsigned long m   = (s % 3600) / 60;
    unsigned long sec = s % 60;
    Serial.print(F("up ")); Serial.print(h); Serial.print(F("h "));
    Serial.print(m); Serial.print(F("m ")); Serial.print(sec); Serial.println(F("s"));
    addDmesg(F("uptime checked"));
  }

  // ── df / free ─────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("df")) == 0 || strcmp_P(cmd, PSTR("free")) == 0) {
    Serial.print(F("Free RAM: ")); Serial.print(freeMemory()); Serial.println(F(" bytes"));
    int usedSlots = 0;
    for (int j = 0; j < MAX_FILES; j++) if (fs[j].active) usedSlots++;
    Serial.print(F("FS slots: ")); Serial.print(usedSlots); Serial.print(F("/")); Serial.println(MAX_FILES);
  }

  // ── NEW: eeprom save / load / clear ───────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("eeprom")) == 0) {
    toLowercase(args);
    if (strcmp_P(args, PSTR("save")) == 0) {
      saveFS();
    } else if (strcmp_P(args, PSTR("load")) == 0) {
      loadFS();
    } else if (strcmp_P(args, PSTR("clear")) == 0) {
      clearEEPROM();
    } else {
      Serial.println(F("Usage: eeprom [save|load|clear]"));
    }
  }

  // ── whoami ────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("whoami")) == 0) {
    Serial.println(F("root"));
  }

  // ── uname ─────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("uname")) == 0) {
    Serial.println(F("KernelUNO v2.0"));
    Serial.println(F("Kernel: Arduino AVR"));
    Serial.println(F("Hardware: Arduino UNO"));
    Serial.print(F("RAM free: ")); Serial.print(freeMemory()); Serial.println(F(" bytes"));
    Serial.print(F("EEPROM: ")); Serial.print(EEPROM.length()); Serial.println(F(" bytes"));
  }

  // ── reboot ────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("reboot")) == 0) {
    Serial.println(F("Rebooting..."));
    addDmesg(F("System reboot"));
    delay(500);
    resetFunc();
  }

  // ── clear ─────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("clear")) == 0) {
    for (int j = 0; j < 40; j++) Serial.println();
  }

  // ── sh ────────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("sh")) == 0) {
    if (args[0] == '\0') { Serial.println(F("Usage: sh [script]")); return; }
    int j, found = 0;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && !fs[j].isDirectory &&
          strcmp(args, fs[j].name) == 0 &&
          strcmp(fs[j].parentDir, currentPath) == 0) {
        found = 1;
        addDmesg(F("sh: running script"));
        runScript(fs[j].content);
        break;
      }
    }
    if (!found) Serial.println(F("Script not found."));
  }

  // ── help ──────────────────────────────────────────────────────────────────
  else if (strcmp_P(cmd, PSTR("help")) == 0) {
    Serial.println(F("=== KernelUNO v2.0 Commands ==="));
    Serial.println(F("--- Filesystem ---"));
    Serial.println(F("  ls  pwd  cd [dir]  mkdir [d]  touch [f]"));
    Serial.println(F("  cat [f]  rm [f/d]  info [f]"));
    Serial.println(F("  echo [txt] > [f]   (overwrite)"));
    Serial.println(F("  echo [txt] >> [f]  (append)"));
    Serial.println(F("  cp [src] [dst]"));
    Serial.println(F("--- GPIO ---"));
    Serial.println(F("  pinmode [pin] in/out"));
    Serial.println(F("  write [pin] high/low"));
    Serial.println(F("  read [pin]"));
    Serial.println(F("  aread [A0-A5]          <- analog read"));
    Serial.println(F("  gpio [pin] on/off/toggle | gpio vixa [n]"));
    Serial.println(F("  pwm [pin] [0-255]      (pins 3,5,6,9,10,11)"));
    Serial.println(F("  tone [pin] [Hz] [ms]   <- buzzer"));
    Serial.println(F("  notone [pin]"));
    Serial.println(F("--- System ---"));
    Serial.println(F("  uptime  uname  whoami  free  df  dmesg"));
    Serial.println(F("  delay [ms]             <- up to 30000"));
    Serial.println(F("  eeprom save|load|clear <- persistence"));
    Serial.println(F("  sh [file]  reboot  clear"));
  }

  // ── unknown ───────────────────────────────────────────────────────────────
  else {
    Serial.print(F("Unknown command: ")); Serial.println(cmd);
    Serial.println(F("Type 'help' for commands."));
  }
}

// ─── Script Runner ────────────────────────────────────────────────────────────
void runScript(const char* content) {
  char line[32];
  int  ci = 0, li = 0, lineNum = 0;
  int  len = strlen(content);

  while (ci <= len) {
    char c = (ci < len) ? content[ci] : ';';
    ci++;
    if (c == ';' || c == '\n' || c == '\r') {
      if (li > 0) {
        line[li] = '\0';
        lineNum++;
        Serial.print(F("[sh:")); Serial.print(lineNum); Serial.print(F("] "));
        Serial.println(line);
        executeCommand(line);
        li = 0;
      }
    } else {
      if (li < 31) line[li++] = c;
    }
  }
  addDmesg(F("sh: script done"));
  Serial.println(F("[sh] done."));
}
