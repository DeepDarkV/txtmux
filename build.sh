#!/bin/sh
# Build txtmux with mingw-w64 (Debian: apt install mingw-w64).
# -lwinhttp is required (URL-entry rules follow 307/302 with WinHTTP).
# -std=gnu99 keeps newer GCC (>=15, which defaults to C23) happy with this code.
# Each target is skipped when its cross toolchain is not installed.
set -e

CC64=${CC64:-x86_64-w64-mingw32-gcc}
CC32=${CC32:-i686-w64-mingw32-gcc}
RC64=${RC64:-x86_64-w64-mingw32-windres}
RC32=${RC32:-i686-w64-mingw32-windres}

CFLAGS="-O2 -Wall -Wextra -municode -mwindows -static -static-libgcc -std=gnu99 -D_WIN32_WINNT=0x0501"
LIBS="-lws2_32 -ldnsapi -lcomctl32 -lwinhttp"

if command -v "$CC64" >/dev/null 2>&1; then
    if command -v "$RC64" >/dev/null 2>&1; then
        "$RC64" res.rc -O coff -o res64.o
        "$CC64" $CFLAGS -o txtmux64.exe txtmux.c res64.o $LIBS
        rm -f res64.o
    else
        "$CC64" $CFLAGS -o txtmux64.exe txtmux.c $LIBS
    fi
    echo "built: txtmux64.exe"
else
    echo "skip: $CC64 not found"
fi

if command -v "$CC32" >/dev/null 2>&1; then
    if command -v "$RC32" >/dev/null 2>&1; then
        "$RC32" res.rc -O coff -o res32.o
        "$CC32" $CFLAGS -o txtmux32.exe txtmux.c res32.o $LIBS
        rm -f res32.o
    else
        "$CC32" $CFLAGS -o txtmux32.exe txtmux.c $LIBS
    fi
    echo "built: txtmux32.exe"
else
    echo "skip: $CC32 not found"
fi
