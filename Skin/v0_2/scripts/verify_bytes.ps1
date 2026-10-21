param([Parameter(Mandatory=$true)][string]$Path)
if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {Write-Error 'No existe el EXE'; exit 2}
$bytes=(Get-Item -LiteralPath $Path).Length
Write-Host "Bytes del binario de prueba: $bytes / 1474560"
if($bytes -gt 1474560){Write-Error 'Supera el limite del juego final'; exit 1}
Write-Host 'PASS de tamano solo para el binario de prueba (NO certifica el EXE combinado).'
