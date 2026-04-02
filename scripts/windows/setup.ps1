# scripts/windows/setup.ps1
#
# Windows environment setup & build
#
# What it does:
#   1. Check dependencies (cmake, cl/g++, ninja)
#   2. Download Wintun DLL
#   3. Download ONNX Runtime if needed
#   4. Download mbedTLS if needed (--with-tls)
#   5. Build NeuStack (auto-enable AI if ORT found)
#
# Usage:
#   .\scripts\windows\setup.ps1 [-NoAI] [-WithTLS]
#
# Prerequisites:
#   - Visual Studio 2019+ with C++ workload, or MinGW g++ >= 10
#   - Run from Developer Command Prompt (for MSVC) or have g++ in PATH

param(
    [switch]$NoAI,
    [switch]$WithTLS
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = (Resolve-Path "$ScriptDir\..\..").Path

Write-Host "=============================================="
Write-Host "  NeuStack Windows Setup"
Write-Host "=============================================="
Write-Host "  Project: $ProjectRoot"
Write-Host "=============================================="
Write-Host ""

# ─── 1. Check dependencies ───
Write-Host "[1/5] Checking dependencies..."

function Test-Command($cmd) {
    $found = Get-Command $cmd -ErrorAction SilentlyContinue
    if ($found) {
        Write-Host "  ✓ $cmd ($($found.Source))"
        return $true
    } else {
        Write-Host "  ✗ $cmd (missing)"
        return $false
    }
}

$hasCmake = Test-Command "cmake"
$hasNinja = Test-Command "ninja"

# 检查 C++ 编译器: 优先 cl (MSVC), 其次 g++
$hasCL = Test-Command "cl"
$hasGpp = Test-Command "g++"

if (-not $hasCmake) {
    Write-Host ""
    Write-Host "  CMake not found. Install options:"
    Write-Host "    winget install Kitware.CMake"
    Write-Host "    choco install cmake"
    exit 1
}

if (-not $hasCL -and -not $hasGpp) {
    Write-Host ""
    Write-Host "  No C++ compiler found."
    Write-Host "  Option 1: Install Visual Studio with C++ workload, run from Developer Command Prompt"
    Write-Host "  Option 2: Install MinGW-w64 (g++ >= 10)"
    exit 1
}

if (-not $hasNinja) {
    Write-Host "  - ninja not found, will use default generator"
    Write-Host "    Install: winget install Ninja-build.Ninja"
}

# ─── 2. Download Wintun ───
Write-Host ""
Write-Host "[2/5] Checking Wintun..."

$WintunDll = Join-Path $ProjectRoot "wintun.dll"
$BuildWintunDll = Join-Path $ProjectRoot "build\wintun.dll"

if (Test-Path $WintunDll) {
    Write-Host "  ✓ wintun.dll already exists"
} else {
    $WintunVersion = "0.14.1"
    $WintunUrl = "https://www.wintun.net/builds/wintun-${WintunVersion}.zip"
    $WintunZip = Join-Path $env:TEMP "wintun-${WintunVersion}.zip"
    $WintunExtract = Join-Path $env:TEMP "wintun-extract"

    Write-Host "  Downloading Wintun ${WintunVersion}..."
    Invoke-WebRequest -Uri $WintunUrl -OutFile $WintunZip

    # 检测架构
    if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") {
        $WintunArch = "arm64"
    } else {
        $WintunArch = "amd64"
    }

    Write-Host "  Extracting ($WintunArch)..."
    if (Test-Path $WintunExtract) { Remove-Item -Recurse -Force $WintunExtract }
    Expand-Archive -Path $WintunZip -DestinationPath $WintunExtract
    Copy-Item "$WintunExtract\wintun\bin\$WintunArch\wintun.dll" $WintunDll
    Remove-Item -Recurse -Force $WintunExtract
    Remove-Item -Force $WintunZip
    Write-Host "  ✓ wintun.dll downloaded"
}

# ─── 3. ONNX Runtime ───
Write-Host ""
Write-Host "[3/5] Checking ONNX Runtime..."

$OrtFound = $false

if (-not $NoAI) {
    # 检测架构
    if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") {
        $OrtPlatform = "win-arm64"
    } else {
        $OrtPlatform = "win-x64"
    }

    $OrtDir = Join-Path $ProjectRoot "third_party\onnxruntime\$OrtPlatform"
    $OrtLib = Join-Path $OrtDir "lib\onnxruntime.dll"

    if (Test-Path $OrtLib) {
        Write-Host "  ✓ ONNX Runtime ($OrtPlatform) already installed"
        $OrtFound = $true
    } else {
        $OrtVersion = "1.17.0"
        $OrtArchive = "onnxruntime-$OrtPlatform-$OrtVersion"
        $OrtUrl = "https://github.com/microsoft/onnxruntime/releases/download/v${OrtVersion}/${OrtArchive}.zip"
        $OrtZip = Join-Path $env:TEMP "${OrtArchive}.zip"

        Write-Host "  Downloading ONNX Runtime ($OrtPlatform)..."
        try {
            Invoke-WebRequest -Uri $OrtUrl -OutFile $OrtZip
            $OrtExtract = Join-Path $env:TEMP "ort-extract"
            if (Test-Path $OrtExtract) { Remove-Item -Recurse -Force $OrtExtract }
            Expand-Archive -Path $OrtZip -DestinationPath $OrtExtract

            New-Item -ItemType Directory -Force -Path $OrtDir | Out-Null
            Copy-Item -Recurse "$OrtExtract\$OrtArchive\*" $OrtDir
            Remove-Item -Recurse -Force $OrtExtract
            Remove-Item -Force $OrtZip
            $OrtFound = $true
            Write-Host "  ✓ ONNX Runtime downloaded"
        } catch {
            Write-Host "  ✗ Download failed, AI features will be disabled"
        }
    }
}

# 确定 AI 开关
if ($NoAI) {
    $EnableAI = "OFF"
    Write-Host "  AI: disabled (-NoAI flag)"
} elseif ($OrtFound) {
    $EnableAI = "ON"
    Write-Host "  AI: enabled (ONNX Runtime found)"
} else {
    $EnableAI = "OFF"
    Write-Host "  AI: disabled (ONNX Runtime not available)"
}

# ─── 4. mbedTLS (TLS support) ───
Write-Host ""
Write-Host "[4/5] Checking mbedTLS..."

$EnableTLS = "OFF"

if ($WithTLS) {
    $MbedtlsDir = Join-Path $ProjectRoot "third_party\mbedtls"
    $MbedtlsCMake = Join-Path $MbedtlsDir "CMakeLists.txt"

    if (Test-Path $MbedtlsCMake) {
        Write-Host "  ✓ mbedTLS found"
        $EnableTLS = "ON"
    } else {
        $MbedtlsVersion = "3.6.0"
        $MbedtlsUrl = "https://github.com/Mbed-TLS/mbedtls/releases/download/v${MbedtlsVersion}/mbedtls-${MbedtlsVersion}.tar.bz2"
        $MbedtlsArchive = Join-Path $env:TEMP "mbedtls-${MbedtlsVersion}.tar.bz2"
        $MbedtlsExtractDir = Join-Path $ProjectRoot "third_party"

        Write-Host "  Downloading mbedTLS v${MbedtlsVersion}..."
        try {
            Invoke-WebRequest -Uri $MbedtlsUrl -OutFile $MbedtlsArchive

            # tar can handle .tar.bz2 on Windows 10+
            Write-Host "  Extracting..."
            New-Item -ItemType Directory -Force -Path $MbedtlsExtractDir | Out-Null
            tar xjf $MbedtlsArchive -C $MbedtlsExtractDir

            $ExtractedDir = Join-Path $MbedtlsExtractDir "mbedtls-${MbedtlsVersion}"
            if (Test-Path $ExtractedDir) {
                Rename-Item $ExtractedDir $MbedtlsDir
                $EnableTLS = "ON"
                Write-Host "  ✓ mbedTLS downloaded"
            } else {
                Write-Host "  ✗ Extraction failed (expected $ExtractedDir)"
            }

            Remove-Item -Force $MbedtlsArchive -ErrorAction SilentlyContinue
        } catch {
            Write-Host "  ✗ mbedTLS download failed: $_"
            Write-Host "  TLS will be disabled"
        }
    }
} else {
    Write-Host "  TLS: skipped (pass -WithTLS to enable)"
}

# ─── 5. Build ───
Write-Host ""
Write-Host "[5/5] Building NeuStack..."

$BuildDir = Join-Path $ProjectRoot "build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Set-Location $BuildDir

$cmakeArgs = @(
    "..",
    "-DNEUSTACK_ENABLE_AI=$EnableAI",
    "-DNEUSTACK_ENABLE_TLS=$EnableTLS",
    "-DCMAKE_BUILD_TYPE=Release"
)

if ($hasNinja) {
    $cmakeArgs += @("-G", "Ninja")
}

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build . --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 拷贝 wintun.dll 到 build 目录
if ((Test-Path $WintunDll) -and -not (Test-Path $BuildWintunDll)) {
    Copy-Item $WintunDll $BuildDir
    Write-Host "  Copied wintun.dll to build/"
}

Set-Location $ProjectRoot

Write-Host ""
Write-Host "=============================================="
Write-Host "  ✓ Build Complete!"
Write-Host "=============================================="
Write-Host ""
Write-Host "  AI enabled:  $EnableAI"
Write-Host "  TLS:         $EnableTLS"
Write-Host "  Binary:      build\examples\neustack_demo.exe"
Write-Host "  Wintun:      build\wintun.dll"
Write-Host ""
Write-Host "  Quick start:"
Write-Host "    # Run as Administrator (required for Wintun)"
Write-Host "    .\build\examples\neustack_demo.exe --ip 192.168.100.2 -v"
Write-Host ""
if ($EnableTLS -eq "ON") {
    Write-Host "    # Generate TLS certificate"
    Write-Host "    openssl req -x509 -newkey rsa:2048 -keyout server_key.pem -out server_cert.pem -days 3650 -nodes -subj '/CN=NeuStack'"
    Write-Host ""
    Write-Host "    # Run HTTPS server"
    Write-Host "    .\build\examples\https_server.exe"
    Write-Host ""
}
