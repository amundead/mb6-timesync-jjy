/* MB6 TimeSync v0.4.5 - original ESP32-WROOM, Arduino ESP32 3.x.
 * USB 5V powers ESP32/expansion board and MT3608 input.
 * GPIO25 (3.3V) junction -> 1k resistor -> 2N2222 Base.
 * Same junction -> 330 ohm resistor -> LED anode; cathode -> common GND.
 * 2N2222 Emitter -> common GND; Collector -> LC node T2.
 * MT3608 VOUT+ (~12V) -> LC node T1. Coil and 15nF capacitor in parallel
 * between T1 and T2. Collector is NOT wired directly to GND.
 * Verify transistor pinout, coil current and Collector peak voltage.
 * OLED SH1106 1.3": VCC 3V3, GND, SDA21, SCL22.
 * WiFi stays ON. Auto windows: 8 minutes. Manual: 10 minutes.
 * Select each Auto slot in Web UI; no selected slots forces Auto OFF.
 * Auto and slot selection are saved together in Preferences.
 * Deep sleep at 04:10 MY; an active Manual session finishes first.
 * No timed wakeup: restart with USB power cycle or EN/reset.
 * ESP32 sleep does not cut USB power to MT3608 or the board power LED.
 * Fill WiFi placeholders locally before upload.
 */
#include <Arduino.h>
#include <initializer_list>
#include <cstring>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <esp_arduino_version.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <sys/time.h>

// Isi maklumat Wi-Fi sendiri sebelum upload.
constexpr char WIFI_SSID[]="NAMA_WIFI_2_4GHZ";
constexpr char WIFI_PASSWORD[]="KATA_LALUAN_WIFI";

constexpr uint8_t TX_PIN=25;
constexpr uint8_t PIN_SDA=21, PIN_SCL=22;
constexpr uint32_t CARRIER_HZ=60000;
constexpr int AUTO_TX_MINUTES=8;
constexpr int MANUAL_TX_MINUTES=10;
constexpr int64_t MAX_NTP_AGE_US=2LL*3600*1000000;
constexpr uint32_t NTP_INTERVAL_MS=3600000;
constexpr char NTP_SERVER_1[]="ntp1.sirim.my";
constexpr char NTP_SERVER_2[]="time.cloudflare.com";
constexpr int64_t NTP_ATTEMPT_US=30LL*1000000;
constexpr int64_t NTP_RETRY_US=120LL*1000000;
// Jadual Auto ikut waktu Malaysia UTC+8; kandungan JJY ikut JST UTC+9.

#include <stdint.h>
#include <time.h>

namespace jjy {
constexpr int64_t SECOND = 1000000;
constexpr int64_t MINUTE = 60 * SECOND;
struct Frame { uint8_t bit[60] = {}; };
struct Civil { int year, month, day, hour, minute, second, yday, weekday; };
Civil civil(int64_t utcSeconds, int offsetHours);
Frame encode(const Civil &jst);
int pulseUs(uint8_t symbol);
bool specialMinute(int minute);
bool scheduled(int malaysiaMinute,uint8_t slotMask);
int minutesUntilNext(int malaysiaMinute,uint8_t slotMask);

struct Input {
  int64_t utcUs, monoUs;
  bool trusted, requested;
  uint32_t syncGeneration;
};
struct Output {
  bool gate = false, active = false;
  int second = 0;
  uint32_t aborted = 0, started = 0;
};
class Engine {
 public:
  Output tick(const Input &in);
 private:
  Frame frame_;
  int64_t frameMinute_ = -1, blockedMinute_ = -1;
  int64_t lastUtc_ = 0, lastMono_ = 0;
  uint32_t generation_ = 0;
  bool seen_ = false, active_ = false;
  Output out_;
};
}

#include <stdlib.h>
#include <initializer_list>
namespace jjy {

// Waktu mula Auto dalam minit, mengikut jam Malaysia.
constexpr int AUTO_START_MINUTES[]={1439,119,239}; // 23:59, 01:59, 03:59
constexpr uint8_t AUTO_SLOT_COUNT=sizeof(AUTO_START_MINUTES)/sizeof(AUTO_START_MINUTES[0]);
constexpr uint8_t ALL_SLOTS=(1U<<AUTO_SLOT_COUNT)-1;

// BEGIN SCHEDULE_SETTINGS -- one NVS byte keeps Auto and slots consistent.
uint8_t selectedSlots(uint8_t mask) {return mask & ALL_SLOTS;}
bool autoAllowed(bool enabled,uint8_t mask) {return enabled && selectedSlots(mask)!=0;}
uint8_t packSettings(bool enabled,uint8_t mask) {
  return static_cast<uint8_t>((selectedSlots(mask)<<1) | (autoAllowed(enabled,mask)?1:0));
}
uint8_t unpackSlots(uint8_t packed) {return selectedSlots(packed>>1);}
bool unpackAuto(uint8_t packed) {return autoAllowed((packed&1)!=0,unpackSlots(packed));}
bool parseSlotMask(const char *value,uint8_t &mask) {
  if(!value || value[0]<'0' || value[0]>'0'+ALL_SLOTS || value[1]!='\0')return false;
  mask=static_cast<uint8_t>(value[0]-'0');
  return true;
}
// END SCHEDULE_SETTINGS

Civil civil(int64_t utcSeconds, int offsetHours) {
  time_t shifted = static_cast<time_t>(utcSeconds + offsetHours * 3600LL);
  struct tm t = {};
  gmtime_r(&shifted, &t);
  return {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
          t.tm_min, t.tm_sec, t.tm_yday + 1, t.tm_wday};
}
static void digit(Frame &f, int value, const int *positions, int n) {
  for (int i = 0; i < n; ++i)
    f.bit[positions[i]] = (value >> (n - 1 - i)) & 1;
}
Frame encode(const Civil &t) {
  Frame f;
  const int mt[] = {1,2,3}, mu[] = {5,6,7,8};
  const int ht[] = {12,13}, hu[] = {15,16,17,18};
  const int dh[] = {22,23}, dt[] = {25,26,27,28}, du[] = {30,31,32,33};
  const int yt[] = {41,42,43,44}, yu[] = {45,46,47,48}, wd[] = {50,51,52};
  digit(f,t.minute/10,mt,3);
  digit(f,t.minute%10,mu,4);
  digit(f,t.hour/10,ht,2);
  digit(f,t.hour%10,hu,4);
  digit(f,t.yday/100,dh,2);
  digit(f,(t.yday/10)%10,dt,4);
  digit(f,t.yday%10,du,4);
  digit(f,(t.year%100)/10,yt,4);
  digit(f,t.year%10,yu,4);
  digit(f,t.weekday,wd,3);
  for (int i=12;i<=18;++i) f.bit[36] ^= f.bit[i];
  for (int i=1;i<=8;++i) f.bit[37] ^= f.bit[i];
  for (int p : {0,9,19,29,39,49,59}) f.bit[p]=2;
  return f;
}
int pulseUs(uint8_t symbol) {
  return symbol==2 ? 200000 : symbol==1 ? 500000 : 800000;
}
bool specialMinute(int minute) {
  return minute==15 || minute==45;
}
bool scheduled(int m,uint8_t slotMask) {
  for(uint8_t i=0;i<AUTO_SLOT_COUNT;++i)
    if((slotMask & (1U<<i)) && (m-AUTO_START_MINUTES[i]+1440)%1440<AUTO_TX_MINUTES)
      return true;
  return false;
}
int minutesUntilNext(int m,uint8_t slotMask) {
  int best=-1;
  for(uint8_t i=0;i<AUTO_SLOT_COUNT;++i) {
    if(!(slotMask & (1U<<i)))continue;
    int d=(AUTO_START_MINUTES[i]-m+1440)%1440;
    if(best<0 || d<best) best=d;
  }
  return best;
}
Output Engine::tick(const Input &in) {
  if(in.utcUs<0) {
    active_=false;
    out_.gate=false;
    out_.active=false;
    return out_;
  }
  const int64_t minute=in.utcUs/MINUTE;
  const int64_t phase=in.utcUs%MINUTE;
  bool discontinuity=false;
  if(seen_) {
    const int64_t dm=in.monoUs-lastMono_, dw=in.utcUs-lastUtc_;
    discontinuity = dm<0 || dm>5000 || llabs(dw-dm)>5000 ||
                    generation_!=in.syncGeneration;
  }
  seen_=true;
  lastMono_=in.monoUs;
  lastUtc_=in.utcUs;
  generation_=in.syncGeneration;
  if(discontinuity) {
    if(active_) ++out_.aborted;
    active_=false;
    blockedMinute_=minute;
  }
  if(!in.trusted || !in.requested) {
    if(active_) blockedMinute_=minute;
    active_=false;
  }
  if(active_ && frameMinute_!=minute) active_=false;
  if(!active_ && in.trusted && in.requested &&
     minute!=blockedMinute_ && phase<3000) {
    const Civil t=civil(in.utcUs/SECOND,9);
    if(!specialMinute(t.minute)) {
      frame_=encode(t);
      frameMinute_=minute;
      active_=true;
      ++out_.started;
    }
  }
  out_.second=static_cast<int>(phase/SECOND);
  out_.active=active_;
  out_.gate=active_ && phase%SECOND<pulseUs(frame_.bit[out_.second]);
  return out_;
}
}

// BEGIN DAILY_SHUTDOWN_POLICY -- pure logic, also exercised by host tests.
namespace daily_shutdown {
constexpr int64_t SECOND_US=1000000;
constexpr int64_t DAY_SECONDS=86400;
constexpr int64_t MY_OFFSET_SECONDS=8*3600;
constexpr int64_t SLEEP_SECOND_OF_DAY=(4*60+10)*60; // 04:10 Malaysia
class Policy {
 public:
  bool update(int64_t utc,int64_t mono,bool freshTime,bool manual) {
    if(!armed_) {
      if(!freshTime || utc<1704067200LL)return false;
      const int64_t localDay=(utc+MY_OFFSET_SECONDS)/DAY_SECONDS;
      deadlineUtc_=localDay*DAY_SECONDS+SLEEP_SECOND_OF_DAY-MY_OFFSET_SECONDS;
      // A fresh boot at/after 04:10 stays awake until the NEXT 04:10.
      if(deadlineUtc_<=utc)deadlineUtc_+=DAY_SECONDS;
      deadlineMono_=mono+(deadlineUtc_-utc)*SECOND_US;
      armed_=true;
    } else if(!pending_ && freshTime) {
      // Fresh NTP corrects the deadline; only the main loop owns this policy.
      if(utc>=deadlineUtc_)pending_=true;
      else deadlineMono_=mono+(deadlineUtc_-utc)*SECOND_US;
    }
    // After the first trusted sync, loss of WiFi/NTP does not cancel shutdown.
    if(!pending_ && mono>=deadlineMono_)pending_=true;
    return pending_ && !manual;
  }
  bool armed() const {return armed_;}
  bool pending() const {return pending_;}
  int64_t deadlineUtc() const {return deadlineUtc_;}
 private:
  bool armed_=false,pending_=false;
  int64_t deadlineUtc_=0,deadlineMono_=0;
};
}
// END DAILY_SHUTDOWN_POLICY

struct SharedState {
  bool synced=false, autoEnabled=false, inhibit=false, outputFault=false;
  uint8_t slotMask=jjy::ALL_SLOTS;
  int64_t lastSyncMono=0, lastSyncUtc=0, manualStart=0, manualUntil=0;
  uint32_t generation=0, appliedGeneration=0;
  uint32_t ntpSyncCount=0;
  uint8_t ntpSource=0; // 0=none, 1=SIRIM, 2=Cloudflare; last successful reply.
  jjy::Output tx;
};

inline bool manualActive(const SharedState &s,int64_t mono) {
  return s.manualUntil>mono;
}
inline bool txRequested(const SharedState &s,int64_t mono,int localMinute) {
  if(s.inhibit) return false;
  if(manualActive(s,mono)) return mono>=s.manualStart;
  return jjy::autoAllowed(s.autoEnabled,s.slotMask) && jjy::scheduled(localMinute,s.slotMask);
}
inline const char *modeName(const SharedState &s,int64_t mono) {
  return manualActive(s,mono)?"MANUAL":s.autoEnabled?"AUTO":"STOPPED";
}
#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Select ESP32 Dev Module (original ESP32-WROOM)."
#endif
#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "Install esp32 by Espressif Systems 3.x."
#endif

SharedState shared;
portMUX_TYPE stateMux=portMUX_INITIALIZER_UNLOCKED;
jjy::Engine engine;
esp_timer_handle_t envelopeTimer=nullptr;
Adafruit_SH1106G oled(128,64,&Wire,-1);
WebServer server(80);
Preferences prefs;
bool oledOk=false, hardwareOk=false, prefsOk=false, saveOk=true;
uint32_t lastScreen=0,lastReconnect=0;
String csrfToken;
uint8_t ntpAttempt=0;
uint32_t ntpSyncSeen=0,ntpEvent=0;
int64_t ntpAttemptStarted=0,ntpNextAttempt=0;
bool ntpWiFiWasConnected=false;
String ntpMessage="NTP: waiting for WiFi.";
daily_shutdown::Policy shutdownPolicy;
bool shutdownFault=false;


const char *ntpSourceName(uint8_t source) {
  return source==1?"SIRIM":source==2?"Cloudflare":"--";
}
const char *ntpServerName(uint8_t source) {
  return source==1?NTP_SERVER_1:source==2?NTP_SERVER_2:"--";
}
void setNtpMessage(const String &message) {
  ntpMessage=message;
  ++ntpEvent;
  Serial.println(message);
}
void stopNtpClient() {
  esp_sntp_stop();
  // setservername uses a synchronous tcpip_api_call: wait until the queued
  // stop (and any preceding callback) has finished before another attempt.
  esp_sntp_setservername(0,nullptr);
  ntpAttempt=0;
}
void startNtpAttempt(uint8_t source) {
  stopNtpClient();
  ntpAttempt=source;
  ntpAttemptStarted=esp_timer_get_time();
  setNtpMessage(String(source==2?"SIRIM timeout; NTP connecting: ":
                                "NTP connecting: ")+
                ntpSourceName(source)+" ("+ntpServerName(source)+").");
  // One configured server per attempt makes the responding source unambiguous.
  // SIRIM is tried first; Cloudflare is the backup after 30 seconds.
  configTime(0,0,ntpServerName(source),nullptr,nullptr);
}

// Ikon kecil pada baris tajuk OLED 128x64.
void drawWiFiIcon(int x,int y) {
  oled.drawLine(x,y+2,x+3,y,SH110X_WHITE);
  oled.drawLine(x+3,y,x+7,y,SH110X_WHITE);
  oled.drawLine(x+7,y,x+10,y+2,SH110X_WHITE);
  oled.drawLine(x+2,y+4,x+5,y+2,SH110X_WHITE);
  oled.drawLine(x+5,y+2,x+8,y+4,SH110X_WHITE);
  oled.drawPixel(x+5,y+6,SH110X_WHITE);
}
void drawClockIcon(int x,int y) {
  oled.drawCircle(x+4,y+4,4,SH110X_WHITE);
  oled.drawLine(x+4,y+4,x+4,y+1,SH110X_WHITE);
  oled.drawLine(x+4,y+4,x+6,y+5,SH110X_WHITE);
}
void drawRadioIcon(int x,int y,bool waves) {
  // Puncak, tiang dan tapak antena
  oled.drawPixel(x+5,y+3,SH110X_WHITE);
  oled.drawLine(x+5,y+4,x+5,y+7,SH110X_WHITE);
  oled.drawLine(x+3,y+7,x+7,y+7,SH110X_WHITE);

  // Gelombang dekat
  oled.drawPixel(x+3,y+2,SH110X_WHITE);
  oled.drawPixel(x+2,y+3,SH110X_WHITE);
  oled.drawPixel(x+7,y+2,SH110X_WHITE);
  oled.drawPixel(x+8,y+3,SH110X_WHITE);

  // Gelombang luar
  oled.drawPixel(x+1,y+1,SH110X_WHITE);
  oled.drawPixel(x+0,y+2,SH110X_WHITE);
  oled.drawPixel(x+0,y+4,SH110X_WHITE);
  oled.drawPixel(x+9,y+1,SH110X_WHITE);
  oled.drawPixel(x+10,y+2,SH110X_WHITE);
  oled.drawPixel(x+10,y+4,SH110X_WHITE);

  // Penanda TX berkelip; bentuk antena tidak berubah
  if(waves)
    oled.drawLine(x+12,y+2,x+13,y+2,SH110X_WHITE);
}

SharedState snapshot();
bool trusted(const SharedState &s,int64_t mono);
String action(const String &name);
String statusJson();
void printStatus();

SharedState snapshot() {
  portENTER_CRITICAL(&stateMux);
  SharedState s=shared;
  portEXIT_CRITICAL(&stateMux);
  return s;
}
bool trusted(const SharedState &s,int64_t mono) {
  return s.synced && mono>=s.lastSyncMono &&
         mono-s.lastSyncMono<MAX_NTP_AGE_US;
}
void onNtp(struct timeval *tv) {
  if(!tv || tv->tv_sec<1704067200LL || tv->tv_sec>=4102444800LL) return;
  // This callback runs on the lwIP thread. Slot 0 is the only configured
  // server; read it here, rather than guessing from the server tried in loop().
  const char *serverName=esp_sntp_getservername(0);
  const uint8_t source=serverName && strcmp(serverName,NTP_SERVER_1)==0?1:
    serverName && strcmp(serverName,NTP_SERVER_2)==0?2:0;
  if(!source)return;
  const int64_t mono=esp_timer_get_time();
  portENTER_CRITICAL(&stateMux);
  shared.synced=true;
  shared.lastSyncMono=mono;
  shared.lastSyncUtc=tv->tv_sec;
  shared.ntpSource=source;
  ++shared.ntpSyncCount;
  ++shared.generation;
  portEXIT_CRITICAL(&stateMux);
}
void serviceNtp() {
  const bool connected=WiFi.status()==WL_CONNECTED;
  const int64_t mono=esp_timer_get_time();
  if(!connected) {
    if(ntpWiFiWasConnected) {
      stopNtpClient();
      setNtpMessage("NTP: waiting for WiFi.");
    }
    ntpWiFiWasConnected=false;
    return;
  }
  if(!ntpWiFiWasConnected) {
    ntpWiFiWasConnected=true;
    ntpNextAttempt=0;
  }
  const SharedState s=snapshot();
  if(s.ntpSyncCount!=ntpSyncSeen) {
    stopNtpClient();
    const SharedState latest=snapshot();
    ntpSyncSeen=latest.ntpSyncCount;
    ntpNextAttempt=mono+static_cast<int64_t>(NTP_INTERVAL_MS)*1000;
    setNtpMessage(String("NTP synced: ")+ntpSourceName(latest.ntpSource)+
      " ("+ntpServerName(latest.ntpSource)+").");
    return;
  }
  if(ntpAttempt && mono-ntpAttemptStarted>=NTP_ATTEMPT_US) {
    const uint8_t previous=ntpAttempt;
    stopNtpClient();
    // A reply can arrive while stopping. Process it before switching servers.
    if(snapshot().ntpSyncCount!=ntpSyncSeen)return;
    if(previous==1)startNtpAttempt(2);
    else {
      ntpNextAttempt=mono+NTP_RETRY_US;
      setNtpMessage("NTP: SIRIM + Cloudflare timeout; retry in 120s.");
    }
  } else if(!ntpAttempt && mono>=ntpNextAttempt) {
    startNtpAttempt(1);
  }
}
void onEnvelope(void *) {
  static bool gateWasOn=false;
  struct timeval tv;
  gettimeofday(&tv,nullptr);
  const int64_t mono=esp_timer_get_time();
  const int64_t utc=static_cast<int64_t>(tv.tv_sec)*1000000+tv.tv_usec;
  const int m=static_cast<int>((tv.tv_sec/60+480)%1440);
  const SharedState s=snapshot();
  jjy::Output out=engine.tick({utc,mono,trusted(s,mono),
    !s.outputFault && txRequested(s,mono,m),s.generation});
  bool fault=false;
  if(out.gate!=gateWasOn) {
    if(ledcWrite(TX_PIN,out.gate?1:0)) gateWasOn=out.gate;
    else {
      ledcWrite(TX_PIN,0);
      gateWasOn=false;
      fault=true;
      out.gate=false;
      out.active=false;
    }
  }
  portENTER_CRITICAL(&stateMux);
  if(fault) shared.outputFault=true;
  shared.appliedGeneration=s.generation;
  shared.tx=out;
  portEXIT_CRITICAL(&stateMux);
}
String myTime(int64_t utc,bool date) {
  if(utc<=0) return "--";
  auto t=jjy::civil(utc,8);
  char b[48];
  if(date)
    snprintf(b,sizeof(b),"%02d/%02d/%04d %02d:%02d:%02d %s",
      t.day,t.month,t.year,t.hour%12?t.hour%12:12,t.minute,t.second,
      t.hour<12?"AM":"PM");
  else
    snprintf(b,sizeof(b),"%02d:%02d:%02d %s",
      t.hour%12?t.hour%12:12,t.minute,t.second,t.hour<12?"AM":"PM");
  return String(b);
}
String nextSlot(const SharedState &s,int64_t utc) {
  if(!jjy::selectedSlots(s.slotMask)) return "No slots selected";
  if(!s.autoEnabled) return "Auto OFF";
  if(!s.synced) return "Waiting NTP";
  const int m=static_cast<int>((utc/60+480)%1440);
  if(jjy::scheduled(m,s.slotMask)) return "Window active";
  int d=jjy::minutesUntilNext(m,s.slotMask);
  if(d<0)return "No slots selected";
  return myTime((utc/60+d)*60,true);
}
String txState(const SharedState &s,int64_t mono,int64_t utc) {
  if(!hardwareOk || s.outputFault) return "OUTPUT ERROR";
  if(!trusted(s,mono)) return "WAIT NTP / STALE";
  if(s.inhibit) return "UPDATING SETTINGS";
  if(s.tx.active) return "TRANSMITTING";
  if(manualActive(s,mono) && mono<s.manualStart) return "WAIT NEXT MINUTE";
  const int m=static_cast<int>((utc/60+480)%1440);
  if(txRequested(s,mono,m)) {
    if(jjy::specialMinute(static_cast<int>((utc/60)%60)))
      return "SKIP :15 / :45";
    return "WAIT FRAME";
  }
  return s.autoEnabled?"WAIT SCHEDULE":"STOPPED";
}
String shutdownStatus() {
  if(shutdownFault)return "ERROR: shutdown failed; TX disabled";
  if(!shutdownPolicy.armed())return "04:10 MY; waiting first NTP";
  if(shutdownPolicy.pending())return "04:10 passed; waiting Manual TX to finish";
  return myTime(shutdownPolicy.deadlineUtc(),true);
}
void shutdownFailed(const char *reason) {
  shutdownFault=true;
  hardwareOk=false;
  portENTER_CRITICAL(&stateMux);
  shared.outputFault=true;
  portEXIT_CRITICAL(&stateMux);
  Serial.print("Shutdown error: ");
  Serial.println(reason);
}
void enterScheduledSleep() {
  // Freeze NTP generation changes before requesting the timer's mute handshake.
  stopNtpClient();
  // Mute through the same generation handshake used for Preferences writes.
  // Wait for the timer callback to observe inhibit BEFORE touching LEDC/GPIO.
  portENTER_CRITICAL(&stateMux);
  shared.inhibit=true;
  ++shared.generation;
  const uint32_t target=shared.generation;
  portEXIT_CRITICAL(&stateMux);
  bool muted=envelopeTimer==nullptr;
  const uint32_t began=millis();
  while(!muted && millis()-began<200) {
    const SharedState s=snapshot();
    muted=s.appliedGeneration==target && !s.tx.gate;
    if(!muted)delay(1);
  }
  if(!muted) {
    shutdownFailed("TX mute timeout");
    return;
  }
  if(envelopeTimer && esp_timer_stop(envelopeTimer)!=ESP_OK) {
    shutdownFailed("TX timer did not stop");
    return;
  }
  ledcWrite(TX_PIN,0);
  ledcDetach(TX_PIN);
  pinMode(TX_PIN,OUTPUT);
  digitalWrite(TX_PIN,LOW);
  // GPIO25 is RTC-capable on the original ESP32. Latch LOW throughout sleep
  // so the 2N2222 stays OFF while its 12V supply is still powered.
  const gpio_num_t tx=static_cast<gpio_num_t>(TX_PIN);
  if(rtc_gpio_init(tx)!=ESP_OK || rtc_gpio_set_level(tx,0)!=ESP_OK ||
     rtc_gpio_set_direction(tx,RTC_GPIO_MODE_OUTPUT_ONLY)!=ESP_OK ||
     rtc_gpio_hold_en(tx)!=ESP_OK) {
    shutdownFailed("could not hold GPIO25 LOW");
    return;
  }
  // No timer or GPIO wakeup: remain asleep until an external reset/power cycle.
  if(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL)!=ESP_OK) {
    shutdownFailed("could not disable wakeup sources");
    return;
  }
  if(oledOk) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0,0);
    oled.println("MB6 TimeSync");
    oled.println("04:10 shutdown");
    oled.println("TX OFF / Sleeping");
    oled.println("USB OFF/ON to wake");
    oled.display();
    delay(500);
    oled.oled_command(SH110X_DISPLAYOFF);
  }
  Serial.println("04:10 MY: deep sleep. USB OFF/ON or EN/reset to restart.");
  Serial.flush();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_deep_sleep_start();
}
void serviceScheduledSleep() {
  if(shutdownFault)return;
  const SharedState s=snapshot();
  const int64_t mono=esp_timer_get_time();
  if(shutdownPolicy.update(time(nullptr),mono,trusted(s,mono),manualActive(s,mono)))
    enterScheduledSleep();
}

void persistSettings(bool enabled,uint8_t slotMask) {
  portENTER_CRITICAL(&stateMux);
  shared.inhibit=true;
  ++shared.generation;
  const uint32_t target=shared.generation;
  portEXIT_CRITICAL(&stateMux);
  bool muted=!hardwareOk;
  const uint32_t began=millis();
  while(!muted && millis()-began<100) {
    SharedState s=snapshot();
    muted=s.appliedGeneration==target && !s.tx.gate;
    if(!muted) delay(1);
  }
  saveOk=muted && prefsOk && prefs.putUChar("schedule",jjy::packSettings(enabled,slotMask))==1;
  portENTER_CRITICAL(&stateMux);
  shared.inhibit=false;
  ++shared.generation;
  portEXIT_CRITICAL(&stateMux);
}
String setSchedule(uint8_t slotMask) {
  const SharedState s=snapshot();
  slotMask=jjy::selectedSlots(slotMask);
  const bool enabled=jjy::autoAllowed(s.autoEnabled,slotMask);
  if(s.slotMask!=slotMask || s.autoEnabled!=enabled || !saveOk) {
    portENTER_CRITICAL(&stateMux);
    shared.slotMask=slotMask;
    shared.autoEnabled=enabled;
    ++shared.generation;
    portEXIT_CRITICAL(&stateMux);
    persistSettings(enabled,slotMask);
  }
  if(!saveOk)return "Setting applied, but SAVE FAILED. Check status before restart.";
  return slotMask?"Selected times saved. Only checked slots can run when Auto is ON.":
                  "No times selected. Auto OFF saved; Auto checkbox disabled.";
}
String action(const String &name) {
  SharedState s=snapshot();
  const int64_t mono=esp_timer_get_time();
  if(name=="manual") {
    if(!hardwareOk || s.outputFault || !trusted(s,mono))
      return "ERROR: hardware or NTP not ready.";
    if(manualActive(s,mono))
      return "Manual already active; timer unchanged.";
    struct timeval tv;
    gettimeofday(&tv,nullptr);
    const int64_t utc=static_cast<int64_t>(tv.tv_sec)*1000000+tv.tv_usec;
    const int64_t start=mono+jjy::MINUTE-utc%jjy::MINUTE;
    portENTER_CRITICAL(&stateMux);
    shared.manualStart=start;
    shared.manualUntil=start+MANUAL_TX_MINUTES*jjy::MINUTE;
    ++shared.generation;
    portEXIT_CRITICAL(&stateMux);
    return "Manual: next minute, "+String(MANUAL_TX_MINUTES)+
           " minutes, then return to Auto/Stopped.";
  }
  if(name=="auto_on" || name=="auto_off" || name=="stop") {
    const bool enabled=name=="auto_on";
    if(enabled && !jjy::selectedSlots(s.slotMask))
      return "ERROR: select at least one sync time before enabling Auto.";
    portENTER_CRITICAL(&stateMux);
    shared.autoEnabled=enabled;
    if(name=="stop") shared.manualStart=shared.manualUntil=0;
    ++shared.generation;
    portEXIT_CRITICAL(&stateMux);
    if(s.autoEnabled!=enabled || !saveOk) persistSettings(enabled,s.slotMask);
    if(!saveOk)
      return "Setting applied, but SAVE FAILED. Check status before restart.";
    if(name=="stop") return "TX stopped; Auto OFF saved.";
    return enabled?"Auto ON saved. Manual, if active, continues.":
                   "Auto OFF saved. Manual, if active, continues.";
  }
  if(name=="ntp") {
    if(WiFi.status()!=WL_CONNECTED)
      return "ERROR: WiFi disconnected.";
    stopNtpClient();
    portENTER_CRITICAL(&stateMux);
    ntpSyncSeen=shared.ntpSyncCount;
    shared.synced=false;
    ++shared.generation;
    portEXIT_CRITICAL(&stateMux);
    startNtpAttempt(1);
    return ntpMessage+" TX waits for fresh time and next minute.";
  }
  return "ERROR: unknown command.";
}
String statusJson() {
  SharedState s=snapshot();
  const int64_t mono=esp_timer_get_time(),utc=time(nullptr);
  const bool manual=manualActive(s,mono);
  const int64_t wait=manual && mono<s.manualStart?
    (s.manualStart-mono+999999)/1000000:0;
  const int64_t remain=manual?
    (s.manualUntil-(mono<s.manualStart?s.manualStart:mono)+999999)/1000000:0;
  String j;
  j.reserve(1200);
  j="{\"time\":\""+(s.synced?myTime(utc,true):String("--"))+"\"";
  j+=",\"wifi\":"+String(WiFi.status()==WL_CONNECTED?"true":"false");
  j+=",\"ntp\":"+String(trusted(s,mono)?"true":"false");
  j+=",\"ntpSource\":\""+String(ntpSourceName(s.ntpSource))+"\"";
  j+=",\"ntpServer\":\""+String(ntpServerName(s.ntpSource))+"\"";
  j+=",\"ntpTarget\":\""+String(ntpSourceName(ntpAttempt))+"\"";
  j+=",\"ntpMsg\":\""+ntpMessage+"\"";
  j+=",\"ntpEvent\":"+String(ntpEvent);
  j+=",\"hardware\":"+String(hardwareOk && !s.outputFault?"true":"false");
  j+=",\"auto\":"+String(s.autoEnabled?"true":"false");
  j+=",\"slotMask\":"+String(s.slotMask);
  j+=",\"manual\":"+String(manual?"true":"false");
  j+=",\"saved\":"+String(saveOk?"true":"false");
  j+=",\"ip\":\""+WiFi.localIP().toString()+"\"";
  j+=",\"mode\":\""+String(modeName(s,mono))+"\"";
  j+=",\"tx\":\""+txState(s,mono,utc)+"\"";
  j+=",\"next\":\""+nextSlot(s,utc)+"\"";
  j+=",\"shutdown\":\""+shutdownStatus()+"\"";
  j+=",\"lastNtp\":\""+myTime(s.lastSyncUtc,true)+"\"";
  j+=",\"age\":"+String(static_cast<long>(s.lastSyncUtc?
      (mono-s.lastSyncMono)/1000000:-1));
  j+=",\"wait\":"+String(static_cast<long>(wait));
  j+=",\"remaining\":"+String(static_cast<long>(remain));
  j+=",\"duration\":"+String(MANUAL_TX_MINUTES*60);
  j+=",\"started\":"+String(s.tx.started);
  j+=",\"aborted\":"+String(s.tx.aborted)+"}";
  return j;
}
void printStatus() {
  Serial.println(statusJson());
}

const char WEB_PAGE[] PROGMEM=R"HTML(
<!doctype html><html lang="ms"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MB6 TimeSync</title><style>
*{box-sizing:border-box}body{margin:0;background:#101827;color:#edf3ff;
font:16px system-ui,sans-serif}main{max-width:700px;margin:auto;padding:24px}
section{background:#1c293d;padding:20px;border-radius:16px;margin:16px 0}
h1{margin-bottom:6px}h1.brand{display:flex;align-items:center;gap:10px}
.brand svg{width:22px;height:22px;fill:none;stroke:#58d4be;stroke-width:2;
stroke-linecap:round;stroke-linejoin:round;flex:none}
h2{font-size:18px}small,.muted{color:#acbed5}
#clock{font-size:25px;font-weight:700;margin:12px 0}
dl{display:grid;grid-template-columns:1fr 1.5fr;gap:12px}
dt{color:#acbed5}dd{margin:0;overflow-wrap:anywhere}
button{padding:13px;border:0;border-radius:9px;background:#58d4be;
color:#102029;font-weight:700;cursor:pointer}
button:disabled,input:disabled{opacity:.45;cursor:wait}
.buttons{display:flex;gap:10px;flex-wrap:wrap}.stop{background:#ffa59e}
label{display:flex;align-items:center;gap:12px;font-weight:bold}
.slots{display:grid;gap:14px;margin:16px 0}
input{width:25px;height:25px;accent-color:#58d4be}
#connection{color:#ffd479}#message{min-height:40px;overflow-wrap:anywhere}
</style><main><h1 class="brand">
<span>MB6 TimeSync</span>
<svg viewBox="0 0 24 24" aria-hidden="true">
<circle cx="12" cy="10" r="1.7"></circle>
<path d="M12 12v7"></path>
<path d="M9 21h6"></path>
<path d="M8.4 7.4a5 5 0 0 1 7.2 0"></path>
</svg></h1>
<small>JJY 60 kHz &middot; SIRIM / Cloudflare NTP &middot; Malaysia UTC+8 &middot; v0.4.5</small>
<p id="connection">Connecting...</p><section>
<div id="clock">--</div><dl>
<dt>Wi-Fi / NTP</dt><dd id="net">--</dd>
<dt>NTP connecting</dt><dd id="ntpTarget">--</dd>
<dt>Last NTP source</dt><dd id="ntpSource">--</dd>
<dt>Mode</dt><dd id="mode">--</dd><dt>TX</dt><dd id="tx">--</dd>
<dt>Next auto slot</dt><dd id="next">--</dd>
<dt>Auto sleep (04:10 MY)</dt><dd id="shutdown">--</dd>
<dt>Last NTP (MY)</dt><dd id="lastNtp">--</dd>
<dt>NTP age</dt><dd id="age">--</dd>
<dt>Manual</dt><dd id="manual">--</dd>
<dt>Frames / aborts</dt><dd id="frames">--</dd>
<dt>Saved setting</dt><dd id="saved">--</dd></dl></section>
<section><h2>Kawalan</h2>
<label><input id="auto" type="checkbox" disabled>Auto schedule</label>
<p class="muted">Auto ON ikut jadual, bukan pancar sepanjang masa.
Auto OFF tidak membatalkan ujian Manual yang sedang berjalan.</p>
<div class="buttons"><button id="start" disabled>Start Manual (10 min)</button>
<button id="stop" class="stop" disabled>Stop TX + Auto OFF</button>
<button id="ntp" disabled>Sync NTP</button></div>
<p id="message" role="status"></p></section>
<section><h2>Jadual Malaysia (24 jam)</h2>
<div class="slots">
<label><input id="slot0" type="checkbox" disabled>23:59–00:07 (hari berikutnya)</label>
<label><input id="slot1" type="checkbox" disabled>01:59–02:07</label>
<label><input id="slot2" type="checkbox" disabled>03:59–04:07</label>
</div>
<p class="muted" id="scheduleHint">Memuatkan pilihan waktu...</p>
<p class="muted">Pilihan waktu disimpan automatik. Hanya waktu bertanda berjalan
apabila Auto ON. Jika semua kosong, Auto OFF dan checkbox Auto dinyahaktifkan.
Tandakan semula waktu, kemudian aktifkan Auto jika diperlukan.</p>
<small>Manual kembali kepada pilihan Auto selepas tamat. Stop disimpan selepas
restart. Status TX menunjukkan output kod; tiada sensor antena atau pengesahan GET jam.
Prototaip OOK: minit :15/:45 dilangkau; leap second belum disokong.
Deep sleep pada 04:10 MY; Manual yang aktif ditunggu hingga tamat.
Tiada bangun automatik: OFF/ON USB atau tekan EN/reset untuk hidup semula.
MT3608 dan LED kuasa masih menerima bekalan USB.
Kawalan untuk rangkaian rumah sahaja.</small>
</section></main><script>
const token="__TOKEN__";
const el=id=>document.getElementById(id);
let busy=false,refreshing=false;
let controlRevision=0;
let lastNtpEvent=null;
const slotIds=["slot0","slot1","slot2"];
function selectedMask(){return slotIds.reduce((mask,id,i)=>mask|(el(id).checked?1<<i:0),0)}
function disableAll(){["auto","start","stop","ntp",...slotIds].forEach(id=>el(id).disabled=true)}
async function request(path,options={}){
 const controller=new AbortController();
 const timer=setTimeout(()=>controller.abort(),5000);
 try{
  const r=await fetch(path,{...options,cache:"no-store",signal:controller.signal});
  if(!r.ok)throw Error(await r.text());
  return r;
 }finally{clearTimeout(timer)}
}
async function refresh(){
 if(refreshing||busy)return;
 refreshing=true;
 const revision=controlRevision;
 try{
  const r=await request("/api/status");
  const s=await r.json();
  if(busy||revision!==controlRevision)return; // Ignore polls started before an edit.
  el("clock").textContent=s.time;
  el("connection").textContent="Connected · "+s.ip+" · refresh 3s";
  el("net").textContent=(s.wifi?"OK":"OFF")+" / "+(s.ntp?"OK":"WAIT / STALE");
  el("ntpTarget").textContent=s.ntpTarget==="--"?"Idle":s.ntpTarget;
  el("ntpSource").textContent=s.ntpSource==="--"?"--":
    s.ntpSource+" ("+s.ntpServer+")";
  if(s.ntpEvent!==lastNtpEvent){
    el("message").textContent=s.ntpMsg;
    lastNtpEvent=s.ntpEvent;
  }
  ["mode","tx","next","lastNtp","shutdown"].forEach(k=>el(k).textContent=s[k]);
  el("age").textContent=s.age<0?"--":s.age+" seconds";
  el("manual").textContent=!s.manual?"Inactive":
    s.wait>0?"Starts in "+s.wait+"s; duration "+s.duration+"s":
    s.remaining+"s remaining";
  el("frames").textContent=s.started+" / "+s.aborted;
  el("saved").textContent=s.saved?"OK":"SAVE ERROR";
  el("auto").checked=s.auto;
  slotIds.forEach((id,i)=>el(id).checked=(s.slotMask&(1<<i))!==0);
  el("scheduleHint").textContent=s.slotMask===0?
    "Tiada waktu dipilih · Auto OFF. Manual masih boleh digunakan.":
    "Waktu bertanda sahaja akan berjalan apabila Auto ON.";
  if(!busy){
   el("auto").disabled=s.slotMask===0;
   slotIds.forEach(id=>el(id).disabled=false);
   el("stop").disabled=false;
   el("start").disabled=!s.ntp||!s.hardware||s.manual;
   el("ntp").disabled=!s.wifi;
  }
 }catch(e){
  if(busy||revision!==controlRevision)return;
  el("connection").textContent="Offline / request failed. Check Wi-Fi or IP.";
  disableAll();
 }finally{
  refreshing=false;
 }
}
async function control(cmd,extra={}){
 if(busy)return;
 busy=true;
 ++controlRevision;
 disableAll();
 el("message").textContent="Sending...";
 try{
  const r=await request("/api/control",{
   method:"POST",
   headers:{
    "Content-Type":"application/x-www-form-urlencoded",
    "X-MB6-Token":token
   },
   body:new URLSearchParams({cmd,...extra}).toString()
  });
  el("message").textContent=await r.text();
 }catch(e){
  el("message").textContent="Not confirmed: "+e.message+
    ". Refresh page if restarted.";
 }finally{
  busy=false;
  await refresh();
 }
}
el("auto").onchange=()=>control(el("auto").checked?"auto_on":"auto_off");
slotIds.forEach(id=>el(id).onchange=()=>control("schedule",{mask:String(selectedMask())}));
el("start").onclick=()=>control("manual");
el("stop").onclick=()=>control("stop");
el("ntp").onclick=()=>control("ntp");
refresh();
setInterval(refresh,3000);
</script></html>
)HTML";

bool allowedHost() {
  String host=server.hostHeader();
  return host==WiFi.localIP().toString() ||
         host==WiFi.localIP().toString()+":80";
}
void webHeaders() {
  server.sendHeader("Cache-Control","no-store");
  server.sendHeader("X-Content-Type-Options","nosniff");
  server.sendHeader("X-Frame-Options","DENY");
}
void setupWeb() {
  char token[33];
  snprintf(token,sizeof(token),"%08lx%08lx%08lx%08lx",
    (unsigned long)esp_random(),(unsigned long)esp_random(),
    (unsigned long)esp_random(),(unsigned long)esp_random());
  csrfToken=token;
  const char *headers[]={"X-MB6-Token"};
  server.collectHeaders(headers,1);
  server.on("/",HTTP_GET,[](){
    webHeaders();
    if(!allowedHost()){
      server.send(403,"text/plain","Use the IP on OLED.");
      return;
    }
    String page=FPSTR(WEB_PAGE);
    page.replace("__TOKEN__",csrfToken);
    server.send(200,"text/html; charset=utf-8",page);
  });
  server.on("/api/status",HTTP_GET,[](){
    webHeaders();
    if(!allowedHost()){
      server.send(403,"text/plain","Invalid host");
      return;
    }
    server.send(200,"application/json",statusJson());
  });
  server.on("/api/control",HTTP_POST,[](){
    webHeaders();
    if(!allowedHost() || server.header("X-MB6-Token")!=csrfToken){
      server.send(403,"text/plain","Reload page; invalid token/host.");
      return;
    }
    String result;
    if(server.arg("cmd")=="schedule") {
      uint8_t mask=0;
      if(!server.hasArg("mask") || !jjy::parseSlotMask(server.arg("mask").c_str(),mask)) {
        server.send(400,"text/plain","ERROR: invalid slot mask; expected 0 to 7.");
        return;
      }
      result=setSchedule(mask);
    } else result=action(server.arg("cmd"));
    server.send(result.startsWith("ERROR")?409:200,"text/plain",result);
  });
  server.onNotFound([](){
    server.send(404,"text/plain","Not found");
  });
  server.begin();
}
void serialCommand(char c) {
  if(c=='t') Serial.println(action("manual"));
  else if(c=='x') Serial.println(action("stop"));
  else if(c=='a') Serial.println(action("auto_on"));
  else if(c=='o') Serial.println(action("auto_off"));
  else if(c=='n') Serial.println(action("ntp"));
  else if(c=='s') printStatus();
  else if(c=='?')
    Serial.println("t=manual x=stop a=auto ON o=auto OFF n=NTP s=status");
}
void drawScreen() {
  if(!oledOk)return;
  const SharedState s=snapshot();
  const int64_t mono=esp_timer_get_time(),utc=time(nullptr);

  oled.clearDisplay();
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);

  oled.setCursor(0,0);
  oled.print("MB6 TimeSync");
  if(WiFi.status()==WL_CONNECTED)drawWiFiIcon(79,0);
  if(trusted(s,mono))drawClockIcon(96,0);
  const bool txBlink=hardwareOk && !s.outputFault && s.tx.active &&
                     (millis()/1000U)%2U==0;
  drawRadioIcon(114,0,txBlink);

  oled.setCursor(0,8);
  oled.println(s.synced?myTime(utc,false):String("Waiting NTP"));
  oled.printf("WiFi:%s NTP:%s\n",
              WiFi.status()==WL_CONNECTED?"OK":"--",
              trusted(s,mono)?"OK":"--");
  oled.printf("Mode:%s\n",modeName(s,mono));
  oled.printf("TX:%s\n",
              (!hardwareOk || s.outputFault)?"ERROR":
              s.tx.active?"FRAME":"IDLE");
  oled.print("IP:");
  oled.println(WiFi.localIP());

  if(!saveOk) {
    oled.println("WARN: SAVE FAILED");
  } else if(manualActive(s,mono)) {
    if(mono<s.manualStart)
      oled.printf("Start in:%lds\n",
                  (long)((s.manualStart-mono+999999)/1000000));
    else
      oled.printf("Remain:%lds\n",
                  (long)((s.manualUntil-mono+999999)/1000000));
  } else if(s.autoEnabled && s.synced) {
    int m=(utc/60+480)%1440;
    if(jjy::scheduled(m,s.slotMask)) {
      oled.println("Auto window active");
    } else {
      const int next=jjy::minutesUntilNext(m,s.slotMask);
      if(next<0)oled.println("Auto: no slots");
      else {
        m=(m+next)%1440;
        oled.printf("Next:%02d:%02d MY\n",m/60,m%60);
      }
    }
  } else {
    oled.println(s.autoEnabled?"Auto: waiting NTP":"Auto: OFF");
  }
  oled.setCursor(0,56);
  if(ntpAttempt)oled.printf("NTP try:%s",ntpSourceName(ntpAttempt));
  else oled.printf("NTP src:%s",ntpSourceName(s.ntpSource));
  oled.display();
}
void setup() {
  // Release any RTC hold left by a previous scheduled sleep / external reset.
  const gpio_num_t tx=static_cast<gpio_num_t>(TX_PIN);
  rtc_gpio_hold_dis(tx);
  rtc_gpio_deinit(tx);
  pinMode(TX_PIN,OUTPUT);
  digitalWrite(TX_PIN,LOW);
  pinMode(26,INPUT);
  pinMode(27,INPUT);
  Serial.begin(115200);
  prefsOk=prefs.begin("mb6bypass",false);
  saveOk=prefsOk;
  // Keep the existing namespace to migrate v0.4.4's saved Auto preference.
  // Missing schedule key: select all three times and restore legacy Auto.
  const uint8_t legacy=jjy::packSettings(prefsOk?prefs.getBool("auto",false):false,jjy::ALL_SLOTS);
  const uint8_t settings=prefsOk?prefs.getUChar("schedule",legacy):legacy;
  shared.slotMask=jjy::unpackSlots(settings);
  shared.autoEnabled=jjy::unpackAuto(settings);
  hardwareOk=ledcSetClockSource(LEDC_USE_APB_CLK)
    && ledcAttachChannel(TX_PIN,CARRIER_HZ,1,0);
  if(hardwareOk) hardwareOk=ledcWrite(TX_PIN,0);
  Wire.begin(PIN_SDA,PIN_SCL);
  Wire.setClock(400000);
  for(uint8_t addr:{0x3C,0x3D}){
    Wire.beginTransmission(addr);
    if(Wire.endTransmission()==0){
      oledOk=oled.begin(addr,true);
      break;
    }
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  sntp_set_time_sync_notification_cb(onNtp);
  sntp_set_sync_interval(NTP_INTERVAL_MS);
  setupWeb();
  if(hardwareOk){
    esp_timer_create_args_t args={};
    args.callback=onEnvelope;
    args.dispatch_method=ESP_TIMER_TASK;
    args.name="jjy-envelope";
    args.skip_unhandled_events=true;
    hardwareOk=esp_timer_create(&args,&envelopeTimer)==ESP_OK;
    if(hardwareOk)
      hardwareOk=esp_timer_start_periodic(envelopeTimer,1000)==ESP_OK;
  }
  if(!hardwareOk){
    ledcWrite(TX_PIN,0);
    digitalWrite(TX_PIN,LOW);
  }
  Serial.println(
    "MB6 TimeSync v0.4.5: open http://IP-on-OLED on your home WiFi.");
  serialCommand('?');
}
void loop() {
  serviceNtp();
  while(Serial.available())
    serialCommand(static_cast<char>(Serial.read()));
  server.handleClient();
  serviceScheduledSleep();
  const uint32_t now=millis();
  if(WiFi.status()!=WL_CONNECTED && now-lastReconnect>=30000){
    lastReconnect=now;
    WiFi.reconnect();
  }
  if(now-lastScreen>=1000){
    lastScreen=now;
    drawScreen();
  }
  delay(2);
}
