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

$remoteUrl = (& git remote get-url $Remote).Trim()
if ($LASTEXITCODE -ne 0 -or -not $remoteUrl) { throw "remote '$Remote' is not configured" }

$work = [System.IO.Path]::GetFullPath($WorkDir)
$clone = Join-Path $work 'Capacity-Observatory'

if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

Write-Step "Cloning $remoteUrl ($Ref) into $clone"
& git clone --branch $Ref --single-branch $remoteUrl $clone 2>&1 | Write-Output
if ($LASTEXITCODE -ne 0) { throw 'clone failed' }

Push-Location $clone
try {
  $head = (& git rev-parse HEAD).Trim()
  $tagCommit = (& git rev-list -n 1 v1.0.0 2>&1).Trim()
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
