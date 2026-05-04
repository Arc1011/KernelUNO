#include <Arduino.h>
#include <string.h>
#include <avr/pgmspace.h>

#define MAX_FILES 10         
#define NAME_LEN 12         
#define CONTENT_LEN 32      
#define PATH_LEN 16         
#define DMESG_LINES 6
#define DMESG_LEN 40
#define NO_SOFT_RESET 0 // Set NO_SOFT_RESET to 1 for boards that encounter errors when running the reboot command

typedef struct {
  char name[NAME_LEN];
  char content[CONTENT_LEN];
  char parentDir[PATH_LEN];
  int isDirectory;
  int active;
} RAMFile;

typedef struct {
  unsigned long timestamp;
  char message[DMESG_LEN];
} DmesgEntry;

RAMFile fs[MAX_FILES];
char currentPath[PATH_LEN] = "/";
char inputBuffer[32] = "";
int inputLen = 0;
DmesgEntry dmesg[DMESG_LINES];
int dmesgIndex = 0;

#define MAX_ALIASES 4
#define ALIAS_NAME_LEN 6
#define ALIAS_VAL_LEN 20
typedef struct {
  char name[ALIAS_NAME_LEN];
  char value[ALIAS_VAL_LEN];
  int active;
} AliasEntry;
AliasEntry aliases[MAX_ALIASES];

int freeMemory() {
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}

#if defined(__AVR__) && NO_SOFT_RESET == 0
void (*resetFunc)(void) = 0;

#elif defined(__arm__) && NO_SOFT_RESET == 0
void resetFunc() {
  NVIC_SystemReset();
}

#elif NO_SOFT_RESET != 0
void resetFunc() {
  Serial.println(F("This build of KernelUNO has been configured to disable software resets."));
}

#else
void resetFunc() {
  Serial.println(F("KernelUNO doesn't support software resets on this hardware."));
}

#endif

// OPT from issue
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
        fs[i].active = 1;
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
        fs[i].isDirectory = 0;
        fs[i].content[0] = '\0';
        fs[i].active = 1;
        break;
      }
    }
  }

  // OPT from isse
  addDmesg(F("Kernel initialized"));
  addDmesg(F("Filesystem mounted"));
  addDmesg(F("Ready for commands"));
}

void printPrompt() {
  Serial.print(F("root@arduino:"));
  Serial.print(currentPath);
  Serial.print(F("# "));
}

void setup() {
  Serial.begin(115200);
  initFS();
  delay(1000);
  Serial.println(F("\n--- KernelUNO v1.0 ---"));
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
  while (*str >= '0' && *str <= '9') {
    num = num * 10 + (*str - '0');
    str++;
  }
  return num;
}

void toLowercase(char* str) {
  int i;
  for (i = 0; str[i] != '\0'; i++) {
    if (str[i] >= 'A' && str[i] <= 'Z') str[i] = str[i] - 'A' + 'a';
  }
}

int safeConcatPath(char* dest, const char* add) {
  int destLen = strlen(dest);
  int addLen = strlen(add);
  if (destLen + addLen + 2 >= PATH_LEN) return 0;
  strncat(dest, add, PATH_LEN - destLen - 1);
  strncat(dest, "/", PATH_LEN - strlen(dest) - 1);
  return 1;
}

void runScript(const char* content);

void executeCommand(char* line) {
  char cmd[32] = "";
  char args[32] = "";
  int space1 = -1;
  int i, sp, pin, count;
  char buf[40];

  strncpy(cmd, line, 31);
  cmd[31] = '\0';

  for (i = 0; cmd[i] != '\0'; i++) {
    if (cmd[i] == ' ') {
      space1 = i;
      strncpy(args, cmd + i + 1, 31);
      args[31] = '\0';
      cmd[i] = '\0';
      break;
    }
  }

  toLowercase(cmd);

  // OPT
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
      snprintf_P(buf, sizeof(buf), PSTR("Pin %d set to OUTPUT"), pin);
      addDmesgRam(buf);
      Serial.println(F("Pin set to OUTPUT"));
    }
    else if (strcmp_P(mode, PSTR("in")) == 0) {
      pinMode(pin, INPUT_PULLUP);
      snprintf_P(buf, sizeof(buf), PSTR("Pin %d set to INPUT"), pin);
      addDmesgRam(buf);
      Serial.println(F("Pin set to INPUT_PULLUP"));
    }
  }
  else if (strcmp_P(cmd, PSTR("write")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) { Serial.println(F("Usage: write [pin] [high/low]")); return; }
    pin = atoi_safe(args);
    char val[8] = "";
    strncpy(val, args + sp + 1, 7);
    val[7] = '\0';
    toLowercase(val);
    digitalWrite(pin, (strcmp_P(val, PSTR("high")) == 0 ? HIGH : LOW));
    snprintf_P(buf, sizeof(buf), PSTR("Pin %d wrote %s"), pin, strcmp_P(val, PSTR("high")) == 0 ? "HIGH" : "LOW");
    addDmesgRam(buf);
    Serial.println(F("Write OK."));
  }
  else if (strcmp_P(cmd, PSTR("read")) == 0) {
    pin = atoi_safe(args);
    int value = digitalRead(pin);
    Serial.print(F("Pin ")); Serial.print(pin);
    Serial.print(F(" value: ")); Serial.println(value);
    snprintf_P(buf, sizeof(buf), PSTR("Pin %d read: %d"), pin, value);
    addDmesgRam(buf);
  }
  else if (strcmp_P(cmd, PSTR("gpio")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) {
      Serial.println(F("Usage: gpio [pin] [on/off] OR gpio vixa [count]"));
      return;
    }
    char pinStr[8] = "";
    strncpy(pinStr, args, sp);
    pinStr[sp] = '\0';
    char action[8] = "";
    strncpy(action, args + sp + 1, 7);
    action[7] = '\0';
    toLowercase(action);

    if (strcmp_P(pinStr, PSTR("vixa")) == 0) {
      count = atoi_safe(action);
      if (count <= 0) count = 10;
      addDmesg(F("LED disco mode activated"));
      Serial.println(F("LED DISCO MODE!"));
      int cycle, p;
      for (cycle = 0; cycle < count; cycle++) {
        for (p = 2; p <= 13; p++) {
          pinMode(p, OUTPUT);
          digitalWrite(p, HIGH);
          delay(50);
          digitalWrite(p, LOW);
        }
      }
      Serial.println(F("Disco finished!"));
      addDmesg(F("Disco complete"));
    } else {
      pin = atoi_safe(pinStr);
      if (strcmp_P(action, PSTR("on")) == 0) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);
        snprintf_P(buf, sizeof(buf), PSTR("GPIO %d ON"), pin);
        addDmesgRam(buf);
        Serial.print(F("GPIO ")); Serial.print(pin); Serial.println(F(" ON"));
      }
      else if (strcmp_P(action, PSTR("off")) == 0) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        snprintf_P(buf, sizeof(buf), PSTR("GPIO %d OFF"), pin);
        addDmesgRam(buf);
        Serial.print(F("GPIO ")); Serial.print(pin); Serial.println(F(" OFF"));
      }
      else if (strcmp_P(action, PSTR("toggle")) == 0) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, !digitalRead(pin));
        snprintf_P(buf, sizeof(buf), PSTR("GPIO %d toggled"), pin);
        addDmesgRam(buf);
        Serial.print(F("GPIO ")); Serial.print(pin); Serial.println(F(" toggled"));
      }
    }
  }
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
  else if (strcmp_P(cmd, PSTR("mkdir")) == 0 || strcmp_P(cmd, PSTR("touch")) == 0) {
    int foundSlot = -1, j;
    for (j = 0; j < MAX_FILES; j++) {
      if (!fs[j].active) { foundSlot = j; break; }
    }
    if (foundSlot == -1) { Serial.println(F("No space.")); return; }
    strncpy(fs[foundSlot].name, args, NAME_LEN - 1);
    fs[foundSlot].name[NAME_LEN - 1] = '\0';
    strncpy(fs[foundSlot].parentDir, currentPath, PATH_LEN - 1);
    fs[foundSlot].parentDir[PATH_LEN - 1] = '\0';
    fs[foundSlot].isDirectory = (strcmp_P(cmd, PSTR("mkdir")) == 0);
    fs[foundSlot].content[0] = '\0';
    fs[foundSlot].active = 1;
    Serial.println(F("OK."));
  }
  else if (strcmp_P(cmd, PSTR("cd")) == 0) {
    if (strcmp_P(args, PSTR("..")) == 0 || strcmp_P(args, PSTR("/")) == 0) {
      strncpy(currentPath, "/", PATH_LEN - 1);
      currentPath[PATH_LEN - 1] = '\0';
    } else {
      int j, found = 0;
      for (j = 0; j < MAX_FILES; j++) {
        if (fs[j].active && fs[j].isDirectory &&
            strcmp(args, fs[j].name) == 0 &&
            strcmp(fs[j].parentDir, currentPath) == 0) {
          if (!safeConcatPath(currentPath, fs[j].name)) {
            strncpy(currentPath, "/", PATH_LEN - 1);
            currentPath[PATH_LEN - 1] = '\0';
            Serial.println(F("Path too long."));
            return;
          }
          found = 1;
          break;
        }
      }
      if (!found) Serial.println(F("No dir."));
    }
  }
  else if (strcmp_P(cmd, PSTR("pwd")) == 0) {
    Serial.println(currentPath);
  }
  else if (strcmp_P(cmd, PSTR("echo")) == 0) {
    int arrow = indexOf(args, " > ");
    if (arrow != -1) {
      char text[40] = "";
      strncpy(text, args, arrow);
      text[arrow] = '\0';
      char filename[12] = "";
      strncpy(filename, args + arrow + 3, NAME_LEN - 1);
      filename[NAME_LEN - 1] = '\0';
      int j, found = 0;
      for (j = 0; j < MAX_FILES; j++) {
        if (fs[j].active && !fs[j].isDirectory &&
            strcmp(filename, fs[j].name) == 0 &&
            strcmp(fs[j].parentDir, currentPath) == 0) {
          strncpy(fs[j].content, text, CONTENT_LEN - 1);
          fs[j].content[CONTENT_LEN - 1] = '\0';
          Serial.println(F("Saved."));
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
      if (!found) Serial.println(F("File not found."));
    } else {
      Serial.println(args);
    }
  }
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
  else if (strcmp_P(cmd, PSTR("info")) == 0) {
    int j, found = 0;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && strcmp(args, fs[j].name) == 0 && strcmp(fs[j].parentDir, currentPath) == 0) {
        Serial.print(F("Name: ")); Serial.println(fs[j].name);
        Serial.print(F("Type: ")); Serial.println(fs[j].isDirectory ? F("Directory") : F("File"));
        Serial.print(F("Size: ")); Serial.print(strlen(fs[j].content)); Serial.println(F(" bytes"));
        found = 1;
        break;
      }
    }
    if (!found) Serial.println(F("Not found."));
  }
  else if (strcmp_P(cmd, PSTR("rm")) == 0) {
    int j, found = 0;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && strcmp(args, fs[j].name) == 0 && strcmp(fs[j].parentDir, currentPath) == 0) {
        if (fs[j].isDirectory) {
          char dirPath[PATH_LEN];
          snprintf_P(dirPath, PATH_LEN, PSTR("%s%s/"), currentPath, args);
          int k;
          for (k = 0; k < MAX_FILES; k++) {
            if (fs[k].active && strncmp(fs[k].parentDir, dirPath, strlen(dirPath)) == 0) {
              fs[k].active = 0;
            }
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
  else if (strcmp_P(cmd, PSTR("dmesg")) == 0) {
    Serial.println(F("=== KERNEL MESSAGES ==="));
    int j;
    for (j = 0; j < DMESG_LINES; j++) {
      if (dmesg[j].message[0] != '\0') {
        Serial.print(F("["));
        Serial.print(dmesg[j].timestamp);
        Serial.print(F("] "));
        Serial.println(dmesg[j].message);
      }
    }
  }
  else if (strcmp_P(cmd, PSTR("uptime")) == 0) {
    unsigned long s = millis() / 1000;
    unsigned long h = s / 3600;
    unsigned long m = (s % 3600) / 60;
    unsigned long sec = s % 60;
    Serial.print(F("up "));
    Serial.print(h); Serial.print(F("h "));
    Serial.print(m); Serial.print(F("m "));
    Serial.print(sec); Serial.println(F("s"));
    addDmesg(F("uptime command"));
  }
  else if (strcmp_P(cmd, PSTR("df")) == 0 || strcmp_P(cmd, PSTR("free")) == 0) {
    Serial.print(F("Free RAM: "));
    Serial.print(freeMemory());
    Serial.println(F(" bytes"));
  }
  else if (strcmp_P(cmd, PSTR("whoami")) == 0) {
    Serial.println(F("root"));
  }
  else if (strcmp_P(cmd, PSTR("uname")) == 0) {
    Serial.println(F("KernelUNO v1.0"));
    Serial.print(F("Kernel: Arduino "));
    Serial.println(F("AVR"));
    Serial.print(F("Hardware: "));
    Serial.println(F("Arduino UNO"));
    Serial.print(F("RAM: "));
    Serial.print(freeMemory());
    Serial.println(F(" bytes free"));
  }
  else if (strcmp_P(cmd, PSTR("reboot")) == 0) {
    Serial.println(F("Rebooting..."));
    addDmesg(F("System reboot"));
    delay(500);
    resetFunc();
  }
  else if (strcmp_P(cmd, PSTR("clear")) == 0) {
    int j;
    for (j = 0; j < 30; j++) Serial.println();
  }
  else if (strcmp_P(cmd, PSTR("sh")) == 0) {
    if (args[0] == '\0') {
      Serial.println(F("Usage: sh [script]"));
      return;
    }
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
  else if (strcmp_P(cmd, PSTR("pwm")) == 0) {
    sp = indexOf(args, " ");
    if (sp == -1) { Serial.println(F("Usage: pwm [pin] [0-255]")); return; }
    pin = atoi_safe(args);
    char valStr[8] = "";
    strncpy(valStr, args + sp + 1, 7);
    valStr[7] = '\0';
    int pwmVal = atoi_safe(valStr);
    if (pwmVal < 0) pwmVal = 0;
    if (pwmVal > 255) pwmVal = 255;
    pinMode(pin, OUTPUT);
    analogWrite(pin, pwmVal);
    snprintf_P(buf, sizeof(buf), PSTR("PWM pin %d value %d"), pin, pwmVal);
    addDmesgRam(buf);
    Serial.print(F("PWM pin ")); Serial.print(pin);
    Serial.print(F(" set to ")); Serial.println(pwmVal);
  }
  else if (strcmp_P(cmd, PSTR("alias")) == 0) {
    if (args[0] == '\0') {
      int j, any = 0;
      for (j = 0; j < MAX_ALIASES; j++) {
        if (aliases[j].active) {
          Serial.print(aliases[j].name);
          Serial.print(F("='"));
          Serial.print(aliases[j].value);
          Serial.println(F("'"));
          any = 1;
        }
      }
      if (!any) Serial.println(F("No aliases."));
    } else {
      int eq = indexOf(args, "=");
      if (eq == -1) {
        // show single alias
        int j, found = 0;
        for (j = 0; j < MAX_ALIASES; j++) {
          if (aliases[j].active && strcmp(aliases[j].name, args) == 0) {
            Serial.print(args); Serial.print(F("='")); Serial.print(aliases[j].value); Serial.println(F("'"));
            found = 1; break;
          }
        }
        if (!found) Serial.println(F("No such alias."));
      } else {
        char aname[ALIAS_NAME_LEN] = "";
        char aval[ALIAS_VAL_LEN] = "";
        strncpy(aname, args, eq < ALIAS_NAME_LEN ? eq : ALIAS_NAME_LEN - 1);
        aname[ALIAS_NAME_LEN - 1] = '\0';
        strncpy(aval, args + eq + 1, ALIAS_VAL_LEN - 1);
        aval[ALIAS_VAL_LEN - 1] = '\0';
        int j, slot = -1;
        for (j = 0; j < MAX_ALIASES; j++) {
          if (aliases[j].active && strcmp(aliases[j].name, aname) == 0) { slot = j; break; }
        }
        if (slot == -1) {
          for (j = 0; j < MAX_ALIASES; j++) {
            if (!aliases[j].active) { slot = j; break; }
          }
        }
        if (slot == -1) { Serial.println(F("Alias table full.")); return; }
        strncpy(aliases[slot].name, aname, ALIAS_NAME_LEN - 1);
        aliases[slot].name[ALIAS_NAME_LEN - 1] = '\0';
        strncpy(aliases[slot].value, aval, ALIAS_VAL_LEN - 1);
        aliases[slot].value[ALIAS_VAL_LEN - 1] = '\0';
        aliases[slot].active = 1;
        Serial.println(F("Alias set."));
      }
    }
  }
  else if (strcmp_P(cmd, PSTR("slots")) == 0) {
    int used = 0, j;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active) used++;
    }
    Serial.print(F("("));
    Serial.print(used);
    Serial.print(F("/"));
    Serial.print(MAX_FILES);
    Serial.println(F(")"));
  }
  else if (strcmp_P(cmd, PSTR("find")) == 0) {
    if (args[0] == '\0') { Serial.println(F("Usage: find [name]")); return; }
    int j, found = 0;
    for (j = 0; j < MAX_FILES; j++) {
      if (fs[j].active && strcmp(fs[j].name, args) == 0) {
        Serial.print(fs[j].parentDir);
        Serial.println(fs[j].name);
        found = 1;
      }
    }
    if (!found) Serial.println(F("Not found."));
  }
  else if (strcmp_P(cmd, PSTR("help")) == 0) {
    Serial.println(F("Commands: ls, cd, pwd, mkdir, touch, cat, echo, rm, info"));
    Serial.println(F("          pinmode, write, read, gpio, pwm, sh"));
    Serial.println(F("          uptime, uname, dmesg, df, free, whoami, clear, reboot"));
    Serial.println(F("          alias, slots, find"));
    Serial.println(F("GPIO: gpio [pin] on/off/toggle  |  gpio vixa [count]"));
    Serial.println(F("SH:   sh [file]  -- run script (use ; as line separator)"));
  }
  else {
    // check alias
    int j, resolved = 0;
    for (j = 0; j < MAX_ALIASES; j++) {
      if (aliases[j].active && strcmp(aliases[j].name, cmd) == 0) {
        char aliasLine[32] = "";
        strncpy(aliasLine, aliases[j].value, 31);
        aliasLine[31] = '\0';
        if (args[0] != '\0') {
          int al = strlen(aliasLine);
          if (al < 30) { aliasLine[al] = ' '; aliasLine[al+1] = '\0'; }
          strncat(aliasLine, args, 31 - strlen(aliasLine));
        }
        executeCommand(aliasLine);
        resolved = 1;
        break;
      }
    }
    if (!resolved) Serial.println(F("Unknown command."));
  }
}

// Interpreter sh
void runScript(const char* content) {
  char line[32];
  int ci = 0, li = 0, lineNum = 0;
  int len = strlen(content);

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
