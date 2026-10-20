#!/bin/sh
# Rebuild only from the user's SHA-verified JP preparation output. No downloads.
set -eu
acfa_source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
  echo "usage: $0 PREPARED_DIR [GEN_DIR] [TOOLS_DIR]" >&2
  exit 2
fi
acfa_prepared=$1
acfa_gen_dir=${2:-"$acfa_source_dir/../local/gen"}
acfa_tools_dir=${3:-"$acfa_source_dir/../local/build-host"}
set -- "$acfa_tools_dir/acfa-recomp" emit "$acfa_prepared/eboot.elf" "$acfa_gen_dir/ppu"
for acfa_module in libsre libsail libpamf libdmux libdmuxpamf libadec libvpost libatrac3plus libatxdec libat3dec; do
  set -- "$@" --module "$acfa_prepared/lle/$acfa_module.elf"
done
"$@" --hooks "$acfa_source_dir/data/profiles/BLJM55005/hooks.txt"
"$acfa_tools_dir/acfa-prep" spu-records --manifest "$acfa_source_dir/data/profiles/BLJM55005/spu_manifest.txt" --out "$acfa_gen_dir/../jp-spu/records" "$acfa_prepared"
"$acfa_tools_dir/acfa-spurecomp" "$acfa_gen_dir/spu" "$acfa_gen_dir/../jp-spu/records"
