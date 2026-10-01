<#
.SYNOPSIS
  Builds Capacity Observatory, installs it to a clean prefix, and proves that an
  independent out-of-tree project can consume the installed package with
  find_package(CapacityObservatory CONFIG).

.DESCRIPTION
  Performs real work only: a real Release build, a real install into an empty
  prefix, a real copy of the example consumer outside the source tree, and a real
  configure/build/run of that consumer against the installed package.
  Nothing outside the paths it creates is modified or deleted.
#>
[CmdletBinding()]
param(
  [string] $Configuration = 'Release',
  [string] $SourceDir = (Split-Path -Parent $PSScriptRoot),
  [string] $WorkDir = (Join-Path $env:TEMP 'capacity-observatory-install-validation'),
  [switch] $Asan,
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

function Invoke-Native([string] $FilePath, [string[]] $Arguments, [string] $WorkingDirectory) {
  # Windows PowerShell turns a native command's stderr into a terminating error
  # when the preference is Stop, which would abort on a harmless CMake notice.
  # The exit code is the contract here, so stderr is captured instead.
  $previous = Get-Location
  $savedPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'
    if ($WorkingDirectory) { Set-Location -LiteralPath $WorkingDirectory }
    $output = & $FilePath @Arguments 2>&1
    $code = $LASTEXITCODE
  } finally {
    Set-Location $previous
    $ErrorActionPreference = $savedPreference
  }
  return [pscustomobject]@{ Output = ($output | Out-String); ExitCode = $code }
}

$vcvarsCandidates = @(
  'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
  'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
  'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
  'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
)
$vcvars = $vcvarsCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $vcvars) { throw 'no Visual Studio x64 developer environment was found' }

$source = (Resolve-Path -LiteralPath $SourceDir).Path
$work = [System.IO.Path]::GetFullPath($WorkDir)
$buildDir = Join-Path $work 'build'
$prefix = Join-Path $work 'prefix'
$consumerSource = Join-Path $source 'examples\consumer'
$consumerDir = Join-Path $work 'consumer'

if (Test-Path -LiteralPath $work) {
  Remove-Item -LiteralPath $work -Recurse -Force
}
New-Item -ItemType Directory -Path $work | Out-Null

Write-Step "Source: $source"
Write-Step "Work directory: $work"

$asanArgs = @()
if ($Asan) { $asanArgs = @('-DCO_ENABLE_ASAN=ON') }

Write-Step 'Configure Release'
$result = Invoke-Native 'cmd.exe' @('/c', "call `"$vcvars`" >nul 2>&1 && cmake -S `"$source`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DCMAKE_INSTALL_PREFIX=`"$prefix`" -DCO_BUILD_TESTS=ON -DCO_BUILD_BENCHMARKS=ON @asanArgs") $null
Assert-True ($result.ExitCode -eq 0) 'configure succeeded'
if ($result.ExitCode -ne 0) { Write-Output $result.Output; exit 1 }

Write-Step 'Build'
$result = Invoke-Native 'cmd.exe' @('/c', "call `"$vcvars`" >nul 2>&1 && cmake --build `"$buildDir`" --config $Configuration") $null
# Compiler diagnostics only: CMake's own capability probes are not first-party warnings.
$compilerWarnings = @($result.Output -split "`n" | Select-String -Pattern 'warning C[0-9]|warning LNK[0-9]')
Assert-True ($result.ExitCode -eq 0) 'build succeeded'
Assert-True ($compilerWarnings.Count -eq 0) 'build produced zero compiler warnings'
if ($compilerWarnings.Count -ne 0) { $compilerWarnings | Select-Object -First 10 | Write-Output }
if ($result.ExitCode -ne 0) { Write-Output $result.Output; exit 1 }

Write-Step 'Test'
$ctest = Join-Path $buildDir 'tests'
$result = Invoke-Native 'ctest.exe' @('--test-dir', $buildDir, '--output-on-failure', '-C', $Configuration) $null
Assert-True ($result.ExitCode -eq 0) 'ctest passed'
$summary = ($result.Output -split "`n" | Select-String -Pattern 'tests passed|Total Test time' | Out-String).Trim()
Write-Output $summary
if ($result.ExitCode -ne 0) { Write-Output $result.Output }

Write-Step 'Install to a clean prefix'
$result = Invoke-Native 'cmake.exe' @('--install', $buildDir, '--config', $Configuration) $null
Assert-True ($result.ExitCode -eq 0) 'install succeeded'
if ($result.ExitCode -ne 0) { Write-Output $result.Output; exit 1 }

$configFile = Join-Path $prefix 'lib\cmake\CapacityObservatory\CapacityObservatoryConfig.cmake'
$targetsFile = Join-Path $prefix 'lib\cmake\CapacityObservatory\CapacityObservatoryTargets.cmake'
$headerFile = Join-Path $prefix 'include\capacity_observatory\ledger.hpp'
Assert-True (Test-Path -LiteralPath $configFile) 'package config file installed'
Assert-True (Test-Path -LiteralPath $targetsFile) 'exported targets file installed'
Assert-True (Test-Path -LiteralPath $headerFile) 'public headers installed'
Assert-True (Test-Path -LiteralPath (Join-Path $prefix 'bin\capacity-observatory.exe')) 'CLI installed'

Write-Step 'Out-of-tree consumer via find_package'
Copy-Item -LiteralPath $consumerSource -Destination $consumerDir -Recurse -Force
$result = Invoke-Native 'cmd.exe' @('/c', "call `"$vcvars`" >nul 2>&1 && cmake -S `"$consumerDir`" -B `"$consumerDir\build`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DCapacityObservatory_DIR=`"$(Split-Path -Parent $configFile)`"") $null
Assert-True ($result.ExitCode -eq 0) 'consumer configure found the installed package'
if ($result.ExitCode -ne 0) { Write-Output $result.Output; exit 1 }

$result = Invoke-Native 'cmd.exe' @('/c', "call `"$vcvars`" >nul 2>&1 && cmake --build `"$consumerDir\build`"") $null
Assert-True ($result.ExitCode -eq 0) 'consumer built against the installed package'
if ($result.ExitCode -ne 0) { Write-Output $result.Output; exit 1 }

$consumerExe = Join-Path $consumerDir 'build\capacity-consumer.exe'
$result = Invoke-Native $consumerExe @() $null
Assert-True ($result.ExitCode -eq 0) 'consumer ran successfully against the installed runtime'
Write-Output ($result.Output.Trim())

Write-Step 'Installed CLI smoke test'
$cli = Join-Path $prefix 'bin\capacity-observatory.exe'
$result = Invoke-Native $cli @('version') $null
Assert-True ($result.ExitCode -eq 0) 'installed CLI reports its version'
Write-Output ($result.Output.Trim())

Write-Step 'Installed runtime end-to-end proof'
# A complete, self consistent evidence set: every identity class is declared and
# every closure residual is exactly zero, so the installed runtime must report a
# complete ledger and exit 0.
$storeDir = Join-Path $work 'runtime-store'
$evidencePath = Join-Path $work 'evidence.jsonl'
$scope = 'site=dc1/hall=h1'
$records = @(
  '{"mutation":"v-1","authority":"policy","scope":"' + $scope + '","dimension":"power","assertion":"nameplate","unit":"kW","amount":2000,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-2","authority":"policy","scope":"' + $scope + '","dimension":"power","assertion":"governed","unit":"kW","amount":1800,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-3","authority":"policy","scope":"' + $scope + '","dimension":"power","assertion":"excluded-policy","unit":"kW","amount":100,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-4","authority":"policy","scope":"' + $scope + '","dimension":"power","assertion":"excluded-maintenance","unit":"kW","amount":0,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-5","authority":"policy","scope":"' + $scope + '","dimension":"power","assertion":"excluded-failure","unit":"kW","amount":0,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-6","authority":"policy","scope":"' + $scope + '","dimension":"power","assertion":"operational-reserve","unit":"kW","amount":100,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-7","authority":"dfi","scope":"' + $scope + '","dimension":"power","assertion":"installed","unit":"kW","amount":1500,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-8","authority":"dfi","scope":"' + $scope + '","dimension":"power","assertion":"available","unit":"kW","amount":300,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-9","authority":"dfi","scope":"' + $scope + '","dimension":"power","assertion":"stranded","unit":"kW","amount":0,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-10","authority":"dccp","scope":"' + $scope + '","dimension":"power","assertion":"committed","unit":"kW","amount":900,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-11","authority":"asi","scope":"' + $scope + '","dimension":"power","assertion":"reserved","unit":"kW","amount":300,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
  '{"mutation":"v-12","authority":"arbiter","scope":"' + $scope + '","dimension":"power","assertion":"disputed","unit":"kW","amount":0,"generation":1,"epoch":1,"revision":0,"observed_at":1700000000000000000}'
)
# Written explicitly without a byte order mark and with one record per line, so
# the file is exactly the JSONL the CLI documents. The CLI tolerates a mark as
# well, but a validation input should not depend on that tolerance.
[System.IO.File]::WriteAllLines($evidencePath, $records, (New-Object System.Text.UTF8Encoding($false)))

$result = Invoke-Native $cli @('ingest', '--store', $storeDir, '--evidence', $evidencePath, '--now', '1700000000000000000') $null
Assert-True ($result.ExitCode -eq 0) 'installed CLI ingested a complete evidence set with exit 0'
if ($result.ExitCode -ne 0) { Write-Output $result.Output }

$result = Invoke-Native $cli @('verify', '--store', $storeDir) $null
Assert-True ($result.ExitCode -eq 0) 'installed CLI verified the store it wrote'

# A second process must see exactly the same derived numbers: durability across
# process boundaries, not just within one process.
$first = Invoke-Native $cli @('explain', '--store', $storeDir, '--scope', $scope, '--dimension', 'power', '--json', '--now', '1700000000000000000') $null
Assert-True ($first.ExitCode -eq 0) 'installed CLI explained the ledger with exit 0 (complete, zero residuals)'
$second = Invoke-Native $cli @('explain', '--store', $storeDir, '--scope', $scope, '--dimension', 'power', '--json', '--now', '1700000000000000000') $null
Assert-True ($second.ExitCode -eq 0) 'a second process reproduces the explanation'
Assert-True ($first.Output -ceq $second.Output) 'two separate processes produced byte identical output'
# --json emits canonical compact JSON, so the patterns tolerate optional whitespace.
Assert-True ($first.Output -match '"free_usable"\s*:\s*300000000') 'derived free capacity is exact (300 kW)'
Assert-True ($first.Output -match '"state"\s*:\s*"complete"') 'the ledger line is complete'
Assert-True ($first.Output -match '"residual"\s*:\s*0') 'the closure residuals are exactly zero'

Write-Output (($first.Output -split "`n" | Select-Object -First 1) -join '')

Write-Step 'Result'
if ($script:Failures -eq 0) {
  Write-Output 'INSTALL/DOWNSTREAM VALIDATION: PASS'
} else {
  Write-Output "INSTALL/DOWNSTREAM VALIDATION: FAIL ($script:Failures check(s))"
}

if (-not $KeepWorkDir) {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  Write-Output "removed validation work directory $work"
}

exit $(if ($script:Failures -eq 0) { 0 } else { 1 })
