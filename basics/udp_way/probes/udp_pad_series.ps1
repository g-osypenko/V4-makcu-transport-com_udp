# udp_pad_series.ps1 - серия udp_pad_inject.exe: режим сглаживания x темп команд по UDP. Окно pad_probe_view --keepalive.
# Порядок - от безопасного к опасному: все 250/с, потом 500/с, потом 1000/с. Отказ платы (код 3) останавливает серию.
#
#   .\udp_pad_series.ps1 -Tag T [-Only "auto-4000","off-2000"] [-NoTexture]
#
# Имя варианта: <сглаживание>[буфер]-<период мкс>: auto, off, fixed1 (буфер 1), fixed4 (буфер 4).
param(
    [string]$Tag = "series",
    [string[]]$Only = @(),
    [switch]$NoTexture
)
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$pad = Join-Path $here "..\..\comport_way\pad_probes"

$all = @("auto-4000", "off-4000", "fixed1-4000", "fixed4-4000",
         "auto-2000", "off-2000", "fixed1-2000",
         "auto-1000", "off-1000", "fixed1-1000")
$names = @($all | Where-Object { $Only.Count -eq 0 -or $Only -contains $_ })
$dur = [int](($(if ($NoTexture) { 15 } else { 40 })) * $names.Count + 40)

[console]::beep(1000, 180)
$view = Start-Process -FilePath (Join-Path $pad "pad_probe_view.exe") -WorkingDirectory $pad -PassThru `
    -ArgumentList @("--rate", "0", "--keepalive", "--dur", "$dur", "--log", "data\view_udp_$Tag.log")
Start-Sleep -Seconds 3

$summary = @()
Push-Location $here
try {
    foreach ($n in $names) {
        $m, $p = $n.Split("-")
        $a = @("--period", $p)
        switch -regex ($m) {
            "^auto$"   { $a += @("--smoothing", "auto", "--buffer", "8") }
            "^off$"    { $a += @("--smoothing", "off") }
            "^fixed(\d+)$" { $a += @("--smoothing", "fixed", "--buffer", $Matches[1]) }
        }
        if ($NoTexture) { $a += "--no-texture" }
        $a += @("--out", "data\udp_${Tag}_$n")
        $log = "udp_pad_${Tag}_$n.log"
        Write-Host "=== $n : udp_pad_inject.exe $($a -join ' ')"
        & .\udp_pad_inject.exe @a | Out-File -Encoding utf8 $log
        $code = $LASTEXITCODE
        $txt = Get-Content $log -Encoding utf8
        $txt | Select-String "среднее по|пакетов Windows|СУММА W1|!!|НЕ ВЕРНУЛОСЬ|\[-\]|сглаживание:" | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
        $sum = ($txt | Select-String "СУММА W1" | Select-Object -Last 1).Line
        $circ = ($txt | Select-String "из них с новым значением" | Select-Object -Last 1).Line
        $summary += "$n | код $code | $(if ($sum) { $sum.Trim() } else { '-' }) | $(if ($circ) { $circ.Trim() } else { '-' })"
        if ($code -eq 3) { Write-Host "!! плата или геймпад не вернулись - серия остановлена"; break }
        Start-Sleep -Seconds 2
    }
} finally {
    Pop-Location
}
Write-Host "=== итог"
$summary | ForEach-Object { Write-Host $_ }
