# =====================================================================
# build.ps1 — equivalente PowerShell do Makefile
# Uso:
#   .\build.ps1          → compila
#   .\build.ps1 run      → compila e abre no navegador
#   .\build.ps1 clean    → apaga a pasta dist/
# =====================================================================

param(
    [string]$Target = "build"
)

$ErrorActionPreference = "Stop"

# --- Diretórios ---
$SrcDir   = "src"
$CoreDir  = "$SrcDir\core"
$PlatDir  = "$SrcDir\platform"
$Win32Dir = "$SrcDir\win32"
$AssetDir = "assets"
$OutDir   = "dist"

# --- Defines herdados do CMakeLists.txt original ---
$Defines = @(
    "-DLODEPNG_NO_COMPILE_ENCODER",
    "-DLODEPNG_NO_COMPILE_DISK",
    "-DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS",
    "-DLODEPNG_NO_COMPILE_ERROR_TEXT",
    "-DLODEPNG_NO_COMPILE_CPP",
    "-DEMUXX=48",
    "-D__EMSCRIPTEN__"
)

# --- Warning suppression (mesmas flags do projeto Android) ---
$WarnFlags = @(
    "-fpermissive",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-absolute-value",
    "-Wno-ignored-pragmas",
    "-Wno-incompatible-pointer-types",
    "-Wno-int-conversion",
    "-Wno-nonportable-include-path",
    "-Wno-pointer-bool-conversion",
    "-Wno-pointer-sign",
    "-Wno-return-type",
    "-Wno-unsequenced"
)

# --- Include paths (win32/ vem ANTES de tudo pra resolver <windows.h>) ---
$Includes = @(
    "-I$Win32Dir",
    "-I$SrcDir",
    "-I$CoreDir",
    "-I$PlatDir"
)

# --- Fontes do core (mesma lista do CMakeLists.txt, sem os excluídos) ---
$CoreSrcs = @(
    "$CoreDir\apple.c",
    "$CoreDir\ddeserv.c",
    "$CoreDir\disasm.c",
    "$CoreDir\dismem.c",
    "$CoreDir\disrpl.c",
    "$CoreDir\engine.c",
    "$CoreDir\fetch.c",
    "$CoreDir\files.c",
    "$CoreDir\i28f160.c",
    "$CoreDir\keyboard.c",
    "$CoreDir\keymacro.c",
    "$CoreDir\kml.c",
    "$CoreDir\lodepng.c",
    "$CoreDir\lowbat.c",
    "$CoreDir\mops.c",
    "$CoreDir\mru.c",
    "$CoreDir\opcodes.c",
    "$CoreDir\pch.c",
    "$CoreDir\redeye.c",
    "$CoreDir\romcrc.c",
    "$CoreDir\rpl.c",
    "$CoreDir\serial.c",
    "$CoreDir\settings.c",
    "$CoreDir\sound.c",
    "$CoreDir\stack.c",
    "$CoreDir\symbfile.c",
    "$CoreDir\timer.c",
    "$CoreDir\display_ex.c"
)

$PlatSrcs = @(
    "$PlatDir\win32-layer.c",
    "$PlatDir\web-emu.c"
)

$Srcs = $CoreSrcs + $PlatSrcs

# --- Flags do Emscripten ---
$EmFlags = @(
    "-s", "USE_SDL=2",
    "-s", "WASM=1",
    "-s", "ALLOW_MEMORY_GROWTH=1",
    "-s", "INITIAL_MEMORY=64MB",
    "-s", "EXIT_RUNTIME=0",
    "-s", "ASYNCIFY",
    "--preload-file", "$AssetDir@/assets"
)

# =====================================================================
# Targets
# =====================================================================

function Invoke-Clean {
    if (Test-Path $OutDir) {
        Write-Host "Apagando $OutDir..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $OutDir
    }
}

function Invoke-Build {
    if (-not (Test-Path $OutDir)) {
        New-Item -ItemType Directory -Path $OutDir | Out-Null
    }

    $output = "$OutDir\emu48.html"
    $args = @("-O2") + $Includes + $Defines + $WarnFlags + $Srcs + @("-o", $output) + $EmFlags

    Write-Host "Compilando com emcc..." -ForegroundColor Cyan
    Write-Host "(isso pode demorar 1-2 minutos na primeira vez)" -ForegroundColor DarkGray
    Write-Host ""

    & emcc @args

    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "Build OK: $output" -ForegroundColor Green
    } else {
        Write-Host ""
        Write-Host "Build falhou com codigo $LASTEXITCODE" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

function Invoke-Run {
    Invoke-Build
    Write-Host "Abrindo no navegador (porta 8000)..." -ForegroundColor Cyan
    & emrun --port 8000 "$OutDir\emu48.html"
}

# =====================================================================
# Dispatch
# =====================================================================

switch ($Target.ToLower()) {
    "clean"  { Invoke-Clean }
    "run"    { Invoke-Run }
    "build"  { Invoke-Build }
    default  {
        Write-Host "Target desconhecido: $Target" -ForegroundColor Red
        Write-Host "Use: build, run, ou clean"
        exit 1
    }
}
