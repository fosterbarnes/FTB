#requires -Version 7.0
# Regenerates romfs theme textures, romfs/textcolor.cfg, the home-menu icon and banner, and the launch
# splash from the tokens below and the logo master. Requires ImageMagick 7 (`magick`), Python 3, and
# devkitPro's tex3ds.
# Safe to rerun: icon recolors are idempotent.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\scriptHelper.ps1"
Set-Location -LiteralPath $repoRoot
$romfs = "$repoRoot\romfs"
$meta = "$repoRoot\meta"

$APP_BACKGROUND = '#1B1B1B'
$CONTROL_ALT = '#2A2A2A'
$PRESSED = '#383838'
$ACCENT = '#765E98'
# Hand-made 144px "ftb" logo (Consolas Bold on AccentStrong, #3A2C4F border); every logo size comes
# from it.
$LOGO_MASTER = "$repoRoot\.res\logo.png"
# The HOME Menu banner is the logo tile widened to bannertool's 256x128 banner image: same fill and
# border, with the text cut from a $BANNER_TILE px tile. Fill and border match the logo master. (The
# v0.0.16-18 full-screen 3D model showed far too big on hardware, cropped to an oval.)
$BANNER_TILE = 120
$BANNER_BORDER = 8
$LOGO_FILL = '#523E6E'
$LOGO_BORDER = '#3A2C4F'

$TEXT_COLORS = [ordered]@{
    text = '#FFFFFF'; nand = '#F44336'; sd = '#4CAF50'; gamecard = '#03A9F4'; dstitle = '#AC98C7'
    # Deeper HighlightRamp step: the palest yellow washes out to near-white on the 3DS LCD.
    file = '#FFFFFF'; directory = '#EED076'; enabled = '#4CAF50'; disabled = '#F44336'
    ticketinuse = '#4CAF50'; ticketnotinuse = '#F44336'
    # FTP log levels, after ftpd's red, green, and cyan.
    ftperror = '#F44336'; ftpcommand = '#4CAF50'; ftpresponse = '#26C6DA'
}

function invokeMagick { runNativeCommand magick $args 'magick' }

# Smaller tiles are downscaled from the master; the 144px one is the master unchanged.
function newLogoTile([int] $size, [string] $out) {
    invokeMagick $LOGO_MASTER -resize "${size}x$size" "PNG24:$out"
}

function newFill([string] $name, [string] $size, [string] $color) {
    invokeMagick -size $size "xc:$color" "PNG32:$romfs\$name.png"
}

foreach ($screen in @{ top = 400; bottom = 320 }.GetEnumerator()) {
    $prefix = "$($screen.Key)_screen"
    $width = $screen.Value
    newFill "${prefix}_bg" "${width}x240" $APP_BACKGROUND
    foreach ($bar in 'top_bar', 'bottom_bar') {
        newFill "${prefix}_$bar" "${width}x20" $CONTROL_ALT
        newFill "${prefix}_${bar}_shadow" "${width}x16" 'none'
    }
}
newFill 'meta_info_box' '320x64' $CONTROL_ALT
newFill 'meta_info_box_shadow' '352x96' 'none'
newFill 'button' '300x60' $CONTROL_ALT
# Progress track and fill: info.c stretches these solid tiles to each bar's size.
newFill 'progress_bar_bg' '8x8' $PRESSED
newFill 'progress_bar_content' '8x8' $ACCENT
# Opaque so the selection row never shows through; same shade the old 25% white had on APP_BACKGROUND.
newFill 'scroll_bar' '10x200' '#545454'
# Neutral row fill with a purple bar: row text can be yellow, and yellow never sits on purple.
invokeMagick -size 320x20 "xc:$PRESSED" -fill $ACCENT -draw 'rectangle 0,0 2,19' `
    "PNG32:$romfs\selection_overlay.png"

# Touch buttons (UI_BUTTON_SIZE in ui.h): the border keeps a button visible over the selection row,
# which has the same fill.
foreach ($button in @{ touch_button = @($PRESSED, '#545454'); touch_button_held = @($ACCENT, $ACCENT) }.GetEnumerator()) {
    invokeMagick -size 22x22 xc:none -fill $button.Value[0] -stroke $button.Value[1] -strokewidth 1 `
        -draw 'roundrectangle 0.5,0.5 21.5,21.5 3,3' "PNG32:$romfs\$($button.Key).png"
}

# 16x16 white pixel-art button icons. Each rect is x,y,width,height in pixels; edges sit between pixel
# centers, since a 1 px rect drawn center to center has no area and draws nothing.
function newIcon([string] $name, [int[][]] $rects) {
    $draws = foreach ($r in $rects) { '-draw'; "rectangle $($r[0] - 0.5),$($r[1] - 0.5) $($r[0] + $r[2] - 0.5),$($r[1] + $r[3] - 0.5)" }
    invokeMagick -size 16x16 xc:none +antialias -fill '#FFFFFF' @draws "PNG32:$romfs\$name.png"
}

function outlineRects([int] $x, [int] $y, [int] $w, [int] $h) {
    @(@($x, $y, $w, 1), @($x, ($y + $h - 1), $w, 1), @($x, $y, 1, $h), @(($x + $w - 1), $y, 1, $h))
}

# Magnifier: 1 px ring of radius 5 around (6,6), handle down-right; zoom in adds the vertical bar.
$magnifier = @(@(4, 1, 5, 1), @(4, 11, 5, 1), @(3, 2, 1, 1), @(9, 2, 1, 1), @(2, 3, 1, 1), @(10, 3, 1, 1),
    @(1, 4, 1, 5), @(11, 4, 1, 5), @(2, 9, 1, 1), @(10, 9, 1, 1), @(3, 10, 1, 1), @(9, 10, 1, 1),
    @(10, 10, 2, 1), @(10, 11, 3, 1), @(11, 12, 3, 1), @(12, 13, 3, 1), @(13, 14, 3, 1), @(14, 15, 2, 1),
    @(4, 6, 5, 1))
# Layout: the two 3DS screens, titles drawn on the one that holds the list.
$screens = (outlineRects 0 0 16 9) + (outlineRects 3 10 10 6)

newIcon 'icon_sort' @(@(1, 2, 8, 2), @(1, 7, 6, 2), @(1, 12, 3, 2), @(12, 1, 2, 10), @(10, 10, 6, 1),
    @(11, 11, 4, 1), @(12, 12, 2, 2))
newIcon 'icon_zoom_in' ($magnifier + , @(6, 4, 1, 5))
newIcon 'icon_zoom_out' $magnifier
newIcon 'icon_layout_grid' ($screens + @(@(2, 2, 2, 2), @(5, 2, 2, 2), @(9, 2, 2, 2), @(12, 2, 2, 2),
    @(2, 5, 2, 2), @(5, 5, 2, 2), @(9, 5, 2, 2), @(12, 5, 2, 2)))
newIcon 'icon_layout_top' ($screens + @(@(2, 2, 10, 1), @(2, 4, 12, 1), @(2, 6, 8, 1)))
newIcon 'icon_layout_bottom' ($screens + @(@(5, 12, 6, 1), @(5, 14, 4, 1)))

# Wifi bars ship black and vanish on dark bars; recolor near-black to white.
foreach ($name in 'wifi0', 'wifi1', 'wifi2', 'wifi3', 'wifi_disconnected') {
    $path = "$romfs\$name.png"
    invokeMagick $path -fuzz 10% -fill '#FFFFFF' -opaque '#000000' "PNG32:$path"
}

# Battery empty cells ship pure black; use the app background instead.
foreach ($name in 'battery0', 'battery1', 'battery2', 'battery3', 'battery4', 'battery5', 'battery_charging') {
    $path = "$romfs\$name.png"
    invokeMagick $path -fill $APP_BACKGROUND -opaque '#000000' "PNG32:$path"
}

$temp = (New-Item -ItemType Directory -Path "$env:TEMP\ftbTheme$(Get-Random)").FullName
try {
    newLogoTile 48 "$meta\icon_3ds.png"
    newLogoTile 144 "$romfs\logo.png"
    # Inner square past the tile's border (the master's 10 px, scaled, plus one blended pixel) holds
    # only fill and text, so it centers seamlessly.
    $inner = $BANNER_TILE - 2 * ([math]::Ceiling(10 * $BANNER_TILE / 144) + 1)
    newLogoTile $BANNER_TILE "$temp\bannerTile.png"
    invokeMagick "$temp\bannerTile.png" -gravity center -crop "${inner}x$inner+0+0" +repage `
        "PNG24:$temp\bannerText.png"
    invokeMagick -size 256x128 "xc:$LOGO_BORDER" -fill $LOGO_FILL `
        -draw "rectangle $BANNER_BORDER,$BANNER_BORDER $(255 - $BANNER_BORDER),$(127 - $BANNER_BORDER)" `
        "$temp\bannerText.png" -gravity center -composite "PNG24:$meta\banner_3ds.png"

    # Launch splash textures keep the original sizes: a 128px logo and a 16x256 strip (240 rows shown)
    # stretched across both screens, here drawn with the app's 20px top and bottom bars.
    newLogoTile 96 "$temp\splashTile.png"
    invokeMagick -size 128x128 "xc:$APP_BACKGROUND" "$temp\splashTile.png" -gravity center -composite `
        "PNG24:$temp\splashLogo.png"
    invokeMagick -size 16x256 "xc:$CONTROL_ALT" -fill $APP_BACKGROUND -draw 'rectangle 0,20 15,219' `
        "PNG24:$temp\splashStrip.png"
    foreach ($name in 'splashLogo', 'splashStrip') {
        runNativeCommand $tex3ds @('-f', 'etc1', '-q', 'high', '-z', 'none', '--raw', '-o', "$temp\$name.bin", "$temp\$name.png") 'tex3ds'
    }
    runNativeCommand python @('-I', "$PSScriptRoot\genSplash.py", "$meta\logo_3ds.bcma.lz",
        "3dsLogo_01.bclim=$temp\splashLogo.bin", "3dsLogo_00.bclim=$temp\splashStrip.bin") 'genSplash.py'
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force
    if (Test-Path -LiteralPath $temp) { throw "Could not remove '$temp'." }
}

# textcolor.cfg wants AABBGGRR.
$lines = foreach ($entry in $TEXT_COLORS.GetEnumerator()) {
    $hex = $entry.Value.TrimStart('#')
    '{0}=FF{1}{2}{3}' -f $entry.Key, $hex.Substring(4, 2), $hex.Substring(2, 2), $hex.Substring(0, 2)
}
[IO.File]::WriteAllText("$romfs\textcolor.cfg", ($lines -join "`n") + "`n")
closeOut 0
