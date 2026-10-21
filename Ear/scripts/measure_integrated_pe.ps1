param(
    [Parameter(Mandatory=$true)][string]$Baseline,
    [Parameter(Mandatory=$true)][string]$WithSurface
)
$limit = 1474560
if (!(Test-Path -LiteralPath $Baseline -PathType Leaf) -or
    !(Test-Path -LiteralPath $WithSurface -PathType Leaf)) {
    Write-Error 'Compila ambas variantes integradas de Bones: sin/con superficie.'
    exit 2
}
$old = (Get-Item -LiteralPath $Baseline).Length
$new = (Get-Item -LiteralPath $WithSurface).Length
$delta = $new - $old
Write-Output "EXE Bones baseline (bytes): $old"
Write-Output "EXE Bones + Ear acoustic surfaces (bytes): $new"
Write-Output "Diferencia integrada (bytes): $delta"
Write-Output "Restante bajo objetivo A: $($limit - $new) bytes"
if ($new -gt $limit) { Write-Error 'El ejecutable integrado supera 1.474.560 bytes'; exit 1 }
