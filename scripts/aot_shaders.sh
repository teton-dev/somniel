#!/bin/bash
# Ahead-of-time shaders: translate every shader stage in the romfs on the PC (tools/aot_shaders.py) and push the set to
# the device. Run once per game version or translator change; sync_shaders.sh remains for the rare per-state variants.
# usage: aot_shaders.sh [--rescan]
set -e
export MSYS_NO_PATHCONV=1
A=/s/Engage-Port/platform-tools/adb
D=/sdcard/Android/data/dev.teton.somniel/files
DATA=/s/Engage-Port/work/engage_2.0.0/romfs/Data
CAP=/s/Engage-Port/work/shaders_cap/shaders
OUT=/s/Engage-Port/work/spv_aot
DLL=/s/Engage-Port/work/xlate_build/xlate/bin/Release/net10.0/totk_xlate.dll
python "$(cygpath -w /s/Engage-Port/somniel/tools/aot_shaders.py)" "$(cygpath -w $DATA)" "$(cygpath -w $OUT)" \
    "$(cygpath -w $DLL)" --exefs "$(cygpath -w /s/Engage-Port/work/engage_2.0.0/exefs)" --cap "$(cygpath -w $CAP)" "$@"
# the three small bookkeeping files (scan.json, stages.tsv, failures.txt) ride along; the renderer ignores them
$A push "$(cygpath -w $OUT)/." $D/spv/ | tail -1
