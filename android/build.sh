#!/bin/sh
# 构建 txtmux 安卓版 APK（不使用 gradle）。
# 依赖：aapt2（Debian 包，arm64）、d8（build-tools 里的 java 脚本）、javac、python3、
#      apksigner.jar（build-tools/lib）、zipalign（可选，缺失时跳过对齐）
set -e
cd "$(dirname "$0")"
SDK=/opt/android-sdk
BT="$SDK/build-tools/36.0.0"
AJ="$SDK/platforms/android-34/android.jar"
AAPT2=${AAPT2:-/usr/bin/aapt2}
KS=${KS:-/workspace/out/ks.p12}
OUT=build

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/res"

echo "[1/5] javac"
javac -nowarn -encoding UTF-8 -source 8 -target 8 -bootclasspath "$AJ" \
      -d "$OUT/classes" $(find src -name '*.java')

echo "[2/5] d8"
"$BT/d8" --min-api 24 --lib "$AJ" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')

echo "[3/5] aapt2"
"$AAPT2" compile --dir res -o "$OUT/res.zip"
"$AAPT2" link -o "$OUT/base.apk" -I "$AJ" --manifest AndroidManifest.xml \
        -R "$OUT/res.zip" --auto-add-overlay

echo "[4/5] 打包 classes.dex"
python3 - "$OUT" <<'PY'
import sys, zipfile, os
out = sys.argv[1]
base = os.path.join(out, 'base.apk')
unsigned = os.path.join(out, 'unsigned.apk')
src = zipfile.ZipFile(base, 'r')
dst = zipfile.ZipFile(unsigned, 'w', zipfile.ZIP_DEFLATED)
for info in src.infolist():
    dst.writestr(info, src.read(info.filename))
dst.write(os.path.join(out, 'dex', 'classes.dex'), 'classes.dex')
src.close()
dst.close()
print('unsigned.apk', os.path.getsize(unsigned), 'bytes')
PY

echo "[5/5] 对齐 + 签名"
if command -v zipalign >/dev/null 2>&1; then
    zipalign -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
    SIGN_IN="$OUT/aligned.apk"
else
    echo "skip: 没有 zipalign，直接签名（无 .so，不影响安装）"
    SIGN_IN="$OUT/unsigned.apk"
fi
java -jar "$BT/lib/apksigner.jar" sign --ks "$KS" --ks-key-alias df \
     --ks-pass pass:eta123 --key-pass pass:eta123 --ks-type PKCS12 \
     --v1-signing-enabled true --v2-signing-enabled true \
     --out txtmux-android.apk "$SIGN_IN"
java -jar "$BT/lib/apksigner.jar" verify --print-certs txtmux-android.apk | head -4
ls -l txtmux-android.apk
