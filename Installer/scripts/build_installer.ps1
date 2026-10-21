# Build a Windows installer from the ALREADY COMPILED native Bones game.
# This script never merges PRs or downloads game binaries.
param(
    [string]$RepositoryRoot = "",
    [string]$GitSha = ""
)
$ErrorActionPreference = "Stop"
if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}
$RepositoryRoot = (Resolve-Path $RepositoryRoot).Path
if (-not $GitSha) {
    $GitSha = (& git -C $RepositoryRoot rev-parse HEAD).Trim()
}
if ($GitSha -notmatch '^[0-9a-fA-F]{40}$') { throw "SHA invalido: $GitSha" }
$short = $GitSha.Substring(0, 10).ToLowerInvariant()
$game = Join-Path $RepositoryRoot 'BONES\build\Proyecto144_Bones_ModKit.exe'
$dir = Join-Path $RepositoryRoot 'Installer\dist'
$iss = Join-Path $RepositoryRoot 'Installer\inno\Proyecto144.iss'
if (!(Test-Path -LiteralPath $game -PathType Leaf)) {
    throw "Falta EXE compilado: $game. Ejecutar BONES\scripts\build_windows_msvc.bat antes."
}
$gameInfo = Get-Item -LiteralPath $game
if ($gameInfo.Length -le 1024 -or $gameInfo.Length -gt 1474560) {
    throw "Tamano PE inesperado o superior al presupuesto: $($gameInfo.Length)"
}
$stream = [System.IO.File]::OpenRead($game)
try {
    if ($stream.ReadByte() -ne 0x4d -or $stream.ReadByte() -ne 0x5a) {
        throw "No es un ejecutable Windows PE (firma MZ ausente)."
    }
} finally { $stream.Dispose() }
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $game).Hash.ToLowerInvariant()
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
$releaseText = @"
PROYECTO 1.44 — COMPILACION INTEGRADA DE MAIN
Git SHA: $GitSha
Fecha compilacion UTC: $buildTime
Motor: Bones 0.4.0 / ModKit ABI 1 (prototipo)
Fichero: Proyecto144.exe
Tamano EXE original: $($gameInfo.Length) bytes
SHA256 EXE: $hash
Modo: prototipo 3D local (no juego RPG completo).
Las versiones de PR pendientes no se instalan automaticamente.
La UI multijugador, el cliente grafico y las ramas experimentales
no se deben considerar integrados en esta compilacion.
El instalador y sus accesos directos no forman parte del limite
de 1.44 MB del EXE del videojuego.
"@
Set-Content -LiteralPath (Join-Path $dir 'VERSION.txt') -Value $releaseText -Encoding UTF8
$manifest = [ordered]@{
    project = "Proyecto 1.44"
    source_ref = "main"
    source_sha = $GitSha.ToLowerInvariant()
    built_utc = $buildTime
    game_version = "Bones ModKit 0.4.0 prototype"
    installer_id = $short
    exe_bytes = $gameInfo.Length
    exe_sha256 = $hash
    status = "Main build, not merged PRs; GUI LAN not yet integrated."
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $dir 'MANIFEST.json') -Encoding UTF8
$portable = Join-Path $dir "Proyecto144_Portable_$short"
New-Item -ItemType Directory -Force -Path $portable | Out-Null
Copy-Item -LiteralPath $game -Destination (Join-Path $portable 'Proyecto144.exe') -Force
Copy-Item -LiteralPath (Join-Path $dir 'VERSION.txt') -Destination (Join-Path $portable 'VERSION.txt') -Force
$zip = Join-Path $dir "Proyecto144_Portable_$short.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath $zip -CompressionLevel Optimal

$candidates = @()
$found = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if ($found) { $candidates += $found.Source }
$x86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
if ($x86) {
    $candidates += (Join-Path $x86 'Inno Setup 6\ISCC.exe')
}
if ($env:ProgramFiles) {
    $candidates += (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
}
$compiler = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
if (-not $compiler) {
    throw "No se encuentra Inno Setup 6 (ISCC.exe). Instalarlo en la maquina de compilacion."
}
Write-Host "Compilando Inno Setup con SHA $short"
& $compiler "/DP144_BUILD_ID=$short" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC fallo (exit code $LASTEXITCODE)" }
$installer = Join-Path $dir "Proyecto144_Instalador_$short.exe"
if (!(Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "No se genero el instalador esperado: $installer"
}
$installerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash.ToLowerInvariant()
Write-Host "EXE juego: $($gameInfo.Length) bytes; SHA256 $hash"
Write-Host "Instalador: $installer"
Write-Host "Instalador SHA256: $installerHash"
Write-Host "Portable: $zip"
Write-Host "No incorpora los PR sin fusionar y no afirma que LAN GUI este listo."
