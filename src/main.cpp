/*
 * Max-Plastix Mapper build for
 * Heltec CubeCell GPS-6502 HTCC-AB02S
 *
 * Based on the CubeCell GPS example from
 * libraries\LoRa\examples\LoRaWAN\LoRaWAN_Sensors\LoRaWan_OnBoardGPS_Air530\LoRaWan_OnBoardGPS_Air530.ino
 * and on Jas Williams version from
 * https://github.com/jas-williams/CubeCell-Helium-Mapper.git with GPS Distance
 * and improvements from https://github.com/hkicko/CubeCell-GPS-Helium-Mapper
 *
 * Apologies for inconsistent style and naming; there were many hands in this code and Heltec has quite the Platform library.
 *
 */
#include "Arduino.h"
#include "GPS_Air530Z.h"      // Forsaken Heltec packed-in library
#include "HT_DisplayFonts.h"  // For Arial
#include "HT_SSD1306Wire.h"   // HelTec's old version of SSD1306Wire.h
#include "LoRaWan_APP.h"
#include "TinyGPS++.h"        // More recent/superior library
#include "fonts.inc"          // for Custom_ArialMT_Plain_10
#include "images.h"           // For Satellite icon
#include "configuration.h"    // User configuration
#include "credentials.h"      // Helium LoRaWAN credentials

extern SSD1306Wire display;       // Defined in LoRaWan_APP.cpp (!)
extern uint8_t isDispayOn;      // [sic] Defined in LoRaWan_APP.cpp
#define isDisplayOn isDispayOn  // Seriously.  It's wrong all over LoRaWan_APP.

extern uint32_t UpLinkCounter;  // FCnt, Frame Count, Uplink Sequence Number

extern HardwareSerial GPSSerial;  // Defined in GPSTrans.cpp
Air530ZClass AirGPS;              // Just for Init.  Has some Air530Z specials.
TinyGPSPlus GPS;                  // Avoid the Heltec library as much as we can
boolean is_gps_started = false;

#if defined(REGION_EU868)
/*LoraWan channelsmask, default channels 0-7*/
uint16_t userChannelsMask[6] = {0x00FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
#else
uint16_t userChannelsMask[6] = {0xFF00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
#endif

/* Unset APB stuff, but CubeCellLib.a requires that we declare them */
uint8_t nwkSKey[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t appSKey[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint32_t devAddr = 0;

/* LoRaWAN Configuration.  Edit in platformio.ini */
LoRaMacRegion_t loraWanRegion = ACTIVE_REGION;
DeviceClass_t loraWanClass = LORAWAN_CLASS;

/* Application data transmission duty cycle.  value in [ms]. */
uint32_t appTxDutyCycle = 1000 - 100;  // Must be a bit less than GPS update rate!

/* OTAA or ABP.  (Helium only supports OTAA) */
bool overTheAirActivation = LORAWAN_NETMODE;

/* ADR enable */
bool loraWanAdr = LORAWAN_ADR;

/* set LORAWAN_Net_Reserve ON, the node could save the network info to flash,
 * when node reset not need to join again */
bool keepNet = LORAWAN_NET_RESERVE;

/* Indicates if the node is sending confirmed or unconfirmed messages */
bool isTxConfirmed = LORAWAN_UPLINKMODE;

#define APP_PORT_DEFAULT 2  // Mapper

/* Application port */
uint8_t appPort = APP_PORT_DEFAULT;

/*!
  Number of trials to transmit the frame, if the LoRaMAC layer did not
  receive an acknowledgment. The MAC performs a datarate adaptation,
  according to the LoRaWAN Specification V1.0.2, chapter 18.4, according
  to the following table:
  Transmission nb | Data Rate
  ----------------|-----------
  1 (first)       | DR
  2               | DR
  3               | max(DR-1,0)
  4               | max(DR-1,0)
  5               | max(DR-2,0)
  6               | max(DR-2,0)
  7               | max(DR-3,0)
  8               | max(DR-3,0)
  Note, that if NbTrials is set to 1 or 2, the MAC will not decrease
  the datarate, in case the LoRaMAC layer did not receive an acknowledgment
*/
uint8_t confirmedNbTrials = 4;

static TimerEvent_t BatteryUpdateTimer;
static TimerEvent_t ScreenUpdateTimer;
static TimerEvent_t KeyDownTimer;
static TimerEvent_t MenuIdleTimer;

unsigned int tx_interval_s = 60;
unsigned int stationary_tx_interval_s = 60;
float min_dist_moved = 68;
char *cached_sf_name = "";
boolean in_menu = false;
boolean justSendNow = false;

const char *menu_prev;
const char *menu_cur;
const char *menu_next;
boolean is_highlighted = false;
int menu_entry = 0;

#define LONG_PRESS_MS 500
boolean key_down = false;
uint32_t keyDownTime;

void userKeyIRQ(void);

boolean long_press = false;

uint32_t last_fix = 0;
double last_send_lat, last_send_lon;
uint32_t last_send_ms;

char buffer[40];

#if 0
int32_t fracPart(double val, int n) {
  return (int32_t)abs(((val - (int32_t)(val)) * pow(10, n)));
}
#endif

// SK6812 (WS2812).  DIN is IO13/GP13/Pin45
void testRGB(void) {
  for (uint32_t i = 0; i <= 30; i++) {
    turnOnRGB(i << 16, 10);
  }
  for (uint32_t i = 0; i <= 30; i++) {
    turnOnRGB(i << 8, 10);
  }
  for (uint32_t i = 0; i <= 30; i++) {
    turnOnRGB(i, 10);
  }
  turnOnRGB(0, 0);
}

// RGB LED power on (Vext to SK5812 pin 4)
void VextON(void) {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

// RGB LED power off
void VextOFF(void) {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH);
}

void printGPSInfo(void) {
  // Serial.print("Date/Time: ");
  if (GPS.date.isValid()) {
    Serial.printf("%d-%02d-%02d", GPS.date.year(), GPS.date.month(), GPS.date.day());
  } else {
    Serial.print("INVALID");
  }

  if (GPS.time.isValid()) {
    Serial.printf(" %02d:%02d:%02d.%02d", GPS.time.hour(), GPS.time.minute(), GPS.time.second(), GPS.time.centisecond());
  } else {
    Serial.print(" INVALID");
  }
  // Serial.println();

#if 0
  Serial.print(" LAT: ");
  Serial.print(GPS.location.lat(), 6);
  Serial.print(", LON: ");
  Serial.print(GPS.location.lng(), 6);
  Serial.print(", ALT: ");
  Serial.print(GPS.altitude.meters());

  Serial.print(", HDOP: ");
  Serial.print(GPS.hdop.hdop());
  Serial.print(", AGE: ");
  Serial.print(GPS.location.age());
  Serial.print(", COURSE: ");
  Serial.print(GPS.course.deg());
  Serial.print(", SPEED: ");
  Serial.print(GPS.speed.kmph());
#endif
  Serial.print(", Sats: ");
  Serial.print(GPS.satellites.value());

  Serial.println();
}

void displayLogoAndMsg(String msg, uint32_t wait_ms) {
  display.clear();
  display.drawXbm(0, 0, 128, 42, helium_logo_bmp);
  // displayBatteryLevel();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 54 - 16 / 2, msg);
  Serial.println(msg);
  display.display();

  if (wait_ms) {
    delay(wait_ms);
  }
}

boolean screenOffMode = false;

void switchScreenOffMode() {
  screenOffMode = true;
  VextOFF();
  display.stop();
  isDisplayOn = 0;
}

void switchScreenOnMode() {
  screenOffMode = false;
  VextON();
  isDisplayOn = 1;
  display.init();
  display.clear();
  display.display();
}

// Fetch Data Rate? (DR_0 to DR_5)
int8_t loraDataRate(void) {
  MibRequestConfirm_t mibReq;
  LoRaMacStatus_t status;
  int8_t ret = -1;

  mibReq.Type = MIB_CHANNELS_DATARATE;
  status = LoRaMacMibGetRequestConfirm(&mibReq);
  if (status == LORAMAC_STATUS_OK) {
    ret = mibReq.Param.ChannelsDatarate;
  }

  return ret;
}

uint16_t gpsSearchStart;

void start_gps() {
  Serial.println("Starting GPS:");
  AirGPS.begin(115200);  // Faster messages; less CPU, more idle.
  AirGPS.setmode(MODE_GPS_BEIDOU_GLONASS);
  AirGPS.setNMEA(NMEA_RMC | NMEA_GGA);  // Eliminate unused message traffic like SV

#if 1
  // GPSSerial.write("$PCAS02,500*1A\r\n"); /* 500mS updates */
  GPSSerial.write("$PCAS02,1000*2E\r\n"); /* 1S updates */
  GPSSerial.flush();
  delay(10);
#endif

  // Adjust the Green 1pps LED to have shorter blinks.
  const uint8_t cmdbuf[] = {0xBA, 0xCE, 0x10, 0x00, 0x06, 0x03, 0x40, 0x42, 0x0F, 0x00, 0x10, 0x27, 0x00,
                            0x00, 0x03, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x63, 0x69, 0x15, 0x0B};
  /* CFG-TX Time Pulse: 10mS width (vs 100), only if fixed, UTC Time, Automatic source */
  GPSSerial.write(cmdbuf, sizeof(cmdbuf));
  GPSSerial.flush();
  delay(10);

  gpsSearchStart = millis();
  is_gps_started = true;
  Serial.println("..done.");
}

void update_gps() {
  while (GPSSerial.available()) {
    int c = GPSSerial.read();
    if (c > 0) {
      char c8 = c;
      // Serial.print(c8);
      GPS.encode(c8);
    }
  }

  if (GPS.location.isValid() && GPS.location.isUpdated() && GPS.time.isValid() && GPS.satellites.value() > 3) {
    // Serial.printf(" %02d:%02d:%02d.%02d\n", GPS.time.hour(), GPS.time.minute(), GPS.time.second(), GPS.time.centisecond());
    // printGPSInfo();
    last_fix = millis();
    GPS.location.lat();  // Reset the isUpdated
  }
}

void stopGPS() {
  AirGPS.end();
}

uint16_t battery_mv;
void update_battery_mv(void) {
  detachInterrupt(USER_KEY);
  battery_mv = getBatteryVoltage() * VBAT_CORRECTION;
  attachInterrupt(USER_KEY, userKeyIRQ, BOTH);
  Serial.printf("Bat: %d mV\n", battery_mv);
}

#define BATTERY_UPDATE_RATE_MS (10 * 1000)
void onBatteryUpdateTimer(void) {
  update_battery_mv();
  TimerSetValue(&BatteryUpdateTimer, BATTERY_UPDATE_RATE_MS);
  TimerStart(&BatteryUpdateTimer);
}

/* Populate appData/appDataSize with Mapper frame */
bool prepare_map_uplink(uint8_t port) {
  uint32_t lat, lon;
  int alt, course, speed, hdop, sats;

  unsigned char *puc;

  appDataSize = 0;

  if (!GPS.location.isValid())
    return false;

  last_send_lat = GPS.location.lat();
  last_send_lon = GPS.location.lng();

  lat = ((last_send_lat + 90) / 180.0) * 16777215;
  lon = ((last_send_lon + 180) / 360.0) * 16777215;

  alt = (uint16_t)GPS.altitude.meters();
  course = GPS.course.deg();
  speed = (uint16_t)GPS.speed.kmph();
  sats = GPS.satellites.value();
  hdop = GPS.hdop.hdop();

  uint16_t batteryVoltage = ((float_t)((float_t)((float_t)battery_mv * VBAT_CORRECTION) / 10) + 0.5);

  puc = (unsigned char *)(&lat);
  appData[appDataSize++] = puc[2];
  appData[appDataSize++] = puc[1];
  appData[appDataSize++] = puc[0];

  puc = (unsigned char *)(&lon);
  appData[appDataSize++] = puc[2];
  appData[appDataSize++] = puc[1];
  appData[appDataSize++] = puc[0];

  puc = (unsigned char *)(&alt);
  appData[appDataSize++] = puc[1];
  appData[appDataSize++] = puc[0];

  puc = (unsigned char *)(&speed);
  appData[appDataSize++] = puc[0];

  appData[appDataSize++] = (uint8_t)((batteryVoltage - 200) & 0xFF);

  appData[appDataSize++] = (uint8_t)(sats & 0xFF);
  return true;
}

void gps_passthrough(void) {
  Serial.println("GPS Passthrough forever...");

  while (1) {
    if (GPSSerial.available())
      Serial.write(GPSSerial.read());
    if (Serial.available())
      GPSSerial.write(Serial.read());
  }
}

#define SCREEN_HEADER_HEIGHT 24
uint8_t _screen_line = SCREEN_HEADER_HEIGHT - 1;
SSD1306Wire *disp;

void draw_screen(void);

#define SCREEN_UPDATE_RATE_MS (500)
void onScreenUpdateTimer(void) {
  draw_screen();
  TimerSetValue(&ScreenUpdateTimer, SCREEN_UPDATE_RATE_MS);
  TimerStart(&ScreenUpdateTimer);
}

void screen_print(const char *text, uint8_t x, uint8_t y, uint8_t alignment) {
  disp->setTextAlignment((DISPLAY_TEXT_ALIGNMENT)alignment);
  disp->drawString(x, y, text);
}

void screen_print(const char *text, uint8_t x, uint8_t y) {
  screen_print(text, x, y, TEXT_ALIGN_LEFT);
}

void screen_print(const char *text) {
  Serial.printf(">>> %s\n", text);

  disp->print(text);
  if (_screen_line + 8 > disp->getHeight()) {
    // scroll
  }
  _screen_line += 8;
}

void screen_setup() {
  // Display instance
  disp = &display;
  disp->setFont(Custom_ArialMT_Plain_10);

  // Scroll buffer
  disp->setLogBuffer(4, 40);
}

struct menu_item {
  const char *name;
  void (*func)(void);
};

void menu_send_now(void) {
  justSendNow = true;
}
void menu_power_off(void) {
  screen_print("\nPOWER OFF...\n");
  delay(4000);  // Give some time to read the screen
  // clean_shutdown();
}
void menu_flush_prefs(void) {
  screen_print("\nFlushing Prefs!\n");
}
void menu_distance_plus(void) {
  min_dist_moved += 10;
}
void menu_distance_minus(void) {
  min_dist_moved -= 10;
  if (min_dist_moved < 10)
    min_dist_moved = 10;
}
void menu_time_plus(void) {
  stationary_tx_interval_s += 60;
}
void menu_time_minus(void) {
  stationary_tx_interval_s -= 60;
  if (stationary_tx_interval_s < 60)
    stationary_tx_interval_s = 60;
}
void menu_gps_passthrough(void) {
  gps_passthrough();
  // Does not return.
}

struct menu_item menu[] = {
    {"Send Now", menu_send_now}, {"Power Off", menu_power_off}, {"Distance +", menu_distance_plus}, {"Distance -", menu_distance_minus},
    {"Time +", menu_time_plus},  {"Time -", menu_time_minus},   {"Flush Prefs", menu_flush_prefs},  {"USB GPS", menu_gps_passthrough},
};
#define MENU_ENTRIES (sizeof(menu) / sizeof(menu[0]))

#define MENU_TIMEOUT_MS 3000

void onMenuIdleTimer(void) {
  in_menu = false;
  is_highlighted = false;
}

void menu_press(void) {
  if (in_menu)
    menu_entry = (menu_entry + 1) % MENU_ENTRIES;
  else
    in_menu = true;

  menu_prev = menu[(menu_entry - 1) % MENU_ENTRIES].name;
  menu_cur = menu[menu_entry].name;
  menu_next = menu[(menu_entry + 1) % MENU_ENTRIES].name;

  draw_screen();
  TimerSetValue(&MenuIdleTimer, MENU_TIMEOUT_MS);
  TimerStart(&MenuIdleTimer);
}

void menu_selected(void) {
  if (in_menu) {
    is_highlighted = true;
    draw_screen();
    menu[menu_entry].func();  // Might not return.
  } else {
    in_menu = true;
    draw_screen();
  }
}

void menu_deselected(void) {
  is_highlighted = false;
  draw_screen();
}

void onKeyDownTimer(void) {
  // Long Press!
  long_press = true;
  menu_selected();
}

// Interrupt handler for button press
void userKeyIRQ(void) {
  if (!key_down && digitalRead(USER_KEY) == LOW) {
    // Key Pressed
    key_down = true;
    keyDownTime = millis();
    TimerSetValue(&KeyDownTimer, LONG_PRESS_MS);
    TimerStart(&KeyDownTimer);
    long_press = false;
  }

  if (key_down && digitalRead(USER_KEY) == HIGH) {
    // Key Released
    key_down = false;
    if (long_press) {
      menu_deselected();
    } else {
      TimerStop(&KeyDownTimer);  // Cancel timer
      menu_press();
    }
#if 1
    uint32_t press_ms = millis() - keyDownTime;
    Serial.printf("[Key Pressed %lu ms.]\n", press_ms);
#endif
  }

  // Is this kosher?
  TimerReset(&MenuIdleTimer);
}

void gps_time(char *buffer, uint8_t size) {
  snprintf(buffer, size, "%02d:%02d:%02d", GPS.time.hour(), GPS.time.minute(), GPS.time.second());
}

void screen_header(void) {
  uint32_t sats;

  sats = GPS.satellites.value();

  // Cycle display every 3 seconds
  if (millis() % 6000 < 3000) {
    // 2 bytes of Device EUI with Voltage and Current
    snprintf(buffer, sizeof(buffer), "#%04X", (devEui[6] << 8) | devEui[7]);
    disp->setTextAlignment(TEXT_ALIGN_LEFT);
    disp->drawString(0, 2, buffer);

    snprintf(buffer, sizeof(buffer), "%d.%02dV", battery_mv / 1000, (battery_mv % 1000) / 10);
  } else {
    if (!GPS.time.isValid() || sats < 3)
      snprintf(buffer, sizeof(buffer), "*** NO GPS ***");
    else
      gps_time(buffer, sizeof(buffer));
  }

  disp->setTextAlignment(TEXT_ALIGN_CENTER);
  disp->drawString(disp->getWidth() / 2, 2, buffer);

  // Satellite count
  disp->setTextAlignment(TEXT_ALIGN_RIGHT);
  disp->drawString(disp->getWidth() - SATELLITE_IMAGE_WIDTH - 4, 2, itoa(sats, buffer, 10));
  disp->drawXbm(disp->getWidth() - SATELLITE_IMAGE_WIDTH, 0, SATELLITE_IMAGE_WIDTH, SATELLITE_IMAGE_HEIGHT, SATELLITE_IMAGE);

  // Second status row:
  snprintf(buffer, sizeof(buffer), "%us   %dm", stationary_tx_interval_s, (int)min_dist_moved);
  disp->setTextAlignment(TEXT_ALIGN_LEFT);
  disp->drawString(0, 12, buffer);

  disp->setTextAlignment(TEXT_ALIGN_RIGHT);
  disp->drawString(disp->getWidth(), 12, cached_sf_name);

  disp->drawHorizontalLine(0, SCREEN_HEADER_HEIGHT, disp->getWidth());
}

#define MARGIN 15

void draw_screen(void) {
  disp->setFont(Custom_ArialMT_Plain_10);
  disp->clear();
  screen_header();

  if (in_menu) {
    disp->setTextAlignment(TEXT_ALIGN_CENTER);
    disp->drawString(disp->getWidth() / 2, SCREEN_HEADER_HEIGHT + 5, menu_prev);
    disp->drawString(disp->getWidth() / 2, SCREEN_HEADER_HEIGHT + 28, menu_next);
    // if (is_highlighted)
    //    disp->clear();
    disp->drawHorizontalLine(MARGIN, SCREEN_HEADER_HEIGHT + 16, disp->getWidth() - MARGIN * 2);
    snprintf(buffer, sizeof(buffer), is_highlighted ? ">>> %s <<<" : "%s", menu_cur);
    disp->drawString(disp->getWidth() / 2, SCREEN_HEADER_HEIGHT + 16, buffer);
    disp->drawHorizontalLine(MARGIN, SCREEN_HEADER_HEIGHT + 28, disp->getWidth() - MARGIN * 2);
    disp->drawVerticalLine(MARGIN, SCREEN_HEADER_HEIGHT + 16, 28 - 16);
    disp->drawVerticalLine(disp->getWidth() - MARGIN, SCREEN_HEADER_HEIGHT + 16, 28 - 16);
  } else {
    disp->drawLogBuffer(0, SCREEN_HEADER_HEIGHT);
  }
  disp->display();
}

void setup() {
  boardInitMcu();

  Serial.begin(115200);

  if (!(devEui[0] || devEui[1] || devEui[2] || devEui[3] || devEui[4] || devEui[5] || devEui[6] || devEui[7]))
    LoRaWAN.generateDeveuiByChipID();  // Overwrite devEui with chip-unique value

#if (STAGING_CONSOLE)
  devEui[0] = 0xBB;
#endif

#if (AT_SUPPORT)
  enableAt();
#endif

  display.init();  // displayMcuInit() will init the display, but if we want to
                   // show our logo before that, we need to init ourselves.
  isDisplayOn = 1;
  displayLogoAndMsg("MaxP Map", 100);

  start_gps();  // GPS takes the longest to settle.

  LoRaWAN.displayMcuInit();  // This inits and turns on the display
  deviceState = DEVICE_STATE_INIT;

  /* This will switch deviceState to DEVICE_STATE_SLEEP and schedule a SEND
   timer which will switch to DEVICE_STATE_SEND if saved network info exists
   and no new JOIN is necessary */
  LoRaWAN.ifskipjoin();

  // Setup user button - this must be after LoRaWAN.ifskipjoin(), because the
  // button is used there to cancel stored settings load and initiate a new join
  pinMode(USER_KEY, INPUT);
  attachInterrupt(USER_KEY, userKeyIRQ, BOTH);

  screen_setup();

  TimerInit(&BatteryUpdateTimer, onBatteryUpdateTimer);
  onBatteryUpdateTimer();

  TimerInit(&ScreenUpdateTimer, onScreenUpdateTimer);
  onScreenUpdateTimer();

  TimerInit(&KeyDownTimer, onKeyDownTimer);

  TimerInit(&MenuIdleTimer, onMenuIdleTimer);
}

uint32_t joinStart;
uint32_t lastSend;
boolean first_send = true;
double min_dist_m = MIN_DIST_M;
uint32_t max_time_ms = MAX_TIME_S * 1000;

#if 0
      // only if screen on mode (otherwise the dispaly object is
      // not initialized and calling methods from it will cause
      // a crash)
      if (0 && !screenOffMode && isDisplayOn) {
        display.sleep();
        VextOFF();
        isDisplayOn = 0;
      }

#endif

//#define MIN_DIST_M              68.0       // Minimum distance in meters from the last sent location before we can send again. A hex is about 340m.
//#define MAX_TIME_S              ( 2 * 60)  // If no minimum movement, the LoRa frame will still be sent once every N seconds

//#define REST_WAIT_S             (30 * 60)  // If we still haven't moved in this many seconds, start sending even slower
//#define REST_TIME_S             (10 * 60)  // Slow resting ping frequency in seconds

boolean send_mapper_uplink(void) {
  uint32_t now = millis();

  if (!GPS.location.isValid())
    return false;

  double dist_moved = GPS.distanceBetween(last_send_lat, last_send_lon, GPS.location.lat(), GPS.location.lng());

  Serial.printf("[%ds, %dm]\n", (now - last_send_ms) / 1000, (int32_t)dist_moved);
  char because = '?';
  if (justSendNow) {
    justSendNow = false;
    Serial.println("** SEND_NOW");
    because = '>';
  } else if (dist_moved > min_dist_m) {
    Serial.println("** MOVING");
    // last_moved_millis = now;
    because = 'D';
  } else if (now - last_send_ms > max_time_ms) {
    Serial.println("** TIME");
    because = 'T';
  } else {
    return false;  // Nothing to do, go home early
  }

  if (!prepare_map_uplink(appPort))  // Don't send bad data
    return false;

#if 0
  if (!screenOffMode) {
    LoRaWAN.displaySending();
  }
#endif


  // The first distance-moved is crazy, since has no origin.. don't put it on screen.
  if (dist_moved > 1000000)
    dist_moved = 0;

  printGPSInfo();
  snprintf(buffer, sizeof(buffer), "%d %c %ds %dm\n", UpLinkCounter, because, (now - last_send_ms) / 1000, (int32_t)dist_moved);
  // Serial.print(buffer);
  screen_print(buffer);

  LoRaWAN.send();
  last_send_ms = now;
}

void loop() {
  update_gps();  // Digest any pending bytes to update position

  switch (deviceState) {
    case DEVICE_STATE_INIT: {
      Serial.print("[INIT] ");
#if (AT_SUPPORT)
      getDevParam();
#endif
      printDevParam();
      LoRaWAN.init(loraWanClass, loraWanRegion);
      LoRaWAN.setDataRateForNoADR(0);  // Set DR_0
      // deviceState = DEVICE_STATE_JOIN;
      break;
    }
    case DEVICE_STATE_JOIN: {
      Serial.print("[JOIN] ");
      LoRaWAN.displayJoining();
      LoRaWAN.join();
      joinStart = millis();
      break;
    }
    case DEVICE_STATE_SEND: {
      if (first_send) {
        screen_print("Joined Helium!\n");
        justSendNow = true;
        first_send = false;
      }
      if (!send_mapper_uplink())
        deviceState = DEVICE_STATE_CYCLE;  // take a break;
      break;
    }
    case DEVICE_STATE_CYCLE: {
      // Serial.print("[CYCLE] ");

      LoRaWAN.cycle(appTxDutyCycle);  // Sets a timer to check state
      deviceState = DEVICE_STATE_SLEEP;
      break;
    }
    case DEVICE_STATE_SLEEP: {
      extern bool wakeByUart;  // Declared in binary-only AT_Command.o!
      wakeByUart = true;       // Should awake before GPS activity, but just in case..
      // Serial.end();
      // GPSSerial.end();
      LoRaWAN.sleep();  // Causes serial port noise
      // Serial.begin(115200);
      // GPSSerial.begin(115200);
      break;
    }
    default: {
      Serial.printf("Surprising state: %d\n", deviceState);
      deviceState = DEVICE_STATE_INIT;
      break;
    }
  }
}