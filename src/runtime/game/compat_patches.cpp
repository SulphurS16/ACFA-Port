// Compatibility patches. Races in the game's own code: windows that the console's scheduling (two
// hardware threads, priority-driven) rarely opens, but that this port's fully concurrent,
// host-scheduled threads can hit, more so when the PC is busy; each hook adds only the missing
// synchronization and then runs the original guest function. And the guest's longjmp, which
// recompiled code cannot return through on its own. The hooked addresses are listed in hooks.txt
// (applied by acfa-recomp when generating code).
#include <atomic>

#include "ppu/ppu_recomp.h"
#include "runtime/log.h"

void f_00ede7e8(ppu::Ctx&);  // MultiStream: unregister a stream (takes the stream list lock)

namespace ppu {
namespace {

// Calls a guest function with r3 = arg from inside a hook, keeping the hooked function's
// argument registers and link register for the original.
void call_guest_r3(Ctx& ctx, void (*fn)(Ctx&), u64 arg) {
  u64 saved[8];
  for (int i = 0; i < 8; ++i) saved[i] = ctx.r[3 + i];
  u64 lr = ctx.lr;
  ctx.r[3] = arg;
  fn(ctx);
  for (int i = 0; i < 8; ++i) ctx.r[3 + i] = saved[i];
  ctx.lr = lr;
}

// A stream that reached its end is marked inactive (byte +828 = 0) but stays in the manager's
// list until the streaming thread's next tick drops it (5 ms later on the console). Its destructor
// unregisters only active streams, so a stream released in between stayed listed and the next
// tick used the freed object (crash in 0xef84d0). Unregistering an inactive stream as well is a
// no-op once the tick has dropped it.
void unregister_inactive_stream(Ctx& ctx) {
  u32 s = u32(ctx.r[3]);
  if (s && r8(s + 828) == 0) call_guest_r3(ctx, f_00ede7e8, s);
}

}  // namespace

void hook_ms_stream_dtor(Ctx& ctx, void (*orig)(Ctx&)) {
  unregister_inactive_stream(ctx);
  orig(ctx);
}

void hook_ms_stream_dtor_delete(Ctx& ctx, void (*orig)(Ctx&)) {
  unregister_inactive_stream(ctx);
  orig(ctx);
}

// Sound bank activation (0xefbec0, on MOMainThread) builds the bank's cue table (0xefe168) and
// only then marks the bank active (state 2 at +8). Name lookups (0xf21548, on the game's threads)
// skip only removed banks (state 3), so a lookup during activation walked the half-built table and
// followed uninitialized links: the crash in 0xf387a0 at boot and at mission starts, more frequent
// when the host is busy and MOMainThread runs late. Find on a bank that is not active reports
// "not found" (0), as it would have before the bank was loaded (logged: the first few, then at
// powers of two).
void hook_snd_bank_find(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 bank = u32(ctx.r[3]);
  if (u32 state = bank ? r32(bank + 8) : 2; state != 2) {
    static std::atomic<u32> count{0};
    u32 n = ++count;
    if (n <= 3 || (n & (n - 1)) == 0)
      LOG_I("snd", "sound bank 0x%08x looked up in state %u (not active): reported not found (%u so far)", bank, state, n);
    ctx.r[3] = 0;
    return;
  }
  orig(ctx);
}

// Bank removal (0xef9ff0) destroys the cue table and sets the removed state only at its end: the
// bank is marked removed first, so lookups starting meanwhile skip it.
// The libc longjmp (0x10f5894): it restores r1, r2, r13-r31, the link register, CR, the FPRs and
// the vector registers from the jmp_buf and sets r3 to the value (1 for 0), then returns through
// the restored link register into the function that called setjmp. Generated code returns into
// its own caller instead, with that other frame's stack pointer: the game's Lua (5.0.2) raises
// script errors this way (luaD_throw 0xeb7ed8 -> luaD_rawrunprotected 0xeb7e38), and the stack
// it then ran on was corrupted (a player's crash in ORDER MATCH against Collared 11, whose script
// calls a misspelled function once his Primal Armor is below 40 %; 2026-10-24). After the
// original restores the registers, the throw lands in the setjmp caller, which acfa-recomp emits
// resumable (hooks.txt "setjmp").
void hook_longjmp(Ctx& ctx, void (*orig)(Ctx&)) {
  orig(ctx);
  static std::atomic<int> logged{0};
  int n = logged.fetch_add(1);
  if (n < 20)
    LOG_I("ppu", "longjmp to 0x%08x (stack 0x%08x, value %llu)", u32(ctx.lr), u32(ctx.r[1]), (unsigned long long)ctx.r[3]);
  throw GuestLongjmp{u32(ctx.r[1]), u32(ctx.lr)};
}

void hook_ms_bank_remove(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32 bank = u32(ctx.r[3])) w32(bank + 8, 3);
  orig(ctx);
}

}  // namespace ppu
