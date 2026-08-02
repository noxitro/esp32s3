# Builds sketch.ino. arduino-cli requires the .ino name to match its folder name,
# so we copy sketch.ino into build/sketch/ before compiling. webui/index.html is
# embedded into the firmware as webui_html.h (regenerated on every build).
#
# Usage:
#   .\build.ps1           # simulation build (HW CDC, HID compiled out — for wokwi-cli)
#   .\build.ps1 -Hid      # real-hardware build: XIAO ESP32S3, USB-OTG (TinyUSB) HID

param([switch]$Hid)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

New-Item -ItemType Directory -Force (Join-Path $root "build/sketch") | Out-Null
Copy-Item (Join-Path $root "sketch.ino") (Join-Path $root "build/sketch/sketch.ino") -Force

# embed web UI into a C header
$html = Get-Content (Join-Path $root "webui/index.html") -Raw -Encoding UTF8
if ($html.Contains(')WEBUI(')) { throw "webui/index.html must not contain ')WEBUI('" }
$header = "// auto-generated from webui/index.html - do not edit`n" +
          "#pragma once`n" +
          "const char CONFIG_HTML[] PROGMEM = R`"WEBUI($html)WEBUI`";`n"
Set-Content -Path (Join-Path $root "build/sketch/webui_html.h") -Value $header -Encoding UTF8 -NoNewline

$fqbn = $Hid ? "esp32:esp32:XIAO_ESP32S3:USBMode=default" : "esp32:esp32:esp32s3"

arduino-cli compile --fqbn $fqbn --output-dir (Join-Path $root "build") (Join-Path $root "build/sketch")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Build OK -> build/sketch.ino.bin"
