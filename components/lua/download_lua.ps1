# Download Lua 5.4.7 source for the ESP-IDF lua component
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$LuaVersion = "5.4.7"
$LuaUrl = "https://www.lua.org/ftp/lua-$LuaVersion.tar.gz"
$DestDir = Join-Path $ScriptDir "src"

if (Test-Path (Join-Path $DestDir "lua.h")) {
    Write-Host "[OK] Lua $LuaVersion source already present in $DestDir"
    exit 0
}

Write-Host "Downloading Lua $LuaVersion..."
$TmpDir = Join-Path $env:TEMP "lua_download"
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

$TarFile = Join-Path $TmpDir "lua.tar.gz"
Invoke-WebRequest -Uri $LuaUrl -OutFile $TarFile

Write-Host "Extracting to $DestDir..."
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null

# Extract using tar (available on Windows 10+)
tar -xzf $TarFile -C $TmpDir

$SrcDir = Join-Path $TmpDir "lua-$LuaVersion" "src"
Copy-Item "$SrcDir\*.c" $DestDir
Copy-Item "$SrcDir\*.h" $DestDir

Remove-Item -Recurse -Force $TmpDir
Write-Host "[OK] Lua $LuaVersion source installed to $DestDir"
