// The player's game files in the keyless setup (docs/INPUTS.md): the disc folder (BLUS30187, disc
// VERSION 01.01, read in place), files the player decrypted with RPCS3 (Utilities > Decrypt PS3
// Binaries: EBOOT.elf next to EBOOT.BIN, the 10 system modules of system software 4.93 as .prx next
// to their .sprx in RPCS3's dev_flash\sys\external) and plain files from RPCS3's folders
// (flashATRAC.pic; optionally the update 01.35 as RPCS3 installs it). Each file is accepted only
// with its SHA-256 (common/known_inputs.h); the launcher contains no keys and decrypts nothing.
#pragma once
#include <filesystem>
#include <string>

namespace launcher {

namespace fs = std::filesystem;

enum class Verdict { Empty, Ok, Optional, Bad };
struct Status {
  Verdict verdict = Verdict::Empty;
  std::string text;     // one line for the window
  std::string details;  // every problem, for the log
};

// Where the files are, found from the disc folder and the RPCS3 folder (or any folder holding the
// decrypted files). Empty paths: not found.
struct Found {
  fs::path eboot_elf;    // EBOOT.elf
  fs::path modules_dir;  // the folder holding the .prx files and flashATRAC.pic
  fs::path game_data;    // the installed update (dev_hdd0/game/BLUS30187), when present
  std::string rpcs3_system_software;  // e.g. "4.89" (dev_flash/vsh/etc/version.txt), when known
};
Found find_files(const fs::path& disc, const fs::path& rpcs3);

Status check_disc(const fs::path& disc, const Found& f);    // the disc and its decrypted EBOOT.elf
Status check_modules(const fs::path& rpcs3, const Found& f);  // the 10 modules and flashATRAC.pic
Status check_update(const fs::path& rpcs3, const Found& f);  // optional

// The steps to decrypt the files with RPCS3, naming the player's own folders where known.
std::wstring decrypt_help(const fs::path& disc, const fs::path& rpcs3, const Found& f);

// A likely RPCS3 folder (rpcs3.exe in a few usual places), or empty.
fs::path find_rpcs3();

}  // namespace launcher
