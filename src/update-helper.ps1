param([Parameter(Mandatory=$true)][string]$PlanPath)
$ErrorActionPreference = 'Stop'
$plan = Get-Content -LiteralPath $PlanPath -Raw | ConvertFrom-Json
$root = [IO.Path]::GetFullPath($plan.root)
$stage = [IO.Path]::GetFullPath($plan.stage)
$backup = [IO.Path]::GetFullPath($plan.backup)
$parent = Split-Path -Parent $root
if ((Split-Path -Parent $stage) -ne $parent -or (Split-Path -Parent $backup) -ne $parent -or (Split-Path -Leaf $stage) -notmatch '^\.qrazy-update-' -or (Split-Path -Leaf $backup) -notmatch '^\.qrazy-previous-[a-f0-9]+$' -or $root -eq [IO.Path]::GetPathRoot($root)) { throw 'Unsafe update folders' }
function Assert-NoLink([string]$Target) {
  $current = $Target
  while ($current) {
    if (Test-Path -LiteralPath $current) {
      if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Update folders cannot contain links' }
    }
    $next = Split-Path -Parent $current
    if ($next -eq $current) { break }
    $current = $next
  }
}
function Report([string]$Phase, [string]$Message) {
  @{ phase=$Phase; message=$Message; backup=$backup; root=$root; version=$plan.version } | ConvertTo-Json | Set-Content -LiteralPath $plan.result -Encoding UTF8
}
$moved = $false
$installed = $false
$ready = $false
try {
  Assert-NoLink $root
  Assert-NoLink $stage
  if (Test-Path -LiteralPath $backup) { throw 'Recovery folder already exists' }
  if ($plan.helperReady) { [Console]::Out.WriteLine('READY'); [Console]::Out.Flush(); $ready = $true }
  # Wait only for this client. Never terminate it or unrelated parent processes.
  $client = Get-Process -Id $plan.pid -ErrorAction SilentlyContinue
  if ($client) { Wait-Process -Id $plan.pid -Timeout 120 -ErrorAction Stop }
  $launcher = Get-Process -Id $plan.parentPid -ErrorAction SilentlyContinue
  if ($launcher -and $launcher.Path -eq (Join-Path $root 'Qrazy.exe')) { Wait-Process -Id $plan.parentPid -Timeout 30 -ErrorAction Stop }
  if ($plan.workerPid) {
    $worker = Get-Process -Id $plan.workerPid -ErrorAction SilentlyContinue
    if ($worker) { Wait-Process -Id $plan.workerPid -Timeout 30 -ErrorAction Stop }
  }
  foreach ($file in $plan.files) {
    $target = [IO.Path]::GetFullPath((Join-Path $stage $file.path))
    if (!$target.StartsWith($stage + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe staged path' }
    Assert-NoLink $target
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { throw 'Staged update failed verification' }
  }
  Move-Item -LiteralPath $root -Destination $backup
  $moved = $true
  Move-Item -LiteralPath $stage -Destination $root
  $installed = $true
  # Installer metadata is outside the swapped folder and belongs to this user.
  try {
    $uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Qrazy'
    if ($plan.allUsers) { $uninstallKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Qrazy' }
    $registration = Get-ItemProperty -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
    if ($registration.InstallLocation -eq $root -and $plan.version) { Set-ItemProperty -LiteralPath $uninstallKey -Name DisplayVersion -Value $plan.version }
  } catch { # Metadata failure must not undo a verified application update.
  }
  Report 'installed' 'Client updated. The previous application folder is retained for recovery.'
  if (!$plan.noRestart) { Start-Process -FilePath (Join-Path $root 'Qrazy.exe') -WorkingDirectory $root -WindowStyle Normal }
} catch {
  $failure = $_.Exception.Message
  try {
    if ($moved) {
      if ($installed) { Move-Item -LiteralPath $root -Destination $stage; $installed = $false }
      Move-Item -LiteralPath $backup -Destination $root; $moved = $false
    }
    Report 'error' ('Update could not complete: ' + $failure + '. Your profile was not changed.')
    if ($plan.helperReady -and !$ready) { [Console]::Out.WriteLine('ERROR: ' + $failure); [Console]::Out.Flush() }
    if ((!$plan.helperReady -or $ready) -and !$plan.noRestart -and !$installed -and (Test-Path -LiteralPath (Join-Path $root 'Qrazy.exe'))) { Start-Process -FilePath (Join-Path $root 'Qrazy.exe') -WorkingDirectory $root -WindowStyle Normal }
  } catch { Report 'error' ('Recovery needs manual attention. Previous client: ' + $backup + '. ' + $failure) }
}
