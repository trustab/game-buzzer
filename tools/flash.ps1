<#
.SYNOPSIS
  Compile and flash the hub or a button with arduino-cli.

.EXAMPLE
  .\tools\flash.ps1 -Setup                          # one-time: install ESP32 core + NimBLE
  .\tools\flash.ps1 hub -Port COM4 -Monitor          # flash hub, then open serial monitor
  .\tools\flash.ps1 button -Player 2 -Port COM5      # flash button as player 2
  .\tools\flash.ps1 button -Player 3 -CompileOnly    # just check that it builds
#>
param(
  [ValidateSet('hub', 'button')] [string]$Target,
  [ValidateRange(1, 4)] [int]$Player = 1,
  [string]$Port,
  [switch]$CompileOnly,
  [switch]$Monitor,
  [switch]$Setup
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# ESP32C3 Dev Module with USB CDC On Boot enabled (otherwise no Serial output)
$fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc'

# Prefer arduino-cli on PATH, fall back to the copy bundled with Arduino IDE 2
$cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $cli) {
  $bundled = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
  if (Test-Path $bundled) { $cli = $bundled }
  else { throw 'arduino-cli not found. Install it with: winget install ArduinoSA.CLI' }
}

function Invoke-Cli {
  & $cli @args
  if ($LASTEXITCODE -ne 0) { throw "arduino-cli $($args[0]) failed (exit $LASTEXITCODE)" }
}

if ($Setup) {
  Invoke-Cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
  Invoke-Cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
  Invoke-Cli lib install NimBLE-Arduino
  if (-not $Target) { return }
}

if (-not $Target) { throw 'Specify a target: hub or button (or -Setup).' }
if (-not $CompileOnly -and -not $Port) {
  Write-Host 'No -Port given. Connected boards:'
  Invoke-Cli board list
  throw 'Re-run with -Port COMx (or -CompileOnly).'
}

$sketch = Join-Path $root "buzzer_$Target"
$build = Join-Path $root "build\$Target"
$compileArgs = @('compile', '-b', $fqbn, '--build-path')

if ($Target -eq 'button') {
  $build = Join-Path $root "build\button-p$Player"
  $compileArgs += $build
  $compileArgs += @('--build-property', "compiler.cpp.extra_flags=-DPLAYER_ID=$Player")
  Write-Host "Building button as player $Player"
} else {
  $compileArgs += $build
  Write-Host 'Building hub'
}

# protocol.h must be identical in both sketch folders
$hubProto = Get-FileHash (Join-Path $root 'buzzer_hub\protocol.h')
$btnProto = Get-FileHash (Join-Path $root 'buzzer_button\protocol.h')
if ($hubProto.Hash -ne $btnProto.Hash) { throw 'buzzer_hub/protocol.h and buzzer_button/protocol.h differ.' }

Invoke-Cli @compileArgs $sketch
if ($CompileOnly) { return }

Invoke-Cli upload -b $fqbn -p $Port --input-dir $build $sketch
Write-Host "Flashed $Target$(if ($Target -eq 'button') { " (player $Player)" }) on $Port"

if ($Monitor) {
  Invoke-Cli monitor -p $Port -c baudrate=115200
}
