#!/usr/bin/env bash
# Wisp: the Noto fonts the pages fall back to for every script Liberation and Roboto lack (Arabic,
# Hebrew, the Indic scripts, Thai, CJK, emoji, ...). Fetched once into the build tree; package.sh puts
# them in /app0/wisp/fonts/noto, which fontconfig scans (data/fonts.conf). All SIL OFL 1.1.
#
#   bash ~/wisp/scripts/get-noto-fonts.sh
#
# Sources: github.com/notofonts/notofonts.github.io (fonts/<Family>/hinted/ttf, else unhinted),
# github.com/notofonts/noto-cjk (Sans/OTC: Japanese, Korean, Simplified and Traditional Chinese in one
# collection), github.com/googlefonts/noto-emoji (2D/fonts/NotoColorEmoji.ttf, PNG glyphs: FreeType is
# built with PNG).
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
OUT="$WB/fonts/noto"
mkdir -p "$OUT"
NOTO=https://raw.githubusercontent.com/notofonts/notofonts.github.io/main/fonts

# Regular and Bold: the scripts most pages use
MAJOR=(NotoSans NotoSansArabic NotoNaskhArabic NotoSansHebrew NotoSansDevanagari NotoSansBengali
    NotoSansGujarati NotoSansGurmukhi NotoSansKannada NotoSansMalayalam NotoSansOriya NotoSansTamil
    NotoSansTelugu NotoSansSinhala NotoSansThai NotoSansLao NotoSansKhmer NotoSansMyanmar NotoSansArmenian
    NotoSansGeorgian NotoSansEthiopic NotoSansThaana NotoSerifTibetan)
# Regular only: every other living or historic script, symbols, music
MINOR=(NotoNastaliqUrdu NotoKufiArabic NotoRashiHebrew NotoSansAdlam NotoSansAnatolianHieroglyphs
    NotoSansAvestan NotoSansBalinese NotoSansBamum NotoSansBassaVah NotoSansBatak NotoSansBhaiksuki
    NotoSansBrahmi NotoSansBuginese NotoSansBuhid NotoSansCanadianAboriginal NotoSansCarian
    NotoSansCaucasianAlbanian NotoSansChakma NotoSansCham NotoSansCherokee NotoSansChorasmian NotoSansCoptic
    NotoSansCuneiform NotoSansCypriot NotoSansCyproMinoan NotoSansDeseret NotoSansDuployan
    NotoSansEgyptianHieroglyphs NotoSansElbasan NotoSansElymaic NotoSansGlagolitic NotoSansGothic
    NotoSansGrantha NotoSansGunjalaGondi NotoSansHanifiRohingya NotoSansHanunoo NotoSansHatran
    NotoSansImperialAramaic NotoSansIndicSiyaqNumbers NotoSansInscriptionalPahlavi
    NotoSansInscriptionalParthian NotoSansJavanese NotoSansKaithi NotoSansKawi NotoSansKayahLi
    NotoSansKharoshthi NotoSansKhojki NotoSansKhudawadi NotoSansLepcha NotoSansLimbu NotoSansLinearA
    NotoSansLinearB NotoSansLisu NotoSansLycian NotoSansLydian NotoSansMahajani NotoSansMandaic
    NotoSansManichaean NotoSansMarchen NotoSansMasaramGondi NotoSansMath NotoSansMayanNumerals
    NotoSansMedefaidrin NotoSansMeeteiMayek NotoSansMendeKikakui NotoSansMeroitic NotoSansMiao NotoSansModi
    NotoSansMongolian NotoSansMro NotoSansMultani NotoSansNKo NotoSansNabataean NotoSansNagMundari
    NotoSansNandinagari NotoSansNewTaiLue NotoSansNewa NotoSansNushu NotoSansOgham NotoSansOlChiki
    NotoSansOldHungarian NotoSansOldItalic NotoSansOldNorthArabian NotoSansOldPermic NotoSansOldPersian
    NotoSansOldSogdian NotoSansOldSouthArabian NotoSansOldTurkic NotoSansOsage NotoSansOsmanya
    NotoSansPahawhHmong NotoSansPalmyrene NotoSansPauCinHau NotoSansPhagsPa NotoSansPhoenician
    NotoSansPsalterPahlavi NotoSansRejang NotoSansRunic NotoSansSamaritan NotoSansSaurashtra NotoSansSharada
    NotoSansShavian NotoSansSiddham NotoSansSignWriting NotoSansSogdian NotoSansSoraSompeng NotoSansSoyombo
    NotoSansSundanese NotoSansSunuwar NotoSansSylotiNagri NotoSansSymbols NotoSansSymbols2 NotoSansSyriac
    NotoSansTagalog NotoSansTagbanwa NotoSansTaiLe NotoSansTaiTham NotoSansTaiViet NotoSansTakri
    NotoSansTamilSupplement NotoSansTangsa NotoSansTifinagh NotoSansTirhuta NotoSansUgaritic NotoSansVai
    NotoSansVithkuqi NotoSansWancho NotoSansWarangCiti NotoSansYi NotoSansZanabazarSquare NotoSerifAhom
    NotoSerifDivesAkuru NotoSerifDogra NotoSerifKhitanSmallScript NotoSerifMakasar NotoSerifNPHmong
    NotoSerifOldUyghur NotoSerifOttomanSiyaq NotoSerifTangut NotoSerifTodhri NotoSerifToto NotoSerifYezidi
    NotoTraditionalNushu NotoMusic NotoZnamennyMusicalNotation)

# a font file: TrueType (00 01 00 00), OpenType CFF (OTTO) or a collection (ttcf); anything else
# (an error page) is removed
valid() {
    local magic
    magic="$(head -c4 "$1" | od -An -tx1 | tr -d ' \n')"
    [ "$magic" = 00010000 ] || [ "$magic" = 4f54544f ] || [ "$magic" = 74746366 ]
}

# fetch <family> <style>: hinted TrueType, else unhinted TrueType, else unhinted OpenType
fetch() {
    local family="$1" style="$2" kind ext
    for kind in hinted/ttf unhinted/ttf unhinted/otf; do
        ext="${kind##*/}"
        local file="$OUT/$family-$style.$ext"
        [ -s "$file" ] && valid "$file" && return 0
        if curl -sfL --retry 3 --max-time 120 -o "$file.part" "$NOTO/$family/$kind/$family-$style.$ext" \
                && valid "$file.part"; then
            mv "$file.part" "$file"
            return 0
        fi
        rm -f "$file.part"
    done
    echo "no $family-$style" >&2
    return 1
}
export -f fetch valid
export OUT NOTO

missing=0
{ for f in "${MAJOR[@]}"; do echo "$f Regular"; echo "$f Bold"; done
  for f in "${MINOR[@]}"; do echo "$f Regular"; done; } |
    xargs -P 8 -L 1 bash -c 'fetch "$0" "$1"' || missing=1

# CJK (one collection per weight) and color emoji
big() {
    local url="$1" file="$OUT/$2"
    [ -s "$file" ] && valid "$file" && return 0
    curl -sfL --retry 3 --max-time 900 -o "$file.part" "$url" && valid "$file.part" && mv "$file.part" "$file" \
        || { rm -f "$file.part"; echo "no $2" >&2; return 1; }
}
big https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTC/NotoSansCJK-Regular.ttc NotoSansCJK-Regular.ttc || missing=1
big https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTC/NotoSansCJK-Bold.ttc NotoSansCJK-Bold.ttc || missing=1
big https://raw.githubusercontent.com/googlefonts/noto-emoji/main/2D/fonts/NotoColorEmoji.ttf NotoColorEmoji.ttf || missing=1
curl -sfL --retry 3 -o "$OUT/OFL.txt" "$NOTO/LICENSE" || curl -sfL --retry 3 -o "$OUT/OFL.txt" \
    https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE || missing=1

echo "noto: $(ls "$OUT" | grep -c -e '\.ttf$' -e '\.otf$' -e '\.ttc$') fonts, $(du -sh "$OUT" | cut -f1) in $OUT"
exit $missing
