param([Parameter(Mandatory = $true)][string]$Exe)
$maxBytes = 1474560
if (!(Test-Path -LiteralPath $Exe -PathType Leaf)) {
    Write-Error "No existe el EXE: $Exe"
    exit 2
}
$item = Get-Item -LiteralPath $Exe
Write-Host "Archivo: $($item.FullName)"
Write-Host "Tamano real: $($item.Length) bytes / $maxBytes bytes"
if ($item.Length -gt $maxBytes) {
    Write-Error "INCUMPLIMIENTO: el ejecutable excede el maximo del objetivo A"
    exit 1
}
Write-Host "OK: binario bajo el tope individual de 1.44 MB (aun no valida el juego completo)."
exit 0
