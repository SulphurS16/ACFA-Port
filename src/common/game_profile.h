// Profile-specific source metadata shared by preparation frontends. These paths
// select position manifests; they do not relax executable or firmware SHA checks.
#pragma once

#ifndef ACFA_GAME_JP
#define ACFA_GAME_JP 1
#endif

namespace acfa::game_profile {

enum class Profile { JapanBLJM55005, UsaBLUS30187 };

constexpr const char* cmake_profile_name(Profile profile) {
  switch (profile) {
    case Profile::JapanBLJM55005: return "jp-bljm55005";
    case Profile::UsaBLUS30187: return "us-blus30187";
  }
  return nullptr;
}

constexpr const char* spu_manifest_file(Profile profile) {
  switch (profile) {
    case Profile::JapanBLJM55005: return "data/profiles/BLJM55005/spu_manifest.txt";
    case Profile::UsaBLUS30187: return "data/spu_manifest.txt";
  }
  return nullptr;  // An unknown profile has no accepted metadata path.
}

#if ACFA_GAME_JP
inline constexpr Profile kCurrent = Profile::JapanBLJM55005;
#else
inline constexpr Profile kCurrent = Profile::UsaBLUS30187;
#endif
inline constexpr const char* kSpuManifestFile = spu_manifest_file(kCurrent);
inline constexpr const char* kCMakeProfileName = cmake_profile_name(kCurrent);

}  // namespace acfa::game_profile
