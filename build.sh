#!/bin/sh
# 需要 mingw-w64（Debian: apt install mingw-w64）
set -e
OUT=${1:-.}
i686-w64-mingw32-windres res.rc -O coff -o res32.o
x86_64-w64-mingw32-windres res.rc -O coff -o res64.o
i686-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static -static-libgcc \
    -D_WIN32_WINNT=0x0501 -o "$OUT/txtmux32.exe" txtmux.c res32.o -lws2_32 -ldnsapi -lcomctl32
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static -static-libgcc \
    -D_WIN32_WINNT=0x0501 -o "$OUT/txtmux64.exe" txtmux.c res64.o -lws2_32 -ldnsapi -lcomctl32
rm -f res32.o res64.o
echo "built: $OUT/txtmux32.exe $OUT/txtmux64.exe"
