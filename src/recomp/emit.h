#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "recomp/analysis.h"

namespace recomp {

struct EmitOptions {
  bool comments = false;           // disassembly comment per instruction
  size_t insns_per_file = 24000;   // translation-unit size target
  // Guest functions routed through runtime hooks (address -> hook name): the guest code is
  // emitted as f_<addr>_guest and f_<addr> calls ppu::hook_<name>(ctx, f_<addr>_guest), so direct
  // and indirect calls both reach the hook, which decides when to run the original.
  std::map<u32, std::string> hooks;
  // The guest's setjmp (hooks file lines named "setjmp"). A function that calls it (directly or
  // through stubs that only branch to it) is emitted resumable: the runtime's longjmp throws
  // ppu::GuestLongjmp after restoring the registers from the jmp_buf, and the function whose
  // setjmp saved them (same stack pointer, return address one of its setjmp calls) catches it
  // and continues after that call, as setjmp returning a second time does.
  std::set<u32> setjmp;
};

// Writes ppu_gen_*.cpp, ppu_functab.cpp and gen_info.txt into out_dir. Files whose contents are
// unchanged are not rewritten (keeps incremental builds cheap).
struct Unit {
  const Image* img;
  const Analysis* an;
  std::string prefix;  // file prefix, e.g. "ppu" for the executable, "libsre" for a module
};
void emit_all(const std::vector<Unit>& units, const std::string& out_dir, const EmitOptions& opt);

}  // namespace recomp
