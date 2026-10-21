# Build two isolated copies of the same Bones checkout, with Ear disabled vs
# Ear enabled; report real PE delta. Do not change the active project tree.
param([string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'Requires x64 Native Tools Command Prompt for Visual Studio (cl.exe)'
}
$root = (Resolve-Path $RepositoryRoot).Path
$original = Join-Path $root 'BONES/modules/ear/modkit.c'
if (!(Test-Path $original)) { throw "Missing $original" }
$temp = Join-Path $env:TEMP ('EarPE_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $results = @{}
    foreach ($variant in @('baseline', 'new')) {
        $copy = Join-Path $temp $variant
        New-Item -ItemType Directory -Path $copy | Out-Null
        Copy-Item -LiteralPath (Join-Path $root 'BONES') -Destination $copy -Recurse
        $bones = Join-Path $copy 'BONES'
        if ($variant -eq 'baseline') {
            $stub = @'
#include "../../include/bones/modkit.h"
#include <string.h>
int32_t bones_mod_ear_query(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=abi;
    memcpy(out->id,"ear",sizeof("ear"));out->role=BONES_ROLE_EAR;
    out->implementation_version=1;
    out->requested_permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME;
    return BONES_MOD_OK;
}
'@
            Set-Content -LiteralPath (Join-Path $bones 'modules/ear/modkit.c') -Value $stub -Encoding ascii
        }
        Push-Location $bones
        try {
            cmd.exe /d /c 'scripts\build_windows_msvc.bat'
            if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $variant" }
        } finally { Pop-Location }
        $exe = Join-Path $bones 'build/Proyecto144_Bones_ModKit.exe'
        if (!(Test-Path $exe)) { throw "No executable from $variant" }
        $results[$variant] = (Get-Item -LiteralPath $exe).Length
    }
    $delta = $results['new'] - $results['baseline']
    Write-Host "Baseline real Windows PE: $($results['baseline']) bytes"
    Write-Host "Ear 0.4 real Windows PE: $($results['new']) bytes"
    Write-Host "Increment attributable to compiled Ear: $delta bytes"
    Write-Host 'The final EXE still must fit within 1,474,560 bytes.'
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force
}
