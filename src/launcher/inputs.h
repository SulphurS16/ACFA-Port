// The player's game files in the keyless setup (docs/INPUTS.md): the game's disc folder
// (BLUS30187, disc VERSION 01.01, read in place) and files the player decrypted themselves (for
// example with RPCS3's Utilities > Decrypt PS3 Binaries): EBOOT.elf (from PS3_GAME\USRDIR\
// EBOOT.BIN), the 10 system modules of PS3 system software 4.93 as .prx with flashATRAC.pic from
// the same folder; optionally the update 01.35: its package (the player's RPCS3 installs it) or
// its regulation.bin. Each file is accepted only with its SHA-256 (common/known_inputs.h); the
// port contains no keys and decrypts nothing.
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

// Where the files are: EBOOT.elf next to EBOOT.BIN in the game folder or in the decrypted files'
// folder; the modules and flashATRAC.pic in that folder or a folder below it; regulation.bin from
// `update` (that file itself, or in the folder the update package was installed to) or in the
// decrypted files' folder. Empty paths: not found.
struct Found {
  fs::path eboot_elf;    // EBOOT.elf
  fs::path modules_dir;  // the folder holding the .prx files and flashATRAC.pic
  fs::path regulation;   // the update's regulation.bin, when present
  bool encrypted_modules = false;  // only the encrypted .sprx files were found
};
Found find_files(const fs::path& disc, const fs::path& files, const fs::path& update = {});

Status check_disc(const fs::path& disc, const Found& f);    // the disc and its decrypted EBOOT.elf
Status check_modules(const fs::path& files, const Found& f);  // the 10 modules and flashATRAC.pic

// The update row (optional): the file the player chose, the update package (.pkg) or regulation.bin
// taken from it; none: a regulation.bin in the decrypted files' folder still counts.
bool is_package(const fs::path& p);
struct UpdateCheck {
  Status status;
  bool package = false;  // the chosen file is the update package 01.35
  bool install = false;  // that package is not installed yet (Prepare files or Install update: RPCS3 installs it)
};
UpdateCheck check_update(const fs::path& chosen, const Found& f);

// The files the port needs and where they come from (shown when some are missing).
std::wstring files_help();

}  // namespace launcher
