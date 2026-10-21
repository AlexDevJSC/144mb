# Install and uninstall the actual setup in CI; compare installed PE SHA256.
# Runs only on Windows GitHub Actions (uses RUNNER_TEMP). No GUI gameplay claim.
param(
  [Parameter(Mandatory=$true)][string]$Installer,
  [Parameter(Mandatory=$true)][string]$GameExe
)
$ErrorActionPreference="Stop"
$installerPath=(Resolve-Path $Installer).Path
$gamePath=(Resolve-Path $GameExe).Path
$expected=(Get-FileHash -Algorithm SHA256 -LiteralPath $gamePath).Hash
$base=if($env:RUNNER_TEMP){$env:RUNNER_TEMP}else{[IO.Path]::GetTempPath()}
$target=Join-Path $base ("P144_installer_smoke_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $target | Out-Null
try {
  Write-Host "Installing silently to $target"
  $proc=Start-Process -FilePath $installerPath -ArgumentList @(
    '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/NOICONS',"/DIR=$target"
  ) -PassThru -Wait
  if($proc.ExitCode -ne 0){throw "Installer exit code $($proc.ExitCode)"}
  $installed=Join-Path $target 'Proyecto144.exe'
  $version=Join-Path $target 'VERSION.txt'
  if(!(Test-Path -LiteralPath $installed -PathType Leaf)) {
    throw "Installer did not create the game executable"
  }
  if(!(Test-Path -LiteralPath $version -PathType Leaf)) {
    throw "Installer did not create VERSION.txt"
  }
  $actual=(Get-FileHash -Algorithm SHA256 -LiteralPath $installed).Hash
  if($actual -ne $expected){throw "Installed game SHA256 differs from compiled executable"}
  $uninstaller=Join-Path $target 'unins000.exe'
  if(!(Test-Path -LiteralPath $uninstaller -PathType Leaf)){
    throw "No uninstaller found"
  }
  Write-Host "PASS install and content SHA256: $actual"
  $proc=Start-Process -FilePath $uninstaller -ArgumentList @(
    '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART'
  ) -PassThru -Wait
  if($proc.ExitCode -ne 0){throw "Uninstaller exit code $($proc.ExitCode)"}
  Start-Sleep -Seconds 1
  if(Test-Path -LiteralPath $installed){throw "Uninstall left the game executable behind"}
  Write-Host "PASS silent uninstall"
} finally {
  if(Test-Path -LiteralPath $target){
    Remove-Item -LiteralPath $target -Force -Recurse -ErrorAction SilentlyContinue
  }
}
