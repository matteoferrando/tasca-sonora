/*
  Tasca Sonora v6 — firmware per M5Stack StickS3

  TASTO FRONTALE
    premi              suona il suono attivo (ripremi per fermare)
    tieni 2 s          accende / spegne il Bluetooth
  TASTO LATERALE
    tieni premuto      registra (max 30 s); si salvano le ultime 6 registrazioni
    premi              riascolta l'ultima registrazione
    doppio clic        apre il menu
  MENU
    laterale           pagina successiva
    frontale           cambia il valore
    tieni frontale 1 s esce (esce da solo dopo 6 s)
  BLOCCO TASTI (anti-tasca)
    clic sul tasto di accensione, oppure tieni premuti insieme frontale e laterale per 1 s
  TASTO DI ACCENSIONE: premi = accende, doppio clic = spegne

  Bluetooth: protocollo compatibile con le versioni precedenti, piu' banco suoni, registrazioni,
  impostazioni, ora, nome e aggiornamento del firmware (OTA).
  Console USB a 115200 baud: 'h' per l'elenco dei comandi di prova.

  Arduino IDE: ESP32S3 Dev Module, Flash 8MB, PSRAM OPI, USB CDC On Boot Enabled.
  partitions.csv nella cartella dello sketch: due partizioni app (per l'OTA) e ~4,4 MB per i suoni.
*/

#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_sleep.h>
#include <esp_ota_ops.h>
#include <driver/rtc_io.h>
#include <sys/time.h>
#include <time.h>

// ============================================================ configurazione
#define PIN_KEY1          11
#define PIN_KEY2          12
#define PM1_ADDR          0x6E
#define ES8311_ADDR       0x18

#define FW_VERSION        6
#define DEFAULT_NAME      "Tasca Sonora"
#define SAMPLE_RATE       16000
#define MAX_SOUND_BYTES   960000UL       // 30 s
#define MAX_REC_BYTES     960000UL       // 30 s
#define NUM_SLOTS         6
#define MAX_RECS          6
#define NAME_LEN          16
#define MIN_REC_SAMPLES   (SAMPLE_RATE / 3)
#define REC_CHUNK         800
#define FS_MARGIN         (48 * 1024)

#define BT_WINDOW_MS      120000UL
#define LONG_PRESS_MS     2000UL
#define REC_HOLD_MS       400UL
#define BOTH_HOLD_MS      900UL
#define DOUBLE_CLICK_MS   400UL
#define MENU_IDLE_MS      6000UL
#define SCREEN_DIM_MS     5000UL
#define SCREEN_OFF_MS     9000UL
#define LOCKED_SLEEP_MS   8000UL
#define BRIGHT_FULL       110
#define BRIGHT_DIM        22
#define SPK_MAX_BATTERY   255
#define SPK_MAX_USB       255
#define CRITICAL_MV       3300
#define SAG_MV            3400        // sotto questa tensione, mentre suona, il volume scende subito
#define VCEIL_MIN         130         // tetto minimo del volume a batteria
#define VCEIL_START       215         // tetto iniziale: cresce da solo se la batteria regge

#define TEMP_PATH  "/upload.tmp"

#define SVC_UUID     "8f1d0000-6c2a-4b8e-9d3f-2a7c5e1b4d60"
#define CTRL_UUID    "8f1d0001-6c2a-4b8e-9d3f-2a7c5e1b4d60"
#define DATA_UUID    "8f1d0002-6c2a-4b8e-9d3f-2a7c5e1b4d60"
#define STATUS_UUID  "8f1d0003-6c2a-4b8e-9d3f-2a7c5e1b4d60"
#define INFO_UUID    "8f1d0004-6c2a-4b8e-9d3f-2a7c5e1b4d60"
#define DL_UUID      "8f1d0005-6c2a-4b8e-9d3f-2a7c5e1b4d60"
#define CATALOG_UUID "8f1d0007-6c2a-4b8e-9d3f-2a7c5e1b4d60"
#define DIAG_UUID    "8f1d0008-6c2a-4b8e-9d3f-2a7c5e1b4d60"

enum : uint8_t {
  // v1-v5
  CMD_START = 0x01, CMD_END = 0x02, CMD_PLAY = 0x03, CMD_VOLUME = 0x04, CMD_ABORT = 0x05,
  CMD_DOWNLOAD_REC = 0x06, CMD_REC_TO_SOUND = 0x07, CMD_PLAY_REC = 0x08,
  CMD_DELETE_SOUND = 0x09, CMD_DELETE_REC = 0x0A, CMD_DOWNLOAD_SOUND = 0x0B, CMD_FIND = 0x0C, CMD_STOP = 0x0D,
  // v6
  CMD_SELECT_SLOT = 0x10, CMD_SET_SLOT_NAME = 0x11, CMD_SELECT_REC = 0x12, CMD_SET_TIME = 0x13,
  CMD_SET_SETTING = 0x14, CMD_SET_DEVICE_NAME = 0x15, CMD_OTA_BEGIN = 0x16, CMD_OTA_END = 0x17,
  CMD_LOCK = 0x18, CMD_PLAY_SLOT = 0x19, CMD_DOWNLOAD_SLOT = 0x1A, CMD_DELETE_SLOT = 0x1B,
  CMD_START_SLOT = 0x1C, CMD_REC_TO_SLOT = 0x1D
};
enum : uint8_t {
  ST_READY = 0x10, ST_SAVED = 0x11, ST_DL_DONE = 0x12, ST_COPIED = 0x13, ST_DELETED = 0x14,
  ST_OK = 0x15, ST_OTA_OK = 0x16,
  ST_ERR_SIZE = 0x20, ST_ERR_CRC = 0x21, ST_ERR_FS = 0x22, ST_ERR_STATE = 0x23, ST_ERR_BUSY = 0x24,
  ST_ERR_NOREC = 0x25, ST_ERR_SPACE = 0x26, ST_ERR_OTA = 0x27, ST_ERR_ARG = 0x28
};
enum : uint8_t { SET_REPEAT = 1, SET_PUNCH = 2, SET_AUTOLOCK = 3, SET_FLIP = 4, SET_SLEEP = 5 };

// ============================================================ tipi per l'elaborazione audio
struct LoudCfg { float hpfHz, presHz, presDb, targetDb, maxGainDb, gateDb, drive; };
static const LoudCfg LOUD_MODES[3] = {
  {  0,    0, 0,   0,  0,   0, 0   },   // 0 naturale: nessuna elaborazione
  { 300, 2700, 4, -10, 15, -42, 2.2f }, // 1 forte
  { 400, 2800, 6,  -7, 20, -40, 3.0f }, // 2 massimo
};

struct Biquad {
  float b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  float run(float x) { float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; }
};

// ============================================================ colori e grafica
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t C_BG    = rgb(13, 17, 23);
static const uint16_t C_PANEL = rgb(36, 44, 56);
static const uint16_t C_TEXT  = rgb(236, 240, 244);
static const uint16_t C_MUTED = rgb(134, 146, 160);
static const uint16_t C_BRASS = rgb(222, 166, 64);
static const uint16_t C_CYAN  = rgb(84, 186, 240);
static const uint16_t C_RED   = rgb(240, 84, 72);
static const uint16_t C_GREEN = rgb(92, 204, 124);

M5Canvas cv(&M5.Display);
#define FONT_BIG   (&fonts::FreeSansBold12pt7b)
#define FONT_SMALL (&fonts::FreeSans9pt7b)
#define FONT_HUGE  (&fonts::FreeSansBold18pt7b)
#define FONT_SMALLB (&fonts::FreeSansBold9pt7b)
#define PEAK_BINS  200

// ============================================================ stato persistente
struct RecMeta { uint32_t id; uint32_t epoch; uint32_t bytes; };
struct Settings { uint8_t repeat, punch, autolock, flip; uint16_t sleepSec; };
struct BootRec { uint32_t epoch; uint8_t reason; uint8_t fw; uint16_t mv; uint8_t pm1wake; uint8_t pad[3]; };

Preferences prefs;
Settings    cfg = { 0, 2, 0, 0, 45 };          // punch: 0 naturale, 1 forte, 2 massimo
char        devName[24] = DEFAULT_NAME;
char        slotNames[NUM_SLOTS][NAME_LEN + 1];
uint32_t    slotBytes[NUM_SLOTS];
uint8_t     activeSlot = 0;
RecMeta     recs[MAX_RECS];
uint8_t     recCount = 0;
uint32_t    nextRecId = 1;
volatile uint8_t selRec = 0;
volatile uint8_t volume = 70;
int16_t     tzMinutes = 60;                 // fuso attuale in minuti
int16_t     tzNextMinutes = 0;              // fuso dopo il prossimo cambio d'ora (ora legale/solare)
uint32_t    tzNextAt = 0;                   // istante UTC del prossimo cambio, 0 = nessuno

RTC_DATA_ATTR bool rtcLocked = false;      // il blocco sopravvive al deep sleep
RTC_DATA_ATTR int8_t rtcSoc = -1;          // ultima percentuale mostrata (niente salti al risveglio)
RTC_NOINIT_ATTR uint32_t rtcOtaMagic;      // sopravvive al riavvio software dopo l'OTA
#define OTA_MAGIC 0x50A7C0DEUL

static const uint16_t SLEEP_CHOICES[] = { 20, 45, 90, 180, 300 };

// ============================================================ buffer audio (PSRAM)
int16_t* soundBuf = nullptr;  size_t soundSamples = 0;        // suono attivo
int16_t* punchBuf = nullptr;  bool punchReady = false;         // suono attivo elaborato "potente"
int16_t* recBuf   = nullptr;  size_t recSamples = 0;  uint32_t recBufId = 0;   // registrazione in memoria
int16_t* auxBuf   = nullptr;  size_t auxSamples = 0;  int auxSlot = -1;        // anteprima di un altro suono
int16_t* recLoud  = nullptr;  uint32_t recLoudId = 0;  uint8_t recLoudMode = 0;  // registrazione elaborata
int16_t* auxLoud  = nullptr;  int auxLoudSlot = -1;     uint8_t auxLoudMode = 0;  // anteprima elaborata
uint8_t  peaks[PEAK_BINS];

// ============================================================ stato Bluetooth
NimBLEServer*         bleServer = nullptr;
NimBLECharacteristic* chStatus  = nullptr;
NimBLECharacteristic* chInfo    = nullptr;
NimBLECharacteristic* chDl      = nullptr;
NimBLECharacteristic* chCatalog = nullptr;
NimBLECharacteristic* chDiag    = nullptr;

volatile bool     bleReady = false, btMode = false, connected = false;
volatile bool     receiving = false, receivingOta = false;
volatile uint32_t btUntil = 0;
volatile uint16_t peerMtu = 23;
File              uploadFile;
volatile uint8_t  uploadSlot = 0;
uint32_t          expectedBytes = 0, runningCrc = 0;
volatile uint32_t receivedBytes = 0;
volatile bool     otaFailed = false;
volatile uint32_t rebootAt = 0;

// richieste dai callback BLE, eseguite nel loop
volatile bool    playRequested = false, playRecRequested = false, stopReq = false, findReq = false;
volatile int8_t  playSlotReq = -1, downloadSlotReq = -1, deleteSlotReq = -1, recToSlotReq = -1, selectSlotReq = -1;
volatile bool    downloadRecReq = false, deleteRecReq = false, reloadSlot = false, refreshNameReq = false;
volatile bool    justConnected = false, justDisconnected = false, settingsDirty = false;
volatile int8_t  lockReq = -1;
volatile uint8_t pendingEvent = 0;
volatile bool    recording = false, downloading = false;
volatile size_t  recPos = 0, dlPos = 0, dlTotal = 0;

// ============================================================ stato interfaccia
volatile uint8_t  batteryLevel = 0;
volatile int      batteryMv = 0;
volatile bool     usbPowered = false;
volatile uint32_t fsTotal = 0, fsUsed = 0;

bool     screenOn = false;
uint8_t  brightness = 255;
uint32_t lastActivity = 0, lastUi = 0;
float    holdProgress = 0;
float    bothProgress = 0;
bool     menuOpen = false;
uint8_t  menuPage = 0;
uint32_t menuUntil = 0;
uint8_t  playing = 0;                     // 0 niente, 1 suono attivo, 2 registrazione, 3 anteprima
uint8_t  lastPlayKind = 0;
bool     userStopped = false;
uint32_t playStart = 0, playMs = 0;
uint16_t recLevel = 0;
char     toastTitle[32] = "", toastSub[44] = "";
uint16_t toastColor = C_GREEN;
uint8_t  toastIcon = 1;                   // 1 ok, 0 errore, 2 lucchetto
uint32_t toastUntil = 0;

bool     k1Menu = false;
bool     k1Was = false, k1Long = false, k2Was = false, k2Handled = false, k2Pending = false, bothDone = false;
uint32_t k1Since = 0, k2Since = 0, k2LastClick = 0, bothSince = 0;
uint8_t  wakeKey = 0;
uint32_t simUntil[2] = { 0, 0 };          // tasti simulati dalla console USB

bool     boostOn = false;
uint32_t boostIdleSince = 0;

// Protezione adattiva contro i blackout a batteria (picchi di corrente dell'altoparlante)
uint8_t  volCeil = VCEIL_START;           // tetto del volume a batteria, 0-255 (NVS "vceil")
bool     pbGuard = false;                 // riproduzione a batteria in corso (NVS "pb" = 1)
bool     pbSagged = false;
int      pbMinMv = 9999;

// ============================================================ utilita'
static uint32_t crc32Update(uint32_t crc, const uint8_t* d, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *d++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
  }
  return ~crc;
}
static uint32_t readU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void writeU32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static bool keyDown(int pin) {
  int i = pin == PIN_KEY1 ? 0 : 1;
  return digitalRead(pin) == LOW || (int32_t)(simUntil[i] - millis()) > 0;
}
static void touch() { lastActivity = millis(); lastUi = lastActivity; }
static bool busy() { return receiving || receivingOta || recording || downloading; }
static bool timeValid() { return time(nullptr) > 1700000000; }

// Fuso da usare adesso: dopo il cambio d'ora comunicato dall'app passa da solo al nuovo valore
static int localOffsetMin() {
  if (tzNextAt && timeValid() && (uint32_t)time(nullptr) >= tzNextAt) return tzNextMinutes;
  return tzMinutes;
}

static void fmtSeconds(char* out, size_t len, float s) {
  snprintf(out, len, "%.1f s", s);
  for (char* c = out; *c; c++) if (*c == '.') *c = ',';
}

static void slotPath(char* out, size_t len, int slot) { snprintf(out, len, "/s%d.pcm", slot); }
static void recPath(char* out, size_t len, uint32_t id) { snprintf(out, len, "/r%lu.pcm", (unsigned long)id); }

// Copia un nome UTF-8 senza spezzare i caratteri a meta'
static void copyName(char* dst, size_t cap, const uint8_t* src, size_t n) {
  size_t o = 0, i = 0;
  while (i < n && src[i]) {
    uint8_t c = src[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (o + len >= cap || i + len > n) break;
    if (c < 0x20 || c == '"' || c == '\\') { i++; continue; }
    memcpy(dst + o, src + i, len);
    o += len; i += len;
  }
  dst[o] = 0;
}

// Versione ASCII per lo schermo (i font non hanno lettere accentate)
static void asciiName(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  const uint8_t* s = (const uint8_t*)src;
  while (*s && o + 1 < cap) {
    if (*s < 0x80) { dst[o++] = *s++; continue; }
    if (s[0] == 0xC3 && s[1]) {
      uint8_t c = s[1];
      char m = '?';
      if (c >= 0x80 && c <= 0x85) m = 'A'; else if (c >= 0xA0 && c <= 0xA5) m = 'a';
      else if (c >= 0x88 && c <= 0x8B) m = 'E'; else if (c >= 0xA8 && c <= 0xAB) m = 'e';
      else if (c >= 0x8C && c <= 0x8F) m = 'I'; else if (c >= 0xAC && c <= 0xAF) m = 'i';
      else if (c >= 0x92 && c <= 0x96) m = 'O'; else if (c >= 0xB2 && c <= 0xB6) m = 'o';
      else if (c >= 0x99 && c <= 0x9C) m = 'U'; else if (c >= 0xB9 && c <= 0xBC) m = 'u';
      else if (c == 0x87) m = 'C'; else if (c == 0xA7) m = 'c'; else if (c == 0xB1) m = 'n';
      dst[o++] = m;
      s += 2;
      continue;
    }
    s++;
    while ((*s & 0xC0) == 0x80) s++;          // salta il resto del carattere
    dst[o++] = '?';
  }
  dst[o] = 0;
}

static void slotLabel(char* out, size_t len, int slot) {
  if (slotNames[slot][0]) asciiName(out, len, slotNames[slot]);
  else snprintf(out, len, "Suono %d", slot + 1);
}

TaskHandle_t loopTask = nullptr;

// Se la coda Bluetooth e' piena (per esempio subito dopo un download) la notifica va ritentata,
// altrimenti l'app resterebbe ad aspettare una conferma persa. Si ritenta solo dal loop:
// dentro i callback del Bluetooth aspettare bloccherebbe proprio chi svuota la coda.
static void notifyStatus(uint8_t code, const uint8_t* extra = nullptr, size_t extraLen = 0) {
  if (code != ST_READY && code != ST_OK) pendingEvent = code;
  if (!chStatus) return;
  uint8_t buf[8] = { code };
  if (extra && extraLen <= 7) memcpy(buf + 1, extra, extraLen);
  chStatus->setValue(buf, 1 + extraLen);
  if (!connected) return;
  bool fromLoop = xTaskGetCurrentTaskHandle() == loopTask;
  uint32_t t = millis();
  while (!chStatus->notify() && fromLoop && connected && millis() - t < 2000) delay(5);
}

// ============================================================ persistenza
static void saveSettings() {
  prefs.putUChar("vol", volume);
  prefs.putUChar("slot", activeSlot);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.putShort("tz", tzMinutes);
  prefs.putShort("tzn", tzNextMinutes);
  prefs.putULong("tzat", tzNextAt);
}

static void saveRecMeta() {
  prefs.putBytes("recs", recs, sizeof(recs));
  prefs.putUChar("rcnt", recCount);
  prefs.putULong("rnext", nextRecId);
}

static void loadPersistent() {
  volume = prefs.getUChar("vol", 70);
  volCeil = max<uint8_t>(prefs.getUChar("vceil", VCEIL_START), VCEIL_MIN);
  activeSlot = min<uint8_t>(prefs.getUChar("slot", 0), NUM_SLOTS - 1);
  if (prefs.getBytesLength("cfg") == sizeof(cfg)) prefs.getBytes("cfg", &cfg, sizeof(cfg));
  bool okSleep = false;
  for (uint16_t s : SLEEP_CHOICES) if (s == cfg.sleepSec) okSleep = true;
  if (!okSleep) cfg.sleepSec = 45;
  if (cfg.punch > 2) cfg.punch = 2;
  if (!prefs.getBool("aud2", false)) { cfg.punch = 2; prefs.putBool("aud2", true); prefs.putBytes("cfg", &cfg, sizeof(cfg)); }
  tzMinutes = prefs.getShort("tz", 60);
  tzNextMinutes = prefs.getShort("tzn", 0);
  tzNextAt = prefs.getULong("tzat", 0);
  String n = prefs.getString("dev", DEFAULT_NAME);
  strlcpy(devName, n.length() ? n.c_str() : DEFAULT_NAME, sizeof(devName));
  for (int i = 0; i < NUM_SLOTS; i++) {
    char key[4] = { 'n', char('0' + i), 0 };
    String s = prefs.getString(key, "");
    strlcpy(slotNames[i], s.c_str(), sizeof(slotNames[i]));
  }
  memset(recs, 0, sizeof(recs));
  if (prefs.getBytesLength("recs") == sizeof(recs)) prefs.getBytes("recs", recs, sizeof(recs));
  recCount = min<uint8_t>(prefs.getUChar("rcnt", 0), MAX_RECS);
  nextRecId = max<uint32_t>(prefs.getULong("rnext", 1), 1);
}

// Registro degli avvii: aiuta a capire riavvii e cali di tensione anche a posteriori
uint8_t pm1WakeAtBoot = 0;

// Causa di accensione secondo il chip di alimentazione (si accumula finche' non la azzeriamo)
static const char* pm1WakeName(uint8_t w) {
  if (w & 0x04) return "tasto accensione";
  if (w & 0x02) return "USB inserita";
  if (w & 0x40) return "porta 5V";
  if (w & 0x08) return "tasto reset";
  if (w & 0x10) return "comando";
  if (w & 0x20) return "GPIO";
  if (w & 0x01) return "timer";
  return "";
}

static void logBoot() {
  pm1WakeAtBoot = M5.In_I2C.readRegister8(PM1_ADDR, 0x05, 100000);
  M5.In_I2C.writeRegister8(PM1_ADDR, 0x05, 0, 100000);
  bool crashInPlay = prefs.getUChar("pb", 0) == 1;
  if (crashInPlay) {                                         // spento mentre suonava: abbasso il tetto
    prefs.putUChar("pb", 0);
    volCeil = max<int>(VCEIL_MIN, (int)volCeil - 40);
    prefs.putUChar("vceil", volCeil);
    applyVolume();
  }
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) return;      // i risvegli sono normali: non li annoto
  BootRec ring[8];
  memset(ring, 0, sizeof(ring));
  if (prefs.getBytesLength("boot") == sizeof(ring)) prefs.getBytes("boot", ring, sizeof(ring));
  uint8_t idx = prefs.getUChar("bidx", 0) % 8;
  ring[idx] = { (uint32_t)(timeValid() ? time(nullptr) : 0), (uint8_t)esp_reset_reason(), FW_VERSION, (uint16_t)M5.Power.getBatteryVoltage(), pm1WakeAtBoot, { (uint8_t)crashInPlay, volCeil, 0 } };
  prefs.putBytes("boot", ring, sizeof(ring));
  prefs.putUChar("bidx", (idx + 1) % 8);
  prefs.putULong("bcnt", prefs.getULong("bcnt", 0) + 1);
}

static const char* reasonName(uint8_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "accensione";
    case ESP_RST_SW: return "riavvio";
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "risveglio";
    case ESP_RST_BROWNOUT: return "calo tensione";
    case ESP_RST_USB: return "USB";
    default: return "altro";
  }
}

// ============================================================ memoria, file e audio
static void audioPower(bool on) {
  if (on == boostOn) return;
  M5.Power.setExtOutput(on);
  boostOn = on;
  boostIdleSince = millis();
  if (on) delay(40);
}

static void applyVolume() {
  uint16_t cap = usbPowered ? SPK_MAX_USB : min<uint16_t>(SPK_MAX_BATTERY, volCeil);
  M5.Speaker.setVolume((uint16_t)volume * cap / 100);
}

static void beep(std::initializer_list<uint16_t> notes, uint16_t ms = 70) {
  if (playing) return;
  audioPower(true);
  for (uint16_t f : notes) { M5.Speaker.tone(f, ms); delay(ms + 25); }
}

static int16_t* psBuf(int16_t*& p, size_t bytes) {
  if (!p) p = (int16_t*)ps_malloc(bytes);
  return p;
}

static size_t loadFile(const char* path, int16_t*& buf, size_t maxBytes) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  size_t bytes = min((size_t)f.size(), (size_t)maxBytes) & ~(size_t)1;
  size_t got = psBuf(buf, maxBytes) ? f.read((uint8_t*)buf, bytes) : 0;
  f.close();
  return got == bytes ? bytes / 2 : 0;
}

static bool saveFile(const char* path, const int16_t* buf, size_t samples) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  size_t bytes = samples * 2, done = 0;
  while (done < bytes) {
    size_t n = min((size_t)8192, bytes - done);
    if (f.write((const uint8_t*)buf + done, n) != n) { f.close(); return false; }
    done += n;
  }
  f.close();
  return true;
}

static uint32_t fileBytes(const char* path) {
  File f = LittleFS.open(path, "r");
  uint32_t n = f ? f.size() : 0;
  if (f) f.close();
  return n;
}

static uint32_t freeBytes() {
  uint32_t t = LittleFS.totalBytes(), u = LittleFS.usedBytes();
  return t > u ? t - u : 0;
}

static void scanSlots() {
  char p[16];
  for (int i = 0; i < NUM_SLOTS; i++) { slotPath(p, sizeof(p), i); slotBytes[i] = fileBytes(p); }
}

// ---- Elaborazione "volume alto" (vedi loud.h, collaudata sul computer con i file veri)
// Elaborazione "volume alto" per l'altoparlante da 1 W della StickS3 (16 kHz, mono, int16).
// 1) passa-alto: toglie i bassi che l'altoparlante non riproduce e che sprecano margine
// 2) presenza: piccolo rinforzo dove l'altoparlante rende di piu' (2,5-3 kHz)
// 3) livellatore: alza le parti deboli verso un livello obiettivo (con soglia per non gonfiare il rumore)
// 4) saturazione morbida (tanh): i picchi vengono arrotondati invece di essere tagliati di netto

static Biquad bqHighpass(float fs, float fc, float q) {
  float w = 2 * (float)M_PI * fc / fs, c = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
  Biquad b; b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = (1 + c) / 2 / a0; b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0; return b;
}
static Biquad bqPeak(float fs, float fc, float q, float db) {
  float A = powf(10, db / 40), w = 2 * (float)M_PI * fc / fs, c = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al / A;
  Biquad b; b.b0 = (1 + al * A) / a0; b.b1 = -2 * c / a0; b.b2 = (1 - al * A) / a0; b.a1 = -2 * c / a0; b.a2 = (1 - al / A) / a0; return b;
}

// tanh approssimata (Pade'), errore < 2% e molto piu' veloce di tanhf sull'ESP32
static inline float fastTanh(float x) {
  if (x > 3) return 1;
  if (x < -3) return -1;
  float x2 = x * x;
  return x * (27 + x2) / (27 + 9 * x2);
}

// in e out possono coincidere.
static void loudProcessCfg(const int16_t* in, int16_t* out, size_t n, const LoudCfg& c, float fs = 16000) {
  Biquad hp = bqHighpass(fs, c.hpfHz, 0.707f), pk = bqPeak(fs, c.presHz, 1.0f, c.presDb);
  // passata 1: filtri, picco
  float peak = 1;
  for (size_t i = 0; i < n; i++) {
    float y = pk.run(hp.run(in[i]));
    y = y > 32767 ? 32767 : (y < -32768 ? -32768 : y);
    out[i] = (int16_t)y;                     // salvo filtrato (gia' entro int16)
    float a = fabsf(y); if (a > peak) peak = a;
  }
  // passata 2: livellatore + saturazione morbida
  const float norm = 1.0f / peak;
  const float target = powf(10, c.targetDb / 20), maxG = powf(10, c.maxGainDb / 20), gate = powf(10, c.gateDb / 20);
  const float aRms = expf(-1.0f / (0.030f * fs));       // finestra RMS 30 ms
  const float aAtt = expf(-1.0f / (0.005f * fs));       // il guadagno scende in 5 ms
  const float aRel = expf(-1.0f / (0.200f * fs));       // e risale in 200 ms
  const float itdrv = 1.0f / fastTanh(c.drive);
  // il guadagno si aggiorna ogni 8 campioni (0,5 ms): stesso risultato, molti meno calcoli
  const float aAtt8 = powf(aAtt, 8), aRel8 = powf(aRel, 8);
  float e2 = 0, g = 1;
  for (size_t i = 0; i < n; i++) {
    float x = out[i] * norm;
    e2 = aRms * e2 + (1 - aRms) * x * x;
    if ((i & 7) == 0) {
      float env = sqrtf(e2);
      float gt = env > gate ? target / env : 1;
      if (gt > maxG) gt = maxG;
      if (gt < 1) gt = 1;
      g = gt < g ? aAtt8 * g + (1 - aAtt8) * gt : aRel8 * g + (1 - aRel8) * gt;
    }
    float v = fastTanh(x * g * c.drive) * itdrv;        // |x*g| = 1 -> 1
    if (v > 1) v = 1; else if (v < -1) v = -1;
    out[i] = (int16_t)(v * 31500);
  }
}

// Restituisce false se mode == 0 (nessuna elaborazione).
static bool loudProcess(const int16_t* in, int16_t* out, size_t n, int mode, float fs = 16000) {
  if (mode <= 0 || mode > 2 || !n) return false;
  loudProcessCfg(in, out, n, LOUD_MODES[mode], fs);
  return true;
}

// Elabora alla massima velocita' della CPU e torna a 80 MHz
static void loudFast(const int16_t* in, int16_t* out, size_t n) {
  uint32_t t = millis();
  setCpuFrequencyMhz(240);
  loudProcess(in, out, n, cfg.punch);
  setCpuFrequencyMhz(80);
  Serial.printf("[audio] modo %d, %u campioni in %lu ms\n", cfg.punch, (unsigned)n, millis() - t);
}

static void computePeaks(const int16_t* buf, size_t n) {
  memset(peaks, 0, sizeof(peaks));
  if (!buf || !n) return;
  for (int b = 0; b < PEAK_BINS; b++) {
    size_t a = (size_t)b * n / PEAK_BINS, e = min(n, (size_t)(b + 1) * n / PEAK_BINS);
    int32_t pk = 0;
    for (size_t i = a; i < e; i += 3) pk = max(pk, (int32_t)abs((int)buf[i]));
    peaks[b] = (uint8_t)min(255L, (long)pk * 255 / 32767);
  }
}

static void loadActive() {
  char p[16];
  slotPath(p, sizeof(p), activeSlot);
  soundSamples = loadFile(p, soundBuf, MAX_SOUND_BYTES);
  punchReady = false;
  if (cfg.punch && soundSamples && psBuf(punchBuf, MAX_SOUND_BYTES)) {
    loudFast(soundBuf, punchBuf, soundSamples);
    punchReady = true;
  }
  if (auxSlot == activeSlot) auxSlot = -1;
}

static bool ensureRecLoaded(uint8_t index) {
  if (index >= recCount) return false;
  if (recBufId == recs[index].id && recSamples) return true;
  char p[20];
  recPath(p, sizeof(p), recs[index].id);
  recSamples = loadFile(p, recBuf, MAX_REC_BYTES);
  recBufId = recSamples ? recs[index].id : 0;
  recLoudId = 0;
  return recSamples > 0;
}

static bool ensureAuxSlot(int slot) {
  if (slot == auxSlot && auxSamples) return true;
  char p[16];
  slotPath(p, sizeof(p), slot);
  auxSamples = loadFile(p, auxBuf, MAX_SOUND_BYTES);
  auxSlot = auxSamples ? slot : -1;
  auxLoudSlot = -1;
  return auxSamples > 0;
}

static void deleteRecAt(uint8_t index) {
  if (index >= recCount) return;
  char p[20];
  recPath(p, sizeof(p), recs[index].id);
  LittleFS.remove(p);
  if (recBufId == recs[index].id) { recBufId = 0; recSamples = 0; }
  for (uint8_t i = index; i + 1 < recCount; i++) recs[i] = recs[i + 1];
  recCount--;
  memset(&recs[recCount], 0, sizeof(RecMeta));
  if (selRec >= recCount) selRec = 0;
  saveRecMeta();
}

// Percentuale dalla curva di scarica reale di una cella LiPo a riposo (mV -> %).
// La libreria usa una retta 3,30-4,15 V: a 3,70 V direbbe 50% quando la batteria e' intorno al 15-20%.
static int socFromMv(int mv) {
  static const int16_t T[][2] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70}, {3910, 65},
    {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35}, {3770, 30}, {3750, 25},
    {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5}, {3270, 0} };
  const int n = sizeof(T) / sizeof(T[0]);
  if (mv >= T[0][0]) return 100;
  for (int i = 1; i < n; i++)
    if (mv >= T[i][0]) return T[i][1] + (T[i - 1][1] - T[i][1]) * (mv - T[i][0]) / (T[i - 1][0] - T[i][0]);
  return 0;
}

static float socSmooth = -1;

static void updateBattery() {
  int mv = M5.Power.getBatteryVoltage();
  if (mv <= 0) return;
  batteryMv = mv;
  if (playing || recording || (boostOn && millis() - boostIdleSince < 2000)) return;   // tensione falsata dal carico audio
  int est;
  if (usbPowered) est = mv >= 4190 ? 100 : min(99, socFromMv(mv - 100));          // in carica la tensione e' piu' alta
  else            est = socFromMv(mv + (screenOn ? 20 : 0));                      // schermo acceso: piccolo calo sotto carico
  if (socSmooth < 0) socSmooth = (rtcSoc >= 0 && !usbPowered && est <= rtcSoc + 5) ? min(est, (int)rtcSoc) : est;
  else socSmooth += 0.25f * (est - socSmooth);
  int shown = (int)(socSmooth + 0.5f);
  // a batteria la percentuale non risale per piccole oscillazioni (solo se sale di molto, es. dopo una ricarica)
  if (!usbPowered && rtcSoc >= 0 && shown > rtcSoc && shown - rtcSoc <= 5) shown = rtcSoc;
  batteryLevel = constrain(shown, 0, 100);
  rtcSoc = batteryLevel;
}

static void updateStats() {
  bool usb = M5.Power.getVBUSVoltage() > 4000;
  if (usb != usbPowered) { usbPowered = usb; applyVolume(); touch(); socSmooth = -1; }
  updateBattery();
  fsTotal = LittleFS.totalBytes();
  fsUsed = LittleFS.usedBytes();
}

// ============================================================ dati per l'app
static void refreshInfo() {
  // formato v5: [u16 mtu][u32 maxBytes][u32 rate][u8 vol][u8 hasSound][u32 received][u32 recBytes]
  //             [u8 batteria][u8 in carica][u32 fs totale][u32 fs usato][u32 byte suono][u8 fw]
  uint8_t buf[35];
  buf[0] = peerMtu & 0xFF; buf[1] = peerMtu >> 8;
  writeU32(buf + 2, MAX_SOUND_BYTES);
  writeU32(buf + 6, SAMPLE_RATE);
  buf[10] = volume;
  buf[11] = slotBytes[activeSlot] ? 1 : 0;
  writeU32(buf + 12, receivedBytes);
  writeU32(buf + 16, selRec < recCount ? recs[selRec].bytes : 0);
  buf[20] = batteryLevel;
  buf[21] = usbPowered && batteryLevel < 100;
  writeU32(buf + 22, fsTotal);
  writeU32(buf + 26, fsUsed);
  writeU32(buf + 30, slotBytes[activeSlot]);
  buf[34] = FW_VERSION;
  chInfo->setValue(buf, sizeof(buf));
}

static void jsonStr(String& out, const char* s) {
  out += '"';
  for (const char* c = s; *c; c++) {
    if (*c == '"' || *c == '\\') out += '\\';
    if ((uint8_t)*c >= 0x20) out += *c;
  }
  out += '"';
}

static String buildCatalog() {
  String j;
  j.reserve(512);
  j += "{\"fw\":"; j += FW_VERSION;
  j += ",\"n\":"; jsonStr(j, devName);
  j += ",\"b\":"; j += batteryLevel;
  j += ",\"mv\":"; j += batteryMv;
  j += ",\"u\":"; j += usbPowered ? 1 : 0;
  j += ",\"v\":"; j += volume;
  j += ",\"a\":"; j += activeSlot;
  j += ",\"fu\":"; j += fsUsed / 1024;
  j += ",\"ft\":"; j += fsTotal / 1024;
  j += ",\"lk\":"; j += rtcLocked ? 1 : 0;
  j += ",\"t\":"; j += (uint32_t)(timeValid() ? time(nullptr) : 0);
  j += ",\"st\":["; j += cfg.repeat; j += ','; j += cfg.punch; j += ','; j += cfg.autolock; j += ','; j += cfg.flip; j += ','; j += cfg.sleepSec; j += ']';
  j += ",\"s\":[";
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (i) j += ',';
    j += '['; j += slotBytes[i]; j += ','; jsonStr(j, slotNames[i]); j += ']';
  }
  j += "],\"r\":[";
  for (int i = 0; i < recCount; i++) {
    if (i) j += ',';
    j += '['; j += recs[i].id; j += ','; j += recs[i].epoch; j += ','; j += recs[i].bytes; j += ']';
  }
  j += "],\"sr\":"; j += selRec;
  j += '}';
  return j;
}

static String buildDiag() {
  BootRec ring[8];
  memset(ring, 0, sizeof(ring));
  if (prefs.getBytesLength("boot") == sizeof(ring)) prefs.getBytes("boot", ring, sizeof(ring));
  uint8_t idx = prefs.getUChar("bidx", 0);
  String d;
  d.reserve(512);
  d += "Tasca Sonora v"; d += FW_VERSION;
  d += " | avvii "; d += prefs.getULong("bcnt", 0);
  const esp_partition_t* run = esp_ota_get_running_partition();
  d += " | app "; d += run ? run->label : "?";
  d += "\nPSRAM libera "; d += ESP.getFreePsram() / 1024; d += " KB | heap "; d += ESP.getFreeHeap() / 1024; d += " KB";
  d += " | protezione batteria "; d += 2000 + (int)(M5.In_I2C.readRegister8(PM1_ADDR, 0x08, 100000) * 7.81f); d += " mV\n";
  d += "tetto volume a batteria "; d += volCeil * 100 / 255; d += "% | minimo ultima riproduzione "; d += prefs.getUShort("pbmin", 0); d += " mV\n";
  for (int k = 0; k < 8; k++) {
    const BootRec& b = ring[(idx + 7 - k) % 8];
    if (!b.fw) continue;
    char line[80];
    if (b.epoch) {
      time_t t = b.epoch + localOffsetMin() * 60;
      struct tm tmv;
      gmtime_r(&t, &tmv);
      snprintf(line, sizeof(line), "%02d/%02d %02d:%02d %s v%d %dmV %s%s\n", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min, reasonName(b.reason), b.fw, b.mv, pm1WakeName(b.pm1wake), b.pad[0] ? " MENTRE SUONAVA" : "");
    } else {
      snprintf(line, sizeof(line), "--/-- --:-- %s v%d %dmV %s%s\n", reasonName(b.reason), b.fw, b.mv, pm1WakeName(b.pm1wake), b.pad[0] ? " MENTRE SUONAVA" : "");
    }
    d += line;
  }
  return d;
}

static void abortUpload() {
  if (uploadFile) uploadFile.close();
  if (LittleFS.exists(TEMP_PATH)) LittleFS.remove(TEMP_PATH);
  if (receivingOta) { Update.abort(); receivingOta = false; }
  receiving = false;
  receivedBytes = 0;
}

// ============================================================ schermo: accensione
static void setBrightness(uint8_t b) {
  if (b == brightness) return;
  M5.Display.setBrightness(b);
  brightness = b;
}

static void wakeScreen() {
  lastUi = millis();
  if (!screenOn) { M5.Display.wakeup(); screenOn = true; }
  setBrightness(BRIGHT_FULL);
}

static void toast(const char* title, const char* sub, uint16_t color, uint8_t icon, uint32_t ms = 1800) {
  strlcpy(toastTitle, title, sizeof(toastTitle));
  strlcpy(toastSub, sub ? sub : "", sizeof(toastSub));
  toastColor = color;
  toastIcon = icon;
  toastUntil = millis() + ms;
  wakeScreen();
}

static void setLocked(bool on, bool fromUser) {
  if (rtcLocked == on) return;
  rtcLocked = on;
  if (on) { menuOpen = false; if (playing) { M5.Speaker.stop(); playing = 0; } }
  if (fromUser) {
    toast(on ? "Tasti bloccati" : "Tasti sbloccati", on ? "Al sicuro in tasca" : "Pronto", on ? C_BRASS : C_GREEN, 2);
    if (on) beep({ 1800, 1200 }, 50); else beep({ 1200, 1800 }, 50);
  }
}

static void updateScreenPower() {
  if (!screenOn) return;
  uint32_t now = millis();
  bool pinned = playing || recording || busy() || menuOpen || holdProgress > 0 || bothProgress > 0 || now < toastUntil || rebootAt;
  if (pinned) { lastUi = now; setBrightness(BRIGHT_FULL); return; }
  uint32_t idle = now - lastUi;
  bool keepLit = btMode || connected;
  if (idle > SCREEN_OFF_MS && !keepLit) {
    setBrightness(0);
    M5.Display.sleep();
    screenOn = false;
    if (cfg.autolock && !rtcLocked && !usbPowered) setLocked(true, false);
  } else if (idle > SCREEN_DIM_MS) {
    setBrightness(BRIGHT_DIM);
  } else {
    setBrightness(BRIGHT_FULL);
  }
}

// ============================================================ schermo: disegno
static void text(const char* s, int x, int y, const lgfx::IFont* font, uint16_t color, lgfx::textdatum_t datum = top_left) {
  cv.setFont(font);
  cv.setTextColor(color);
  cv.setTextDatum(datum);
  cv.drawString(s, x, y);
}

// Testo che non supera maxW pixel: prima prova il carattere di ripiego (piu' piccolo),
// poi, solo se serve ancora, accorcia e aggiunge "..."
static void textFit(const char* s, int x, int y, int maxW, const lgfx::IFont* font, uint16_t color,
                    const lgfx::IFont* fallback = nullptr) {
  cv.setFont(font);
  if (fallback && cv.textWidth(s) > maxW) { font = fallback; cv.setFont(font); }
  char buf[48];
  strlcpy(buf, s, sizeof(buf));
  if (cv.textWidth(buf) > maxW) {
    size_t n = strlen(buf);
    while (n > 1) {
      buf[--n] = 0;
      char t[52];
      snprintf(t, sizeof(t), "%s...", buf);
      if (cv.textWidth(t) <= maxW) { strlcpy(buf, t, sizeof(buf)); break; }
    }
  }
  text(buf, x, y, font, color);
}

// Come textFit ma centrato orizzontalmente sullo schermo
static void textCenterFit(const char* s, int y, const lgfx::IFont* font, uint16_t color, const lgfx::IFont* fallback = nullptr) {
  const int maxW = cv.width() - 12;
  cv.setFont(font);
  if (fallback && cv.textWidth(s) > maxW) font = fallback;
  cv.setFont(font);
  int w = min((int)cv.textWidth(s), maxW);
  textFit(s, (cv.width() - w) / 2, y, maxW, font, color);
}

static void drawBattery(int x, int y, int lvl, bool charging) {
  uint16_t col = lvl <= 10 ? C_RED : (lvl <= 25 ? C_BRASS : C_GREEN);
  cv.drawRoundRect(x, y, 22, 11, 2, C_MUTED);
  cv.fillRect(x + 22, y + 3, 2, 5, C_MUTED);
  cv.fillRect(x + 2, y + 2, max(1, 18 * lvl / 100), 7, col);
  if (charging) {
    cv.fillTriangle(x + 12, y + 1, x + 7, y + 6, x + 11, y + 6, C_TEXT);
    cv.fillTriangle(x + 10, y + 5, x + 15, y + 5, x + 10, y + 10, C_TEXT);
  }
}

static void drawBt(int x, int y, int h, uint16_t c, int thick = 1) {
  int w = h / 2, cx = x + w / 2;
  for (int t = 0; t < thick; t++) {
    int o = t - thick / 2;
    cv.drawLine(cx + o, y, cx + o, y + h, c);
    cv.drawLine(cx + o, y, cx + w / 2 + o, y + h / 4, c);
    cv.drawLine(cx + w / 2 + o, y + h / 4, cx - w / 2 + o, y + 3 * h / 4, c);
    cv.drawLine(cx + o, y + h, cx + w / 2 + o, y + 3 * h / 4, c);
    cv.drawLine(cx + w / 2 + o, y + 3 * h / 4, cx - w / 2 + o, y + h / 4, c);
  }
}

static void drawLock(int x, int y, uint16_t c, uint16_t bg) {   // lucchetto 14x16
  cv.fillRoundRect(x, y + 7, 14, 10, 2, c);
  cv.drawRoundRect(x + 3, y, 8, 11, 4, c);
  cv.drawRoundRect(x + 4, y + 1, 6, 10, 3, c);
  cv.fillRect(x + 6, y + 10, 2, 4, bg);
}

static void drawSpeaker(int x, int y, uint16_t c) {
  cv.fillRect(x, y + 4, 4, 6, c);
  cv.fillTriangle(x + 3, y + 7, x + 9, y + 1, x + 9, y + 13, c);
  cv.drawLine(x + 12, y + 4, x + 12, y + 10, c);
  cv.drawLine(x + 15, y + 2, x + 15, y + 12, c);
}

static void drawCheck(int cx, int cy, uint16_t c) {
  for (int t = -1; t <= 1; t++) {
    cv.drawLine(cx - 9, cy + t, cx - 3, cy + 6 + t, c);
    cv.drawLine(cx - 3, cy + 6 + t, cx + 9, cy - 6 + t, c);
  }
}

static void drawCross(int cx, int cy, uint16_t c) {
  for (int t = -1; t <= 1; t++) {
    cv.drawLine(cx - 7 + t, cy - 7, cx + 7 + t, cy + 7, c);
    cv.drawLine(cx + 7 + t, cy - 7, cx - 7 + t, cy + 7, c);
  }
}

static void bar(int x, int y, int w, int h, float frac, uint16_t col) {
  frac = constrain(frac, 0.0f, 1.0f);
  cv.fillRoundRect(x, y, w, h, h / 2, C_PANEL);
  if (frac > 0) cv.fillRoundRect(x, y, max(h, (int)(w * frac)), h, h / 2, col);
}

static void drawWave(int x, int y, int w, int h, float played) {
  int mid = y + h / 2;
  for (int i = 0; i < w; i += 3) {
    int b = i * PEAK_BINS / w;
    int a = max(1, peaks[b] * (h / 2) / 255);
    bool done = (float)i / w <= played;
    cv.fillRect(x + i, mid - a, 2, a * 2, done ? C_BRASS : C_PANEL);
  }
  if (played > 0 && played < 1) cv.drawFastVLine(x + (int)(w * played), y, h, C_TEXT);
}

static void statusBar() {
  int W = cv.width();
  if (timeValid()) {
    time_t t = time(nullptr) + localOffsetMin() * 60;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    text(hm, 10, 4, FONT_SMALL, C_TEXT);
  } else {
    text("Tasca Sonora", 10, 4, FONT_SMALL, C_BRASS);
  }
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", batteryLevel);
  text(pct, W - 36, 4, FONT_SMALL, batteryLevel <= 10 && !usbPowered ? C_RED : C_MUTED, top_right);
  drawBattery(W - 32, 6, batteryLevel, usbPowered && batteryLevel < 100);
  int x = W - 36 - cv.textWidth(pct) - 14;
  if (btMode || connected) { drawBt(x, 4, 13, connected ? C_CYAN : C_MUTED); x -= 18; }
  if (rtcLocked) drawLock(x - 2, 2, C_BRASS, C_BG);
}

static void hint(const char* s) { text(s, cv.width() / 2, 117, FONT_SMALL, C_MUTED, top_center); }

static void viewIdle() {
  char name[40], line[48], s[16];
  slotLabel(name, sizeof(name), activeSlot);
  drawSpeaker(12, 31, soundSamples ? C_BRASS : C_MUTED);
  textFit(name, 34, 26, cv.width() - 46, FONT_BIG, soundSamples ? C_TEXT : C_MUTED, FONT_SMALLB);
  if (soundSamples) { fmtSeconds(s, sizeof(s), soundSamples / (float)SAMPLE_RATE); snprintf(line, sizeof(line), "Suono %d di %d  -  %s", activeSlot + 1, NUM_SLOTS, s); }
  else snprintf(line, sizeof(line), "Suono %d di %d  -  vuoto", activeSlot + 1, NUM_SLOTS);
  text(line, 34, 54, FONT_SMALL, C_MUTED);
  cv.fillCircle(19, 89, 5, recCount ? C_RED : C_MUTED);
  if (recCount) {
    fmtSeconds(s, sizeof(s), recs[0].bytes / 2 / (float)SAMPLE_RATE);
    snprintf(line, sizeof(line), "Registrazioni: %d  -  %s", recCount, s);
  } else {
    strlcpy(line, "Nessuna registrazione", sizeof(line));
  }
  text(line, 34, 81, FONT_SMALL, recCount ? C_TEXT : C_MUTED);
  if (rtcLocked) hint("Bloccato: premi accensione");
  else if (usbPowered) hint(batteryLevel < 100 ? "In carica" : "Carica completa");
  else if (batteryLevel <= 10) hint("Batteria quasi scarica");
  else hint("Premi suona, tieni Bluetooth");
}

static void viewPlaying() {
  const char* title = playing == 2 ? "Registrazione" : (playing == 3 ? "Anteprima" : "Suona");
  text(title, 12, 28, FONT_BIG, playing == 2 ? C_RED : C_BRASS);
  if (cfg.repeat && playing == 1) text("ripeti", cv.width() - 12, 32, FONT_SMALL, C_MUTED, top_right);
  float frac = playMs ? (millis() - playStart) / (float)playMs : 1;
  drawWave(12, 56, cv.width() - 24, 50, frac);
  char a[16], b[16], line[40];
  fmtSeconds(a, sizeof(a), min(frac, 1.0f) * playMs / 1000.0f);
  fmtSeconds(b, sizeof(b), playMs / 1000.0f);
  snprintf(line, sizeof(line), "%s / %s", a, b);
  hint(line);
}

static void viewRecording() {
  if ((millis() / 400) % 2) cv.fillCircle(20, 40, 7, C_RED);
  else cv.drawCircle(20, 40, 7, C_RED);
  text("Registro", 34, 28, FONT_BIG, C_RED);
  char s[16];
  fmtSeconds(s, sizeof(s), recPos / (float)SAMPLE_RATE);
  text(s, cv.width() - 12, 30, FONT_BIG, C_TEXT, top_right);
  float db = recLevel > 0 ? 20.0f * log10f(recLevel / 32767.0f) : -60;
  int lit = constrain((int)((db + 54) / 54 * 24), 0, 24);
  for (int i = 0; i < 24; i++) {
    uint16_t c = i < lit ? (i >= 21 ? C_RED : (i >= 16 ? C_BRASS : C_GREEN)) : C_PANEL;
    cv.fillRoundRect(12 + i * 9, 64, 7, 26, 2, c);
  }
  bar(12, 100, cv.width() - 24, 5, recPos / (float)(MAX_REC_BYTES / 2), C_RED);
  hint("Rilascia per salvare");
}

static void viewHold() {
  drawBt(18, 30, 26, C_CYAN, 2);
  text("Bluetooth", 40, 32, FONT_BIG, C_TEXT);
  text(btMode ? "Continua per spegnerlo" : "Continua per accenderlo", 12, 70, FONT_SMALL, C_MUTED);
  bar(12, 96, cv.width() - 24, 8, holdProgress, C_CYAN);
}

static void viewBoth() {
  drawLock(14, 30, C_BRASS, C_BG);
  text(rtcLocked ? "Sblocco" : "Blocco tasti", 40, 30, FONT_BIG, C_TEXT);
  text("Tieni premuti i due tasti", 12, 70, FONT_SMALL, C_MUTED);
  bar(12, 96, cv.width() - 24, 8, bothProgress, C_BRASS);
}

static void viewLocked() {
  cv.fillCircle(36, 70, 24, C_PANEL);
  drawLock(29, 61, C_BRASS, C_PANEL);
  text("Bloccato", 76, 40, FONT_BIG, C_TEXT);
  text("Premi accensione o", 76, 70, FONT_SMALL, C_MUTED);
  text("tieni i due tasti", 76, 88, FONT_SMALL, C_MUTED);
}

static const char* MENU_TITLES[] = { "Suono", "Volume", "Ripeti", "Audio", "Blocco auto", "Schermo", "Spegnimento", "Info" };
#define MENU_PAGES 8

static void viewMenu() {
  int W = cv.width();
  for (int i = 0; i < MENU_PAGES; i++)                         // puntini di pagina
    cv.fillCircle(W / 2 - (MENU_PAGES - 1) * 6 + i * 12, 112, i == menuPage ? 3 : 2, i == menuPage ? C_BRASS : C_PANEL);
  text(MENU_TITLES[menuPage], 12, 26, FONT_BIG, C_BRASS);
  char v[48], s[16];
  switch (menuPage) {
    case 0: {
      slotLabel(v, sizeof(v), activeSlot);
      textFit(v, 12, 56, W - 24, FONT_BIG, slotBytes[activeSlot] ? C_TEXT : C_MUTED, FONT_SMALLB);
      if (slotBytes[activeSlot]) { fmtSeconds(s, sizeof(s), slotBytes[activeSlot] / 2 / (float)SAMPLE_RATE); snprintf(v, sizeof(v), "%d di %d  -  %s", activeSlot + 1, NUM_SLOTS, s); }
      else snprintf(v, sizeof(v), "%d di %d  -  vuoto", activeSlot + 1, NUM_SLOTS);
      text(v, 12, 84, FONT_SMALL, C_MUTED);
      break;
    }
    case 1: {
      snprintf(v, sizeof(v), "%d%%", volume);
      text(v, W - 12, 24, FONT_HUGE, C_TEXT, top_right);
      for (int i = 0; i < 10; i++) {
        bool on = volume >= (i + 1) * 10;
        int h = 8 + i * 3;
        cv.fillRoundRect(14 + i * 21, 100 - h, 16, h, 3, on ? C_BRASS : C_PANEL);
      }
      break;
    }
    case 2: text(cfg.repeat ? "Attivo" : "Spento", 12, 58, FONT_HUGE, cfg.repeat ? C_GREEN : C_MUTED);
            text("Riparte finche' non lo fermi", 12, 92, FONT_SMALL, C_MUTED); break;
    case 3: text(cfg.punch == 2 ? "Massimo" : (cfg.punch == 1 ? "Forte" : "Naturale"), 12, 58, FONT_HUGE, cfg.punch ? C_BRASS : C_TEXT);
            text(cfg.punch == 2 ? "Il piu' forte possibile" : (cfg.punch == 1 ? "Piu' forte, meno bassi" : "Come l'hai caricato"), 12, 92, FONT_SMALL, C_MUTED); break;
    case 4: text(cfg.autolock ? "Attivo" : "Spento", 12, 58, FONT_HUGE, cfg.autolock ? C_GREEN : C_MUTED);
            text("Blocca a schermo spento", 12, 92, FONT_SMALL, C_MUTED); break;
    case 5: text(cfg.flip ? "Capovolto" : "Normale", 12, 58, FONT_HUGE, C_TEXT);
            text("Gira lo schermo sottosopra", 12, 92, FONT_SMALL, C_MUTED); break;
    case 6: snprintf(v, sizeof(v), "%d s", cfg.sleepSec);
            text(v, 12, 58, FONT_HUGE, C_TEXT);
            text("Si addormenta dopo", 12, 92, FONT_SMALL, C_MUTED); break;
    case 7: {
      char nm[28];
      asciiName(nm, sizeof(nm), devName);
      snprintf(v, sizeof(v), "v%d  -  %d mV", FW_VERSION, batteryMv);
      text(v, 12, 54, FONT_SMALL, C_TEXT);
      snprintf(v, sizeof(v), "Memoria %u / %u KB", (unsigned)(fsUsed / 1024), (unsigned)(fsTotal / 1024));
      text(v, 12, 72, FONT_SMALL, C_TEXT);
      textFit(nm, 12, 90, W - 24, FONT_SMALL, C_MUTED);
      break;
    }
  }
}

static void viewTransfer(const char* title, size_t done, size_t total) {
  drawBt(16, 30, 22, C_CYAN, 2);
  text(title, 36, 31, FONT_BIG, C_TEXT);
  char s[8];
  snprintf(s, sizeof(s), "%u%%", (unsigned)(total ? (uint64_t)done * 100 / total : 0));
  text(s, cv.width() / 2, 62, FONT_HUGE, C_CYAN, top_center);
  bar(12, 102, cv.width() - 24, 6, total ? done / (float)total : 0, C_CYAN);
}

static void viewToast() {
  int cx = cv.width() / 2, cy = 44;
  cv.fillCircle(cx, cy, 16, toastColor);
  if (toastIcon == 1) drawCheck(cx, cy, C_BG);
  else if (toastIcon == 2) drawLock(cx - 7, cy - 8, C_BG, toastColor);
  else drawCross(cx, cy, C_BG);
  textCenterFit(toastTitle, 67, FONT_BIG, C_TEXT, FONT_SMALLB);
  textCenterFit(toastSub, 94, FONT_SMALL, C_MUTED);
}

static void viewBluetooth() {
  int cx = 40, cy = 70;
  char nm[28];
  asciiName(nm, sizeof(nm), devName);
  if (connected) {
    cv.fillCircle(cx, cy, 26, C_CYAN);
    drawBt(cx - 8, cy - 15, 30, C_BG, 3);
    text("Collegato", 80, 44, FONT_BIG, C_TEXT);
    textFit(nm, 80, 72, cv.width() - 88, FONT_SMALL, C_MUTED);
    hint("Tieni premuto per spegnere");
    return;
  }
  uint32_t now = millis();
  int32_t left = max((int32_t)0, (int32_t)(btUntil - now));
  float frac = left / (float)BT_WINDOW_MS;
  cv.fillArc(cx, cy, 28, 25, -90, -90 + (int)(360 * frac), C_CYAN);
  cv.fillCircle(cx, cy, 20, C_PANEL);
  drawBt(cx - 7, cy - 13, 26, (now / 500) % 2 ? C_CYAN : C_TEXT, 3);
  text("Bluetooth", 80, 30, FONT_BIG, C_TEXT);
  text("in attesa", 80, 54, FONT_SMALL, C_CYAN);
  textFit(nm, 80, 72, cv.width() - 88, FONT_SMALL, C_MUTED);
  text("Apri l'app: Collega", 80, 90, FONT_SMALL, C_MUTED);
  char s[16];
  snprintf(s, sizeof(s), "%d:%02d", (int)(left / 60000), (int)(left / 1000) % 60);
  hint(s);
}

static void render(bool force = false) {
  if (!screenOn) return;
  static uint32_t last = 0;
  uint32_t now = millis();
  bool anim = playing || recording || busy() || holdProgress > 0 || bothProgress > 0 || btMode;
  if (!force && now - last < (anim ? 50U : 250U)) return;
  last = now;
  cv.fillSprite(C_BG);
  statusBar();
  if (rebootAt)                 { text("Aggiornato", 12, 40, FONT_HUGE, C_GREEN); text("Riavvio...", 12, 86, FONT_SMALL, C_MUTED); }
  else if (recording)           viewRecording();
  else if (bothProgress > 0)    viewBoth();
  else if (holdProgress > 0)    viewHold();
  else if (receivingOta)        viewTransfer("Aggiornamento", receivedBytes, expectedBytes);
  else if (receiving)           viewTransfer("Ricevo il suono", receivedBytes, expectedBytes);
  else if (downloading)         viewTransfer("Invio al telefono", dlPos, dlTotal);
  else if (now < toastUntil)    viewToast();
  else if (menuOpen)            viewMenu();
  else if (playing)             viewPlaying();
  else if (btMode || connected) viewBluetooth();
  else if (rtcLocked)           viewLocked();
  else                          viewIdle();
  cv.pushSprite(0, 0);
}

static void applyRotation() {
  M5.Display.setRotation(cfg.flip ? 3 : 1);
  if (screenOn) render(true);
}

static void splash() {
  cv.fillSprite(C_BG);
  int W = cv.width();
  text("Tasca", W / 2, 38, FONT_HUGE, C_BRASS, top_center);
  text("Sonora", W / 2, 76, FONT_BIG, C_TEXT, top_center);
  char v[16];
  snprintf(v, sizeof(v), "v%d", FW_VERSION);
  text(v, W / 2, 112, FONT_SMALL, C_MUTED, top_center);
  cv.pushSprite(0, 0);
  M5.Display.wakeup();
  screenOn = true;
  for (int b = 0; b <= BRIGHT_FULL; b += 10) { M5.Display.setBrightness(b); delay(18); }
  brightness = BRIGHT_FULL;
  delay(600);
}

// ============================================================ Bluetooth
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    if (!btMode) { s->disconnect(ci.getConnHandle()); return; }
    connected = true;
    justConnected = true;
    NimBLEDevice::getAdvertising()->stop();
    s->updateConnParams(ci.getConnHandle(), 6, 12, 0, 300);
    btUntil = millis() + BT_WINDOW_MS;
    touch();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    connected = false;
    justDisconnected = true;
    peerMtu = 23;
    if (receiving || receivingOta) abortUpload();
    if (btMode) NimBLEDevice::getAdvertising()->start();
    touch();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override { peerMtu = mtu; }
};

class InfoCB : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic*, NimBLEConnInfo&) override {
    refreshInfo();
    btUntil = millis() + BT_WINDOW_MS;
  }
};

class CatalogCB : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    String j = buildCatalog();
    c->setValue((const uint8_t*)j.c_str(), j.length());
    btUntil = millis() + BT_WINDOW_MS;
  }
};

class DiagCB : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    String d = buildDiag();
    c->setValue((const uint8_t*)d.c_str(), min((size_t)d.length(), (size_t)500));
  }
};

class DataCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    size_t n = v.size();
    if (receivingOta) {
      if (receivedBytes + n > expectedBytes) { abortUpload(); notifyStatus(ST_ERR_SIZE); return; }
      if (Update.write((uint8_t*)v.data(), n) != n) { abortUpload(); notifyStatus(ST_ERR_OTA); return; }
    } else if (receiving && uploadFile) {
      if (receivedBytes + n > expectedBytes) { abortUpload(); notifyStatus(ST_ERR_SIZE); return; }
      if (uploadFile.write(v.data(), n) != n) { abortUpload(); notifyStatus(ST_ERR_FS); return; }
    } else {
      return;
    }
    runningCrc = crc32Update(runningCrc, v.data(), n);
    receivedBytes += n;
    btUntil = millis() + BT_WINDOW_MS;
    touch();
  }
};

static void beginUpload(uint8_t slot, uint32_t size) {
  if (playing || recording || downloading || receivingOta) { notifyStatus(ST_ERR_BUSY); return; }
  if (slot >= NUM_SLOTS) { notifyStatus(ST_ERR_ARG); return; }
  if (size == 0 || size > MAX_SOUND_BYTES || (size & 1)) { notifyStatus(ST_ERR_SIZE); return; }
  abortUpload();
  if (freeBytes() + slotBytes[slot] < size + FS_MARGIN) { notifyStatus(ST_ERR_SPACE); return; }
  if (freeBytes() < size + FS_MARGIN && slotBytes[slot]) {   // non c'e' posto per vecchio e nuovo insieme
    char p[16];
    slotPath(p, sizeof(p), slot);
    LittleFS.remove(p);
    slotBytes[slot] = 0;
    if (auxSlot == slot) auxSlot = -1;
    if (slot == activeSlot) reloadSlot = true;
  }
  uploadFile = LittleFS.open(TEMP_PATH, "w");
  if (!uploadFile) { notifyStatus(ST_ERR_FS); return; }
  uploadSlot = slot;
  expectedBytes = size; receivedBytes = 0; runningCrc = 0; receiving = true;
  notifyStatus(ST_READY);
}

class CtrlCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    size_t n = v.size();
    if (n < 1) return;
    const uint8_t* d = v.data();
    btUntil = millis() + BT_WINDOW_MS;
    touch();
    switch (d[0]) {
      case CMD_START:
        if (n < 5) { notifyStatus(ST_ERR_STATE); return; }
        beginUpload(activeSlot, readU32(d + 1));
        break;
      case CMD_START_SLOT:
        if (n < 6) { notifyStatus(ST_ERR_ARG); return; }
        beginUpload(d[1], readU32(d + 2));
        break;
      case CMD_END: {
        if (!receiving || n < 5) { notifyStatus(ST_ERR_STATE); return; }
        uint32_t crc = readU32(d + 1);
        uploadFile.close();
        receiving = false;
        if (receivedBytes != expectedBytes) { LittleFS.remove(TEMP_PATH); notifyStatus(ST_ERR_SIZE); return; }
        if (crc != runningCrc)              { LittleFS.remove(TEMP_PATH); notifyStatus(ST_ERR_CRC);  return; }
        char p[16];
        slotPath(p, sizeof(p), uploadSlot);
        if (LittleFS.exists(p)) LittleFS.remove(p);
        if (!LittleFS.rename(TEMP_PATH, p)) { notifyStatus(ST_ERR_FS); return; }
        slotBytes[uploadSlot] = receivedBytes;
        if (auxSlot == uploadSlot) auxSlot = -1;
        if (uploadSlot == activeSlot) reloadSlot = true;
        notifyStatus(ST_SAVED);
        break;
      }
      case CMD_OTA_BEGIN: {
        if (n < 5) { notifyStatus(ST_ERR_ARG); return; }
        if (busy() || playing) { notifyStatus(ST_ERR_BUSY); return; }
        uint32_t size = readU32(d + 1);
        abortUpload();
        if (size < 100000 || !Update.begin(size, U_FLASH)) { notifyStatus(ST_ERR_OTA); return; }
        expectedBytes = size; receivedBytes = 0; runningCrc = 0; receivingOta = true;
        notifyStatus(ST_READY);
        break;
      }
      case CMD_OTA_END: {
        if (!receivingOta || n < 5) { notifyStatus(ST_ERR_STATE); return; }
        uint32_t crc = readU32(d + 1);
        receivingOta = false;
        if (receivedBytes != expectedBytes || crc != runningCrc) { Update.abort(); notifyStatus(ST_ERR_CRC); return; }
        if (!Update.end(true)) { notifyStatus(ST_ERR_OTA); return; }
        notifyStatus(ST_OTA_OK);
        rebootAt = millis() + 1500;
        break;
      }
      case CMD_PLAY:     if (!busy()) playRequested = true; break;
      case CMD_PLAY_REC: if (!busy()) playRecRequested = true; break;
      case CMD_STOP:     stopReq = true; break;
      case CMD_FIND:     if (!busy()) findReq = true; break;
      case CMD_ABORT:    abortUpload(); break;
      case CMD_VOLUME:
        if (n >= 2) { volume = min<uint8_t>(d[1], 100); applyVolume(); settingsDirty = true; }
        break;
      case CMD_DOWNLOAD_SOUND:
      case CMD_DOWNLOAD_SLOT: {
        int slot = d[0] == CMD_DOWNLOAD_SLOT && n >= 2 ? d[1] : activeSlot;
        if (slot >= NUM_SLOTS) { notifyStatus(ST_ERR_ARG); return; }
        if (!slotBytes[slot]) { notifyStatus(ST_ERR_NOREC); return; }
        if (busy()) { notifyStatus(ST_ERR_BUSY); return; }
        downloadSlotReq = slot;
        break;
      }
      case CMD_DOWNLOAD_REC:
        if (selRec >= recCount) { notifyStatus(ST_ERR_NOREC); return; }
        if (busy()) { notifyStatus(ST_ERR_BUSY); return; }
        downloadRecReq = true;
        break;
      case CMD_REC_TO_SOUND:
      case CMD_REC_TO_SLOT: {
        int slot = d[0] == CMD_REC_TO_SLOT && n >= 2 ? d[1] : activeSlot;
        if (slot >= NUM_SLOTS) { notifyStatus(ST_ERR_ARG); return; }
        if (selRec >= recCount) { notifyStatus(ST_ERR_NOREC); return; }
        if (busy()) { notifyStatus(ST_ERR_BUSY); return; }
        recToSlotReq = slot;
        break;
      }
      case CMD_DELETE_SOUND:
      case CMD_DELETE_SLOT: {
        int slot = d[0] == CMD_DELETE_SLOT && n >= 2 ? d[1] : activeSlot;
        if (slot >= NUM_SLOTS) { notifyStatus(ST_ERR_ARG); return; }
        if (busy()) { notifyStatus(ST_ERR_BUSY); return; }
        deleteSlotReq = slot;
        break;
      }
      case CMD_DELETE_REC:
        if (selRec >= recCount) { notifyStatus(ST_ERR_NOREC); return; }
        if (busy()) { notifyStatus(ST_ERR_BUSY); return; }
        deleteRecReq = true;
        break;
      case CMD_SELECT_SLOT:
        if (n < 2 || d[1] >= NUM_SLOTS) { notifyStatus(ST_ERR_ARG); return; }
        selectSlotReq = d[1];               // la conferma arriva dal loop, a cambio fatto
        break;
      case CMD_PLAY_SLOT:
        if (n < 2 || d[1] >= NUM_SLOTS) { notifyStatus(ST_ERR_ARG); return; }
        if (!busy()) playSlotReq = d[1];
        break;
      case CMD_SELECT_REC:
        if (n < 2 || d[1] >= max<uint8_t>(recCount, 1)) { notifyStatus(ST_ERR_ARG); return; }
        selRec = d[1];
        notifyStatus(ST_OK);
        break;
      case CMD_SET_SLOT_NAME: {
        if (n < 2 || d[1] >= NUM_SLOTS) { notifyStatus(ST_ERR_ARG); return; }
        copyName(slotNames[d[1]], sizeof(slotNames[d[1]]), d + 2, n - 2);
        char key[4] = { 'n', char('0' + d[1]), 0 };
        prefs.putString(key, slotNames[d[1]]);
        notifyStatus(ST_OK);
        break;
      }
      case CMD_SET_DEVICE_NAME: {
        char tmp[24];
        copyName(tmp, 21, d + 1, n - 1);
        strlcpy(devName, tmp[0] ? tmp : DEFAULT_NAME, sizeof(devName));
        prefs.putString("dev", devName);
        refreshNameReq = true;
        notifyStatus(ST_OK);
        break;
      }
      case CMD_SET_TIME: {
        if (n < 5) { notifyStatus(ST_ERR_ARG); return; }
        struct timeval tv = { (time_t)readU32(d + 1), 0 };
        settimeofday(&tv, nullptr);
        if (n >= 7) tzMinutes = (int16_t)(d[5] | (d[6] << 8));
        if (n >= 13) { tzNextAt = readU32(d + 7); tzNextMinutes = (int16_t)(d[11] | (d[12] << 8)); }
        else tzNextAt = 0;
        settingsDirty = true;
        notifyStatus(ST_OK);
        break;
      }
      case CMD_SET_SETTING: {
        if (n < 3) { notifyStatus(ST_ERR_ARG); return; }
        uint8_t val = d[2];
        switch (d[1]) {
          case SET_REPEAT:   cfg.repeat = val ? 1 : 0; break;
          case SET_PUNCH:    cfg.punch = min<uint8_t>(val, 2); reloadSlot = true; break;
          case SET_AUTOLOCK: cfg.autolock = val ? 1 : 0; break;
          case SET_FLIP:     cfg.flip = val ? 1 : 0; break;
          case SET_SLEEP: {
            uint16_t s = (uint16_t)val * 5;
            bool ok = false;
            for (uint16_t c2 : SLEEP_CHOICES) if (c2 == s) ok = true;
            if (!ok) { notifyStatus(ST_ERR_ARG); return; }
            cfg.sleepSec = s;
            break;
          }
          default: notifyStatus(ST_ERR_ARG); return;
        }
        settingsDirty = true;
        notifyStatus(ST_OK);
        break;
      }
      case CMD_LOCK:
        lockReq = n >= 2 && d[1] ? 1 : 0;
        notifyStatus(ST_OK);
        break;
      default:
        notifyStatus(ST_ERR_ARG);
        break;
    }
  }
};

static void applyAdvertisingName() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(SVC_UUID);
  NimBLEAdvertisementData scanData;
  scanData.setName(devName);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  NimBLEDevice::setDeviceName(devName);
}

static void initBle() {
  if (bleReady) return;
  NimBLEDevice::init(devName);
  NimBLEDevice::setMTU(247);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCB());
  bleServer->advertiseOnDisconnect(false);
  NimBLEService* svc = bleServer->createService(SVC_UUID);
  svc->createCharacteristic(CTRL_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(new CtrlCB());
  svc->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::WRITE_NR)->setCallbacks(new DataCB());
  chStatus = svc->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chInfo = svc->createCharacteristic(INFO_UUID, NIMBLE_PROPERTY::READ);
  chInfo->setCallbacks(new InfoCB());
  chDl = svc->createCharacteristic(DL_UUID, NIMBLE_PROPERTY::NOTIFY);
  chCatalog = svc->createCharacteristic(CATALOG_UUID, NIMBLE_PROPERTY::READ, 512);
  chCatalog->setCallbacks(new CatalogCB());
  chDiag = svc->createCharacteristic(DIAG_UUID, NIMBLE_PROPERTY::READ, 512);
  chDiag->setCallbacks(new DiagCB());
  refreshInfo();
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setMinInterval(48);
  adv->setMaxInterval(96);
  applyAdvertisingName();
  bleReady = true;
}

static void startBt() {
  initBle();
  btMode = true;
  btUntil = millis() + BT_WINDOW_MS;
  if (!connected) NimBLEDevice::getAdvertising()->start();
  wakeScreen();
  beep({ 1600, 2400 });
}

static void stopBt() {
  btMode = false;
  if (bleReady) NimBLEDevice::getAdvertising()->stop();
  if (connected && bleServer) for (uint16_t h : bleServer->getPeerDevices()) bleServer->disconnect(h);
  wakeScreen();
  beep({ 2400, 1600 });
}

static void streamPcm(const int16_t* buf, size_t samples) {
  downloading = true;
  dlPos = 0;
  dlTotal = samples * 2;
  wakeScreen();
  const uint8_t* p = (const uint8_t*)buf;
  size_t bytes = samples * 2;
  size_t chunk = min((size_t)240, (size_t)(peerMtu > 23 ? peerMtu - 3 : 20));
  uint32_t crc = crc32Update(0, p, bytes);
  while (dlPos < bytes) {
    size_t n = min(chunk, bytes - dlPos);
    uint32_t t = millis();
    while (!chDl->notify(p + dlPos, n)) {
      if (!connected || millis() - t > 3000) { downloading = false; notifyStatus(ST_ERR_STATE); return; }
      delay(4);
    }
    dlPos += n;
    btUntil = millis() + BT_WINDOW_MS;
    touch();
    render();
  }
  downloading = false;
  delay(40);                                   // lascia uscire gli ultimi pacchetti prima della conferma
  uint8_t extra[4];
  writeU32(extra, crc);
  notifyStatus(ST_DL_DONE, extra, 4);          // resta anche leggibile: l'app la rilegge se la notifica si perde
}

// ============================================================ azioni
static void startPlayback(const int16_t* buf, size_t n, uint8_t kind) {
  if (!usbPowered && !pbGuard) {
    prefs.putUChar("pb", 1);
    pbGuard = true; pbSagged = false; pbMinMv = 9999;
    applyVolume();
  }
  audioPower(true);
  M5.Speaker.stop();
  M5.Speaker.playRaw(buf, n, SAMPLE_RATE, false, 1, 0, true);
  playing = kind;
  lastPlayKind = kind;
  userStopped = false;
  playStart = millis();
  playMs = (uint32_t)((uint64_t)n * 1000 / SAMPLE_RATE);
}

static bool batteryTooLow() {
  if (!usbPowered && batteryMv > 0 && batteryMv < CRITICAL_MV) {
    toast("Batteria scarica", "Ricarica con la USB", C_RED, 0);
    return true;
  }
  return false;
}

static void playActive() {
  if (!soundSamples) { toast("Nessun suono", "Caricalo dall'app", C_MUTED, 0); return; }
  if (batteryTooLow()) return;
  const int16_t* buf = (cfg.punch && punchReady) ? punchBuf : soundBuf;
  computePeaks(buf, soundSamples);
  startPlayback(buf, soundSamples, 1);
  touch(); wakeScreen();
}

static void playRec(uint8_t index) {
  if (!ensureRecLoaded(index)) { toast("Nessuna registrazione", "Tieni premuto il laterale", C_MUTED, 0); return; }
  if (batteryTooLow()) return;
  const int16_t* buf = recBuf;
  if (cfg.punch && psBuf(recLoud, MAX_REC_BYTES)) {
    if (recLoudId != recBufId || recLoudMode != cfg.punch) { loudFast(recBuf, recLoud, recSamples); recLoudId = recBufId; recLoudMode = cfg.punch; }
    buf = recLoud;
  }
  computePeaks(buf, recSamples);
  startPlayback(buf, recSamples, 2);
  touch(); wakeScreen();
}

static void playSlotPreview(int slot) {
  if (slot == activeSlot) { playActive(); return; }
  if (!ensureAuxSlot(slot)) { toast("Suono vuoto", "", C_MUTED, 0); return; }
  if (batteryTooLow()) return;
  const int16_t* buf = auxBuf;
  if (cfg.punch && psBuf(auxLoud, MAX_SOUND_BYTES)) {
    if (auxLoudSlot != auxSlot || auxLoudMode != cfg.punch) { loudFast(auxBuf, auxLoud, auxSamples); auxLoudSlot = auxSlot; auxLoudMode = cfg.punch; }
    buf = auxLoud;
  }
  computePeaks(buf, auxSamples);
  startPlayback(buf, auxSamples, 3);
  touch(); wakeScreen();
}

static void stopPlayback() {
  M5.Speaker.stop();
  playing = 0;
  userStopped = true;
}

static size_t cleanRecording(int16_t* buf, size_t n) {
  size_t skip = min(n, (size_t)(SAMPLE_RATE * 8 / 100));
  memmove(buf, buf + skip, (n - skip) * 2);
  n -= skip;
  if (!n) return 0;
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += buf[i];
  int32_t dc = sum / (int64_t)n, peak = 1;
  for (size_t i = 0; i < n; i++) peak = max(peak, abs((int32_t)buf[i] - dc));
  int32_t gain256 = min((int32_t)(29000LL * 256 / peak), (int32_t)(8 * 256));
  size_t fade = min(n / 2, (size_t)(SAMPLE_RATE / 50));
  for (size_t i = 0; i < n; i++) {
    int32_t s = ((int32_t)buf[i] - dc) * gain256 / 256;
    if (i < fade) s = s * (int32_t)i / (int32_t)fade;
    if (i >= n - fade) s = s * (int32_t)(n - i) / (int32_t)fade;
    buf[i] = (int16_t)constrain(s, -32768, 32767);
  }
  return n;
}

static void recordWhileHeld() {
  if (receiving || receivingOta || downloading) return;
  // spazio: se serve elimina le registrazioni piu' vecchie
  while (freeBytes() < MAX_REC_BYTES + FS_MARGIN && recCount > 0) deleteRecAt(recCount - 1);
  if (freeBytes() < MAX_REC_BYTES / 4) { toast("Memoria piena", "Elimina qualche suono", C_RED, 0); return; }
  if (!psBuf(recBuf, MAX_REC_BYTES)) return;
  stopPlayback();
  beep({ 1500 }, 60);
  M5.Speaker.end();
  if (!M5.Mic.begin()) { M5.Speaker.begin(); applyVolume(); toast("Microfono non pronto", "Riprova", C_RED, 0); return; }

  recBufId = 0;                              // il buffer non contiene piu' una registrazione salvata
  recLoudId = 0;
  recording = true;
  recPos = 0;
  recLevel = 0;
  wakeScreen();
  const size_t maxSamples = min((size_t)(MAX_REC_BYTES / 2), (size_t)((freeBytes() - FS_MARGIN / 2) / 2));
  while (keyDown(PIN_KEY2) && recPos + REC_CHUNK <= maxSamples) {
    if (M5.Mic.record(recBuf + recPos, REC_CHUNK, SAMPLE_RATE)) {
      recPos += REC_CHUNK;
      if (recPos >= 3 * REC_CHUNK) {
        const int16_t* blk = recBuf + recPos - 3 * REC_CHUNK;
        int32_t mn = 32767, mx = -32768;
        for (int i = 0; i < REC_CHUNK; i += 2) { mn = min(mn, (int32_t)blk[i]); mx = max(mx, (int32_t)blk[i]); }
        recLevel = (uint16_t)min(32767L, (long)(mx - mn) / 2);
      }
    } else {
      delay(2);
    }
    touch();
    render();
  }
  while (M5.Mic.isRecording()) delay(5);
  M5.Mic.end();
  M5.Speaker.begin();
  applyVolume();
  recording = false;

  size_t n = recPos;
  if (n < MIN_REC_SAMPLES) {
    recSamples = 0;
    toast("Troppo corta", "Tieni premuto piu' a lungo", C_BRASS, 0);
  } else {
    size_t samples = cleanRecording(recBuf, n);
    uint32_t id = nextRecId++;
    char p[20];
    recPath(p, sizeof(p), id);
    if (saveFile(p, recBuf, samples)) {
      if (recCount == MAX_RECS) deleteRecAt(MAX_RECS - 1);
      for (int i = recCount; i > 0; i--) recs[i] = recs[i - 1];
      recs[0] = { id, (uint32_t)(timeValid() ? time(nullptr) : 0), (uint32_t)(samples * 2) };
      recCount++;
      selRec = 0;
      recSamples = samples;
      recBufId = id;
      saveRecMeta();
      char s[16];
      fmtSeconds(s, sizeof(s), samples / (float)SAMPLE_RATE);
      toast("Registrazione salvata", s, C_GREEN, 1);
      beep({ 2000, 2600 });
    } else {
      LittleFS.remove(p);
      recSamples = 0;
      toast("Errore di memoria", "Registrazione non salvata", C_RED, 0);
    }
  }
  updateStats();
  while (keyDown(PIN_KEY2)) delay(10);
  touch();
}

static void findMe() {
  stopPlayback();
  wakeScreen();
  audioPower(true);
  for (int i = 0; i < 4; i++) {
    cv.fillSprite(i % 2 ? C_BG : C_BRASS);
    text("Eccomi!", cv.width() / 2, 52, FONT_HUGE, i % 2 ? C_BRASS : C_BG, top_center);
    cv.pushSprite(0, 0);
    M5.Speaker.tone(2600, 120); delay(160);
    M5.Speaker.tone(2000, 120); delay(300);
  }
  touch();
}

static void selectSlot(int slot, bool feedback) {
  if (slot < 0 || slot >= NUM_SLOTS) return;
  stopPlayback();
  activeSlot = slot;
  loadActive();
  prefs.putUChar("slot", activeSlot);
  if (feedback) beep({ 2200 }, 40);
}

static void menuChange() {
  menuUntil = millis() + MENU_IDLE_MS;
  switch (menuPage) {
    case 0: {                                  // prossimo suono non vuoto
      for (int k = 1; k <= NUM_SLOTS; k++) {
        int s = (activeSlot + k) % NUM_SLOTS;
        if (slotBytes[s]) { selectSlot(s, true); return; }
      }
      toast("Nessun altro suono", "Caricali dall'app", C_MUTED, 0);
      break;
    }
    case 1:
      volume = volume >= 100 ? 10 : volume + 10;     // mai muto dal menu: da 100 riparte da 10
      applyVolume();
      audioPower(true);
      M5.Speaker.tone(1800, 60);
      break;
    case 2: cfg.repeat ^= 1; beep({ 2000 }, 40); break;
    case 3: cfg.punch = (cfg.punch + 1) % 3; loadActive(); beep({ 2000 }, 40); break;
    case 4: cfg.autolock ^= 1; beep({ 2000 }, 40); break;
    case 5: cfg.flip ^= 1; applyRotation(); break;
    case 6: {
      int i = 0;
      for (int k = 0; k < 5; k++) if (SLEEP_CHOICES[k] == cfg.sleepSec) i = k;
      cfg.sleepSec = SLEEP_CHOICES[(i + 1) % 5];
      beep({ 2000 }, 40);
      break;
    }
    default: break;
  }
}

static void closeMenu() {
  menuOpen = false;
  saveSettings();
}

static void handleEvent(uint8_t e) {
  char s[24];
  switch (e) {
    case ST_SAVED: {
      if (reloadSlot) { reloadSlot = false; loadActive(); }
      fmtSeconds(s, sizeof(s), slotBytes[uploadSlot] / 2 / (float)SAMPLE_RATE);
      char t[32];
      snprintf(t, sizeof(t), "Suono %d salvato", uploadSlot + 1);
      toast(t, s, C_GREEN, 1);
      beep({ 2000, 2600 });
      break;
    }
    case ST_COPIED:  toast("Fatto", "Registrazione copiata", C_GREEN, 1); break;
    case ST_DL_DONE: toast("Inviato", "al telefono", C_GREEN, 1); break;
    case ST_DELETED: toast("Eliminato", "", C_GREEN, 1); break;
    case ST_OTA_OK:  break;
    case ST_ERR_CRC:   toast("Dati corrotti", "Riprova dall'app", C_RED, 0); break;
    case ST_ERR_SIZE:  toast("Dimensione errata", "Riprova dall'app", C_RED, 0); break;
    case ST_ERR_FS:    toast("Errore di memoria", "", C_RED, 0); break;
    case ST_ERR_SPACE: toast("Memoria piena", "Elimina qualcosa dall'app", C_RED, 0); break;
    case ST_ERR_OTA:   toast("Aggiornamento fallito", "Resta il firmware di prima", C_RED, 0, 3000); break;
    case ST_ERR_BUSY:  toast("Occupato", "Riprova tra poco", C_BRASS, 0); break;
    default: break;
  }
}

// ============================================================ deep sleep
// Chip audio ES8311 a riposo: la libreria lo spegne per il microfono ma non per l'altoparlante,
// cosi' DAC e uscita restavano alimentati anche durante il deep sleep.
static void codecPowerDown() {
  static const uint8_t seq[][2] = { {0x32, 0x00}, {0x12, 0x02}, {0x13, 0x00}, {0x0E, 0x6A}, {0x0D, 0xFC}, {0x00, 0x00} };
  for (auto& r : seq) M5.In_I2C.writeRegister8(ES8311_ADDR, r[0], r[1], 100000);
}

// Tutto cio' che non serve mentre il dispositivo dorme
static void powerDownPeripherals() {
  M5.Speaker.end();
  codecPowerDown();
  M5.Power.setExtOutput(false);               // uscita 5 V e amplificatore
  boostOn = false;
  M5.Imu.sleep();
}

static void goToSleep() {
  stopPlayback();
  if (menuOpen) closeMenu();
  powerDownPeripherals();
  for (int b = brightness; b > 0; b -= 10) { M5.Display.setBrightness(max(0, b)); delay(12); }
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  while (keyDown(PIN_KEY1) || keyDown(PIN_KEY2)) delay(10);
  const gpio_num_t pins[] = { (gpio_num_t)PIN_KEY1, (gpio_num_t)PIN_KEY2 };
  for (gpio_num_t p : pins) { rtc_gpio_pullup_en(p); rtc_gpio_pulldown_dis(p); }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup((1ULL << PIN_KEY1) | (1ULL << PIN_KEY2), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

// ============================================================ console seriale (prove e diagnosi)
static void console() {
  while (Serial.available()) {
    char c = Serial.read();
    uint32_t now = millis();
    switch (c) {
      case 'i':
        Serial.printf("v%d batt %d%% %dmV usb=%d slot=%d suono=%u reg=%d vol=%u bt=%d conn=%d lock=%d menu=%d play=%d fs=%u/%uKB psram=%uKB\n",
                      FW_VERSION, batteryLevel, batteryMv, (int)usbPowered, activeSlot, (unsigned)soundSamples, recCount,
                      volume, (int)btMode, (int)connected, (int)rtcLocked, (int)menuOpen, playing,
                      (unsigned)(fsUsed / 1024), (unsigned)(fsTotal / 1024), ESP.getFreePsram() / 1024);
        break;
      case 'c': Serial.println(buildCatalog()); break;
      case 'd': Serial.print(buildDiag()); break;
      case '1': simUntil[0] = now + 120; break;                 // clic frontale
      case '2': simUntil[1] = now + 120; break;                 // clic laterale
      case 'B': simUntil[0] = now + 2400; break;                // frontale tenuto: Bluetooth
      case 'R': simUntil[1] = now + 2500; break;                // laterale tenuto: registra ~2 s
      case 'L': simUntil[0] = simUntil[1] = now + 1200; break;  // entrambi: blocco
      case 'b': if (btMode) stopBt(); else startBt(); break;
      case 'm': menuOpen = !menuOpen; menuPage = 0; menuUntil = now + MENU_IDLE_MS; wakeScreen(); break;
      case 'n': if (menuOpen) { menuPage = (menuPage + 1) % MENU_PAGES; menuUntil = now + MENU_IDLE_MS; } break;
      case 'v': if (menuOpen) menuChange(); break;
      case 'k': setLocked(!rtcLocked, true); break;
      case 'V': Serial.printf("tetto volume a batteria %u/255 (%u%%), ultima riproduzione minimo %u mV\n", volCeil, volCeil * 100 / 255, prefs.getUShort("pbmin", 0)); break;
      case 'Z': powerDownPeripherals(); Serial.println("periferiche spente (senza dormire)"); break;
      case 'S': M5.Speaker.begin(); applyVolume(); Serial.println("altoparlante riacceso"); break;
      case 'P': {
        Serial.printf("ES8311 0x00=0x%02X 0x0D=0x%02X 0x12=0x%02X 0x13=0x%02X | batteria %d mV -> %d%% (curva %d%%)\n",
                      M5.In_I2C.readRegister8(ES8311_ADDR, 0x00, 100000), M5.In_I2C.readRegister8(ES8311_ADDR, 0x0D, 100000),
                      M5.In_I2C.readRegister8(ES8311_ADDR, 0x12, 100000), M5.In_I2C.readRegister8(ES8311_ADDR, 0x13, 100000),
                      batteryMv, batteryLevel, socFromMv(batteryMv));
        uint8_t src = M5.In_I2C.readRegister8(PM1_ADDR, 0x04, 100000), wake = M5.In_I2C.readRegister8(PM1_ADDR, 0x05, 100000);
        uint8_t cfgp = M5.In_I2C.readRegister8(PM1_ADDR, 0x06, 100000), lvp = M5.In_I2C.readRegister8(PM1_ADDR, 0x08, 100000);
        Serial.printf("PM1 alimentazione=%u (0 USB, 2 batteria) accensione_ora=0x%02X all'avvio=0x%02X (%s) cfg=0x%02X protezione=%d mV\n",
                      src, wake, pm1WakeAtBoot, pm1WakeName(pm1WakeAtBoot), cfgp, 2000 + (int)(lvp * 7.81f));
        break;
      }
      case '+': volume = min(100, volume + 10); applyVolume(); saveSettings(); Serial.printf("volume %u\n", volume); break;
      case '-': volume = max(10, volume - 10); applyVolume(); saveSettings(); Serial.printf("volume %u\n", volume); break;
      case 'p': playActive(); break;
      case 's': stopPlayback(); break;
      case 'z': Serial.println("deep sleep"); Serial.flush(); goToSleep(); break;
      case 'T': {                                               // genera un suono di prova nel suono attivo
        if (!psBuf(soundBuf, MAX_SOUND_BYTES)) break;
        size_t n = SAMPLE_RATE;
        for (size_t i = 0; i < n; i++) soundBuf[i] = (int16_t)(12000 * sinf(2 * PI * (440 + i / 40.0f) * i / SAMPLE_RATE));
        char p[16];
        slotPath(p, sizeof(p), activeSlot);
        saveFile(p, soundBuf, n);
        slotBytes[activeSlot] = n * 2;
        loadActive();
        updateStats();
        Serial.println("suono di prova creato");
        break;
      }
      case 'h':
        Serial.println("i info | c catalogo | d diagnosi | 1 clic frontale | 2 clic laterale | B tieni frontale | R registra | L blocco (due tasti) | b bluetooth | m menu | n pagina | v cambia | k blocco | p suona | s stop | T suono di prova | z deep sleep | + - volume");
        break;
      default: break;
    }
  }
}

// ============================================================ setup
void setup() {
  loopTask = xTaskGetCurrentTaskHandle();
  auto cfgM5 = M5.config();
  cfgM5.internal_mic = true;
  cfgM5.internal_spk = true;
  M5.begin(cfgM5);
  Serial.begin(115200);
  setCpuFrequencyMhz(80);
  M5.In_I2C.writeRegister8(PM1_ADDR, 0x0A, 0, 100000);     // watchdog del PM1 spento
  M5.Power.setExtOutput(false);                            // uscita 5 V spenta: si accende solo per suonare
  boostOn = false;
  M5.Imu.sleep();                                          // il sensore di movimento non e' usato
  (void)M5.Power.getKeyState();                            // scarta il clic usato per accendere

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t mask = esp_sleep_get_ext1_wakeup_status();
    wakeKey = (mask & (1ULL << PIN_KEY2)) ? 2 : 1;
    if (wakeKey == 1) { k1Was = true; k1Since = 0; }
    else              { k2Was = true; k2Since = 0; }
  } else {
    rtcLocked = false;                                       // accensione vera: niente blocco
  }
  rtc_gpio_deinit((gpio_num_t)PIN_KEY1);
  rtc_gpio_deinit((gpio_num_t)PIN_KEY2);
  pinMode(PIN_KEY1, INPUT_PULLUP);
  pinMode(PIN_KEY2, INPUT_PULLUP);

  prefs.begin("pk", false);
  loadPersistent();

  M5.Display.setRotation(cfg.flip ? 3 : 1);
  M5.Display.setBrightness(0);
  cv.setColorDepth(16);
  cv.setPsram(true);
  cv.createSprite(M5.Display.width(), M5.Display.height());

  M5.Mic.end();
  M5.Speaker.begin();

  LittleFS.begin(true);
  if (LittleFS.exists(TEMP_PATH)) LittleFS.remove(TEMP_PATH);
  if (LittleFS.exists("/sound.pcm") && !LittleFS.exists("/s0.pcm")) LittleFS.rename("/sound.pcm", "/s0.pcm");   // da v1-v5
  scanSlots();
  // registrazioni: tiene solo quelle che esistono davvero sul disco
  bool metaChanged = false;
  for (int i = recCount - 1; i >= 0; i--) {
    char p[20];
    recPath(p, sizeof(p), recs[i].id);
    uint32_t b = fileBytes(p);
    if (!b) { for (uint8_t k = i; k + 1 < recCount; k++) recs[k] = recs[k + 1]; recCount--; metaChanged = true; }
    else if (recs[i].bytes != b) { recs[i].bytes = b; metaChanged = true; }
  }
  if (LittleFS.exists("/rec.pcm") && recCount < MAX_RECS) {                 // registrazione delle versioni precedenti
    uint32_t id = nextRecId++;
    char p[20];
    recPath(p, sizeof(p), id);
    if (LittleFS.rename("/rec.pcm", p)) {
      for (int i = recCount; i > 0; i--) recs[i] = recs[i - 1];
      recs[0] = { id, 0, fileBytes(p) };
      recCount++;
      metaChanged = true;
    }
  }
  if (metaChanged) saveRecMeta();
  loadActive();
  updateStats();
  applyVolume();
  logBoot();
  touch();

  bool afterOta = esp_reset_reason() == ESP_RST_SW && rtcOtaMagic == OTA_MAGIC;
  rtcOtaMagic = 0;
  if (!wakeKey && !afterOta) splash();
  wakeScreen();
  if (afterOta) {                                // aggiornato via Bluetooth: riapre il Bluetooth per l'app
    char v[24];
    snprintf(v, sizeof(v), "Firmware v%d", FW_VERSION);
    toast("Aggiornato", v, C_GREEN, 1, 2500);
    startBt();
  }
  render(true);
}

// ============================================================ loop
void loop() {
  M5.update();
  uint32_t now = millis();
  bool k1 = keyDown(PIN_KEY1), k2 = keyDown(PIN_KEY2);

  // ---- tasto di accensione: blocco / sblocco
  if (M5.BtnPWR.wasClicked() && now > 1500) { touch(); setLocked(!rtcLocked, true); }

  // ---- due tasti insieme: blocco / sblocco
  if (k1 && k2 && !recording) {
    if (!bothSince) bothSince = now;
    bothProgress = bothDone ? 0 : min(1.0f, (now - bothSince) / (float)BOTH_HOLD_MS);
    if (!bothDone && now - bothSince >= BOTH_HOLD_MS) {
      bothDone = true;
      bothProgress = 0;
      setLocked(!rtcLocked, true);
    }
    k1Long = true; k1Menu = false; k2Handled = true; k2Pending = false;   // niente altre azioni per questa pressione
    holdProgress = 0;
    wakeScreen();
  } else {
    bothSince = 0;
    bothProgress = 0;
    if (!k1 && !k2) bothDone = false;
  }

  // ---- tasto frontale
  if (k1 && !k1Was) {
    k1Since = now; k1Long = false; k1Menu = false; touch(); wakeScreen();
    if (rtcLocked)        { k1Long = true; }
    else if (menuOpen)    { k1Menu = true; k1Long = true; menuUntil = now + MENU_IDLE_MS; }
    else if (playing)     { stopPlayback(); k1Long = true; }
  }
  // nel menu il frontale agisce al rilascio: breve = cambia valore, tenuto 1 s = esci
  if (k1Menu && k1 && now - k1Since >= 1000) { k1Menu = false; closeMenu(); beep({ 1600 }, 40); }
  if (k1Menu && !k1 && k1Was) { k1Menu = false; if (menuOpen && now - k1Since > 30) menuChange(); }
  if (k1 && !k1Long && !k2 && now - k1Since > 300 && !busy() && !rtcLocked) {
    holdProgress = min(1.0f, (now - k1Since) / (float)LONG_PRESS_MS);
  } else if (!k1 || k2) {
    holdProgress = 0;
  }
  if (k1 && !k1Long && !k2 && now - k1Since >= LONG_PRESS_MS && !busy() && !rtcLocked) {
    k1Long = true;
    holdProgress = 0;
    if (btMode) stopBt(); else startBt();
  }
  if (!k1 && k1Was && !k1Long && now - k1Since > 30 && !busy() && !rtcLocked) playActive();
  k1Was = k1;

  // ---- tasto laterale
  if (k2 && !k2Was) {
    k2Since = now; k2Handled = false; touch(); wakeScreen();
    if (rtcLocked) { k2Handled = true; }
    else if (menuOpen) { menuPage = (menuPage + 1) % MENU_PAGES; menuUntil = now + MENU_IDLE_MS; k2Handled = true; }
    else if (playing)  { stopPlayback(); k2Handled = true; k2Pending = false; }
  }
  if (k2 && !k1 && !k2Handled && now - k2Since >= REC_HOLD_MS && !busy() && !rtcLocked) {
    k2Handled = true;
    k2Pending = false;
    recordWhileHeld();
    k2 = false;
  }
  if (!k2 && k2Was && !k2Handled && now - k2Since > 30 && !busy() && !rtcLocked) {
    if (k2Pending && now - k2LastClick < DOUBLE_CLICK_MS) {      // doppio clic: menu
      k2Pending = false;
      menuOpen = true;
      menuPage = 0;
      menuUntil = now + MENU_IDLE_MS;
      beep({ 1800, 2200 }, 40);
    } else {
      k2Pending = true;
      k2LastClick = now;
    }
  }
  if (k2Pending && !k2 && now - k2LastClick >= DOUBLE_CLICK_MS) { k2Pending = false; if (!rtcLocked) { selRec = 0; playRec(0); } }
  k2Was = k2;

  if (menuOpen && now > menuUntil) closeMenu();

  // ---- richieste dal Bluetooth
  if (justConnected)    { justConnected = false; beep({ 2000, 2600 }, 50); wakeScreen(); }
  if (justDisconnected) { justDisconnected = false; wakeScreen(); }
  if (reloadSlot && !pendingEvent) { reloadSlot = false; stopPlayback(); loadActive(); }
  if (selectSlotReq >= 0) { int s = selectSlotReq; selectSlotReq = -1; selectSlot(s, false); notifyStatus(ST_OK); wakeScreen(); }
  if (playRequested)    { playRequested = false; playActive(); }
  if (playRecRequested) { playRecRequested = false; playRec(selRec); }
  if (playSlotReq >= 0) { int s = playSlotReq; playSlotReq = -1; playSlotPreview(s); }
  if (stopReq)          { stopReq = false; stopPlayback(); }
  if (findReq)          { findReq = false; findMe(); }
  if (lockReq >= 0)     { bool on = lockReq; lockReq = -1; setLocked(on, true); }
  if (refreshNameReq)   { refreshNameReq = false; if (bleReady) { bool adv = NimBLEDevice::getAdvertising()->isAdvertising(); if (adv) NimBLEDevice::getAdvertising()->stop(); applyAdvertisingName(); if (adv) NimBLEDevice::getAdvertising()->start(); } }
  if (settingsDirty)    { settingsDirty = false; saveSettings(); applyRotation(); }
  if (downloadSlotReq >= 0) {
    int s = downloadSlotReq; downloadSlotReq = -1;
    if (connected) {
      if (s == activeSlot && soundSamples) streamPcm(soundBuf, soundSamples);
      else if (ensureAuxSlot(s)) streamPcm(auxBuf, auxSamples);
      else notifyStatus(ST_ERR_NOREC);
    }
  }
  if (downloadRecReq) {
    downloadRecReq = false;
    if (connected) { if (ensureRecLoaded(selRec)) streamPcm(recBuf, recSamples); else notifyStatus(ST_ERR_NOREC); }
  }
  if (recToSlotReq >= 0) {
    int s = recToSlotReq; recToSlotReq = -1;
    stopPlayback();
    char p[16];
    slotPath(p, sizeof(p), s);
    if (ensureRecLoaded(selRec) && freeBytes() + slotBytes[s] >= recSamples * 2 + FS_MARGIN && saveFile(p, recBuf, recSamples)) {
      slotBytes[s] = recSamples * 2;
      if (auxSlot == s) auxSlot = -1;
      if (s == activeSlot) loadActive();
      notifyStatus(ST_COPIED);
    } else {
      notifyStatus(ST_ERR_SPACE);
    }
    updateStats();
  }
  if (deleteSlotReq >= 0) {
    int s = deleteSlotReq; deleteSlotReq = -1;
    stopPlayback();
    char p[16];
    slotPath(p, sizeof(p), s);
    LittleFS.remove(p);
    slotBytes[s] = 0;
    slotNames[s][0] = 0;                       // un suono eliminato perde anche il nome
    char key[4] = { 'n', char('0' + s), 0 };
    prefs.remove(key);
    if (auxSlot == s) auxSlot = -1;
    if (s == activeSlot) loadActive();
    updateStats();
    notifyStatus(ST_DELETED);
  }
  if (deleteRecReq) {
    deleteRecReq = false;
    stopPlayback();
    deleteRecAt(selRec);
    updateStats();
    notifyStatus(ST_DELETED);
  }
  if (pendingEvent) { uint8_t e = pendingEvent; pendingEvent = 0; handleEvent(e); }
  if (rebootAt && (int32_t)(now - rebootAt) > 0) { rtcOtaMagic = OTA_MAGIC; Serial.println("riavvio dopo OTA"); Serial.flush(); ESP.restart(); }

  // ---- riproduzione, ripetizione, Bluetooth, energia
  if (playing && !M5.Speaker.isPlaying() && now - playStart > 150) {
    uint8_t kind = playing;
    playing = 0;
    if (cfg.repeat && kind == 1 && !userStopped && !rtcLocked) playActive();
  }
  if (pbGuard) {
    if (playing && !usbPowered) {
      static uint32_t lastPoll = 0;
      if (now - lastPoll >= 15) {
        lastPoll = now;
        int mv = M5.Power.getBatteryVoltage();
        if (mv > 0) {
          if (mv < pbMinMv) pbMinMv = mv;
          if (mv < SAG_MV && volCeil > VCEIL_MIN) {              // la batteria cede: giu' il volume subito
            volCeil = max<int>(VCEIL_MIN, (int)volCeil - 25);
            pbSagged = true;
            applyVolume();
          }
        }
      }
    } else {                                                    // finita (o USB collegata): tutto bene
      pbGuard = false;
      if (!pbSagged && volCeil < 255) volCeil = min<int>(255, volCeil + 6);
      prefs.putUChar("pb", 0);
      prefs.putUChar("vceil", volCeil);
      if (pbMinMv < 9999) prefs.putUShort("pbmin", pbMinMv);
    }
  }
  if (btMode && !busy() && (int32_t)(now - btUntil) > 0) stopBt();

  static uint32_t lastStats = 0;
  if (now - lastStats > 3000) { lastStats = now; updateStats(); }

  if (playing || recording || menuOpen || btMode || busy() || usbPowered || rebootAt) lastActivity = now;
  if (boostOn) {
    if (M5.Speaker.isPlaying() || recording) boostIdleSince = now;
    else if (now - boostIdleSince > 2000) audioPower(false);
  }
  uint32_t sleepAfter = rtcLocked && !screenOn ? LOCKED_SLEEP_MS : (uint32_t)cfg.sleepSec * 1000;
  if ((int32_t)(millis() - lastActivity) > (int32_t)sleepAfter && !usbPowered && !k1 && !k2 && !k2Pending) goToSleep();

  console();
  updateScreenPower();
  render();
  delay(8);
}
