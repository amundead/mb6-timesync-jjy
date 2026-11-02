#!/usr/bin/env python3
"""Run actual firmware scheduling/settings code on the host (Python 3, g++, Node)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
firmware = (ROOT / 'sketch_sep23a2.ino').read_text()

def section(start, end):
    first = firmware.index(start)
    return firmware[first:firmware.index(end, first)]

logic = section('namespace jjy {', '#if !defined(CONFIG_IDF_TARGET_ESP32)')
sleep_hardware = section('void shutdownFailed(', 'bool persistPreference(')
settings = section('bool persistPreference(', 'String statusJson() {')
boot = section('  prefsOk=prefs.begin(', '  hardwareOk=ledcSetClockSource(')
shim = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <time.h>
#include <sys/time.h>
#include <algorithm>
constexpr int AUTO_TX_MINUTES=8,MANUAL_TX_MINUTES=10;
'''
hardware = r'''
struct String:std::string {
 using std::string::string;
 String(const std::string &s):std::string(s){}
 String(int n):std::string(std::to_string(n)){}
};
SharedState shared;
bool hardwareOk=true,prefsOk=true,saveOk=true,sleepSaveOk=true,ack=true;
bool shutdownFault=false,oledOk=false;
daily_shutdown::Policy shutdownPolicy;
void *envelopeTimer=reinterpret_cast<void *>(1);
std::string events;uint64_t timerDelay=0;bool timerArmed=false;int slept=0;
bool failWake=false,failRtc=false,failStop=false;
constexpr int TX_PIN=25,ESP_OK=0,ESP_SLEEP_WAKEUP_ALL=99,RTC_GPIO_MODE_OUTPUT_ONLY=1,WIFI_OFF=0,SH110X_DISPLAYOFF=0;
using gpio_num_t=int;
int esp_timer_stop(void*){events+="stop;";return failStop?1:0;}
void ledcWrite(int,int d){assert(d==0);events+="duty0;";}
void ledcDetach(int){events+="detach;";}
constexpr int OUTPUT=1,LOW=0;
void pinMode(int,int){}
void digitalWrite(int,int v){assert(v==LOW);events+="low;";}
int rtc_gpio_init(int){return failRtc?1:0;}
int rtc_gpio_set_level(int,int v){assert(v==0);return 0;}
int rtc_gpio_set_direction(int,int){return 0;}
int rtc_gpio_hold_en(int){events+="hold;";return 0;}
int esp_sleep_disable_wakeup_source(int){events+="clearWake;";timerArmed=false;return 0;}
int esp_sleep_enable_timer_wakeup(uint64_t us){events+="timer;";timerDelay=us;timerArmed=!failWake;return failWake?1:0;}
void esp_deep_sleep_start(){events+="sleep;";++slept;}
struct {template<class T>void println(T){} template<class T>void print(T){} void flush(){}}Serial;
struct {void clearDisplay(){} void setTextSize(int){} void setCursor(int,int){} template<class T>void println(T){} void display(){} void oled_command(int){}}oled;
uint32_t savedWakeToken=0;
int stateMux=0;
int64_t clockUs=1000000;
uint32_t ms=0,ntpSyncSeen=0;
String ntpMessage="NTP test";
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
SharedState snapshot(){return shared;}
bool trusted(const SharedState &s,int64_t){return s.synced;}
int64_t esp_timer_get_time(){return clockUs;}
uint32_t millis(){return ms;}
void delay(int n){ms+=n;if(ack){shared.appliedGeneration=shared.generation;shared.tx.gate=false;}}
struct Preferences {
 bool legacy=false,has=false,fail=false;uint8_t stored=0,sleepStored=0;int writes=0;
 bool begin(const char *name,bool){assert(std::string(name)=="mb6bypass");return true;}
 bool getBool(const char *key,bool fallback){assert(std::string(key)=="auto");return legacy?true:fallback;}
 uint8_t getUChar(const char *key,uint8_t fallback){if(std::string(key)=="deepSleep")return sleepStored;assert(std::string(key)=="schedule");return has?stored:fallback;}
 size_t putUChar(const char *key,uint8_t value){
  assert(std::string(key)=="schedule" || std::string(key)=="deepSleep");++writes;
  assert(shared.inhibit && shared.appliedGeneration==shared.generation && !shared.tx.gate);
  if(fail)return 0;
  if(std::string(key)=="deepSleep")sleepStored=value;else {has=true;stored=value;}return 1;
 }
}prefs;
constexpr int WL_CONNECTED=3;
struct {int status(){return WL_CONNECTED;} void disconnect(bool){events+="wifiOff;";} void mode(int){}}WiFi;
int mockGettimeofday(timeval *tv,void*){tv->tv_sec=clockUs/1000000;tv->tv_usec=clockUs%1000000;return 0;}
#define gettimeofday mockGettimeofday
void stopNtpClient(){}
void startNtpAttempt(int){}
'''
cases = r'''
int main(){
 // Fresh install and upgrade use the actual boot path.
 prefs={};shared={};loadSettings();
 assert(shared.slotMask==7 && !shared.autoEnabled);
 prefs.legacy=true;shared={};loadSettings();
 assert(shared.slotMask==7 && shared.autoEnabled);
 for(uint8_t mask=0;mask<8;++mask){
  for(int minute=0;minute<1440;++minute){
   const bool active=((mask&1)&&(minute==1439||minute<7)) ||
     ((mask&2)&&(minute>=119&&minute<127)) ||
     ((mask&4)&&(minute>=239&&minute<247));
   assert(jjy::scheduled(minute,mask)==active);
   // Independent reference: earliest enabled event on today or tomorrow.
   int next=-1;
   const int events[]={119,239,1439,1559,1679,2879};
   const int bits[]={2,4,1,2,4,1};
   for(int i=0;i<6;++i)if(events[i]>=minute && (mask&bits[i])){next=events[i]-minute;break;}
   assert(jjy::minutesUntilNext(minute,mask)==next);
   SharedState s;s.autoEnabled=true;s.slotMask=mask;
   assert(txRequested(s,clockUs,minute)==active);
   s.autoEnabled=false;assert(!txRequested(s,clockUs,minute));
   s.manualStart=clockUs-1;s.manualUntil=clockUs+1000;
   assert(txRequested(s,clockUs,minute)); // Manual works even with zero slots.
   s.inhibit=true;assert(!txRequested(s,clockUs,minute));
  }
  prefs={};shared={};shared.autoEnabled=true;shared.synced=true;
  setSchedule(mask);
  assert(shared.slotMask==mask && shared.autoEnabled==(mask!=0));
  action("auto_off");assert(!shared.autoEnabled);
  String result=action("auto_on");
  assert(shared.autoEnabled==(mask!=0));
  assert((result.find("ERROR:")==0)==(mask==0));
  shared={};loadSettings();
  assert(shared.slotMask==mask && shared.autoEnabled==(mask!=0));
  const int writes=prefs.writes;setSchedule(mask);assert(prefs.writes==writes);
  shared.manualStart=0;shared.manualUntil=clockUs+100000;
  setSchedule(0);assert(!shared.autoEnabled && shared.manualUntil>clockUs);
  assert(txRequested(shared,clockUs,239));
  setSchedule(4);assert(!shared.autoEnabled); // Reselecting a time does not turn Auto ON.
  action("auto_on");action("stop");
  assert(!shared.autoEnabled && shared.slotMask==4 && shared.manualUntil==0);
  shared={};loadSettings();assert(shared.slotMask==4 && !shared.autoEnabled);
 }
 for(int enabled=0;enabled<=1;++enabled)for(int mask=0;mask<256;++mask){
  auto p=jjy::packSettings(enabled,mask);
  assert(jjy::unpackSlots(p)==(mask&7));
  assert(jjy::unpackAuto(p)==(enabled && (mask&7)!=0));
 }
 for(int mask=0;mask<8;++mask){uint8_t got=99;std::string value=std::to_string(mask);
  assert(jjy::parseSlotMask(value.c_str(),got)&&got==mask);}
 for(const char *invalid:{"","8","-1","01","7junk","true"," 2","1\n","+1"}){
  uint8_t got=99;assert(!jjy::parseSlotMask(invalid,got)&&got==99);}
 uint8_t got=99;assert(!jjy::parseSlotMask(nullptr,got));
 prefs={};shared={};setSchedule(2);action("auto_on");
 prefs.fail=true;setSchedule(0);assert(!saveOk && !shared.autoEnabled);
 shared={};loadSettings();assert(shared.slotMask==2 && shared.autoEnabled);
 prefs.fail=false;setSchedule(0);assert(saveOk);
 // A timer that fails to acknowledge mute must prevent the NVS write.
 prefs={};shared={};ack=false;ms=0;setSchedule(1);
 assert(!saveOk && prefs.writes==0 && ms==100);ack=true;

 // Optional sleep settings use the actual firmware API and boot NVS path.
 prefs={};shared={};sleepSaveOk=true;savedWakeToken=0;loadSettings();
 assert(!shared.sleepEnabled && !shared.wakeEnabled);
 setSleepEnabled(true);assert(shared.sleepEnabled && prefs.sleepStored==1);
 const auto scheduleBefore=prefs.stored;
 action("wake_on");assert(shared.wakeEnabled && savedWakeToken==daily_shutdown::WAKE_TOKEN);
 shared={};loadSettings();assert(shared.sleepEnabled && prefs.stored==scheduleBefore);
 assert(daily_shutdown::restoreWake(true,true,savedWakeToken));
 assert(!daily_shutdown::restoreWake(false,false,savedWakeToken)); // Power-on/EN/SW reset.
 assert(!daily_shutdown::restoreWake(true,false,savedWakeToken));
 assert(!daily_shutdown::restoreWake(true,true,0));
 action("wake_off");assert(!shared.wakeEnabled && savedWakeToken==0);
 setSleepEnabled(false);shared={};loadSettings();assert(!shared.sleepEnabled);
 prefs.fail=true;setSleepEnabled(true);assert(shared.sleepEnabled && !sleepSaveOk);
 shared={};loadSettings();assert(!shared.sleepEnabled);prefs.fail=false;
 setSleepEnabled(true);assert(sleepSaveOk);
 ack=false;ms=0;const int oldWrites=prefs.writes;setSleepEnabled(false);
 assert(!sleepSaveOk && prefs.writes==oldWrites);ack=true;
 // Sleep at 04:10; manual defer; disabling cancels an already pending sleep.
 const int64_t day=20000*86400LL-8*3600LL,at=day+(4*60+10)*60;
 daily_shutdown::Policy p;
 assert(!p.update(at-1,0,true,false,false) && !p.armed());
 assert(!p.update(at-1,0,true,false,true) && p.armed());
 assert(!p.update(at,1000000,true,true,true) && p.pending());
 assert(!p.update(at,1000000,true,false,false) && !p.armed() && !p.pending());
 assert(!p.update(at,1000000,true,false,true)); // Re-enable at cutoff: next day.
 assert(p.deadlineUtc()==at+86400);
 daily_shutdown::Policy q;
 assert(!q.update(at-1,0,true,false,true));
 assert(q.update(at,1000000,false,false,true)); // NTP lost after arming.
 daily_shutdown::Policy manual;
 assert(!manual.update(at-1,0,true,false,true));
 assert(!manual.update(at,1000000,true,true,true));
 assert(manual.update(at+600,601000000,true,false,true));
 // Wake target: each minute of a MY day, UTC rollover and subsecond boundary.
 const int64_t wake=day+(23*60+40)*60;
 for(int minute=0;minute<1440;++minute) {
   const int64_t now=(day+minute*60)*1000000;
   const int64_t target=daily_shutdown::nextWakeUtcUs(now);
   assert(target==(minute<1420?wake:wake+86400)*1000000);
   assert(target>now && target-now<=86400LL*1000000);
 }
 assert(daily_shutdown::nextWakeUtcUs(wake*1000000-1)==wake*1000000);
 assert(daily_shutdown::nextWakeUtcUs(wake*1000000)==(wake+86400)*1000000);
 assert(daily_shutdown::nextWakeUtcUs(wake*1000000+1)==(wake+86400)*1000000);
 assert(daily_shutdown::nextWakeUtcUs(0)==0);

 // Execute the real sleep-entry function with GPIO/timer/Wi-Fi substitutes.
 auto prepare=[&](bool wakeEnabled){
   shared={};shared.wakeEnabled=wakeEnabled;ack=true;shutdownFault=false;
   failWake=failRtc=failStop=false;hardwareOk=true;events.clear();slept=0;
   timerArmed=true;timerDelay=0;ms=0;clockUs=at*1000000;savedWakeToken=0;
 };
 prepare(false);enterScheduledSleep();
 assert(slept==1 && !timerArmed && savedWakeToken==0 && shared.inhibit);
 assert(events.find("hold;")<events.find("sleep;"));
 assert(events.find("duty0;")<events.find("hold;"));
 prepare(true);enterScheduledSleep();
 assert(slept==1 && timerArmed && savedWakeToken==daily_shutdown::WAKE_TOKEN);
 assert(timerDelay==uint64_t((wake-at)*1000000));
 assert(events.find("clearWake;")<events.find("timer;"));
 assert(events.find("timer;")<events.find("sleep;"));
 assert(events.find("timer;")<events.find("wifiOff;"));
 assert(daily_shutdown::restoreWake(true,true,savedWakeToken));
 prepare(true);failWake=true;enterScheduledSleep();
 assert(slept==0 && shutdownFault && !hardwareOk && shared.outputFault);
 assert(events.find("wifiOff;")==std::string::npos);
 prepare(true);failRtc=true;enterScheduledSleep();
 assert(slept==0 && shutdownFault && events.find("timer;")==std::string::npos);
 prepare(true);failStop=true;enterScheduledSleep();
 assert(slept==0 && shutdownFault && events.find("hold;")==std::string::npos);
 prepare(true);ack=false;enterScheduledSleep();
 assert(slept==0 && shutdownFault && ms==200 && events.find("hold;")==std::string::npos);
 ack=true;
 puts("PASS: actual sleep entry mutes/holds LOW, clears wake sources, timer ON/OFF, retains token, and refuses sleep after timer/RTC/stop/mute failures.");
 puts("PASS: sleep opt-in/default OFF, NVS reboot/failure, RTC timer-only retention, power-reset OFF, manual defer, disable/re-enable, 1440 wake times and microsecond boundaries.");
 puts("PASS: 8 selections x 1440 minutes, next event, manual override, settings migration/reboot, empty selection, API validation, save failure and mute timeout.");
}
'''
with tempfile.TemporaryDirectory(prefix='mb6-schedule-') as folder:
    folder = Path(folder)
    cpp = folder / 'schedule.cpp'
    cpp.write_text(shim + logic + hardware + sleep_hardware + settings + '\nvoid loadSettings(){\n' + boot + '}\n' + cases)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(folder / 'schedule')], check=True)
    subprocess.run([str(folder / 'schedule')], check=True)
subprocess.run(['node', str(ROOT / 'tests' / 'check_web.cjs')], check=True)

