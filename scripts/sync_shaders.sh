#!/bin/bash
# Pull shader captures from the device, translate the new (shader, answers) variants on the PC, push the SPIR-V back.
# usage: sync_shaders.sh
set -e
export MSYS_NO_PATHCONV=1
A=/s/Engage-Port/platform-tools/adb
D=/sdcard/Android/data/dev.teton.somniel/files
CAP=/s/Engage-Port/work/shaders_cap
SPV=/s/Engage-Port/work/spv
DLL=/s/Engage-Port/work/xlate_build/xlate/bin/Release/net10.0/totk_xlate.dll
mkdir -p "$CAP" "$SPV"
$A pull $D/shaders "$(cygpath -w $CAP)" | tail -1
before=$(ls "$SPV" | grep -c '\.spv$' || true)
python "$(cygpath -w /s/Engage-Port/somniel/tools/translate_captured.py)" "$(cygpath -w $CAP/shaders)" "$(cygpath -w $SPV)" "$(cygpath -w $DLL)"
after=$(ls "$SPV" | grep -c '\.spv$' || true)
echo "spv: $before -> $after"
$A push "$(cygpath -w $SPV)/." $D/spv/ | tail -1
