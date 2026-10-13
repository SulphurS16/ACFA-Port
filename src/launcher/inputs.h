// The player's own game files, checked before preparation: the BLUS30187 disc folder (disc
// VERSION 01.01, its EBOOT.BIN), the optional update package (APP_VER 01.35, regulation data),
// the PS3 system software file (2.41 from the disc's PS3_UPDATE\PS3UPDAT.PUP, or 4.93 from Sony's
// download) and the key file preparation needs. Known files are recognized by SHA-256; key values
// are compared by their SHA-256 only (the launcher contains no key material).
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
Status check_keys(const fs::path& keys, const std::string& pup_version);  // 4.93 needs two more keys

// The version a PS3UPDAT.PUP names (its version entry, e.g. "4.93"); "" when it is not a PUP.
std::string pup_version(const fs::path& pup);

// Sony's download of the supported current system software (4.93): the update list (plain HTTP:
// Sony's update servers offer no valid HTTPS) and the image it named when this port was tested;
// the file is accepted only with its pinned SHA-256.
struct SonySystemSoftware {
  const char* version;      // "4.93"
  const char* list_url;     // the US update list
  const char* list_version; // the list's SystemSoftwareVersion for it
  const char* image_url;    // the PS3UPDAT.PUP the list named
  unsigned long long size;
};
const SonySystemSoftware& sony_system_software();
bool is_supported_pup(const fs::path& pup);  // hash check (slow: reads the whole file)

// The system software and update package where they usually are, next to the disc folder.
fs::path find_pup(const fs::path& disc);
fs::path find_update(const fs::path& disc);

}  // namespace launcher
