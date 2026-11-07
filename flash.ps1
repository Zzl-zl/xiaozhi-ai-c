$env:IDF_PATH = "D:\Espressif\esp-idf"
$env:IDF_TOOLS_PATH = "D:\Espressif\.espressif"
$env:IDF_PYTHON_ENV_PATH = "D:\Espressif\.espressif\python_env\idf6.1_py3.12_env"
$env:PYTHONUTF8 = "1"
$venvPy = "D:\Espressif\.espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe"
$out = & $venvPy "$env:IDF_PATH\tools\idf_tools.py" export --format key-value 2>&1
foreach ($line in $out) { if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] } }
$env:PYTHONUTF8 = "1"
Set-Location $PSScriptRoot

$ports = [System.IO.Ports.SerialPort]::GetPortNames()
if ($ports.Count -gt 0) { $PORT = $ports[0] } else { $PORT = "COM5" }

Write-Host ("=== Flashing firmware to " + $PORT + " ===") -ForegroundColor Cyan
& $venvPy "$env:IDF_PATH\tools\idf.py" -p $PORT flash
Write-Host ""
Write-Host ("FLASH EXIT CODE: " + $LASTEXITCODE) -ForegroundColor Green

# 开机动画 30 帧资源 -> assets 分区（0xB00000 / 5MB）
$assetsBin = "D:\xiaozhi-c\main\assets.bin"
if (Test-Path -LiteralPath $assetsBin) {
    Write-Host ""
    Write-Host ("=== Flashing assets.bin to 0xB00000 ===") -ForegroundColor Cyan
    & $venvPy -m esptool --chip esp32s3 -p $PORT -b 460800 write_flash 0xB00000 $assetsBin
    Write-Host ("ASSETS EXIT CODE: " + $LASTEXITCODE) -ForegroundColor Green
}
