// The player's own game files, checked before preparation: the BLUS30187 disc folder (disc
// VERSION 01.01, its EBOOT.BIN), the optional update package (APP_VER 01.35, regulation data),
// the PS3 system software file (2.41, normally the disc's PS3_UPDATE\PS3UPDAT.PUP) and the key
// file preparation needs. Known files are recognized by SHA-256; key values are compared by their
// SHA-256 only (the launcher contains no key material).
#pragma once
#include <filesystem>
#include <string>

namespace launcher {

namespace fs = std::filesystem;

enum class Verdict { Empty, Ok, Optional, Bad };
struct Status {
  Verdict verdict = Verdict::Empty;
  std::string text;
};

Status check_disc(const fs::path& disc);
Status check_update(const fs::path& pkg);  // empty: optional
Status check_pup(const fs::path& pup);
Status check_keys(const fs::path& keys);

// The system software and update package where they usually are, next to the disc folder.
fs::path find_pup(const fs::path& disc);
fs::path find_update(const fs::path& disc);

}  // namespace launcher
