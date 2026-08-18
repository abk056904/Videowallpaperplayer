$p = Get-Process VideoWallpaper -ErrorAction SilentlyContinue
Write-Output ("running: " + ($null -ne $p))
if ($p) { $p | Select-Object Id, StartTime, CPU | Format-Table -AutoSize }
Get-WinEvent -FilterHashtable @{LogName='Application'} -MaxEvents 60 -ErrorAction SilentlyContinue |
    Where-Object { $_.TimeCreated -gt (Get-Date).AddHours(-1) -and $_.Message -match 'VideoWallpaper' } |
    ForEach-Object { $_.TimeCreated.ToString('HH:mm:ss') + ' ' + $_.ProviderName + ' :: ' + (($_.Message -split "`n")[0]) }
