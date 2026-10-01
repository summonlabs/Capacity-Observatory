<#
.SYNOPSIS
  Fresh-clone validation of the released state: clones the remote repository into
  a temporary directory, builds, tests, installs, and runs the out-of-tree
  consumer, then removes only the temporary clone it created.
#>
[CmdletBinding()]
param(
  [string] $Remote = 'origin',
  [string] $Ref = 'main',
  [string] $WorkDir = (Join-Path $env:TEMP 'capacity-observatory-fresh-clone'),
  [switch] $KeepWorkDir
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0

function Write-Step([string] $Text) {
  Write-Output ''
  Write-Output "=== $Text ==="
}

function Assert-True([bool] $Condition, [string] $What) {
  if ($Condition) {
    Write-Output "  [ ok ] $What"
  } else {
    Write-Output "  [FAIL] $What"
    $script:Failures++
  }
}

# Windows PowerShell turns a native command's stderr into a terminating error
# when the preference is Stop, and git reports ordinary progress on stderr.
# The exit code is the contract, so output is captured instead.
function Invoke-Git([string[]] $Arguments) {
  $savedPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'
    $output = & git @Arguments 2>&1
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $savedPreference
  }
  return [pscustomobject]@{ Output = ($output | Out-String).Trim(); ExitCode = $code }
}

$remoteResult = Invoke-Git @('remote', 'get-url', $Remote)
$remoteUrl = $remoteResult.Output
if ($remoteResult.ExitCode -ne 0 -or -not $remoteUrl) { throw "remote '$Remote' is not configured" }

$work = [System.IO.Path]::GetFullPath($WorkDir)
$clone = Join-Path $work 'Capacity-Observatory'

if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

Write-Step "Cloning $remoteUrl ($Ref) into $clone"
$cloneResult = Invoke-Git @('clone', '--branch', $Ref, '--single-branch', $remoteUrl, $clone)
Write-Output $cloneResult.Output
if ($cloneResult.ExitCode -ne 0) { throw 'clone failed' }

Push-Location $clone
try {
  $head = (Invoke-Git @('rev-parse', 'HEAD')).Output
  $tagCommit = (Invoke-Git @('rev-list', '-n', '1', 'v1.0.0')).Output
  Write-Output "clone HEAD: $head"
  Write-Output "v1.0.0 dereferences to: $tagCommit"
  Assert-True ($head -eq $tagCommit) 'fresh clone HEAD equals the released tag commit'

  Write-Step 'Installed artifact validation from the fresh clone'
  & (Join-Path $clone 'scripts\validate-install.ps1') -SourceDir $clone -WorkDir (Join-Path $work 'install-validation') | Write-Output
  Assert-True ($LASTEXITCODE -eq 0) 'install/downstream validation passed from the fresh clone'
} finally {
  Pop-Location
}

Write-Step 'Result'
if ($script:Failures -eq 0) {
  Write-Output 'FRESH CLONE VALIDATION: PASS'
} else {
  Write-Output "FRESH CLONE VALIDATION: FAIL ($script:Failures check(s))"
}

if (-not $KeepWorkDir) {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  Write-Output "removed fresh clone work directory $work"
}

exit $(if ($script:Failures -eq 0) { 0 } else { 1 })
