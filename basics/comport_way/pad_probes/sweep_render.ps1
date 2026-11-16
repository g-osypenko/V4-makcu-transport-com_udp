# sweep_render.ps1 - sweep one renderer parameter (board smoothing Off) in the offline simulator pad_render_sim.exe.
# ASCII only on purpose: Windows PowerShell 5.1 reads a .ps1 without BOM as ANSI.
#
#   .\sweep_render.ps1 -Name tremor_rms_per_speed -Values 0.2,0.4,0.6 [-Base "raw_lsb=70","adc_hysteresis_lsb=0.3"] [-Seeds 8]
#
# For every value: W1 sum to the live hand on three fragments of data\rec2
#   aim 17.5..24.5 s (tuned on it), aim2 15.5..22.5 s, circle 6..13 s (check), their mean,
#   and on the aim fragment: hold_rate, delta_ac_lag1, tracking error (units).
# Live hand on aim: hold_rate 0.658, delta_ac_lag1 0.686.
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][double[]]$Values,
    [string[]]$Base = @(),
    [int]$Seeds = 8
)
$frags = @(@('aim', 17.5, 7), @('aim2', 15.5, 7), @('circle', 6, 7))
$inv = [System.Globalization.CultureInfo]::InvariantCulture
"{0,-24} {1,7} {2,7} {3,7} {4,7} | {5,6} {6,6} {7,6}" -f $Name, 'aim', 'aim2', 'circle', 'mean', 'hold', 'lag1', 'err'
foreach ($v in $Values) {
    $sums = @()
    $hold = ''; $lag1 = ''; $err = ''
    foreach ($f in $frags) {
        $a = @('data\rec2', $f[1].ToString($inv), $f[2].ToString($inv), '--seeds', $Seeds) + $Base + @("$Name=" + $v.ToString($inv))
        $line = (& .\pad_render_sim.exe @a | Where-Object { $_ -like 'SUM *' })
        if ($line -match 'render=([0-9.]+) hold=([0-9.]+) lag1=([-0-9.]+) lagms=\S+ err=([0-9.]+)') {
            $sums += [double]::Parse($Matches[1], $inv)
            if ($f[0] -eq 'aim') { $hold = $Matches[2]; $lag1 = $Matches[3]; $err = $Matches[4] }
        } else { $sums += [double]::NaN }
    }
    "{0,-24} {1,7:F3} {2,7:F3} {3,7:F3} {4,7:F3} | {5,6} {6,6} {7,6}" -f $v.ToString($inv), $sums[0], $sums[1], $sums[2], (($sums | Measure-Object -Average).Average), $hold, $lag1, $err
}
