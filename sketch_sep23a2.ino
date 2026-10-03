/* MB6 TimeSync v0.4.1 - original ESP32-WROOM, Arduino ESP32 3.x.
 * USB power only. Disconnect TB6612 and external 5V.
 * D25 -> R1 1k -> node A. Coil: node A -> GND.
 * R2 1k: node A -> GND (parallel with coil).
 * OLED: VCC 3V3, GND, SDA21, SCL22.
 * WiFi stays ON. Auto windows: 8 minutes. Manual: 10 minutes.
 */
#include <Arduino.h>
#include <initializer_list>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <esp_arduino_version.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <sys/time.h>

// Isi maklumat Wi-Fi sendiri sebelum upload.
constexpr char WIFI_SSID[]="Wifi-2.4Ghz";
constexpr char WIFI_PASSWORD[]="Wifi12345";

constexpr uint8_t TX_PIN=25;
constexpr uint8_t PIN_SDA=21, PIN_SCL=22;
constexpr uint32_t CARRIER_HZ=60000;
constexpr int AUTO_TX_MINUTES=8;
constexpr int MANUAL_TX_MINUTES=10;
constexpr int64_t MAX_NTP_AGE_US=2LL*3600*1000000;
constexpr uint32_t NTP_INTERVAL_MS=3600000;
constexpr char NTP_SERVER_1[]="ntp1.sirim.my";
constexpr char NTP_SERVER_2[]="ntp2.sirim.my";
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
bool scheduled(int malaysiaMinute);
int minutesUntilNext(int malaysiaMinute);

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
bool scheduled(int m) {
  for(int start : AUTO_START_MINUTES)
    if ((m-start+1440)%1440<AUTO_TX_MINUTES) return true;
  return false;
}
int minutesUntilNext(int m) {
  int best=1440;
  for(int start : AUTO_START_MINUTES) {
    int d=(start-m+1440)%1440;
    if(d<best) best=d;
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

struct SharedState {
  bool synced=false, autoEnabled=false, inhibit=false, outputFault=false;
  int64_t lastSyncMono=0, lastSyncUtc=0, manualStart=0, manualUntil=0;
  uint32_t generation=0, appliedGeneration=0;
  jjy::Output tx;
};

inline bool manualActive(const SharedState &s,int64_t mono) {
  return s.manualUntil>mono;
}
inline bool txRequested(const SharedState &s,int64_t mono,int localMinute) {
  if(s.inhibit) return false;
  if(manualActive(s,mono)) return mono>=s.manualStart;
  return s.autoEnabled && jjy::scheduled(localMinute);
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
  const int64_t mono=esp_timer_get_time();
  portENTER_CRITICAL(&stateMux);
  shared.synced=true;
  shared.lastSyncMono=mono;
  shared.lastSyncUtc=tv->tv_sec;
  ++shared.generation;
  portEXIT_CRITICAL(&stateMux);
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
  if(!s.autoEnabled) return "Auto OFF";
  if(!s.synced) return "Waiting NTP";
  const int m=static_cast<int>((utc/60+480)%1440);
  if(jjy::scheduled(m)) return "Window active";
  int d=jjy::minutesUntilNext(m);
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
void persistAuto(bool enabled) {
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
  saveOk=muted && prefsOk && prefs.putBool("auto",enabled)==1;
  portENTER_CRITICAL(&stateMux);
  shared.inhibit=false;
  ++shared.generation;
  portEXIT_CRITICAL(&stateMux);
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
    portENTER_CRITICAL(&stateMux);
    shared.autoEnabled=enabled;
    if(name=="stop") shared.manualStart=shared.manualUntil=0;
    ++shared.generation;
    portEXIT_CRITICAL(&stateMux);
    if(s.autoEnabled!=enabled || !saveOk) persistAuto(enabled);
    if(!saveOk)
      return "Setting applied, but SAVE FAILED. Check status before restart.";
    if(name=="stop") return "TX stopped; Auto OFF saved.";
    return enabled?"Auto ON saved. Manual, if active, continues.":
                   "Auto OFF saved. Manual, if active, continues.";
  }
  if(name=="ntp") {
    if(WiFi.status()!=WL_CONNECTED)
      return "ERROR: WiFi disconnected.";
    portENTER_CRITICAL(&stateMux);
    shared.synced=false;
    ++shared.generation;
    portEXIT_CRITICAL(&stateMux);
    sntp_restart();
    return "NTP requested; TX waits for fresh time and next minute.";
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
  j.reserve(900);
  j="{\"time\":\""+(s.synced?myTime(utc,true):String("--"))+"\"";
  j+=",\"wifi\":"+String(WiFi.status()==WL_CONNECTED?"true":"false");
  j+=",\"ntp\":"+String(trusted(s,mono)?"true":"false");
  j+=",\"hardware\":"+String(hardwareOk && !s.outputFault?"true":"false");
  j+=",\"auto\":"+String(s.autoEnabled?"true":"false");
  j+=",\"manual\":"+String(manual?"true":"false");
  j+=",\"saved\":"+String(saveOk?"true":"false");
  j+=",\"ip\":\""+WiFi.localIP().toString()+"\"";
  j+=",\"mode\":\""+String(modeName(s,mono))+"\"";
  j+=",\"tx\":\""+txState(s,mono,utc)+"\"";
  j+=",\"next\":\""+nextSlot(s,utc)+"\"";
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
<small>JJY 60 kHz &middot; SIRIM NTP &middot; Malaysia UTC+8 &middot; v0.4.1</small>
<p id="connection">Connecting...</p><section>
<div id="clock">--</div><dl>
<dt>Wi-Fi / NTP</dt><dd id="net">--</dd>
<dt>Mode</dt><dd id="mode">--</dd><dt>TX</dt><dd id="tx">--</dd>
<dt>Next auto slot</dt><dd id="next">--</dd>
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
<p>23:59–00:07 &middot; 01:59–02:07 &middot; 03:59–04:07</p>
<small>Manual kembali kepada pilihan Auto selepas tamat. Stop disimpan selepas
restart. Status TX menunjukkan output kod; tiada sensor antena atau pengesahan GET jam.
Prototaip OOK: minit :15/:45 dilangkau; leap second belum disokong.
Kawalan untuk rangkaian rumah sahaja.</small>
</section></main><script>
const token="__TOKEN__";
const el=id=>document.getElementById(id);
let busy=false,refreshing=false;
function disableAll(){["auto","start","stop","ntp"].forEach(id=>el(id).disabled=true)}
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
 try{
  const r=await request("/api/status");
  const s=await r.json();
  el("clock").textContent=s.time;
  el("connection").textContent="Connected · "+s.ip+" · refresh 3s";
  el("net").textContent=(s.wifi?"OK":"OFF")+" / "+(s.ntp?"OK":"WAIT / STALE");
  ["mode","tx","next","lastNtp"].forEach(k=>el(k).textContent=s[k]);
  el("age").textContent=s.age<0?"--":s.age+" seconds";
  el("manual").textContent=!s.manual?"Inactive":
    s.wait>0?"Starts in "+s.wait+"s; duration "+s.duration+"s":
    s.remaining+"s remaining";
  el("frames").textContent=s.started+" / "+s.aborted;
  el("saved").textContent=s.saved?"OK":"SAVE ERROR";
  el("auto").checked=s.auto;
  if(!busy){
   el("auto").disabled=false;
   el("stop").disabled=false;
   el("start").disabled=!s.ntp||!s.hardware||s.manual;
   el("ntp").disabled=!s.wifi;
  }
 }catch(e){
  el("connection").textContent="Offline / request failed. Check Wi-Fi or IP.";
  disableAll();
 }finally{
  refreshing=false;
 }
}
async function control(cmd){
 if(busy)return;
 busy=true;
 disableAll();
 el("message").textContent="Sending...";
 try{
  const r=await request("/api/control",{
   method:"POST",
   headers:{
    "Content-Type":"application/x-www-form-urlencoded",
    "X-MB6-Token":token
   },
   body:"cmd="+encodeURIComponent(cmd)
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
    String result=action(server.arg("cmd"));
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

  oled.setCursor(0,10);
  oled.println(s.synced?myTime(utc,false):String("Waiting NTP SIRIM"));
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
    if(jjy::scheduled(m)) {
      oled.println("Auto window active");
    } else {
      m=(m+jjy::minutesUntilNext(m))%1440;
      oled.printf("Next:%02d:%02d MY\n",m/60,m%60);
    }
  } else {
    oled.println(s.autoEnabled?"Auto: waiting NTP":"Auto: OFF");
  }

  oled.display();
}
void setup() {
  pinMode(TX_PIN,OUTPUT);
  digitalWrite(TX_PIN,LOW);
  pinMode(26,INPUT);
  pinMode(27,INPUT);
  Serial.begin(115200);
  prefsOk=prefs.begin("mb6bypass",false);
  saveOk=prefsOk;
  shared.autoEnabled=prefsOk?prefs.getBool("auto",false):false;
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
  configTime(0,0,NTP_SERVER_1,NTP_SERVER_2);
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
    "MB6 TimeSync v0.4.1: open http://IP-on-OLED on your home WiFi.");
  serialCommand('?');
}
void loop() {
  while(Serial.available())
    serialCommand(static_cast<char>(Serial.read()));
  server.handleClient();
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
