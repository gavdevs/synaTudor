#!/bin/bash -e
# PATCHED for HP EliteBook 840 G8 (Synaptics FS7605, 06cb:00f0):
# Instead of downloading the Lenovo 00be driver, use the locally-staged HP "110"
# driver DLLs (hpdrv/), copied into the 104 output names so no symbol churn is needed.

HASH_FILE="$1"
TMP_DIR="$2"
OUT_DIR="$3"
# remaining args ($4..) are the requested output DLL names (synaFpAdapter104.dll, synaWudfBioUsb104.dll)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HP="$SCRIPT_DIR/hpdrv"

mkdir -p "$OUT_DIR"
cp "$HP/synaFpAdapter110.dll"  "$OUT_DIR/synaFpAdapter104.dll"
cp "$HP/synaWudfBioUsb110.dll" "$OUT_DIR/synaWudfBioUsb104.dll"
