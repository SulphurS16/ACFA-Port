#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "runtime/gamepads.h"

namespace rt::pads {
namespace {
std::mutex mutex;
std::once_flag initialized;
dispatch_queue_t input_queue;
struct Slot {
  GCController* controller=nil;
  XINPUT_GAMEPAD state{};
  std::uint32_t packet=0;
};
std::array<Slot,kSlots> slots;
std::chrono::steady_clock::time_point next_discovery{};
unsigned logged_events=0,logged_callbacks=0;
bool diagnostics(){return std::getenv("ACFA_PAD_DIAGNOSTICS")!=nullptr;}
SHORT axis(float f){return SHORT(std::lround(std::clamp(f,-1.0f,1.0f)*32767.0f));}
std::uint8_t trigger(GCControllerButtonInput* b){return std::uint8_t(std::lround(std::clamp(b.value,0.0f,1.0f)*255));}
XINPUT_GAMEPAD sample(GCExtendedGamepad* p){
  XINPUT_GAMEPAD state{};
  auto button=[&](GCControllerButtonInput* b,WORD mask){if(b.isPressed)state.wButtons|=mask;};
  button(p.buttonA,XINPUT_GAMEPAD_A);button(p.buttonB,XINPUT_GAMEPAD_B);
  button(p.buttonX,XINPUT_GAMEPAD_X);button(p.buttonY,XINPUT_GAMEPAD_Y);
  button(p.leftShoulder,XINPUT_GAMEPAD_LEFT_SHOULDER);button(p.rightShoulder,XINPUT_GAMEPAD_RIGHT_SHOULDER);
  button(p.leftThumbstickButton,XINPUT_GAMEPAD_LEFT_THUMB);button(p.rightThumbstickButton,XINPUT_GAMEPAD_RIGHT_THUMB);
  button(p.buttonMenu,XINPUT_GAMEPAD_START);button(p.buttonOptions,XINPUT_GAMEPAD_BACK);button(p.buttonHome,XINPUT_GAMEPAD_GUIDE);
  button(p.dpad.up,XINPUT_GAMEPAD_DPAD_UP);button(p.dpad.down,XINPUT_GAMEPAD_DPAD_DOWN);
  button(p.dpad.left,XINPUT_GAMEPAD_DPAD_LEFT);button(p.dpad.right,XINPUT_GAMEPAD_DPAD_RIGHT);
  state.bLeftTrigger=trigger(p.leftTrigger);state.bRightTrigger=trigger(p.rightTrigger);
  state.sThumbLX=axis(p.leftThumbstick.xAxis.value);state.sThumbLY=axis(p.leftThumbstick.yAxis.value);
  state.sThumbRX=axis(p.rightThumbstick.xAxis.value);state.sThumbRY=axis(p.rightThumbstick.yAxis.value);
  return state;
}
void receive(int index,GCController* controller,GCExtendedGamepad* profile,bool callback){
  // Framework callbacks and the initial snapshot run on input_queue. Guest/UI
  // threads consume only this lock-protected copy, never a live GC profile.
  const XINPUT_GAMEPAD value=sample(profile);
  std::lock_guard guard(mutex);
  Slot& slot=slots[size_t(index)];
  if(!controller||slot.controller!=controller)return; // stale hot-unplug callback
  if(callback&&diagnostics()&&logged_callbacks<8){
    ++logged_callbacks;
    std::fprintf(stderr,"[gamepad] native handler callback received (slot %d, event %u)\n",index,logged_callbacks);
  }
  if(std::memcmp(&value,&slot.state,sizeof value)){
    slot.state=value;++slot.packet;
    if(diagnostics()&&logged_events<8){
      ++logged_events;
      std::fprintf(stderr,"[gamepad] native input event received (slot %d, sample %u)\n",index,logged_events);
    }
  }
}
void discover(){
  const auto now=std::chrono::steady_clock::now();
  if(now<next_discovery)return;
  next_discovery=now+std::chrono::milliseconds(250);
  NSArray<GCController*>* connected=[GCController controllers];
  for(size_t i=0;i<slots.size();++i){
    Slot& slot=slots[i];
    if(slot.controller&&![connected containsObject:slot.controller]){
      slot.controller.extendedGamepad.valueChangedHandler=nil;
      slot.controller=nil;slot.state={};++slot.packet;
      if(diagnostics())std::fprintf(stderr,"[gamepad] native controller disconnected from slot %zu\n",i);
    }
  }
  for(GCController* c in connected){
    GCExtendedGamepad* profile=c.extendedGamepad;
    const bool supported=[profile isKindOfClass:[GCXboxGamepad class]]||
      [profile isKindOfClass:[GCDualShockGamepad class]]||[profile isKindOfClass:[GCDualSenseGamepad class]];
    if(!supported)continue;
    bool found=false;int free_slot=-1;
    for(int i=0;i<kSlots;++i){if(slots[size_t(i)].controller==c)found=true;else if(!slots[size_t(i)].controller&&free_slot<0)free_slot=i;}
    if(found||free_slot<0)continue;
    Slot& slot=slots[size_t(free_slot)];slot.controller=c;slot.state={};++slot.packet;
    // The default main handler queue can be delayed by host event pumping.
    // Never make controller delivery depend on AppKit/window focus or GPU work.
    c.handlerQueue=input_queue;
    if(@available(macOS 11.0,*)){
      // Receive Menu/Options immediately while keeping system gestures enabled.
      // This preference belongs to our GC objects; no OS binding is rewritten.
      profile.buttonMenu.preferredSystemGestureState=GCSystemGestureStateAlwaysReceive;
      profile.buttonOptions.preferredSystemGestureState=GCSystemGestureStateAlwaysReceive;
    }
    __weak GCController* weak_controller=c;
    const int index=free_slot;
    profile.valueChangedHandler=^(GCExtendedGamepad* p,GCControllerElement*){
      @autoreleasepool {receive(index,weak_controller,p,true);}
    };
    dispatch_async(input_queue,^{@autoreleasepool {
      GCController* live=weak_controller;
      if(live)receive(index,live,live.extendedGamepad,false);
    }});
    if(diagnostics())std::fprintf(stderr,"[gamepad] native controller connected in slot %d (serial event queue, handler installed)\n",index);
  }
}
}
bool get(int index,XINPUT_STATE* out){
  if(!out||index<0||index>=kSlots)return false;
  *out={};
  @autoreleasepool {
    std::call_once(initialized,[]{
      input_queue=dispatch_queue_create("acfa.controller.events",DISPATCH_QUEUE_SERIAL);
      if(@available(macOS 11.3,*)){
        GCController.shouldMonitorBackgroundEvents=YES;
        if(diagnostics())std::fprintf(stderr,"[gamepad] background events enabled: %s\n",GCController.shouldMonitorBackgroundEvents?"yes":"NO");
      }
    });
    std::lock_guard guard(mutex);
    discover();
    const Slot& slot=slots[size_t(index)];
    if(!slot.controller)return false;
    out->Gamepad=slot.state;out->dwPacketNumber=slot.packet;
    return true;
  }
}
// Haptics are deliberately not advertised as implemented yet.
void vibrate(int,WORD,WORD){}
}
