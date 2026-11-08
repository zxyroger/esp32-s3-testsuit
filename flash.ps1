# ESP32-S3 Hello Demo - build / flash / monitor
# Usage (PowerShell, in D:\s3demo):
#   .\flash.ps1 build      build only
#   .\flash.ps1 flash      build + flash COM3
#   .\flash.ps1 monitor    build + flash + serial monitor
param([string]$Action = "flash", [string]$Port = "COM3")

$env:IDF_PATH            = "D:\esp\v6.0.2\v6.0.2\esp-idf"
$env:IDF_TOOLS_PATH      = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.0.2\venv"
$env:ESP_IDF_VERSION     = "6.0.2"
$romElf = Get-ChildItem "C:\Espressif\tools\esp-rom-elfs" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if ($romElf) { $env:ESP_ROM_ELF_DIR = $romElf.FullName + "\" }

$toolBins = @(
  "C:\Espressif\tools\python\v6.0.2\venv\Scripts",
  "C:\Espressif\tools\git\bin",
  "C:\Espressif\tools\cmake\3.30.2\bin",
  "C:\Espressif\tools\ninja\1.12.1",
  "C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
  "C:\Espressif\tools\esp32ulp-elf\2.38.1-esp-20220906\esp32ulp-elf-binutils\bin"
)
foreach ($b in $toolBins) { if (Test-Path $b) { $env:PATH = "$b;$env:PATH" } }

# C: 盘空间紧张时，编译临时文件放 D 盘
$tmpDir = "D:\s3demo_tmp"
if (-not (Test-Path $tmpDir)) { New-Item -ItemType Directory -Force $tmpDir | Out-Null }
$env:TMPDIR = $tmpDir; $env:TEMP = $tmpDir; $env:TMP = $tmpDir

$idfPy = Join-Path $env:IDF_PATH "tools\idf.py"
$py    = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"

Write-Host "==> build" -ForegroundColor Cyan
& $py $idfPy build
if ($LASTEXITCODE -ne 0) { Write-Host "build failed" -ForegroundColor Red; exit 1 }
if ($Action -eq "build") { exit 0 }

Write-Host "==> flash $Port" -ForegroundColor Cyan
# 串口桥接（bridge_serial_mqtt.py）常驻占用串口，刷机前先让它松手：
# 建一个标记文件，桥接看到就 close 并停手；刷完删掉，桥接自动收回串口。
$releaseFlag = "D:\s3demo\bridge.release"
$hadFlag = Test-Path $releaseFlag
try {
  Set-Content -Path $releaseFlag -Value (Get-Date -Format o) -Encoding utf8
  Start-Sleep -Milliseconds 1500
  & $py $idfPy -p $Port flash
  if ($LASTEXITCODE -ne 0) { Write-Host "flash failed" -ForegroundColor Red; exit 1 }
} finally {
  if (-not $hadFlag) { Remove-Item $releaseFlag -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 300
}

if ($Action -eq "monitor") { & $py $idfPy -p $Port monitor }
