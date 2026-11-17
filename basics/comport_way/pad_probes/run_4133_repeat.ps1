# run_4133_repeat.ps1 - повтор repro_poll54 --rate 2000 --dur 300 N раз подряд с одним окном pad_probe_view --keepalive.
# Прошивка 4133 (персональная сборка MAKCU под R-DDEJ38). На первом отказе останавливается (код 3), чтобы владелец снял R-код.
param([int]$Runs = 2, [int]$First = 2)
Set-Location $PSScriptRoot
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$last = $First + $Runs - 1
$dur = 310 * $Runs + 30
[console]::beep(1000,180)
$null = Start-Process -FilePath ".\pad_probe_view.exe" -ArgumentList "--rate 0 --keepalive --dur $dur --log data\view_4133_$First-$last.log" -PassThru
Start-Sleep -Seconds 3
for ($i = $First; $i -le $last; $i++) {
    $out = "data\repro_poll54_4133_$i.out"
    & .\repro_poll54.exe --rate 2000 --dur 300 --log "data\repro_poll54_4133_$i.log" 2>&1 | Out-File -Encoding utf8 $out
    $code = $LASTEXITCODE
    "run $i exit $code : " + (Get-Content $out | Select-Object -Last 1)
    if ($code -ne 0) { [console]::beep(1000,180); Start-Sleep -Milliseconds 120; [console]::beep(1000,180); break }
    Start-Sleep -Seconds 2
}
