<#
install.ps1 - from a fresh Windows 10/11 PC to a built M-Strata in one step (run it through install.bat).

  .\install.bat                                        build into ..\M-Strata-app: email alerts, web search, the window and the TUI
  .\install.bat -Out D:\Apps\M-Strata -With email,search,tui      (no window: only the terminal one)
  .\install.bat -Plan                                  only say what it would install, fetch and run
  .\install.bat -Yes                                   do not ask before installing
  .\install.bat -Debugging                             a server that logs verbosely, to find out why a model does not
                                                       start or calibrate (the plan says what to take care of)

What it does, each step only when needed:
  1. finds what the build needs: Git, Python 3.10+, Visual Studio 2022 Build Tools WITH the C++ workload, the CUDA
     Toolkit 13.0, and (for a GPU to run on) the NVIDIA driver;
  2. lists what is missing and asks, then installs it with winget (Microsoft's package manager, part of Windows): each
     installer asks Windows for administrator rights itself. Build Tools installed without C++ get the C++ workload
     added. The NVIDIA driver is not installed here (nvidia.com or Windows Update);
  3. when this script is not inside the repository, clones it (with its submodules) beside the script;
  4. puts CMake and Ninja in the repository's own Python environment (.venv-dev, which make.bat uses);
  5. fetches the pinned llama.cpp commit the engine builds ggml from (.toolchains\llama.cpp; an existing ..\llama.cpp at
     that commit is used as it is), and for -With ...window the WebView2 SDK (its SHA-256 checked);
  6. runs make.bat into a new folder and says how to start it.
Downloads: winget's packages (Microsoft's catalog, hash-checked by winget), Python packages from PyPI, the repositories
from GitHub, the WebView2 SDK from nuget.org. Nothing else is downloaded, and nothing is sent anywhere.
#>
param(
  [string]$Out = "",
  [string]$With = "email,search,window,tui",
  [string]$GgmlDir = "",
  [switch]$Debugging,
  [switch]$Plan,
  [switch]$Yes
)
# Continue, not Stop: in Windows PowerShell 5.1 a native tool's stderr under Stop is a terminating error. Every
# native call's exit code is checked, and the cmdlets that matter stop on their own (-ErrorAction Stop).
$ErrorActionPreference = 'Continue'

# --- what the build pins (keep in step with engine/CMakeLists.txt, README.md and install.sh: tools/secure/test_make.py)
$LlamaPin = '3cf03257f219afbe7334045ff7c6a06ac68c627d'
$LlamaUrl = 'https://github.com/ggml-org/llama.cpp.git'
$RepoUrl = 'https://github.com/shift-f10/m-strata.git'
$CudaVersion = '13.0'
$WebView2Version = '1.0.4314.35'
$WebView2Sha256 = '90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4'
$WebView2Url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$WebView2Version/microsoft.web.webview2.$WebView2Version.nupkg"

function Say([string]$text) { Write-Host $text }
function Step([string]$text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host ""; Write-Host "install: $text" -ForegroundColor Red; exit 1 }

# The machine's and the user's PATH as the registry holds them now (an installer just changed them).
function Update-Path {
  $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
  $user = [Environment]::GetEnvironmentVariable('Path', 'User')
  $env:Path = "$machine;$user"
  $cuda = [Environment]::GetEnvironmentVariable('CUDA_PATH', 'Machine')
  if ($cuda) { $env:CUDA_PATH = $cuda }
}

function Find-Git { $command = Get-Command git.exe -ErrorAction SilentlyContinue; if ($command) { return $command.Source }; return $null }

# A Python 3.10 or newer: the py launcher's, else a python.exe that is not the Microsoft Store's placeholder.
function Find-Python {
  $py = Get-Command py.exe -ErrorAction SilentlyContinue
  if ($py) {
    $version = & $py.Source -3 -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null
    if ($LASTEXITCODE -eq 0 -and $version -and [version]$version -ge [version]'3.10') { return ,@($py.Source, '-3') }
  }
  $python = Get-Command python.exe -ErrorAction SilentlyContinue
  if ($python -and $python.Source -notlike '*\WindowsApps\*') {
    $version = & $python.Source -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null
    if ($LASTEXITCODE -eq 0 -and $version -and [version]$version -ge [version]'3.10') { return ,@($python.Source) }  # (a one-item array stays one)
  }
  return $null
}

function Find-VsWhere {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path $vswhere) { return $vswhere }
  return $null
}

# A Visual Studio 2022 (or Build Tools) WITH the C++ x64 tools: its folder.
function Find-VsCpp {
  $vswhere = Find-VsWhere
  if (-not $vswhere) { return $null }
  $found = & $vswhere -latest -products * -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($found) { return ($found | Select-Object -First 1) }
  return $null
}

# Build Tools 2022 installed WITHOUT the C++ tools (the case that stops make with "cl was not found"): its folder.
function Find-BuildToolsWithoutCpp {
  $vswhere = Find-VsWhere
  if (-not $vswhere) { return $null }
  $found = & $vswhere -products Microsoft.VisualStudio.Product.BuildTools -version '[17.0,18.0)' -property installationPath
  if ($found) { return ($found | Select-Object -First 1) }
  return $null
}

# The CUDA Toolkit 13.x: its folder (13.0, the tested one, first).
function Find-Cuda {
  $root = Join-Path $env:ProgramFiles 'NVIDIA GPU Computing Toolkit\CUDA'
  $candidates = @()
  if (Test-Path $root) { $candidates += Get-ChildItem $root -Directory | Where-Object { $_.Name -like 'v13.*' } | Sort-Object Name | ForEach-Object { $_.FullName } }
  if ($env:CUDA_PATH) { $candidates += $env:CUDA_PATH }
  foreach ($candidate in $candidates) {
    $nvcc = Join-Path $candidate 'bin\nvcc.exe'
    if (-not (Test-Path $nvcc)) { continue }
    $text = (& $nvcc --version 2>$null) -join "`n"
    if ($text -match 'release 13\.') { return $candidate }
  }
  return $null
}

function Find-Driver { $smi = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue; if ($smi) { return $smi.Source }; return $null }

# winget install; "already installed" counts as done.
function Install-WithWinget([string]$id, [string[]]$extra) {
  $arguments = @('install', '--id', $id, '-e', '--accept-package-agreements', '--accept-source-agreements') + $extra
  Say "winget $($arguments -join ' ')"
  & winget @arguments
  $code = $LASTEXITCODE
  # 0x8A150109: installed, a restart finishes it (Visual Studio and CUDA often say so); the tools work before it
  if ($code -eq -1978334967) { $script:restartAfter = $true; $code = 0 }
  # 0x8A15002B: no newer version to install; 0x8A150061: already installed
  if ($code -ne 0 -and $code -ne -1978335189 -and $code -ne -1978335135) { Fail "winget could not install $id (exit code $code)" }
  Update-Path
}
$restartAfter = $false

# --- 1. what is there ------------------------------------------------------------------------------------------------
Step 'Looking at what this PC has'
if (-not [Environment]::Is64BitOperatingSystem) { Fail 'M-Strata builds on 64-bit Windows 10 or 11 only.' }
$features = @($With.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$git = Find-Git
$python = Find-Python
$vs = Find-VsCpp
$buildToolsWithoutCpp = $null
if (-not $vs) { $buildToolsWithoutCpp = Find-BuildToolsWithoutCpp }
$cuda = Find-Cuda
$driver = Find-Driver
function Show([string]$what, $found, [string]$missing) {
  if ($found) { Say ("  {0,-38} {1}" -f $what, $found) } else { Say ("  {0,-38} {1}" -f $what, $missing) }
}
Show 'Git' $git 'missing - will install (Git.Git)'
Show 'Python 3.10+' $(if ($python) { $python[0] } else { $null }) 'missing - will install (Python.Python.3.12)'
if ($vs) { Show 'Visual Studio 2022 C++ tools' $vs '' }
elseif ($buildToolsWithoutCpp) { Show 'Visual Studio 2022 C++ tools' $null "Build Tools at $buildToolsWithoutCpp lack C++ - will add the C++ workload" }
else { Show 'Visual Studio 2022 C++ tools' $null 'missing - will install Build Tools 2022 with the C++ workload' }
Show "CUDA Toolkit $CudaVersion" $cuda "missing - will install (Nvidia.CUDA $CudaVersion)"
Show 'NVIDIA driver (to run, not to build)' $driver 'not found - install it from nvidia.com or Windows Update'

$todo = @()
if (-not $git) { $todo += 'git' }
if (-not $python) { $todo += 'python' }
if (-not $vs) { $todo += 'vs' }
if (-not $cuda) { $todo += 'cuda' }
if ($todo.Count -gt 0) {
  if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
    Fail 'winget is not on this PC: install "App Installer" from the Microsoft Store, then run install.bat again.'
  }
  Say ""
  Say "To install: $($todo -join ', '). Visual Studio Build Tools and the CUDA Toolkit together take about 15 GB and"
  Say 'a while to download; each installer asks Windows for administrator rights.'
}

# --- where the repository is -----------------------------------------------------------------------------------------
$here = $PSScriptRoot
$inRepo = Test-Path (Join-Path $here 'make.py')
$repo = $here
if (-not $inRepo) { $repo = Join-Path $here 'm-strata' }
$parent = Split-Path $repo -Parent
if (-not $Out) {
  $Out = Join-Path $parent 'M-Strata-app'
  $n = 2
  # a new folder each time (make never deletes); never the repository itself (Windows names ignore case)
  while ((Test-Path $Out) -and (Get-ChildItem -Force $Out | Select-Object -First 1)) { $Out = Join-Path $parent "M-Strata-app-$n"; $n++ }
}
if ([IO.Path]::GetFullPath($Out).TrimEnd('\') -ieq [IO.Path]::GetFullPath($repo).TrimEnd('\')) {
  Fail "the out folder $Out is the repository folder itself (Windows names ignore case): choose another with -Out"
}
$ggml = $GgmlDir
if (-not $ggml) {
  $beside = Join-Path $parent 'llama.cpp'
  $ggml = Join-Path $repo '.toolchains\llama.cpp'
  if ((Test-Path (Join-Path $beside '.git')) -and $git) {
    $head = (& $git -C $beside rev-parse HEAD 2>$null)
    if ($head -eq $LlamaPin) { $ggml = $beside }
  }
}

Step 'The plan'
if (-not $inRepo) { Say "  clone $RepoUrl (with its submodules) into $repo" }
Say "  CMake and Ninja into $repo\.venv-dev (pip, from PyPI)"
if ($GgmlDir) { Say "  llama.cpp: $ggml, as it is" } else { Say "  llama.cpp at $LlamaPin in $ggml" }
if ($features -contains 'window') {
  if (Test-Path (Join-Path $repo ".toolchains\webview2\$WebView2Version\build\native\include\WebView2.h")) { Say "  the WebView2 SDK ${WebView2Version}: already in $repo\.toolchains\webview2" }
  else { Say "  the WebView2 SDK $WebView2Version into $repo\.toolchains\webview2 (nuget.org, SHA-256 checked)" }
}
Say "  make.bat --out $Out --with $($features -join ',')$(if ($Debugging) { ' --debugging' })"
if ($Debugging) {
  Say ''
  Say "  -Debugging: the server will log verbosely, to find out why a model does not start or calibrate. It logs"
  Say "  one line per request (never its text) and the engine's own output, which can hold what the engine prints of"
  Say "  your prompts and the paths of your model files. Run it on your own PC, keep the server's log private,"
  Say "  look through a log before you share it, and build again without the debugging option for everyday use."
  Say "  The engine's output can be turned off without a new build: Settings > Troubleshooting."
}
if ($Plan) { Say ""; Say 'install: -Plan given - nothing was installed or changed.'; exit 0 }
if (-not $Yes) {
  $answer = Read-Host 'Go ahead? [y/N]'
  if ($answer -notmatch '^(y|yes)$') { Say 'install: stopped; nothing was changed.'; exit 1 }
}

# --- 2. install what is missing (Visual Studio before CUDA: the CUDA installer adds its Visual Studio integration) -----
if (-not $git) {
  Step 'Installing Git'
  Install-WithWinget 'Git.Git' @()
  $git = Find-Git
  if (-not $git) { Fail 'Git was installed but is not on PATH yet: open a new terminal and run install.bat again.' }
}
if (-not $python) {
  Step 'Installing Python 3.12'
  Install-WithWinget 'Python.Python.3.12' @()
  $python = Find-Python
  if (-not $python) { Fail 'Python was installed but is not on PATH yet: open a new terminal and run install.bat again.' }
}
if (-not $vs) {
  if ($buildToolsWithoutCpp) {
    Step 'Adding the C++ workload to Visual Studio Build Tools 2022'
    $setup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    $arguments = @('modify', '--installPath', "`"$buildToolsWithoutCpp`"", '--add', 'Microsoft.VisualStudio.Workload.VCTools',
                   '--includeRecommended', '--passive', '--norestart')
    $done = Start-Process -FilePath $setup -ArgumentList $arguments -Verb RunAs -Wait -PassThru -ErrorAction Stop
    if ($done.ExitCode -eq 3010) { $restartAfter = $true }
    elseif ($done.ExitCode -ne 0) { Fail "the Visual Studio Installer stopped with code $($done.ExitCode)" }
  } else {
    Step 'Installing Visual Studio Build Tools 2022 with the C++ workload'
    Install-WithWinget 'Microsoft.VisualStudio.2022.BuildTools' @('--override', '--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
  }
  $vs = Find-VsCpp
  if (-not $vs) { Fail 'the C++ tools are still not found: open Visual Studio Installer and add "Desktop development with C++".' }
}
if (-not $cuda) {
  Step "Installing the CUDA Toolkit $CudaVersion"
  Install-WithWinget 'Nvidia.CUDA' @('--version', $CudaVersion)
  $cuda = Find-Cuda
  if (-not $cuda) { Fail "the CUDA Toolkit $CudaVersion is still not found: open a new terminal and run install.bat again." }
}

# --- 3. the repository -------------------------------------------------------------------------------------------------
if (-not $inRepo) {
  Step 'Cloning the repository'
  & $git clone --recurse-submodules $RepoUrl $repo
  if ($LASTEXITCODE -ne 0) { Fail 'git clone failed' }
} elseif (Test-Path (Join-Path $repo '.git')) {
  Step 'The repository''s submodules'
  & $git -C $repo submodule update --init --recursive
  if ($LASTEXITCODE -ne 0) { Fail 'git submodule update failed' }
}

# --- 4. CMake and Ninja in the repository's Python environment (make.bat runs make.py with it) -------------------------
Step 'CMake and Ninja'
$venvPython = Join-Path $repo '.venv-dev\Scripts\python.exe'
if (-not (Test-Path $venvPython)) {
  $pythonArgs = @()
  if ($python.Count -gt 1) { $pythonArgs = $python[1..($python.Count - 1)] }
  & $python[0] @pythonArgs -m venv (Join-Path $repo '.venv-dev')
  if ($LASTEXITCODE -ne 0) { Fail 'python -m venv failed' }
}
& $venvPython -m pip install --disable-pip-version-check cmake ninja
if ($LASTEXITCODE -ne 0) { Fail 'pip could not install cmake and ninja' }

# --- 5. llama.cpp at the pin, and the WebView2 SDK ---------------------------------------------------------------------
Step 'llama.cpp'
if ($GgmlDir) {
  # a folder you named is used as it is: never initialised, fetched or checked out here
  if (-not (Test-Path (Join-Path $ggml 'ggml\CMakeLists.txt'))) { Fail "-GgmlDir $ggml has no ggml\CMakeLists.txt: name a llama.cpp checkout" }
  if (Test-Path (Join-Path $ggml '.git')) {
    $head = (& $git -C $ggml rev-parse HEAD 2>$null)
    if ($head -ne $LlamaPin) { Say "  note: $ggml is at $head, not the tested $LlamaPin" }
  }
} else {
  if (-not (Test-Path (Join-Path $ggml '.git'))) {
    # only the pinned commit's files (a full clone is about 450 MB)
    New-Item -ItemType Directory -Force $ggml -ErrorAction Stop | Out-Null
    & $git -C $ggml init -q
    & $git -C $ggml remote add origin $LlamaUrl
  }
  $head = (& $git -C $ggml rev-parse HEAD 2>$null)
  if ($head -ne $LlamaPin) {
    & $git -C $ggml fetch --depth 1 origin $LlamaPin
    if ($LASTEXITCODE -ne 0) { Fail "could not fetch llama.cpp $LlamaPin" }
    & $git -C $ggml -c advice.detachedHead=false checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { Fail 'could not check out llama.cpp' }
  }
}
Say "  $ggml"

if ($features -contains 'window') {
  Step 'The WebView2 SDK'
  $sdk = Join-Path $repo ".toolchains\webview2\$WebView2Version"
  if (-not (Test-Path (Join-Path $sdk 'build\native\include\WebView2.h'))) {
    $download = Join-Path $repo '.toolchains\webview2\download'
    New-Item -ItemType Directory -Force $download -ErrorAction Stop | Out-Null
    $zip = Join-Path $download "microsoft.web.webview2.$WebView2Version.zip"
    Invoke-WebRequest -UseBasicParsing -Uri $WebView2Url -OutFile $zip -ErrorAction Stop
    $hash = (Get-FileHash -Algorithm SHA256 $zip -ErrorAction Stop).Hash.ToLowerInvariant()
    if ($hash -ne $WebView2Sha256) { Remove-Item $zip; Fail "the WebView2 SDK's SHA-256 is $hash, not $WebView2Sha256" }
    Expand-Archive -Path $zip -DestinationPath $sdk -Force -ErrorAction Stop
  }
  Say "  $sdk"
}

# --- 6. the build ----------------------------------------------------------------------------------------------------
Step "Building into $Out (the first build compiles the CUDA engine: allow an hour or more)"
$makeArgs = @()
if ($Debugging) { $makeArgs += '--debugging' }
& (Join-Path $repo 'make.bat') --out $Out --ggml-dir $ggml --cuda-root $cuda --with ($features -join ',') @makeArgs
if ($LASTEXITCODE -ne 0) { Fail "make stopped (exit code $LASTEXITCODE); its message is above" }

Step 'Done'
Say "  M-Strata is in $Out. Start it with:"
if ($features -contains 'window') { Say "    $Out\strata-window.exe        (a window with the page, and a tray icon)" }
if ($features -contains 'tui') { Say "    $Out\strata-tui.exe           (the same server in this terminal, with a dashboard)" }
Say "    $Out\strata-unified-server.exe  (prints a one-time login link: open it in a browser on this PC)"
if (-not $driver) { Say '  No NVIDIA driver was found: install it before starting a model.' }
if ($restartAfter) { Say '  An installer asked for a restart to finish: restart Windows before the first start.' }
