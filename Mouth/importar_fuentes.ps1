# Ejecutar desde la raiz del repositorio clonado
param(
  [string]$V01 = "MOUTH_P2P_para_Bones_v0_1.zip",
  [string]$V02 = "MOUTH_P2P_Bones_Visual_v0_2.zip"
)
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
foreach ($zip in @($V01, $V02)) {
  if (!(Test-Path -LiteralPath $zip -PathType Leaf)) {
    throw "Archivo ausente: $zip"
  }
}
$destinations = @(
  @{Zip=$V01; Target='Mouth/history/v0_1'},
  @{Zip=$V02; Target='Mouth/current'}
)
foreach ($entry in $destinations) {
  $target = Join-Path $root $entry.Target
  if (Test-Path $target) { throw "Destino existente; no se sobrescribira: $target" }
  New-Item -ItemType Directory -Force -Path $target | Out-Null
  Expand-Archive -LiteralPath $entry.Zip -DestinationPath $target
}
Write-Host "Revisa el contenido y el estado de Git. No se ha publicado automaticamente."
git status --short
Write-Host "Para publicar despues de revisar: git add Mouth; git commit -m 'feat(mouth): import P2P v0.1 and v0.2'; git push"
