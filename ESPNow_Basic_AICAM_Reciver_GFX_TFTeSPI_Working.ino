// Universal ESP-NOW JPEG Receiver (ESP32-S3, Arduino Core 3.3.4)
// Select backend with: #define DISPLAY_LIB 0  // 0=LovyanGFX, 1=TFT_eSPI+TJpg_Decoder, 2=Arduino_GFX+TJpg_Decoder
// This preserves your working LovyanGFX branch exactly and adds TJpg_Decoder for the others.
// CHUNK protocol: chunks of CHUNK_SIZE, last chunk < CHUNK_SIZE -> end-of-frame

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <vector>
#include "esp_wifi.h"
#include "Lilylogo.h"

#define DISPLAY_LIB 0   // set 0 / 1 / 2
#define CHUNK_SIZE 246
#define MAX_FRAME_SIZE 50000
#define DEFAULT_CHANNEL NULL
#define AUTO_CHANNEL_SSID "vivo_office"
volatile bool pendingChannelChange = false;
volatile uint8_t pendingChannel = 0;

  #define GFX_BL 38
// Sender MAC - put your sender's MAC bytes here
uint8_t sender_mac[] = { 0xD4, 0x8C, 0x49, 0xB9, 0xAD, 0x70 };

// --------------------- Display libs ---------------------
#if DISPLAY_LIB == 0
  // You said your LovyanGFX code already works — keep it as-is
  #include <LovyanGFX.hpp>
  LGFX tft;                   // your working object (global)
#elif DISPLAY_LIB == 1
  #include <TFT_eSPI.h>
  #include <TJpg_Decoder.h>
  #include <Lily_T_HMI_pins.h>
  TFT_eSPI *tft = nullptr;    // allocated in setup()
#elif DISPLAY_LIB == 2
  #include <Arduino_GFX_Library.h>
  #include <TJpg_Decoder.h>
  // Adjust these pins to match your board wiring for parallel ST7789

  #define TFT_DC 7
  #define TFT_CS 6
  Arduino_DataBus *bus = nullptr;
  Arduino_GFX *gfx = nullptr;
#endif
// --------------------------------------------------------

std::vector<uint8_t> frame_buffer;
volatile bool channelSet = false;
int currentChannel = DEFAULT_CHANNEL;

// ---------------- helpers ----------------
void printMac() {
  uint8_t mac[6];
  esp_err_t r = esp_wifi_get_mac(WIFI_IF_STA, mac);
  if (r == ESP_OK) {
    Serial.printf("Receiver STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  } else {
    Serial.printf("esp_wifi_get_mac() failed: %d\n", r);
  }
}

bool safeSetChannel(int ch) {
  if (ch < 1 || ch > 14) return false;
  esp_err_t e;

  e = esp_wifi_set_mode(WIFI_MODE_STA);
  Serial.printf("esp_wifi_set_mode => %d\n", e);
  delay(10);

  e = esp_wifi_start();
  Serial.printf("esp_wifi_start => %d\n", e);
  delay(10);

  e = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  Serial.printf("esp_wifi_set_channel(%d) => %d\n", ch, e);
  if (e == ESP_OK) {
    currentChannel = ch;
    channelSet = true;
    delay(10);
    return true;
  }
  return false;
}

esp_err_t addPeerSafe(uint8_t *mac, int ch) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = ch;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA; // correct for Core 3.x
  esp_err_t r = esp_now_add_peer(&peerInfo);
  Serial.printf("esp_now_add_peer => %d\n", r);
  return r;
}

int getWiFiChannelBySSID(const char* ssid) {
  if (!ssid || ssid[0] == '\0') return 0;
  Serial.printf("Scanning for SSID '%s'...\n", ssid);
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("No networks found or scan failed");
    return 0;
  }
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) == ssid) {
      int ch = WiFi.channel(i);
      Serial.printf("Found SSID on channel %d\n", ch);
      return ch;
    }
  }
  Serial.println("SSID not found in scan results");
  return 0;
}

// ---------------- TJpgDec callback used for DISPLAY_LIB 1 & 2 ----------------
#if DISPLAY_LIB == 1 || DISPLAY_LIB == 2
bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  // Clip check
  if (x < 0 || y < 0) return false;

  #if DISPLAY_LIB == 1
    if (!tft) return false;
    // Optional: swap bytes only for TFT_eSPI
    for(int i = 0; i < w*h; i++){
      uint16_t p = bitmap[i];
      bitmap[i] = (p>>8) | (p<<8); // TFT_eSPI expects RGB565 byte order
    }
    tft->pushImage(x, y, w, h, bitmap);
  #elif DISPLAY_LIB == 2
    if (!gfx) return false;
    gfx->draw16bitRGBBitmap(x, y, bitmap, w, h);
  #endif
  return true;
}
#endif


// ---------------- ESP-NOW receiver callback (your chunk logic preserved) ----------------
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
// accept sync only from the known sender AND only if not synced
  // ------- VALID CHANNEL SYNC PACKET --------
  if (len == 1 && !channelSet) {

      // accept sync ONLY from known sender
      if (memcmp(info->src_addr, sender_mac, 6) != 0) {
          Serial.println("Ignoring 1-byte packet from unknown sender");
          return;
      }

      uint8_t newCh = data[0];

      // ignore bad channels
      if (newCh < 1 || newCh > 13) {
          Serial.printf("Ignoring invalid sync channel %d\n", newCh);
          return;
      }

      // schedule change (not inside ISR)
      pendingChannel = newCh;
      pendingChannelChange = true;
      Serial.printf("VALID channel-sync request: %d\n", newCh);
      return;
  }

  // ---------- continue with JPEG chunks ---------
  
   static unsigned int total_bytes = 0;

  if (info && info->src_addr) {
    Serial.printf("recv from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
      info->src_addr[0],info->src_addr[1],info->src_addr[2],
      info->src_addr[3],info->src_addr[4],info->src_addr[5], len);
  }

  // channel-sync packet (1 byte) - only if not yet set
  /*if (len == 1 && !channelSet) {
    uint8_t newCh = data[0];
    Serial.printf("Channel-sync packet received: %d\n", newCh);
    esp_now_deinit();
    esp_wifi_set_promiscuous(false);

    if (!safeSetChannel(newCh)) {
      Serial.println("safeSetChannel failed on sync; re-init esp_now and continue");
      if (esp_now_init() == ESP_OK) addPeerSafe(sender_mac, currentChannel);
      return;
    }
    if (esp_now_init() != ESP_OK) {
      Serial.println("esp_now_init failed after channel change");
      return;
    }
    addPeerSafe(sender_mac, newCh);
    Serial.printf("✅ Synced to channel %d\n", newCh);
    return;
  }
*/
  if (len <= 0 || len > CHUNK_SIZE) {
    Serial.printf("Ignored invalid chunk len=%d\n", len);
    return;
  }

  // guard frame size
  if (frame_buffer.size() + (size_t)len > MAX_FRAME_SIZE) {
    Serial.println("Frame too large: discarding buffer");
    frame_buffer.clear();
    total_bytes = 0;
    return;
  }

  frame_buffer.insert(frame_buffer.end(), data, data + len);
  total_bytes += len;
  Serial.printf("Chunk appended: %d bytes, total=%u\n", len, total_bytes);

  if (len < CHUNK_SIZE) {
    Serial.printf("Frame complete: %u bytes -> decoding\n", total_bytes);

    if (total_bytes == 0 || total_bytes > MAX_FRAME_SIZE) {
      Serial.printf("Frame size suspicious: %u\n", total_bytes);
      frame_buffer.clear();
      total_bytes = 0;
      return;
    }

    // DRAW depending on DISPLAY_LIB (preserve Lovyan behavior)
  #if DISPLAY_LIB == 0
    tft.drawJpg(frame_buffer.data(), frame_buffer.size(), 0, 0);
  #elif DISPLAY_LIB == 1 || DISPLAY_LIB == 2
    // TJpg_Decoder expects a pointer to the full JPEG in RAM
    TJpgDec.drawJpg(0, 0, frame_buffer.data(), frame_buffer.size());
  #endif

    frame_buffer.clear();
    total_bytes = 0;
  }
}

void handleChannelSwitch() {
  if (!pendingChannelChange) return;

  pendingChannelChange = false;

  uint8_t ch = pendingChannel;
  Serial.printf("=== Channel change requested: %d ===\n", ch);

  // stop ESP-NOW before touching WiFi channel
  esp_now_deinit();
  esp_wifi_set_promiscuous(false);

  // apply channel
  if (!safeSetChannel(ch)) {
    Serial.println("safeSetChannel FAILED!");
    return;
  }

  Serial.printf("Now on channel %d\n", ch);

  // restart ESP-NOW
  if (esp_now_init() == ESP_OK) {
    addPeerSafe(sender_mac, ch);
    Serial.println("ESP-NOW restarted OK");
    channelSet = true;
  } else {
    Serial.println("esp_now_init FAILED after channel switch");
  }
}

// ---------------- Setup display (minimal changes; Lovyan kept identical) ----------------
void setupDisplay(int channel) {
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);

#if DISPLAY_LIB == 0
  // Your original working LovyanGFX flow (kept exactly)
  tft.initDMA();
  tft.init();
  tft.setSwapBytes(true);
  tft.setRotation(1);
  // some boards provide setBrightness; if not, uncomment or remove
  // tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);
  tft.pushImage(0, 0, 240, 320, (uint16_t*)gImage_logo);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(0, 0);
  tft.println("Receiver Ready");
  tft.setCursor(10, 20);
  tft.printf("CH: %d", channel);

#elif DISPLAY_LIB == 1
  // TFT_eSPI + TJpg_Decoder
  tft = new TFT_eSPI();
  if (!tft) {
    Serial.println("TFT_eSPI alloc failed");
    return;
  }
   pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  tft->begin();
  tft->setRotation(1);
  tft->setSwapBytes(true);
  //tft->setColorDepth(16);  // Make sure TFT uses 16-bit
  tft->fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);  // will call pushImage inside tftOutput
  // optional splash (TJpgDec or pushImage)
  // tft->pushImage(0,0,240,320,(uint16_t*)gImage_logo);
  tft->setCursor(0,0);
  tft->println("Receiver Ready");
  tft->printf("CH: %d", channel);

#elif DISPLAY_LIB == 2
  // Arduino_GFX + TJpg_Decoder (parallel ST7789 example)
  // allocate bus & gfx now (avoid global ctor nulls)
  bus = new Arduino_ESP32PAR8(TFT_DC, TFT_CS, 8 /*WR*/, GFX_NOT_DEFINED /*RD*/,
                              48,47,39,40,41,42,45,46);
  if (!bus) {
    Serial.println("Arduino_DataBus alloc failed");
    return;
  }
  gfx = new Arduino_ST7789(bus, GFX_NOT_DEFINED /*RST*/, 0 /*rotation*/, false /*IPS*/);
  if (!gfx) {
    Serial.println("Arduino_GFX alloc failed");
    delete bus;
    bus = nullptr;
    return;
  }
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed");
    delete gfx; gfx = nullptr;
    delete bus; bus = nullptr;
    return;
  }
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  gfx->fillScreen(0x0000);
  // register TJpg callback (tftOutput uses gfx)
  TJpgDec.setCallback(tftOutput);
  gfx->setCursor(0,0);
  gfx->println("Receiver Ready");
  gfx->printf("CH: %d", channel);
#endif
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n=== ESP-NOW Multi-backend JPEG Receiver ===");

  printMac();

  WiFi.mode(WIFI_STA);

  // try detect channel via SSID
  int detected = 0;
  if (strlen(AUTO_CHANNEL_SSID) > 0) {
    detected = getWiFiChannelBySSID(AUTO_CHANNEL_SSID);
    if (detected > 0) {
      Serial.printf("Using detected channel %d\n", detected);
      safeSetChannel(detected);
    } else {
      Serial.println("SSID channel detection failed, will wait for sync packet or use default");
    }
  } else {
    Serial.println("AUTO_CHANNEL_SSID empty; skipping scan");
  }

  #if DISPLAY_LIB == 1
if (!channelSet) {
    if (DEFAULT_CHANNEL > 0) {
        Serial.printf("Setting DEFAULT channel %d\n", DEFAULT_CHANNEL);
        safeSetChannel(DEFAULT_CHANNEL);
    } else {
        Serial.println("DEFAULT_CHANNEL not set, waiting for sync packet");
    }
}
#endif


  // ensure promiscuous off before esp_now_init (S3: important)
  esp_wifi_set_promiscuous(false);

  // init display now that radio stable
  setupDisplay(currentChannel);

  // init esp-now (best-effort)
  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init FAILED (continuing to allow debug)");
  } else {
    Serial.println("esp_now initialized");
  }

  // add peer (best-effort)
  esp_err_t r = addPeerSafe(sender_mac, currentChannel);
  if (r != ESP_OK) Serial.printf("Warning: esp_now_add_peer returned %d\n", r);
  else Serial.println("added peer (initial)");

  // register callback
  esp_now_register_recv_cb(onDataRecv);

  Serial.println("Receiver ready - listening");
}

void loop() {

  handleChannelSwitch();
  delay(10);
}
