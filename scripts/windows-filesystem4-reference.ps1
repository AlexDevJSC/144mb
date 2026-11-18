# SPDX-License-Identifier: MIT
# Run on a REAL Windows machine with NTFS to capture a comparison reference.
# This is not executed by Linux GitHub Actions or a Windows simulator.
param(
    [Parameter(Mandatory=$true)][string]$Output,
    [switch]$PhysicalWindowsConfirmed
)

$ErrorActionPreference = 'Stop'
if (-not $PhysicalWindowsConfirmed) {
    throw "Specify -PhysicalWindowsConfirmed on an actual Windows NTFS host."
}
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw "Windows NT is required."
}

$root = [IO.Path]::GetTempPath()
$drive = [IO.Path]::GetPathRoot($root)
$volume = $null
try {
    $volume = Get-Volume -DriveLetter $drive.Substring(0,1) -ErrorAction Stop
} catch {
    throw "Cannot verify actual volume filesystem type: $_"
}
if ($volume.FileSystem -ne 'NTFS') {
    throw "The testing drive must be NTFS, not $($volume.FileSystem)"
}

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class FS4Native {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern uint GetShortPathNameW(string path, StringBuilder buffer, uint size);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CreateHardLinkW(string newName, string existingName,
                                              IntPtr security);
}
'@

$folder = Join-Path $root ("fs4-reference-" + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($folder) | Out-Null
try {
    $longName = 'longFileSystem4ReferenceFilename.txt'
    $first = Join-Path $folder $longName
    $content = [Text.Encoding]::UTF8.GetBytes('NT144 Windows reference')
    [IO.File]::WriteAllBytes($first, $content)
    $uppercase = Join-Path $folder $longName.ToUpperInvariant()
    $caseInsensitive = [IO.File]::Exists($uppercase)
    $shortBuffer = New-Object System.Text.StringBuilder(32767)
    $shortLen = [FS4Native]::GetShortPathNameW($first, $shortBuffer,
                                               [uint32]$shortBuffer.Capacity)
    $shortName = if ($shortLen -gt 0 -and $shortLen -lt $shortBuffer.Capacity) {
        $shortBuffer.ToString()
    } else { $null }
    $target = Join-Path $folder 'renamedFileSystem4Reference.txt'
    [IO.File]::Move($first, $target)
    $renamePreservedBytes = ([Convert]::ToBase64String([IO.File]::ReadAllBytes($target)) -eq
                             [Convert]::ToBase64String($content))
    $link = Join-Path $folder 'hardlinkFileSystem4Reference.txt'
    $hardlinkCreated = [FS4Native]::CreateHardLinkW($link, $target, [IntPtr]::Zero)
    $hardlinkContentsSame = $hardlinkCreated -and
        ([IO.File]::ReadAllBytes($link).Length -eq $content.Length)
    [IO.File]::SetAttributes($target, [IO.FileAttributes]::ReadOnly)
    $readOnlyReported = (([IO.File]::GetAttributes($target) -band
                          [IO.FileAttributes]::ReadOnly) -ne 0)
    [IO.File]::SetAttributes($target, [IO.FileAttributes]::Normal)

    # Additional NTFS checks: evidence only, never claim full ACL equivalence.
    $aclDescriptorReadable = $null
    try {
        $security = Get-Acl -LiteralPath $first -ErrorAction Stop
        $aclDescriptorReadable = -not [string]::IsNullOrWhiteSpace($security.Owner)
    } catch {}

    $aclExplicitDenyEnforced = $null
    $aclProbe = Join-Path $folder 'aclWriteDenyReference.txt'
    [IO.File]::WriteAllText($aclProbe, 'keep')
    $originalAcl = $null
    try {
        $originalAcl = Get-Acl -LiteralPath $aclProbe -ErrorAction Stop
        $candidateAcl = Get-Acl -LiteralPath $aclProbe -ErrorAction Stop
        $who = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        $rule = New-Object Security.AccessControl.FileSystemAccessRule(
            $who, [Security.AccessControl.FileSystemRights]::WriteData,
            [Security.AccessControl.AccessControlType]::Deny)
        $candidateAcl.AddAccessRule($rule)
        Set-Acl -LiteralPath $aclProbe -AclObject $candidateAcl -ErrorAction Stop
        try {
            $stream = [IO.File]::Open($aclProbe, [IO.FileMode]::Open,
                                      [IO.FileAccess]::Write)
            $stream.Dispose()
            $aclExplicitDenyEnforced = $false
        } catch [UnauthorizedAccessException] {
            $aclExplicitDenyEnforced = $true
        }
    } catch {
        $aclExplicitDenyEnforced = $null
    } finally {
        if ($originalAcl) {
            try { Set-Acl -LiteralPath $aclProbe -AclObject $originalAcl -ErrorAction Stop } catch {}
        }
    }

    $junctionReparseVisible = $null
    $symlinkReparseVisible = $null
    $reparseTarget = Join-Path $folder 'reparseTarget'
    [IO.Directory]::CreateDirectory($reparseTarget) | Out-Null
    $junction = Join-Path $folder 'junctionReference'
    try {
        New-Item -ItemType Junction -Path $junction -Target $reparseTarget -ErrorAction Stop | Out-Null
        $junctionReparseVisible = (([IO.File]::GetAttributes($junction) -band
                                  [IO.FileAttributes]::ReparsePoint) -ne 0)
    } catch {} finally {
        if (Test-Path -LiteralPath $junction) {
            try { [IO.Directory]::Delete($junction) } catch {}
        }
    }
    $symlink = Join-Path $folder 'symlinkReference'
    try {
        New-Item -ItemType SymbolicLink -Path $symlink -Target $reparseTarget -ErrorAction Stop | Out-Null
        $symlinkReparseVisible = (([IO.File]::GetAttributes($symlink) -band
                                 [IO.FileAttributes]::ReparsePoint) -ne 0)
    } catch {} finally {
        if (Test-Path -LiteralPath $symlink) {
            try { [IO.Directory]::Delete($symlink) } catch {}
        }
    }

    $result = [ordered]@{
        schema = 'filesystem4-windows-reference-v2'
        captured_utc = [DateTime]::UtcNow.ToString('o')
        os = [Environment]::OSVersion.VersionString
        volume_filesystem = $volume.FileSystem
        temp_drive = $drive
        case_insensitive_exact_name = [bool]$caseInsensitive
        rename_preserves_content = [bool]$renamePreservedBytes
        hardlink_created = [bool]$hardlinkCreated
        hardlink_readable = [bool]$hardlinkContentsSame
        readonly_attribute_visible = [bool]$readOnlyReported
        acl_descriptor_readable = $aclDescriptorReadable
        acl_explicit_deny_enforced = $aclExplicitDenyEnforced
        junction_reparse_visible = $junctionReparseVisible
        symlink_reparse_visible = $symlinkReparseVisible
        short_path = $shortName
        short_path_generated = [bool]($shortName -and
                                       $shortName.ToUpperInvariant().Contains('~'))
        caveat = 'NTFS 8.3 generation can be disabled; no guarantee of a short alias'
    }
} finally {
    try {
        if (Test-Path $target) {
            [IO.File]::SetAttributes($target, [IO.FileAttributes]::Normal)
        }
        [IO.Directory]::Delete($folder, $true)
    } catch {
        Write-Warning "Manual cleanup may be required for $folder : $_"
    }
}
$destination = [IO.Path]::GetFullPath($Output)
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $destination -Encoding UTF8
Write-Host "Windows NTFS reference captured at $destination"
Write-Warning "This is a REFERENCE, not proof of NT144 parity. Compare on Fedora."
