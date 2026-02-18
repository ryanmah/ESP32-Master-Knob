#define LGFX_USE_V1
#include <lvgl.h>               // For graphical interface
#include <LovyanGFX.hpp>        // For display driver

#include <Adafruit_NeoPixel.h>  // For controlling LED
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WebServer.h>

#include <ArduinoJson.h>

#include "CST816D.h"      // Touch driver file
#include "ui.h"           // All LVGL interfaces designed, indexed by this

// I2C pins for the touch screen
#define TP_I2C_SDA_PIN 6
#define TP_I2C_SCL_PIN 7
#define I2C_SDA_PIN 38
#define I2C_SCL_PIN 39

String wifiId = "mahquez-mesh";            // WiFi SSID to connect to
String wifiPwd = "makajack2020";          // PASSWORD of the WiFi SSID to connect to

// Home Assistant webhook configuration
static const char* HA_WEBHOOK_URL = "http://homeassistant.local:8123/api/webhook/crowpanel_knob_controller";
static const char* DEVICE_ID = "crowpanel_knob";


/*
  Five LEDs below the 1.28-inch display
*/
#define POWER_LIGHT_PIN 40            // Power light pin                               
#define LED_PIN 48                    // LED pin
#define LED_NUM 5                     // Total of 5 rendering LEDs

Adafruit_NeoPixel led = Adafruit_NeoPixel(LED_NUM, LED_PIN, NEO_GRB + NEO_KHZ800);  
bool isLed = true;

#define ENCODER_A_PIN 45              // Rotary display control pin 1, CLK pin
#define ENCODER_B_PIN 42              // Rotary display control pin 2, DT pin
#define SWITCH_PIN 41                 // Interrupt pin for rotary knob press

volatile int8_t position_tmp = -1;    // Temporary position of the rotary knob (0: clockwise, 1: counterclockwise)
volatile int8_t currentA = 0;         // For rotary encoder state detection, current state
volatile int8_t lastA = 0;            // For rotary encoder state detection, previous pin state

volatile unsigned long lastPressTime = 0;     // Record the last button press time (for debounce and double-click detection)
volatile bool pressFlag = false;      // Button press flag
volatile int clickCount = 0;          // Number of clicks (for detecting double-click)
const unsigned long debounceTime = 20;        // Debounce time (milliseconds)
const unsigned long doubleClickTime = 200;    // Double-click interval (milliseconds)

// Long-press detection variables
const unsigned long longPressTime = 600;
static bool longPressSent = false;
static unsigned long pressStartTime = 0;

void IRAM_ATTR buttonISR() {
  static unsigned long lastInterruptTime = 0;
  unsigned long interruptTime = millis();
  if (interruptTime - lastInterruptTime > debounceTime) {
    if (digitalRead(SWITCH_PIN)) {
      pressFlag = false;
    } else {
      pressFlag = true;
      lastPressTime = interruptTime;
      clickCount++;
    }
  }
  lastInterruptTime = interruptTime;
}

TaskHandle_t ledTestTaskHandle = NULL;
TaskHandle_t encTaskHandle = NULL;

#define TP_INT 5                  // Touch interrupt pin
#define TP_RST 13                 // Touch reset pin
#define SCREEN_BACKLIGHT_PIN 46   // Screen backlight pin

const int pwmFreq = 5000;
const int pwmChannel = 0;
const int pwmResolution = 8;


class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST; 
      cfg.spi_mode = 0;      
      cfg.freq_write = 80000000;    
      cfg.freq_read = 20000000;         
      cfg.spi_3wire = true;      
      cfg.use_lock = true;       
      cfg.dma_channel = SPI_DMA_CH_AUTO;  
      cfg.pin_sclk = 10; 
      cfg.pin_mosi = 11;  
      cfg.pin_miso = -1; 
      cfg.pin_dc = 3;  
      _bus_instance.config(cfg);          
      _panel_instance.setBus(&_bus_instance); 
    }
    {                            
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 9;                 
      cfg.pin_rst = 14;       
      cfg.pin_busy = -1;         
      cfg.memory_width = 240; 
      cfg.memory_height = 240;  
      cfg.panel_width = 240;
      cfg.panel_height = 240; 
      cfg.offset_x = 0;   
      cfg.offset_y = 0;   
      cfg.offset_rotation = 0;  
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;  
      cfg.readable = false;  
      cfg.invert = true;    
      cfg.rgb_order = false;   
      cfg.dlen_16bit = false;   
      cfg.bus_shared = false;   
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);  
  }
};

LGFX gfx;                                                  
CST816D touch(TP_I2C_SDA_PIN, TP_I2C_SCL_PIN, TP_RST, TP_INT); 

static const uint32_t screenWidth = 240;    
static const uint32_t screenHeight = 240;

static lv_disp_draw_buf_t draw_buf;     // Drawing buffer
static lv_color_t *buf = NULL;          // Buffer required by LVGL
static lv_color_t *buf1 = NULL;         // Buffer required by LVGL

lv_obj_t *current_screen = NULL;        // Pointer to the current screen
int screen1_index = 1;                  // Main screen index (0: ARCADE, 1: REC PLAYER, 2: RADIO)

lv_obj_t *loadingScreen = NULL;
lv_obj_t *loadingLabel = NULL;

// Home Assistant communication functions
bool sendToHomeAssistant(const char* action, int value = 0, bool hasValue = false) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, HA_WEBHOOK_URL)) {
    Serial.println("Failed to begin HTTP");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  String body = "{";
  body += "\"device\":\"" + String(DEVICE_ID) + "\",";
  body += "\"action\":\"" + String(action) + "\"";
  if (hasValue) {
    body += ",\"value\":" + String(value);
  }
  body += "}";

  int code = http.POST((uint8_t*)body.c_str(), body.length());
  http.end();

  if (code > 0 && code < 400) {
    Serial.printf("HA sent: %s\n", action);
    return true;
  }
  Serial.printf("HA failed: %d\n", code);
  return false;
}

// Disable touch on a hidden element so it cannot block buttons beneath it
void hideAndDisableTouch(lv_obj_t *obj) {
  lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

// Style a button as a clean white-text menu item on black background
// highlight=true adds a rounded white border (for the "selected" item look)
void styleMenuBtn(lv_obj_t *btn, const char *text, int y, int w, bool highlight) {
  lv_obj_set_size(btn, w, 28);
  lv_obj_set_align(btn, LV_ALIGN_TOP_MID);
  lv_obj_set_x(btn, 0);
  lv_obj_set_y(btn, y);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_outline_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 4, 0);

  if (highlight) {
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
  } else {
    lv_obj_set_style_border_width(btn, 0, 0);
  }

  lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_PRESSED);

  lv_obj_t *label = lv_obj_get_child(btn, 0);
  if (label == NULL) {
    label = lv_label_create(btn);
  }
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_obj_center(label);
}

// Style the return button as "BACK" text
void styleBackBtn(lv_obj_t *btn, int y) {
  lv_obj_set_size(btn, 80, 24);
  lv_obj_set_align(btn, LV_ALIGN_TOP_MID);
  lv_obj_set_x(btn, 0);
  lv_obj_set_y(btn, y);

  lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_outline_width(btn, 0, 0);
  lv_obj_set_style_border_width(btn, 0, 0);

  lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_PRESSED);

  uint32_t cnt = lv_obj_get_child_cnt(btn);
  for (uint32_t i = 0; i < cnt; i++) {
    lv_obj_add_flag(lv_obj_get_child(btn, i), LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, "BACK");
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(lbl);
}

// --- Knob navigation state (declared early for use in haButtonCb) ---
static bool haCustomized = false;
static lv_obj_t *defaultTitleLabel = NULL;

int menuSelIdx = 0;
int radioSelIdx = 0;
int playlistSelIdx = 0;

static lv_obj_t *volumeLabel = NULL;
static lv_obj_t *volumeDot = NULL;
static int currentVolume = 20;
WebServer server(80);

// --- Now Playing state ---
static lv_obj_t *songTitleLabel = NULL;
static lv_obj_t *artistLabel = NULL;
static lv_obj_t *ctrlTitleLabel = NULL;
static lv_obj_t *ctrlArtistLabel = NULL;
static volatile bool pendingNowPlaying = false;
static char npTitle[128] = "";
static char npArtist[128] = "";
static char npButtonTitle[128] = "";

void applyNowPlaying();

void updateVolumeDotPosition() {
  if (!volumeDot) return;
  float startDeg = 240.0f;
  float endDeg = -60.0f;
  float angleDeg = startDeg + ((float)currentVolume / 100.0f) * (endDeg - startDeg);
  float rad = angleDeg * 3.14159265f / 180.0f;
  float radius = 110.0f;
  int cx = 120 + (int)(radius * cosf(rad)) - 6;
  int cy = 120 - (int)(radius * sinf(rad)) - 6;
  lv_obj_set_pos(volumeDot, cx, cy);
}

void updateVolumeDisplay() {
  if (volumeLabel) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", currentVolume);
    lv_label_set_text(volumeLabel, buf);
  }
  updateVolumeDotPosition();
}

volatile int pendingNavDirection = 0;
volatile bool pendingActivate = false;
volatile bool pendingVolumeSend = false;
unsigned long lastVolumeChangeTime = 0;
const unsigned long volumeDebounceMs = 80;
unsigned long lastLocalVolumeChange = 0;
const unsigned long volumeSyncCooldownMs = 2000;
volatile unsigned long lastActivityTime = 0;
const unsigned long sleepTimeoutMs = 300000;
volatile bool screenAsleep = false;
volatile bool pendingOpenMenu = false;
volatile int pendingDoubleClick = 0;
volatile int pendingLongPress = 0;
volatile int pendingVolumeChange = 0;

lv_obj_t* menuBtns[6];
int menuBtnCount = 6;
lv_obj_t* radioBtns[6];
int radioBtnCount = 6;
lv_obj_t* playlistBtns[5];
int playlistBtnCount = 5;
lv_obj_t* controlsBtns[4];
int controlsBtnCount = 4;
int controlsSelIdx = 0;
lv_obj_t *ui_Screen5 = NULL;

void updateMenuHighlight(lv_obj_t* btns[], int count, int selIdx);

static const char* buttonDisplayName(int id) {
  switch (id) {
    case 1:  return "ARCADE";
    case 2:  return "RECORD PLAYER";
    case 6:  return "KCRW";
    case 7:  return "KQED";
    case 8:  return "KEXP";
    case 9:  return "NTS 1";
    case 10: return "NTS 2";
    case 11: return "PLAYLIST 1";
    case 12: return "PLAYLIST 2";
    case 13: return "PLAYLIST 3";
    case 14: return "PLAYLIST 4";
    default: return NULL;
  }
}

// Button click callback for menu and submenu buttons
// user_data encodes the button number (1-13)
// Button mapping:
//   1=ARCADE, 2=RECORD PLAYER, 3=RADIO...(nav only), 4=PLAYLISTS...(nav only), 5=CONTROLS...(nav only)
//   6=KCRW, 7=KQED, 8=KEXP, 9=NTS1, 10=NTS2
//   11=PLAYLIST 1, 12=PLAYLIST 2, 13=PLAYLIST 3, 14=PLAYLIST 4
//   Media controls on Screen 5 use dedicated webhooks (media_play_pause, media_next, media_previous)
void haButtonCb(lv_event_t *e) {
  int id = (int)(uintptr_t)lv_event_get_user_data(e);

  if (id > 0 && id != 3 && id != 4 && id != 5) {
    char action[16];
    snprintf(action, sizeof(action), "button_%d", id);
    sendToHomeAssistant(action);
  }

  if (id == 3) {
    _ui_screen_change(&ui_Screen3, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    radioSelIdx = 0;
    updateMenuHighlight(radioBtns, radioBtnCount, radioSelIdx);
  } else if (id == 4) {
    _ui_screen_change(&ui_Screen4, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    playlistSelIdx = 0;
    updateMenuHighlight(playlistBtns, playlistBtnCount, playlistSelIdx);
  } else if (id == 5) {
    _ui_screen_change(&ui_Screen5, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    controlsSelIdx = 0;
    updateControlsHighlight();
  } else {
    const char *name = buttonDisplayName(id);
    if (name && defaultTitleLabel) {
      lv_label_set_text(defaultTitleLabel, name);
    }
    _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
  }
}


// Apply or remove highlight border on a button
void setHighlight(lv_obj_t *btn, bool on) {
  if (on) {
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
  } else {
    lv_obj_set_style_border_width(btn, 0, 0);
  }
}

// Update highlights for a button array based on selected index
void updateMenuHighlight(lv_obj_t** btns, int count, int selIdx) {
  for (int i = 0; i < count; i++) {
    setHighlight(btns[i], i == selIdx);
  }
}

// Move selection up or down on the current menu screen
// direction: +1 = down (knob right/clockwise), -1 = up (knob left/counter-clockwise)
void navigateMenu(int direction) {
  lv_obj_t *scr = lv_scr_act();
  if (scr == ui_Screen2) {
    menuSelIdx = (menuSelIdx + direction + menuBtnCount) % menuBtnCount;
    updateMenuHighlight(menuBtns, menuBtnCount, menuSelIdx);
  } else if (scr == ui_Screen3) {
    radioSelIdx = (radioSelIdx + direction + radioBtnCount) % radioBtnCount;
    updateMenuHighlight(radioBtns, radioBtnCount, radioSelIdx);
  } else if (scr == ui_Screen4) {
    playlistSelIdx = (playlistSelIdx + direction + playlistBtnCount) % playlistBtnCount;
    updateMenuHighlight(playlistBtns, playlistBtnCount, playlistSelIdx);
  } else if (scr == ui_Screen5) {
    controlsSelIdx = (controlsSelIdx + direction + controlsBtnCount) % controlsBtnCount;
    updateControlsHighlight();
  }
}

// Activate the currently highlighted button via knob press
void activateMenuSelection() {
  lv_obj_t *scr = lv_scr_act();
  Serial.printf("activateMenuSelection: scr=%p\n", scr);
  bool isBack = false;
  if (scr == ui_Screen2) {
    isBack = (menuSelIdx == menuBtnCount - 1);
    if (isBack) {
      sendToHomeAssistant("menu_back_default");
      _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    } else {
      lv_event_send(menuBtns[menuSelIdx], LV_EVENT_CLICKED, NULL);
    }
  } else if (scr == ui_Screen3) {
    isBack = (radioSelIdx == radioBtnCount - 1);
    if (isBack) {
      sendToHomeAssistant("submenu_back_menu");
      menuSelIdx = 0;
      updateMenuHighlight(menuBtns, menuBtnCount, menuSelIdx);
      _ui_screen_change(&ui_Screen2, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    } else {
      lv_event_send(radioBtns[radioSelIdx], LV_EVENT_CLICKED, NULL);
    }
  } else if (scr == ui_Screen4) {
    isBack = (playlistSelIdx == playlistBtnCount - 1);
    if (isBack) {
      sendToHomeAssistant("submenu_back_menu");
      menuSelIdx = 0;
      updateMenuHighlight(menuBtns, menuBtnCount, menuSelIdx);
      _ui_screen_change(&ui_Screen2, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    } else {
      lv_event_send(playlistBtns[playlistSelIdx], LV_EVENT_CLICKED, NULL);
    }
  } else if (scr == ui_Screen5) {
    isBack = (controlsSelIdx == 2);
    if (isBack) {
      sendToHomeAssistant("controls_back_default");
      _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    } else {
      lv_event_send(controlsBtns[controlsSelIdx], LV_EVENT_CLICKED, NULL);
    }
  }
}

void customizeUIForHA() {
  // === SCREEN 1 (DEFAULT) - Plain black background, relabel carousel items ===
  lv_obj_set_style_bg_color(ui_Screen1, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui_Screen1, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_img_opa(ui_Screen1, LV_OPA_TRANSP, 0);
  hideAndDisableTouch(ui_Image4);

  hideAndDisableTouch(ui_s1select);

  lv_obj_set_size(ui_volumeBlue, 0, 0);
  lv_obj_set_size(ui_volumeWhite, 0, 0);
  lv_obj_set_size(ui_tempBlue, 0, 0);
  lv_obj_set_size(ui_tempWhite, 0, 0);
  lv_obj_set_size(ui_lightBlue, 0, 0);
  lv_obj_set_size(ui_lightWhite, 0, 0);
  lv_obj_set_style_opa(ui_volumeBlue, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_volumeWhite, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_tempBlue, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_tempWhite, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_lightBlue, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_lightWhite, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_volumeTextBlue, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_volumeTextWhite, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_tempTextBlue, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_tempTextWhite, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_lightTextBlue, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(ui_lightTextWhite, LV_OPA_TRANSP, 0);

  if (!haCustomized) {
    volumeDot = lv_obj_create(ui_Screen1);
    lv_obj_set_size(volumeDot, 12, 12);
    lv_obj_set_style_radius(volumeDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(volumeDot, lv_color_make(220, 30, 30), 0);
    lv_obj_set_style_bg_opa(volumeDot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(volumeDot, 0, 0);
    lv_obj_clear_flag(volumeDot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    updateVolumeDotPosition();

    volumeLabel = lv_label_create(ui_Screen1);
    lv_label_set_text(volumeLabel, "20");
    lv_obj_set_style_text_color(volumeLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(volumeLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_align(volumeLabel, LV_ALIGN_CENTER);
    lv_obj_set_y(volumeLabel, -25);

    songTitleLabel = lv_label_create(ui_Screen1);
    lv_label_set_text(songTitleLabel, "");
    lv_obj_set_style_text_color(songTitleLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(songTitleLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(songTitleLabel, 5, 0);
    lv_obj_set_align(songTitleLabel, LV_ALIGN_CENTER);
    lv_obj_set_y(songTitleLabel, 15);
    lv_obj_set_width(songTitleLabel, 180);
    lv_label_set_long_mode(songTitleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(songTitleLabel, LV_TEXT_ALIGN_CENTER, 0);

    artistLabel = lv_label_create(ui_Screen1);
    lv_label_set_text(artistLabel, "");
    lv_obj_set_style_text_color(artistLabel, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(artistLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(artistLabel, 5, 0);
    lv_obj_set_align(artistLabel, LV_ALIGN_CENTER);
    lv_obj_set_y(artistLabel, 38);
    lv_obj_set_width(artistLabel, 160);
    lv_label_set_long_mode(artistLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(artistLabel, LV_TEXT_ALIGN_CENTER, 0);

    defaultTitleLabel = lv_label_create(ui_Screen1);
    lv_label_set_text(defaultTitleLabel, "HA CONTROLLER");
    lv_obj_set_style_text_color(defaultTitleLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(defaultTitleLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(defaultTitleLabel, 5, 0);
    lv_obj_set_align(defaultTitleLabel, LV_ALIGN_CENTER);
    lv_obj_set_y(defaultTitleLabel, 15);
    lv_obj_add_flag(defaultTitleLabel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = lv_label_create(ui_Screen1);
    lv_label_set_text(hint, "PRESS FOR MENU");
    lv_obj_set_style_text_color(hint, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(hint, 5, 0);
    lv_obj_set_align(hint, LV_ALIGN_CENTER);
    lv_obj_set_y(hint, 60);
    haCustomized = true;
  }

  // === SCREEN 2 (MENU) ===
  lv_obj_set_style_bg_color(ui_Screen2, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui_Screen2, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_img_opa(ui_Screen2, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(ui_Screen2, 0, 0);
  hideAndDisableTouch(ui_VolumeArc);
  hideAndDisableTouch(ui_VolNum);
  hideAndDisableTouch(ui_Image3);
  hideAndDisableTouch(ui_Label4);
  hideAndDisableTouch(ui_Label3);
  hideAndDisableTouch(ui_Button6);

  styleMenuBtn(ui_Button1, "ARCADE",        28,  140, false);
  styleMenuBtn(ui_Button2, "RECORD PLAYER", 63,  140, false);
  styleMenuBtn(ui_Button3, "RADIO...",      98,  140, false);
  styleMenuBtn(ui_Button4, "PLAYLISTS...",  133, 140, false);
  styleMenuBtn(ui_Button5, "CONTROLS...",   168, 140, false);
  styleBackBtn(ui_screen2ReturnBt, 203);

  menuBtns[0] = ui_Button1;
  menuBtns[1] = ui_Button2;
  menuBtns[2] = ui_Button3;
  menuBtns[3] = ui_Button4;
  menuBtns[4] = ui_Button5;
  menuBtns[5] = ui_screen2ReturnBt;
  menuSelIdx = 0;
  updateMenuHighlight(menuBtns, menuBtnCount, menuSelIdx);

  lv_obj_add_event_cb(ui_Button1, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)1);
  lv_obj_add_event_cb(ui_Button2, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)2);
  lv_obj_add_event_cb(ui_Button3, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)3);
  lv_obj_add_event_cb(ui_Button4, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)4);
  lv_obj_add_event_cb(ui_Button5, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)5);

  // === SCREEN 3 (RADIO SUBMENU) ===
  lv_obj_set_style_bg_color(ui_Screen3, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui_Screen3, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_img_opa(ui_Screen3, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(ui_Screen3, 0, 0);
  hideAndDisableTouch(ui_TempArc);
  hideAndDisableTouch(ui_TempNum);
  hideAndDisableTouch(ui_Image6);
  hideAndDisableTouch(ui_Label8);
  hideAndDisableTouch(ui_Label11);
  hideAndDisableTouch(ui_Button18);

  styleMenuBtn(ui_Button7,  "KCRW",   27,  120, false);
  styleMenuBtn(ui_Button8,  "KQED",   60,  120, false);
  styleMenuBtn(ui_Button11, "KEXP",   93, 120, false);
  styleMenuBtn(ui_Button9,  "NTS 1",  126, 120, false);
  styleMenuBtn(ui_Button10, "NTS 2",  159, 120, false);
  styleBackBtn(ui_screen3ReturnBt, 192);

  radioBtns[0] = ui_Button7;
  radioBtns[1] = ui_Button8;
  radioBtns[2] = ui_Button11;
  radioBtns[3] = ui_Button9;
  radioBtns[4] = ui_Button10;
  radioBtns[5] = ui_screen3ReturnBt;
  radioSelIdx = 0;
  updateMenuHighlight(radioBtns, radioBtnCount, radioSelIdx);

  lv_obj_add_event_cb(ui_Button7,  haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)6);
  lv_obj_add_event_cb(ui_Button8,  haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)7);
  lv_obj_add_event_cb(ui_Button11, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)8);
  lv_obj_add_event_cb(ui_Button9,  haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)9);
  lv_obj_add_event_cb(ui_Button10, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)10);

  // === SCREEN 4 (PLAYLIST SUBMENU) ===
  lv_obj_set_style_bg_color(ui_Screen4, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui_Screen4, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_img_opa(ui_Screen4, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(ui_Screen4, 0, 0);
  hideAndDisableTouch(ui_lightArc);
  hideAndDisableTouch(ui_LightNum);
  hideAndDisableTouch(ui_Image7);
  hideAndDisableTouch(ui_Label13);
  hideAndDisableTouch(ui_Label16);
  hideAndDisableTouch(ui_Button17);
  hideAndDisableTouch(ui_Button19);

  styleMenuBtn(ui_Button13, "PLAYLIST 1", 46,  130, false);
  styleMenuBtn(ui_Button14, "PLAYLIST 2", 81,  130, false);
  styleMenuBtn(ui_Button15, "PLAYLIST 3", 116, 130, false);
  styleMenuBtn(ui_Button16, "PLAYLIST 4", 151, 130, false);
  styleBackBtn(ui_screen4ReturnBt, 186);

  playlistBtns[0] = ui_Button13;
  playlistBtns[1] = ui_Button14;
  playlistBtns[2] = ui_Button15;
  playlistBtns[3] = ui_Button16;
  playlistBtns[4] = ui_screen4ReturnBt;
  playlistSelIdx = 0;
  updateMenuHighlight(playlistBtns, playlistBtnCount, playlistSelIdx);

  lv_obj_add_event_cb(ui_Button13, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)11);
  lv_obj_add_event_cb(ui_Button14, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)12);
  lv_obj_add_event_cb(ui_Button15, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)13);
  lv_obj_add_event_cb(ui_Button16, haButtonCb, LV_EVENT_CLICKED, (void*)(uintptr_t)14);

  // === SCREEN 5 (CONTROLS SUBMENU) - Icon-based UI ===
  ui_Screen5 = lv_obj_create(NULL);
  lv_obj_set_size(ui_Screen5, 240, 240);
  lv_obj_set_style_bg_color(ui_Screen5, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui_Screen5, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(ui_Screen5, 0, 0);
  lv_obj_clear_flag(ui_Screen5, LV_OBJ_FLAG_SCROLLABLE);

  ctrlTitleLabel = lv_label_create(ui_Screen5);
  lv_label_set_text(ctrlTitleLabel, "");
  lv_obj_set_style_text_color(ctrlTitleLabel, lv_color_white(), 0);
  lv_obj_set_style_text_font(ctrlTitleLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_line_space(ctrlTitleLabel, 5, 0);
  lv_obj_set_align(ctrlTitleLabel, LV_ALIGN_TOP_MID);
  lv_obj_set_y(ctrlTitleLabel, 40);
  lv_obj_set_width(ctrlTitleLabel, 180);
  lv_label_set_long_mode(ctrlTitleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(ctrlTitleLabel, LV_TEXT_ALIGN_CENTER, 0);

  ctrlArtistLabel = lv_label_create(ui_Screen5);
  lv_label_set_text(ctrlArtistLabel, "");
  lv_obj_set_style_text_color(ctrlArtistLabel, lv_color_make(180, 180, 180), 0);
  lv_obj_set_style_text_font(ctrlArtistLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(ctrlArtistLabel, 5, 0);
  lv_obj_set_align(ctrlArtistLabel, LV_ALIGN_TOP_MID);
  lv_obj_set_y(ctrlArtistLabel, 62);
  lv_obj_set_width(ctrlArtistLabel, 160);
  lv_label_set_long_mode(ctrlArtistLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(ctrlArtistLabel, LV_TEXT_ALIGN_CENTER, 0);

  // Icon buttons row: Previous | Play/Pause | Next
  // Each is a circular button with a symbol label
  int iconY = 120;
  int iconSize = 38;
  int spacing = 70;

  // Previous track button (left)
  lv_obj_t *prevBtn = lv_btn_create(ui_Screen5);
  lv_obj_set_size(prevBtn, iconSize, iconSize);
  lv_obj_set_style_radius(prevBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(prevBtn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(prevBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(prevBtn, 0, 0);
  lv_obj_set_style_shadow_width(prevBtn, 0, 0);
  lv_obj_set_align(prevBtn, LV_ALIGN_CENTER);
  lv_obj_set_pos(prevBtn, -spacing, iconY - 120);
  lv_obj_t *prevLbl = lv_label_create(prevBtn);
  lv_label_set_text(prevLbl, LV_SYMBOL_PREV);
  lv_obj_set_style_text_color(prevLbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(prevLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(prevLbl);

  // Play/Pause button (center, larger)
  lv_obj_t *playBtn = lv_btn_create(ui_Screen5);
  lv_obj_set_size(playBtn, iconSize + 7, iconSize + 7);
  lv_obj_set_style_radius(playBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(playBtn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(playBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(playBtn, 0, 0);
  lv_obj_set_style_shadow_width(playBtn, 0, 0);
  lv_obj_set_align(playBtn, LV_ALIGN_CENTER);
  lv_obj_set_pos(playBtn, 0, iconY - 120);
  lv_obj_t *playLbl = lv_label_create(playBtn);
  lv_label_set_text(playLbl, LV_SYMBOL_PLAY " " LV_SYMBOL_PAUSE);
  lv_obj_set_style_text_color(playLbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(playLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(playLbl);

  // Next track button (right)
  lv_obj_t *nextBtn = lv_btn_create(ui_Screen5);
  lv_obj_set_size(nextBtn, iconSize, iconSize);
  lv_obj_set_style_radius(nextBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(nextBtn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(nextBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(nextBtn, 0, 0);
  lv_obj_set_style_shadow_width(nextBtn, 0, 0);
  lv_obj_set_align(nextBtn, LV_ALIGN_CENTER);
  lv_obj_set_pos(nextBtn, spacing, iconY - 120);
  lv_obj_t *nextLbl = lv_label_create(nextBtn);
  lv_label_set_text(nextLbl, LV_SYMBOL_NEXT);
  lv_obj_set_style_text_color(nextLbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(nextLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(nextLbl);

  // BACK button at bottom
  lv_obj_t *ctrlBackBtn = lv_btn_create(ui_Screen5);
  lv_obj_set_size(ctrlBackBtn, 80, 30);
  lv_obj_set_style_radius(ctrlBackBtn, 14, 0);
  lv_obj_set_style_bg_color(ctrlBackBtn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ctrlBackBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ctrlBackBtn, 0, 0);
  lv_obj_set_style_shadow_width(ctrlBackBtn, 0, 0);
  lv_obj_set_align(ctrlBackBtn, LV_ALIGN_CENTER);
  lv_obj_set_y(ctrlBackBtn, 65);
  lv_obj_t *backLbl = lv_label_create(ctrlBackBtn);
  lv_label_set_text(backLbl, "BACK");
  lv_obj_set_style_text_color(backLbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_12, 0);
  lv_obj_center(backLbl);

  // Order: play/pause (0), next (1), back (2), prev (3) — knob cycles through
  controlsBtns[0] = playBtn;
  controlsBtns[1] = nextBtn;
  controlsBtns[2] = ctrlBackBtn;
  controlsBtns[3] = prevBtn;
  controlsSelIdx = 0;
  updateControlsHighlight();

  lv_obj_add_event_cb(playBtn, [](lv_event_t *e) {
    sendToHomeAssistant("media_play_pause");
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(nextBtn, [](lv_event_t *e) {
    sendToHomeAssistant("media_next");
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(prevBtn, [](lv_event_t *e) {
    sendToHomeAssistant("media_previous");
  }, LV_EVENT_CLICKED, NULL);
}

void updateControlsHighlight() {
  for (int i = 0; i < controlsBtnCount; i++) {
    if (i == controlsSelIdx) {
      lv_obj_set_style_outline_color(controlsBtns[i], lv_color_white(), 0);
      lv_obj_set_style_outline_width(controlsBtns[i], 2, 0);
      lv_obj_set_style_outline_pad(controlsBtns[i], 2, 0);
      lv_obj_set_style_outline_opa(controlsBtns[i], LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(controlsBtns[i], 0, 0);
    } else {
      lv_obj_set_style_outline_width(controlsBtns[i], 0, 0);
      lv_obj_set_style_border_width(controlsBtns[i], 0, 0);
    }
  }
}

void applyNowPlaying() {
  bool hasTitle = (strlen(npTitle) > 0);
  bool hasArtist = (strlen(npArtist) > 0);

  const char* displayTitle = hasTitle ? npTitle : (strlen(npButtonTitle) > 0 ? npButtonTitle : "");
  const char* displayArtist = hasArtist ? npArtist : "";

  if (hasTitle || strlen(npButtonTitle) > 0) {
    lv_label_set_text(songTitleLabel, displayTitle);
    lv_obj_clear_flag(songTitleLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(defaultTitleLabel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(songTitleLabel, "");
  }

  lv_label_set_text(artistLabel, displayArtist);

  if (ctrlTitleLabel) lv_label_set_text(ctrlTitleLabel, displayTitle);
  if (ctrlArtistLabel) lv_label_set_text(ctrlArtistLabel, displayArtist);
}

//****************************************************SETUP****************************************************
void setup() {
  Serial.begin(115200);

  pinMode(POWER_LIGHT_PIN, OUTPUT);   
  digitalWrite(POWER_LIGHT_PIN, LOW); 

  pinMode(1, OUTPUT);         // Power pin 1, output current
  digitalWrite(1, HIGH);  
  pinMode(2, OUTPUT);         // Power pin 2, output current
  digitalWrite(2, HIGH);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);  // Initialize I2C bus
  touch.begin();              // Start the touch screen

  gfx.init();               
  gfx.initDMA();
  gfx.startWrite();
  gfx.setColor(0, 0, 0);
  gfx.setTextSize(2);
  gfx.fillScreen(TFT_BLACK);

  pinMode(ENCODER_A_PIN, INPUT);        // Set rotary encoder pins
  pinMode(ENCODER_B_PIN, INPUT);
  pinMode(SWITCH_PIN, INPUT_PULLUP);    // Set switch (press) pin
  attachInterrupt(digitalPinToInterrupt(SWITCH_PIN), buttonISR, CHANGE);

  lv_init();

  size_t buffer_size = sizeof(lv_color_t) * screenWidth * screenHeight;
  buf = (lv_color_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  buf1 = (lv_color_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, buf, buf1, screenWidth * screenHeight);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // Touchscreen disabled - knob-only control
  // static lv_indev_drv_t indev_drv;
  // lv_indev_drv_init(&indev_drv);
  // indev_drv.type = LV_INDEV_TYPE_POINTER;
  // indev_drv.read_cb = my_touchpad_read;
  // lv_indev_drv_register(&indev_drv);
  delay(100);

  ui_init();

  // Create loading screen and show it immediately
  loadingScreen = lv_obj_create(NULL);
  lv_obj_set_size(loadingScreen, 240, 240);
  lv_obj_set_style_bg_color(loadingScreen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(loadingScreen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(loadingScreen, 0, 0);
  lv_obj_clear_flag(loadingScreen, LV_OBJ_FLAG_SCROLLABLE);

  loadingLabel = lv_label_create(loadingScreen);
  lv_label_set_text(loadingLabel, "LOADING");
  lv_obj_set_style_text_color(loadingLabel, lv_color_white(), 0);
  lv_obj_set_style_text_font(loadingLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(loadingLabel, 2, 0);
  lv_obj_set_align(loadingLabel, LV_ALIGN_CENTER);

  lv_scr_load(loadingScreen);
  lv_timer_handler();

  // Apply Home Assistant menu customization after UI is initialized
  customizeUIForHA();

  led.begin();            
  led.setBrightness(0);  
  led.clear();
  led.show();

  delay(500);
  initBacklight();

  // LED task disabled - perimeter LEDs off
  xTaskCreatePinnedToCore(encTask, "ENC", 4096, NULL, 1, &encTaskHandle, 0);

  lastActivityTime = millis();

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    bool haOk = sendToHomeAssistant("boot");
    if (haOk) {
      sendToHomeAssistant("button_1");
      if (defaultTitleLabel) {
        lv_label_set_text(defaultTitleLabel, "ARCADE");
      }
      _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, NULL);
    } else {
      lv_label_set_text(loadingLabel, "FAILED TO CONNECT");
    }
  } else {
    lv_label_set_text(loadingLabel, "FAILED TO CONNECT");
  }
}


void loop() {
  server.handleClient();
  processPendingKnobActions();
  lv_timer_handler();
  if (!screenAsleep && millis() - lastActivityTime >= sleepTimeoutMs) {
    sleepScreen();
  }
  delay(5);
}

void connectWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiId.c_str(), wifiPwd.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    server.on("/volume", HTTP_POST, []() {
      if (server.hasArg("plain")) {
        int newVol = server.arg("plain").toInt();
        if (newVol >= 0 && newVol <= 100) {
          if (millis() - lastLocalVolumeChange < volumeSyncCooldownMs) {
            server.send(200, "application/json", "{\"status\":\"ignored_cooldown\",\"volume\":" + String(currentVolume) + "}");
            return;
          }
          currentVolume = newVol;
          pendingVolumeChange = 1;
          server.send(200, "application/json", "{\"status\":\"ok\",\"volume\":" + String(currentVolume) + "}");
        } else {
          server.send(400, "application/json", "{\"error\":\"volume must be 0-100\"}");
        }
      } else {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
      }
    });

    server.on("/volume", HTTP_GET, []() {
      server.send(200, "application/json", "{\"volume\":" + String(currentVolume) + "}");
    });

    server.on("/nowplaying", HTTP_POST, []() {
      if (server.hasArg("plain")) {
        StaticJsonDocument<1024> doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (err) {
          server.send(400, "application/json", "{\"error\":\"invalid json\"}");
          return;
        }

        const char* title = doc["title"] | "";
        const char* artist = doc["artist"] | "";
        const char* buttonTitle = doc["button_title"] | "";

        strncpy(npTitle, title, sizeof(npTitle) - 1);
        npTitle[sizeof(npTitle) - 1] = '\0';
        strncpy(npArtist, artist, sizeof(npArtist) - 1);
        npArtist[sizeof(npArtist) - 1] = '\0';
        strncpy(npButtonTitle, buttonTitle, sizeof(npButtonTitle) - 1);
        npButtonTitle[sizeof(npButtonTitle) - 1] = '\0';

        pendingNowPlaying = true;
        server.send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
      }
    });

    server.on("/nowplaying", HTTP_GET, []() {
      StaticJsonDocument<512> doc;
      doc["title"] = npTitle;
      doc["artist"] = npArtist;
      doc["button_title"] = npButtonTitle;
      String out;
      serializeJson(doc, out);
      server.send(200, "application/json", out);
    });

    server.begin();
    Serial.println("HTTP server started on port 80");
  } else {
    Serial.println("\nFailed to connect to WiFi");
  }
}

int last_counter = 0;       
int counter = 0;            
int currentStateCLK;        
int lastStateCLK;           
bool one_test = false;      

// Process pending knob actions from the LVGL-safe main loop
void processPendingKnobActions() {
  if (pendingOpenMenu) {
    pendingOpenMenu = false;
    sendToHomeAssistant("default_open_menu");
    menuSelIdx = 0;
    updateMenuHighlight(menuBtns, menuBtnCount, menuSelIdx);
    _ui_screen_change(&ui_Screen2, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
  }
  int dir = pendingNavDirection;
  if (dir != 0) {
    pendingNavDirection = 0;
    navigateMenu(dir);
  }
  if (pendingActivate) {
    pendingActivate = false;
    activateMenuSelection();
  }
  int dc = pendingDoubleClick;
  if (dc != 0) {
    pendingDoubleClick = 0;
    if (dc == 1) {
      sendToHomeAssistant("default_volume_mute");
    } else {
      sendToHomeAssistant("menu_back_default");
      _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    }
  }
  if (pendingVolumeChange != 0) {
    pendingVolumeChange = 0;
    updateVolumeDisplay();
  }
  if (pendingNowPlaying) {
    pendingNowPlaying = false;
    applyNowPlaying();
  }
  if (pendingVolumeSend && (millis() - lastVolumeChangeTime >= volumeDebounceMs)) {
    pendingVolumeSend = false;
    sendToHomeAssistant("set_volume", currentVolume, true);
  }
  int lp = pendingLongPress;
  if (lp != 0) {
    pendingLongPress = 0;
    if (lp == 1) {
      controlsSelIdx = 0;
      updateControlsHighlight();
      _ui_screen_change(&ui_Screen5, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    } else if (lp == 3) {
      sendToHomeAssistant("controls_back_default");
      _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, NULL);
    } else {
      sendToHomeAssistant("menu_long_press");
    }
  }
}

void performClickAction() {
  current_screen = lv_scr_act();
  Serial.printf("Click! screen=%p (S1=%p S2=%p S3=%p S4=%p)\n",
    current_screen, ui_Screen1, ui_Screen2, ui_Screen3, ui_Screen4);
  if (current_screen == ui_Screen1) {
    pendingOpenMenu = true;
  } else {
    pendingActivate = true;
  }
}

void performDoubleClickAction() {
  current_screen = lv_scr_act();
  if (current_screen == ui_Screen1) {
    pendingDoubleClick = 1;
  } else {
    pendingDoubleClick = 2;
  }
}

void performLongPressAction() {
  current_screen = lv_scr_act();
  if (current_screen == ui_Screen1) {
    pendingLongPress = 1;
  } else if (current_screen == ui_Screen5) {
    pendingLongPress = 3;
  } else {
    pendingLongPress = 2;
  }
}

void encTask(void *pvParameters) {
  lastStateCLK = digitalRead(ENCODER_A_PIN);
  while (1) {
    currentStateCLK = digitalRead(ENCODER_A_PIN);

    if (currentStateCLK != lastStateCLK && currentStateCLK == 1) {      
      if (screenAsleep) {
        wakeScreen();
        lastStateCLK = currentStateCLK;
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      wakeScreen();
      current_screen = lv_scr_act();
      if (digitalRead(ENCODER_B_PIN) != currentStateCLK) {          
        if (abs(last_counter - counter) > 200) {                     
          lastStateCLK = currentStateCLK;
          vTaskDelay(pdMS_TO_TICKS(2));
          continue;
        }
        position_tmp = 1;             
        counter++;                
      } else {                    
        if (one_test == false) {
          one_test = true;
          lastStateCLK = currentStateCLK;
          vTaskDelay(pdMS_TO_TICKS(2));
          continue;
        }
        position_tmp = 0;       
        counter--;              
      }

      // Rotary knob: volume on default screen, navigate on menu screens
      if (current_screen == ui_Screen1) {
        if (position_tmp == 1) {
          if (currentVolume < 100) currentVolume += 2;
          pendingVolumeChange = 1;
          pendingVolumeSend = true;
          lastVolumeChangeTime = millis();
          lastLocalVolumeChange = millis();
        } else if (position_tmp == 0) {
          if (currentVolume > 0) currentVolume -= 2;
          pendingVolumeChange = 1;
          pendingVolumeSend = true;
          lastVolumeChangeTime = millis();
          lastLocalVolumeChange = millis();
        }
      } else {
        pendingNavDirection = (position_tmp == 1) ? 1 : -1;
      }

      position_tmp = -1;
      last_counter = counter;     
    }

    lastStateCLK = currentStateCLK;

    // Button handling
    bool pressedNow = (digitalRead(SWITCH_PIN) == LOW);
    if (pressedNow) {
      if (screenAsleep) {
        wakeScreen();
        while (digitalRead(SWITCH_PIN) == LOW) { vTaskDelay(pdMS_TO_TICKS(10)); }
        pressStartTime = 0;
        clickCount = 0;
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      wakeScreen();
      if (pressStartTime == 0) {
        pressStartTime = millis();
        longPressSent = false;
      } else if (!longPressSent && (millis() - pressStartTime >= longPressTime)) {
        performLongPressAction();
        longPressSent = true;
        clickCount = 0; 
      }
    } else {
      if (longPressSent) {
        clickCount = 0;
      }
      pressStartTime = 0;
      longPressSent = false;
    }

    if (clickCount == 1 && !pressedNow && millis() - lastPressTime > doubleClickTime) {
      if (!longPressSent) performClickAction();
      clickCount = 0; 
    }
    if (clickCount >= 2 && !pressedNow) {
      if (!longPressSent) performDoubleClickAction();
      clickCount = 0; 
    }
    if (clickCount > 2) clickCount = 0;

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  if (gfx.getStartCount() > 0) gfx.endWrite();
  gfx.pushImageDMA(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1, (lgfx::rgb565_t *)&color_p->full);
  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  uint16_t touchX, touchY;
  uint8_t gesture;
  if (!touch.getTouch(&touchX, &touchY, &gesture)) {
    data->state = LV_INDEV_STATE_REL;
  } else {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touchX;
    data->point.y = touchY;
  }
}

void sleepScreen() {
  if (!screenAsleep) {
    screenAsleep = true;
    _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_NONE, 0, 0, NULL);
    ledcWrite(pwmChannel, 0);
  }
}

void wakeScreen() {
  if (screenAsleep) {
    screenAsleep = true;
    lastActivityTime = millis();
    ledcWrite(pwmChannel, (50 * 255) / 100);
    screenAsleep = false;
  }
  lastActivityTime = millis();
}

void initBacklight() {
  ledcSetup(pwmChannel, pwmFreq, pwmResolution);
  ledcAttachPin(SCREEN_BACKLIGHT_PIN, pwmChannel);
  ledcWrite(pwmChannel, (50 * 255) / 100);
}

void ledTestTask(void *pvParameters) {
  while (1) {
    led.clear();
    led.show();
    for (int i = 0; i < 5; i++) {
      led.setPixelColor(i, led.Color(255, 255, 255));
      led.show();
      vTaskDelay(pdMS_TO_TICKS(250));
      led.clear();
      led.show();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
