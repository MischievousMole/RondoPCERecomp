#!/bin/sh
# build_player.sh linux|windows  -> build/player/<target>/
set -e
cd "$(dirname "$0")"
python3 - <<'PY'
def cstr(p):
    return '"' + open(p).read().replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n"\n"') + '"'
open('shaders_gen.h', 'w').write('static const char SHADER_VERT[] = ' + cstr('../shaders/rondo_video.vert') + ';\nstatic const char SHADER_FRAG[] = ' + cstr('../shaders/rondo_video.frag') + ';\n')
PY
python3 ../tools/gen_font.py font_gen.h
if [ ! -f native/native_table.c ]; then
  [ -n "$RONDO_DISC" ] || { echo "Set RONDO_DISC=/path/to/your/game.cue to generate the recompiled code from your disc"; exit 1; }
  python3 ../tools/gen_from_disc.py "$RONDO_DISC" native ${RONDO_SYSCARD:+"$RONDO_SYSCARD"}
fi
SRC="player.c menu.c pce.c cpu6280.c bios_hle.c render.c audio.c video.c disc.c native/*.c"
T=${1:-linux}
VARIANT=${2:-}
EXTRA=""
if [ "$VARIANT" = santable ]; then
  python3 - <<'PY2'
import subprocess, sys
subprocess.check_call([sys.executable, '../extras/santable/gen_wordmark.py', 'santable_word.tmp'])
frag = open('../extras/santable/santable.frag').read().replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n"\n"')
open('santable_gen.h', 'w').write('static const char SANTABLE_FRAG[] = "' + frag + '";\n' + open('santable_word.tmp').read())
PY2
  EXTRA="-DSANTABLE"
fi
OUT=../build/player/$T${VARIANT:+-$VARIANT}; mkdir -p $OUT
if [ "$T" = windows ]; then
  SDL=${SDL_MINGW:-$HOME/rondo/third_party/SDL2-2.30.9/x86_64-w64-mingw32}
  x86_64-w64-mingw32-gcc -O2 -w $EXTRA -I$SDL/include/SDL2 -Dmain=SDL_main $SRC -o $OUT/RondoRecomp.exe \
    -L$SDL/lib -static -lmingw32 -lSDL2main -lSDL2 -mwindows -lsetupapi -limm32 -lversion -lwinmm -lole32 -loleaut32 \
    -lgdi32 -lcomdlg32 -luuid -lcfgmgr32 -lopengl32 -lm -lpthread
else
  gcc -O2 -w $EXTRA $(sdl2-config --cflags) $SRC -o $OUT/rondo $(sdl2-config --libs) -lGL -lm -lpthread
fi
ls -la $OUT
