#!/bin/bash
# M1 test: batched text runs stay aligned to the cell grid (no drift over 99 columns).
Xvfb :9 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1
L=$(printf 'x%.0s' $(seq 99))
./seance sh -c "printf '%s\033[41m|\033[0m\n' $L; sleep 20" &
sleep 3
xwd -root -silent | convert xwd:- /out/align.png
# last cell spans x 891..899 (cw=9), row 0 (y 0..17). Non-red, non-black pixels inside = the '|' glyph landed in its cell.
cnt=$(convert /out/align.png -crop 9x18+891+0 +repage -fill black +opaque '#aa0000' -format %c histogram:info:- | grep -vi "#000000" | grep -ci "[0-9a-f]\{6\}")
echo "distinct non-black colors inside last cell: $cnt"
echo "glyph pixels in last cell (white-ish): $(convert /out/align.png -crop 9x18+891+0 +repage -fill black +opaque white -format %c histogram:info:- | grep -c 'FFFFFF\|white')"
echo "glyph pixels leaking into col 98 right edge? (should be 'x' only): $(convert /out/align.png -crop 9x18+882+0 +repage -format %c histogram:info:- | grep -ci 'AA0000')"
