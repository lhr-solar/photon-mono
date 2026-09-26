Get-CimInstance Win32_Process |
  Where-Object { $_.Name -match 'photon-(server|backend|index)' } |
  Select-Object Name, ProcessId, ExecutablePath, CommandLine |
  Format-List
