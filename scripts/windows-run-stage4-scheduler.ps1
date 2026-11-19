# SPDX-License-Identifier: MIT
# Native Windows (NOT Wine) scheduler differential evidence.
# Requires installed Microsoft C++ Build Tools / MSVC. No admin rights.
[CmdletBinding()]
param(
    [string]$OutputPath = "build/stage4-windows-native.txt",
    [switch]$RequireMultiGroup,
    [switch]$RequirePhysical
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $build | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio vswhere.exe unavailable; install MSVC Build Tools." }
$install = (& $vswhere -latest -products "*" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $install) { throw "No MSVC x64 toolset found." }
$vcvars = Join-Path $install "VC/Auxiliary/Build/vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "Missing vcvars64.bat" }
$source = Join-Path $root "tests/windows/stage4_scheduler_probe.c"
$fixture = Join-Path $root "tests/pe/thread_group_affinity_fixture.c"
$quantumSource = Join-Path $root "tests/bench/scheduler_quantum_observation.c"
$fpSource = Join-Path $root "tests/windows/stage4_fp_context_probe.c"
$exe = Join-Path $build "stage4-scheduler-native.exe"
$fixtureExe = Join-Path $build "stage4-group-fixture-native.exe"
$quantumExe = Join-Path $build "stage4-quantum-native.exe"
$fpExe = Join-Path $build "stage4-fp-context-native.exe"
$compileCmd = Join-Path $build "compile-stage4-windows.cmd"
@"
@echo off
call "$vcvars" >nul || exit /b 1
cl /nologo /W4 /O2 /TC /D_WIN32_WINNT=0x0A00 /Fe:"$exe" "$source" /link kernel32.lib
if errorlevel 1 exit /b 1
rem The EXACT same PE fixture used by the Linux guest runtime is now
rem compiled and run natively. A behavior divergence is preserved, never hidden.
cl /nologo /std:c11 /W4 /O2 /TC /D_WIN32_WINNT=0x0A00 /Dimport_fixture_entry=main /Fe:"$fixtureExe" "$fixture" /link kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O2 /TC /D_WIN32_WINNT=0x0A00 /Fe:"$quantumExe" "$quantumSource" /link kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O2 /TC /D_WIN32_WINNT=0x0A00 /Fe:"$fpExe" "$fpSource" /link kernel32.lib
exit /b %errorlevel%
"@ | Set-Content -Encoding ASCII $compileCmd
& cmd.exe /d /c "`"$compileCmd`""
if ($LASTEXITCODE -ne 0) { throw "MSVC native compilation failed (exit $LASTEXITCODE)." }
$os = Get-CimInstance -ClassName Win32_OperatingSystem
$machine = Get-CimInstance -ClassName Win32_ComputerSystem
if ($RequirePhysical -and $machine.Model -match "(?i)virtual|vmware|hyper-v|kvm|qemu|xen|bochs") {
    throw "Recognized virtual hardware ($($machine.Model)); cannot certify a physical Windows PC."
}
if (-not [IO.Path]::IsPathRooted($OutputPath)) { $OutputPath = Join-Path $root $OutputPath }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null
& $exe | Set-Content -Encoding ASCII $OutputPath
$exit = $LASTEXITCODE
Get-Content $OutputPath
if ($exit -ne 0) { throw "Native Windows scheduler probe failed (exit $exit). Evidence: $OutputPath" }
# Diagnostic comparison, not an assertion of full Windows parity:
# the fixture may encode an overly narrow Linux contract. Preserve the
# native Windows exit code as evidence rather than treating it as a PASS.
& $fixtureExe | Out-Null
$fixtureExit = $LASTEXITCODE
$fixtureReport = Join-Path (Split-Path -Parent $OutputPath) "stage4-windows-linux-fixture.txt"
@(
    "SCHEMA=ntnative.stage4.fixture.crossos.v1"
    "PLATFORM=WINDOWS_NATIVE"
    "PE_GROUP_FIXTURE_EXIT=$fixtureExit"
    "PE_GROUP_FIXTURE_RESULT=$(if ($fixtureExit -eq 0) { 'MATCHED' } else { 'DIVERGED' })"
    "NOTE=This is the same tests/pe/thread_group_affinity_fixture.c used by Linux guest CI"
) | Set-Content -Encoding ASCII $fixtureReport
Get-Content $fixtureReport
# Same-source Windows/Linux scheduler contention observation, no NT quantum claim.
$quantumReport = Join-Path (Split-Path -Parent $OutputPath) "stage4-windows-quantum.txt"
& $quantumExe | Set-Content -Encoding ASCII $quantumReport
$quantumExit = $LASTEXITCODE
Get-Content $quantumReport
if ($quantumExit -ne 0 -or
    -not (Select-String -Path $quantumReport -Pattern '^RESULT=PASS$' -Quiet) -or
    -not (Select-String -Path $quantumReport -Pattern '^NT_QUANTUM_VERIFIED=NO$' -Quiet)) {
    throw "Native Windows pinned CPU scheduler benchmark failed. See $quantumReport."
}
# The FP reference is run on a *real Windows kernel*, including when
# GitHub uses a VM. The report records API failures instead of inventing
# CONTEXT_FLOATING_POINT parity with Linux.
$fpReport = Join-Path (Split-Path -Parent $OutputPath) "stage4-windows-fp-context.txt"
& $fpExe | Set-Content -Encoding ASCII $fpReport
$fpExit = $LASTEXITCODE
Get-Content $fpReport
if ($fpExit -ne 0 -or
    -not (Select-String -Path $fpReport -Pattern '^SCHEMA=ntnative.stage4.fp-context.v1
foreach ($line in (Get-Content $OutputPath)) {
    if ($line -match '^([A-Z_]+)=(.*)$') { $entries[$matches[1]] = $matches[2] }
}
if ($entries["SCHEMA"] -ne "ntnative.stage4.scheduler.v1" -or
    $entries["PLATFORM"] -ne "WINDOWS_NATIVE" -or $entries["THREAD_GROUP"] -ne "PASS") {
    throw "Missing or failed contract evidence in $OutputPath."
}
if ($RequireMultiGroup -and ($entries["GROUPS"] -lt 2 -or
                            $entries["ACTIVE_CPUS"] -lt 65 -or
                            $entries["CROSS_GROUP"] -ne "PASS")) {
    throw "INCOMPLETE: native Windows host did not verify a real >64 CPU group transition."
}
$meta = Join-Path (Split-Path -Parent $OutputPath) "stage4-windows-meta.txt"
@(
    "OS=$($os.Caption)"
    "BUILD=$($os.BuildNumber)"
    "ARCH=$($os.OSArchitecture)"
    "MODEL=$($machine.Model)"
    "PHYSICAL_HARDWARE_VERIFIED=NO"
    "REQUIRES_INDEPENDENT_PHYSICAL_ATTESTATION=YES"
    "GIT_COMMIT=$(git rev-parse HEAD)"
) | Set-Content -Encoding UTF8 $meta
Write-Host "Native Windows evidence saved to $OutputPath; metadata $meta."
Write-Host "NOTE: a hosted Windows VM is a real Windows kernel, NOT a physical hardware acceptance."
 -Quiet) -or
    -not (Select-String -Path $fpReport -Pattern '^PLATFORM=WINDOWS_NATIVE
foreach ($line in (Get-Content $OutputPath)) {
    if ($line -match '^([A-Z_]+)=(.*)$') { $entries[$matches[1]] = $matches[2] }
}
if ($entries["SCHEMA"] -ne "ntnative.stage4.scheduler.v1" -or
    $entries["PLATFORM"] -ne "WINDOWS_NATIVE" -or $entries["THREAD_GROUP"] -ne "PASS") {
    throw "Missing or failed contract evidence in $OutputPath."
}
if ($RequireMultiGroup -and ($entries["GROUPS"] -lt 2 -or
                            $entries["ACTIVE_CPUS"] -lt 65 -or
                            $entries["CROSS_GROUP"] -ne "PASS")) {
    throw "INCOMPLETE: native Windows host did not verify a real >64 CPU group transition."
}
$meta = Join-Path (Split-Path -Parent $OutputPath) "stage4-windows-meta.txt"
@(
    "OS=$($os.Caption)"
    "BUILD=$($os.BuildNumber)"
    "ARCH=$($os.OSArchitecture)"
    "MODEL=$($machine.Model)"
    "PHYSICAL_HARDWARE_VERIFIED=NO"
    "REQUIRES_INDEPENDENT_PHYSICAL_ATTESTATION=YES"
    "GIT_COMMIT=$(git rev-parse HEAD)"
) | Set-Content -Encoding UTF8 $meta
Write-Host "Native Windows evidence saved to $OutputPath; metadata $meta."
Write-Host "NOTE: a hosted Windows VM is a real Windows kernel, NOT a physical hardware acceptance."
 -Quiet)) {
    throw "Invalid native Windows FXSAVE context reference: $fpReport"
}
$entries = @{}
foreach ($line in (Get-Content $OutputPath)) {
    if ($line -match '^([A-Z_]+)=(.*)$') { $entries[$matches[1]] = $matches[2] }
}
if ($entries["SCHEMA"] -ne "ntnative.stage4.scheduler.v1" -or
    $entries["PLATFORM"] -ne "WINDOWS_NATIVE" -or $entries["THREAD_GROUP"] -ne "PASS") {
    throw "Missing or failed contract evidence in $OutputPath."
}
if ($RequireMultiGroup -and ($entries["GROUPS"] -lt 2 -or
                            $entries["ACTIVE_CPUS"] -lt 65 -or
                            $entries["CROSS_GROUP"] -ne "PASS")) {
    throw "INCOMPLETE: native Windows host did not verify a real >64 CPU group transition."
}
$meta = Join-Path (Split-Path -Parent $OutputPath) "stage4-windows-meta.txt"
@(
    "OS=$($os.Caption)"
    "BUILD=$($os.BuildNumber)"
    "ARCH=$($os.OSArchitecture)"
    "MODEL=$($machine.Model)"
    "PHYSICAL_HARDWARE_VERIFIED=NO"
    "REQUIRES_INDEPENDENT_PHYSICAL_ATTESTATION=YES"
    "GIT_COMMIT=$(git rev-parse HEAD)"
) | Set-Content -Encoding UTF8 $meta
Write-Host "Native Windows evidence saved to $OutputPath; metadata $meta."
Write-Host "NOTE: a hosted Windows VM is a real Windows kernel, NOT a physical hardware acceptance."
