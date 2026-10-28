#requires -Version 7.0
# Regenerates romfs theme textures, romfs/textcolor.cfg, the home-menu icon and banner, and the launch
# splash from the tokens below. Requires ImageMagick 7 (`magick`), Python 3, and devkitPro's tex3ds.
# Safe to rerun: icon recolors are idempotent.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\scriptHelper.ps1"
Set-Location -LiteralPath $repoRoot
$romfs = "$repoRoot\romfs"
$meta = "$repoRoot\meta"

$APP_BACKGROUND = '#1B1B1B'
$CONTROL_ALT = '#2A2A2A'
$PRESSED = '#383838'
$ACCENT_STRONG = '#523E6E'
$ACCENT = '#765E98'
$LOGO_TEXT = '#D0D0D0'
# Forward slashes: ImageMagick treats backslashes in font paths as escapes.
$FONT = 'C:/Windows/Fonts/consolab.ttf'
# Chosen so the ink box (28x12) leaves even margins on the 48px icon, which keeps it exactly centered.
$LOGO_POINTSIZE = 18.5

$TEXT_COLORS = [ordered]@{
    text = '#FFFFFF'; nand = '#F44336'; sd = '#4CAF50'; gamecard = '#03A9F4'; dstitle = '#AC98C7'
    # Deeper HighlightRamp step: the palest yellow washes out to near-white on the 3DS LCD.
    file = '#FFFFFF'; directory = '#EED076'; enabled = '#4CAF50'; disabled = '#F44336'
    ticketinuse = '#4CAF50'; ticketnotinuse = '#F44336'
    # FTP log levels, after ftpd's red, green, and cyan.
    ftperror = '#F44336'; ftpcommand = '#4CAF50'; ftpresponse = '#26C6DA'
}

function invokeMagick { runNativeCommand magick $args 'magick' }

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
newFill 'progress_bar_bg' '300x50' $CONTROL_ALT
newFill 'progress_bar_content' '280x30' $ACCENT
# Translucent so long names stay readable where the bar overlaps them.
newFill 'scroll_bar' '10x200' '#FFFFFF40'
# Neutral row fill with a purple bar: row text can be yellow, and yellow never sits on purple.
invokeMagick -size 320x20 "xc:$PRESSED" -fill $ACCENT -draw 'rectangle 0,0 2,19' `
    "PNG32:$romfs\selection_overlay.png"

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

# One 48px pixel-art tile feeds every logo; larger copies are integer point scales so pixels stay crisp.
$temp = (New-Item -ItemType Directory -Path "$env:TEMP\ftbTheme$(Get-Random)").FullName
try {
    # Center by the glyphs' ink box: -gravity center on text alone centers the font line box, and caps
    # have no descenders, so they would sit high.
    invokeMagick -background none -fill $LOGO_TEXT -font $FONT -pointsize $LOGO_POINTSIZE +antialias `
        label:FTB -trim +repage "PNG32:$temp\text.png"
    $inkSize = & magick identify -format '%w %h' "$temp\text.png"
    if ($LASTEXITCODE) { throw "magick identify failed (exit $LASTEXITCODE)." }
    $inkW, $inkH = $inkSize -split ' ' | ForEach-Object { [int]$_ }
    if ((48 - $inkW) % 2 -or (48 - $inkH) % 2) { throw "Logo text ${inkW}x$inkH cannot center on 48px; adjust LOGO_POINTSIZE." }

    $icon = "$meta\icon_3ds.png"
    invokeMagick -size 48x48 "xc:$ACCENT_STRONG" "$temp\text.png" -gravity center -composite "PNG24:$icon"
    invokeMagick $icon -filter point -resize 300% "PNG24:$romfs\logo.png"
    invokeMagick $icon -filter point -resize 200% "PNG24:$temp\icon2x.png"
    invokeMagick -size 256x128 "xc:$APP_BACKGROUND" "$temp\icon2x.png" -gravity center -composite `
        "PNG24:$meta\banner_3ds.png"

    # Launch splash textures keep the original sizes: a 128px logo and a 16x256 strip (240 rows shown)
    # stretched across both screens, here drawn with the app's 20px top and bottom bars.
    invokeMagick -size 128x128 "xc:$APP_BACKGROUND" "$temp\icon2x.png" -gravity center -composite `
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
