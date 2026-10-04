param([Parameter(Mandatory=$true)][string]$PlanPath, [Parameter(Mandatory=$true)][string]$Executable)
# This broker remains unelevated. Only the installed application is elevated;
# it verifies the signed manifest and makes its own protected helper and plan.
$ErrorActionPreference = 'Stop'
try {
  $request = Get-Content -LiteralPath $PlanPath -Raw | ConvertFrom-Json
  if ($request.jobToken -notmatch '^[a-f0-9]{24}$') { throw 'Invalid update job' }
  $root = [IO.Path]::GetFullPath($request.root)
  $job = Join-Path (Split-Path -Parent $root) ('.qrazy-job-' + $request.jobToken)
  if (Test-Path -LiteralPath $job) { throw 'The update job already exists. Please try again.' }
  $argument = '"--qrazy-admin-update=' + $PlanPath.Replace('"','') + '"'
  try {
    $worker = Start-Process -FilePath $Executable -ArgumentList $argument -Verb RunAs -WindowStyle Hidden -PassThru
    $worker.WaitForExit()
  } catch { throw 'Administrator approval was cancelled or unavailable. Your client has not been closed.' }
  if ($worker.ExitCode -ne 0) { throw 'The update could not be prepared. Check for updates and try again.' }
  [Console]::Out.WriteLine('READY')
  [Console]::Out.Flush()
  $resultPath = Join-Path $job 'result.json'
  $deadline = [DateTime]::UtcNow.AddMinutes(5)
  while (!(Test-Path -LiteralPath $resultPath)) {
    if ([DateTime]::UtcNow -gt $deadline) { throw 'Timed out waiting for the update to finish' }
    Start-Sleep -Milliseconds 500
  }
  # Profile writes and restarting the game use the original user's permissions.
  Copy-Item -LiteralPath $resultPath -Destination (Join-Path (Split-Path -Parent $PlanPath) 'result.json') -Force
  Start-Process -FilePath (Join-Path $root 'Qrazy.exe') -WorkingDirectory $root -WindowStyle Normal
} catch {
  [Console]::Out.WriteLine('ERROR: ' + $_.Exception.Message)
  exit 1
}
