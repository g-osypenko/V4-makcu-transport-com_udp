# smooth_sweep.ps1 - серия pad_bench_com.exe: текстура инжекта при разных режимах сглаживания платы (ставит транспорт кодом,
# 0x1D BEGIN / WRITE / APPLY без SAVE; close() возвращает прежнее). Одна и та же запись руки, один и тот же участок.
# Окно наблюдения pad_probe_view --keepalive (COM не трогает) открывается на всю серию и закрывается само.
#
#   .\smooth_sweep.ps1 -Tag s4133_20261119 [-Only off2,fixed1] [-Live data\rec2] [-From 17.5] [-Len 7]
#
# Варианты (имя -> ключи бенчмарка):
#   auto2    Auto, буфер 8, команды раз в 2 мс, рендерер под Auto (опережение 1.0)
#   off2     Off, команды раз в 2 мс, рендерер --mid2 (АЦП на каждой команде, без середины)
#   fixed1   Fixed, буфер 1 мс, раз в 2 мс, --mid2
#   fixed2   Fixed, буфер 2 мс, раз в 2 мс, --mid2
#   fixed4   Fixed, буфер 4 мс, раз в 2 мс, --mid2
#   off1     Off, раз в 1 мс, presetOff (эталон 3.49 на 4122 по кабелю); 1000 команд/с - последним, на 4133 рвёт за ~2 мин
param(
    [string]$Tag = "sweep",
    [string[]]$Only = @(),
    [string]$Live = "..\pad_probes\data\rec2",
    [double]$From = 17.5,
    [double]$Len = 7,
    [int]$Poll = 0,       # мкс, период опроса 0x54 (--poll); 0 - по умолчанию транспорта (2000 = 500/с)
    [int]$Repeat = 1
)
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$ref = Join-Path $here "..\perfect-transport-gamepad-com"
$data = Join-Path $here "data"

$variants = [ordered]@{
    "auto2"  = @("--set-smoothing", "auto", "--buffer", "8", "--period", "2000")
    "off2"   = @("--set-smoothing", "off", "--period", "2000", "--kappa", "0", "--mid2")
    "fixed1" = @("--set-smoothing", "fixed", "--buffer", "1", "--period", "2000", "--kappa", "0", "--mid2")
    "fixed2" = @("--set-smoothing", "fixed", "--buffer", "2", "--period", "2000", "--kappa", "0", "--mid2")
    "fixed4" = @("--set-smoothing", "fixed", "--buffer", "4", "--period", "2000", "--kappa", "0", "--mid2")
    "off1"   = @("--set-smoothing", "off", "--off")
}
$base = @($variants.Keys | Where-Object { $Only.Count -eq 0 -or $Only -contains $_ })
$names = @(); for ($r = 1; $r -le $Repeat; $r++) { foreach ($b in $base) { $names += $(if ($Repeat -gt 1) { "$b#$r" } else { $b }) } }
$dur = [int](45 * $names.Count + 40)

[console]::beep(1000, 180)
$view = Start-Process -FilePath (Join-Path $here "pad_probe_view.exe") -WorkingDirectory $here -PassThru `
    -ArgumentList @("--rate", "0", "--keepalive", "--dur", "$dur", "--log", "data\view_smooth_$Tag.log")
Start-Sleep -Seconds 3

$summary = @()
Push-Location $ref
try {
    foreach ($n in $names) {
        $v = $n.Split("#")[0]; $f = $n.Replace("#", "_r")
        $log = "pad_smooth_${Tag}_$f.log"
        $args2 = @("--live", $Live, "--from", "$From", "--len", "$Len", "--out", "..\pad_probes\data\bench_smooth_${Tag}_$f") + $variants[$v]
        if ($Poll -gt 0) { $args2 += @("--poll", "$Poll") }
        Write-Host "=== $n : pad_bench_com.exe $($args2 -join ' ')"
        & .\pad_bench_com.exe @args2 | Out-File -Encoding utf8 $log
        $txt = Get-Content $log -Encoding utf8
        $sum = ($txt | Select-String "СУММА W1" | Select-Object -Last 1).Line
        $lag = $txt | Select-String "пакетов Windows .* за .* с; отставание" | ForEach-Object { $_.Line.Trim() }
        $lat = ($txt | Select-String "среднее по" | Select-Object -Last 1).Line
        $phys = ($txt | Select-String "запросов .*/с, новых отчётов" | Select-Object -First 1).Line
        $fin = ($txt | Select-String "геймпад .*интервал" | Select-Object -Last 1).Line
        $err = $txt | Select-String "^\[-\]|не видит|НЕТ" | ForEach-Object { $_.Line }
        Write-Host "  $phys"
        Write-Host "  $lat"
        $lag | ForEach-Object { Write-Host "  $_" }
        Write-Host "  $sum"
        Write-Host "  $fin"
        $warn = $txt | Select-String "НЕ возвращено" | ForEach-Object { $_.Line }
        if ($warn) { Write-Host "!! $warn - серия остановлена"; break }
        if ($err) { $err | ForEach-Object { Write-Host "  !! $_" } }
        $summary += "$n | $sum"
        if ($txt -match "плата не видит геймпад") { Write-Host "!! геймпада нет - серия остановлена"; break }
        Start-Sleep -Seconds 2
    }
} finally {
    Pop-Location
}
Write-Host "=== итог (столбцы W1: кривая / кривая+опереж. / рендерер / запись руки)"
$summary | ForEach-Object { Write-Host $_ }
& (Join-Path $here "pad_probe_info.exe") --port COM5 --poll 1 | Select-String "FIRMWARE|kinds|интерполяция|буфер|sequence менялся" | ForEach-Object { Write-Host $_.Line }
