$env:IDF_PATH = "D:\Espressif\esp-idf"
$env:IDF_TOOLS_PATH = "D:\Espressif\.espressif"
$env:IDF_PYTHON_ENV_PATH = "D:\Espressif\.espressif\python_env\idf6.1_py3.12_env"
$env:PYTHONUTF8 = "1"
$venvPy = "D:\Espressif\.espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe"
$out = & $venvPy "$env:IDF_PATH\tools\idf_tools.py" export --format key-value 2>&1
foreach ($line in $out) { if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] } }
$env:PYTHONUTF8 = "1"
Set-Location $PSScriptRoot
Write-Host "=== Building xiaozhi-c ===" -ForegroundColor Cyan
& $venvPy "$env:IDF_PATH\tools\idf.py" build
Write-Host ""
Write-Host ("BUILD EXIT CODE: " + $LASTEXITCODE) -ForegroundColor Green
