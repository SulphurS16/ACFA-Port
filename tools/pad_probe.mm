#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include "runtime/gamepads.h"
int main(int argc,char** argv){
  double seconds=2;
  if(argc==3&&std::string(argv[1])=="--events")seconds=std::clamp(std::atof(argv[2]),0.5,30.0);
  else if(argc!=1){std::cerr<<"usage: acfa-pad-probe [--events seconds]\n";return 2;}
  @autoreleasepool {
    XINPUT_STATE initial{};rt::pads::get(0,&initial);
    std::array<bool,rt::pads::kSlots> seen{};
    std::array<std::uint32_t,rt::pads::kSlots> previous{};
    unsigned events=0;
    const auto end=std::chrono::steady_clock::now()+std::chrono::duration<double>(seconds);
    while(std::chrono::steady_clock::now()<end){
      [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
      for(int slot=0;slot<rt::pads::kSlots;++slot){
        XINPUT_STATE state{};const bool present=rt::pads::get(slot,&state);
        if(present){
          if(!seen[size_t(slot)])std::cout<<"controller slot "<<slot<<" available\n"<<std::flush;
          else if(state.dwPacketNumber!=previous[size_t(slot)])++events;
          previous[size_t(slot)]=state.dwPacketNumber;
        }
        seen[size_t(slot)]=present;
      }
    }
    int count=0;for(bool present:seen)if(present)++count;
    std::cout<<"native GameController devices: "<<count<<"; observed input changes: "<<events;
    if(@available(macOS 11.3,*))std::cout<<"; background events: "<<(GCController.shouldMonitorBackgroundEvents?"yes":"NO");
    std::cout<<"; no keyboard/mouse gameplay input\n";
  }
}
