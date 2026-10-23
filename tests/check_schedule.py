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
settings = section('void persistSettings(', 'String statusJson() {')
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
bool hardwareOk=true,prefsOk=true,saveOk=true,ack=true;
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
 bool legacy=false,has=false,fail=false;uint8_t stored=0;int writes=0;
 bool begin(const char *name,bool){assert(std::string(name)=="mb6bypass");return true;}
 bool getBool(const char *key,bool fallback){assert(std::string(key)=="auto");return legacy?true:fallback;}
 uint8_t getUChar(const char *key,uint8_t fallback){assert(std::string(key)=="schedule");return has?stored:fallback;}
 size_t putUChar(const char *key,uint8_t value){
  assert(std::string(key)=="schedule");++writes;
  assert(shared.inhibit && shared.appliedGeneration==shared.generation && !shared.tx.gate);
  if(fail)return 0;
  has=true;stored=value;return 1;
 }
}prefs;
constexpr int WL_CONNECTED=3;
struct {int status(){return WL_CONNECTED;}}WiFi;
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
 puts("PASS: 8 selections x 1440 minutes, next event, manual override, settings migration/reboot, empty selection, API validation, save failure and mute timeout.");
}
'''
with tempfile.TemporaryDirectory(prefix='mb6-schedule-') as folder:
    folder = Path(folder)
    cpp = folder / 'schedule.cpp'
    cpp.write_text(shim + logic + hardware + settings + '\nvoid loadSettings(){\n' + boot + '}\n' + cases)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(folder / 'schedule')], check=True)
    subprocess.run([str(folder / 'schedule')], check=True)
subprocess.run(['node', str(ROOT / 'tests' / 'check_web.cjs')], check=True)
