# GPU sampler v2 - adapter + per-process + engines + summary
param([int]$TargetPid = 3948, [int]$Samples = 30, [int]$Interval = 2)

Write-Output "GPU metrics for PID $TargetPid | $Samples samples @ ${Interval}s interval"
Write-Output ""

$amdVRAM = @(); $amdShared = @(); $amd3d = @(); $amdcopy = @(); $nv3d = @()

for ($i = 1; $i -le $Samples; $i++) {
    $ts = Get-Date -Format 'HH:mm:ss'
    $engines = Get-Counter "\GPU Engine(pid_${TargetPid}*)\Utilization Percentage" -ErrorAction SilentlyContinue
    $procD   = Get-Counter "\GPU Process Memory(pid_${TargetPid}*)\Dedicated Usage" -ErrorAction SilentlyContinue
    $procS   = Get-Counter "\GPU Process Memory(pid_${TargetPid}*)\Shared Usage" -ErrorAction SilentlyContinue

    $a3d = 0; $acopy = 0; $n3d = 0; $vd = 0; $vs = 0
    if ($engines) {
        foreach ($s in $engines.CounterSamples) {
            if ($s.Path -match 'engtype_3d' -and $s.Path -match 'luid_0x00010767') { $a3d = $s.CookedValue }
            if ($s.Path -match 'engtype_copy' -and $s.Path -match 'luid_0x00010767') { $acopy = $s.CookedValue }
            if ($s.Path -match 'engtype_3d' -and $s.Path -match 'luid_0x0000e8b7') { $n3d = $s.CookedValue }
        }
    }
    if ($procD) { foreach ($s in $procD.CounterSamples) { if ($s.Path -match '0x00010767') { $vd = $s.CookedValue } } }
    if ($procS) { foreach ($s in $procS.CounterSamples) { if ($s.Path -match '0x00010767') { $vs = $s.CookedValue } } }

    $amdVRAM += $vd; $amdShared += $vs; $amd3d += $a3d; $amdcopy += $acopy; $nv3d += $n3d

    Write-Output ('[{0}] AMD3D {1,4:N1}%  COPY {2,4:N1}%  NV3D {3,4:N1}%  VRAM {4,5:N1}MB  Shared {5,5:N1}MB' -f `
        $ts, $a3d, $acopy, $n3d, ($vd/1MB), ($vs/1MB))

    if ($i -lt $Samples) { Start-Sleep -Seconds $Interval }
}

Write-Output ""
Write-Output "=== Summary (AMD iGPU) ==="
Write-Output ("  VRAM dedicated : min {0:N1} MB  avg {1:N1} MB  max {2:N1} MB" -f ($amdVRAM | Measure-Object -Minimum -Average -Maximum | % { $_.Minimum/1MB; $_.Average/1MB; $_.Maximum/1MB }))
Write-Output ("  VRAM shared    : min {0:N1} MB  avg {1:N1} MB  max {2:N1} MB" -f (($amdShared | Measure-Object -Minimum -Average -Maximum).Minimum/1MB, ($amdShared | Measure-Object -Minimum -Average -Maximum).Average/1MB, ($amdShared | Measure-Object -Minimum -Average -Maximum).Maximum/1MB))
Write-Output ("  3D engine      : min {0:N1}%  avg {1:N1}%  max {2:N1}%" -f (($amd3d | Measure-Object -Minimum -Average -Maximum).Minimum, ($amd3d | Measure-Object -Minimum -Average -Maximum).Average, ($amd3d | Measure-Object -Minimum -Average -Maximum).Maximum))
Write-Output ("  COPY engine    : min {0:N1}%  avg {1:N1}%  max {2:N1}%" -f (($amdcopy | Measure-Object -Minimum -Average -Maximum).Minimum, ($amdcopy | Measure-Object -Minimum -Average -Maximum).Average, ($amdcopy | Measure-Object -Minimum -Average -Maximum).Maximum))
Write-Output ("  NV3D (other)   : min {0:N1}%  avg {1:N1}%  max {2:N1}%" -f (($nv3d | Measure-Object -Minimum -Average -Maximum).Minimum, ($nv3d | Measure-Object -Minimum -Average -Maximum).Average, ($nv3d | Measure-Object -Minimum -Average -Maximum).Maximum))
