#include "common/known_inputs.h"

namespace acfa::inputs {

#if ACFA_GAME_JP
const Known kEbootBin = {"EBOOT.BIN", "556da01a845f97299bc89ef5e7a13e20998db36fedf345d3456a887c9a62ccf1", 20283320};
const Known kEbootElf = {"EBOOT.elf", "ee03439ea2cbda0de31b32848bc8b686c60dc0761b54db25be7281afbe20d5d2", 20280888};
#else
const Known kEbootBin = {"EBOOT.BIN", "96fb034cb52748c76d243d665b1aa4979ba384b7c5deba38b02438ff7de52ce1", 20282920};
const Known kEbootElf = {"EBOOT.elf", "084c9c3fbe6b68e0e3427a056385f50474cdc7d28cabb167bb2c5d1e91c7aa15", 20280488};
#endif

const char* const kModules[kModuleCount] = {"libsre",  "libsail", "libpamf",       "libdmux",   "libdmuxpamf",
                                            "libadec", "libvpost", "libatrac3plus", "libatxdec", "libat3dec"};

namespace {

// RPCS3's decryptions of the 4.93 modules (verified 2026-10-15 with RPCS3 0.0.42-19698: identical to
// this project's own decryption; relocated they give the modules data/spu_manifest.txt lists).
const Known kModules493[kModuleCount] = {
    {"libsre.prx", "9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6", 239344},
    {"libsail.prx", "ac33b80980d45113d887c868ff02619dc145e8a80bc102749a31fbd750431bbb", 203400},
    {"libpamf.prx", "363d9681d7f24201b1dfb9cc8714e7182a72a25a57734db2a7e1c3dccad09646", 21336},
    {"libdmux.prx", "2a53cad71a50e2042a7f78ff61675e99deebbdb0be57ab6fdeeaf85b224ca918", 13000},
    {"libdmuxpamf.prx", "792e1fd566717b15a6f5a270cbcb3092ce84abc7f562632fcc1bc580b5960579", 54152},
    {"libadec.prx", "88a8f75b97f59930c88695e4436383300f2112badfa44ea4e6dccad6eda60f61", 65552},
    {"libvpost.prx", "19953feeb9af6858b3cbfaca76bbda1d49ee78216d214ee9cdce7191b361308a", 347552},
    {"libatrac3plus.prx", "85b16a71db983ba298c881d4f439e0930969a1455d07fbebbe59ea4a3df04151", 20792},
    {"libatxdec.prx", "cad06f3a72111aca8f4d6a9c7397310a40a9ef98c703406a75935aaa422f1da1", 183600},
    {"libat3dec.prx", "67f51c3a556fef540ca5da15b1f7560d949640a7607fe4629c3c767264b6ad1b", 52200},
};

// Other versions' decryptions that differ from 4.93 (the other modules are identical there).
struct Other {
  const char* module;
  const char* sha256;
  const char* version;
};
const Other kOtherVersions[] = {
    {"libsre", "74a023767aae35838f26ef1a846806caad438a041a06ba16b1165050aa403e8e", "4.89"},
    {"libsail", "837e2c5a2d53b344b897aa99573ba4c776417d5359970e5a6d23fda9d2fa3441", "4.89"},
    {"libadec", "c3682f6109b1ecf49871b4550464164524dfe39ccea142299d7564565313bee2", "4.89"},
};

}  // namespace

const Known* module(const std::string& name) {
  for (int i = 0; i < kModuleCount; ++i)
    if (name == kModules[i]) return &kModules493[i];
  return nullptr;
}

std::string module_other_version(const std::string& name, const std::string& sha256) {
  for (const Other& o : kOtherVersions)
    if (name == o.module && sha256 == o.sha256) return o.version;
  return "";
}

const Known kFlashAtrac = {"flashATRAC.pic", "1edf54e1323dd736b71958ca56307d536dedc7c05ccb0dd3b2a3be1af5b50e30", 35131};
#if ACFA_GAME_JP
// Exact installed regulation, not a claim that all Japanese regulation revisions match.
const Known kRegulation = {"regulation.bin", "b99c9198d6862e683bad62e1fafc82a83bc7ff799dd20540b206899b6cca1118", 242580};
#else
const Known kRegulation = {"regulation.bin", "c7e993ad592c681963426b7ac7f2c5f4de28aa8b8a1a466f2b1518954232f70f", 245656};
#endif

bool valid_hook(u32 address, const std::string& name) {
#if ACFA_GAME_JP
  return (address == 0x00ef97c8 && name == "ms_stream_dtor") ||
         (address == 0x00ef95f8 && name == "ms_stream_dtor_delete") ||
         (address == 0x00efad28 && name == "snd_bank_find") ||
         (address == 0x00efa740 && name == "ms_bank_remove") ||
         (address == 0x003fdfa0 && name == "texture_compress") ||
         (address == 0x00d60f30 && name == "compression_texture_lock");
#else
  // Exact US executable SHA is checked before applying the upstream hook map.
  return true;
#endif
}
}  // namespace acfa::inputs
