<#
install.ps1 - from a fresh Windows 10/11 PC to a built M-Strata in one step (run it through install.bat).

  .\install.bat                                        ask what to build (Enter takes each default: everything, the
                                                       Debugging plugin off), then build into ..\M-Strata-app
  .\install.bat -Out D:\Apps\M-Strata -With email,search,tui      (no window: only the terminal one)
  .\install.bat -Without maya,raon1,vision              leave the GLM/DeepSeek/MiniMax engines, raon1 or pictures out
  .\install.bat -Plan                                  only say what it would install, fetch and run
  .\install.bat -Yes                                   ask nothing: build everything (no debugging), install without asking
  .\install.bat -Debugging                             the Debugging plugin on: a server that logs verbosely, to find
                                                       out why a model does not start or calibrate (the plan says what
                                                       to take care of)
  .\install.bat -SigningPassphrase                     seal the plugin signing key the build makes with a passphrase
                                                       (make asks for it; asked when this PC has no key yet)
  .\install.bat -SearXNG                               a local SearXNG for web search, in Docker Desktop (which must be
                                                       installed and running): 127.0.0.1:8888, this PC only; the Web
                                                       search plugin uses it from the first start (asked when web search
                                                       is built and Docker Desktop is there)
  .\install.bat -SearXNGUrl http://host:port            a SearXNG you already run, for web search (asked before the local
                                                       one); the Web search plugin uses it from the first start
  .\install.bat -Boot logon|startup|no                 start with Windows (asked; -Yes takes no): logon - at your log-on,
                                                       strata-window in the tray (no password, no administrator);
                                                       startup - when this PC starts, the server itself, as you, before
                                                       anyone logs on (an administrator's approval and your Windows
                                                       password, typed into schtasks' own prompt, once)

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
  6. with -SearXNG (or its question's yes), sets up SearXNG in Docker Desktop beside the repository (docker\searxng);
  7. runs make.bat into a new folder and says how to start it;
  8. with -Boot logon|startup (or its question's answer), makes the Task Scheduler task M-Strata that starts it with
     Windows.
Downloads: winget's packages (Microsoft's catalog, hash-checked by winget), Python packages from PyPI, the repositories
from GitHub, the WebView2 SDK from nuget.org, and with -SearXNG SearXNG's image from Docker Hub. Nothing else is
downloaded, and nothing is sent anywhere.
#>
param(
  [string]$Out = "",
  [string]$With = "email,search,window,tui",
  [string]$Without = "",
  [string]$GgmlDir = "",
  [switch]$Debugging,
  [switch]$SigningPassphrase,
  [switch]$SearXNG,
  [string]$SearXNGUrl = "",
  [string]$Boot = '',
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

# A SearXNG address as make and the server take one (make.py's valid_searxng_url; make checks the port's range).
function Test-SearXNGUrl([string]$url) {
  return ($url.Length -le 200 -and $url -cmatch '^(?i:https?)://(\[[0-9A-Fa-f:.]{2,}\]|[A-Za-z0-9][A-Za-z0-9.-]{0,252})(:[0-9]{1,5})?(/[A-Za-z0-9._~/-]*)?$' -and
          ($url -replace '^[^:]*://', '') -notmatch '//')
}

# y or n; Enter takes the default
function Ask([string]$question, [bool]$default) {
  $hint = if ($default) { '[Y/n]' } else { '[y/N]' }
  while ($true) {
    $answer = Read-Host "  $question $hint"
    if (-not $answer) { return $default }
    if ($answer -match '^(y|yes)$') { return $true }
    if ($answer -match '^(n|no)$') { return $false }
    Say '  y or n, please (Enter takes the capital one)'
  }
}

# --- what to build (owner 2026-10-29: "we should ask these questions, if they want email alerts, search, etc so that
# they can build exactly what they want"): a question for each of make's choices that -With, -Without or -Debugging
# did not answer on the command line; -Yes asks nothing and takes the defaults. make's lists (make.py's
# OPTIONAL_FEATURES and LEAVE_OUT: tools/secure/test_install.py)
$KnownFeatures = @('email', 'search', 'window', 'tui')
$KnownLeaveOut = @('maya', 'raon1', 'vision')
$features = @($With.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$leaveOut = @($Without.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($name in $features) { if ($KnownFeatures -notcontains $name) { Fail "-With takes only $($KnownFeatures -join ', '), not $name" } }
foreach ($name in $leaveOut) { if ($KnownLeaveOut -notcontains $name) { Fail "-Without takes only $($KnownLeaveOut -join ', '), not $name" } }
$askFeatures = -not $Yes -and -not $PSBoundParameters.ContainsKey('With')
$askLeaveOut = -not $Yes -and -not $PSBoundParameters.ContainsKey('Without')
$askDebugging = -not $Yes -and -not $PSBoundParameters.ContainsKey('Debugging')
# this PC's plugin signing key, where make keeps it (scripts/plugin_signing.py's default_key_path): the build makes one
# when there is none - sealed with a passphrase if asked - and one already sealed needs its passphrase (make asks)
$appData = if ($env:APPDATA) { $env:APPDATA } else { Join-Path $HOME 'AppData\Roaming' }
$signingKey = Join-Path $appData 'M-Strata\plugin-signing-key.json'
$keyExists = Test-Path -LiteralPath $signingKey
$keySealed = $keyExists -and ((Get-Content -Raw -LiteralPath $signingKey) -match '"passphrase"\s*:\s*true')
$askPassphrase = -not $Yes -and -not $PSBoundParameters.ContainsKey('SigningPassphrase') -and -not $keyExists
# a local SearXNG for web search (owner 2026-10-30: "ask if the user wants it locally which we will install if yes"):
# asked when web search is built and Docker Desktop is installed and running (this script does not install it: its
# licence can need payment in larger organisations); -Yes takes yes there
$dockerReady = $false
$dockerExe = Get-Command docker.exe -ErrorAction SilentlyContinue
if ($dockerExe) { & $dockerExe.Source info *> $null; $dockerReady = $LASTEXITCODE -eq 0 }
# (or a SearXNG you already run: its address, asked first - owner 2026-10-30: "if so they can just provide details")
if ($SearXNGUrl) {
  if (-not (Test-SearXNGUrl $SearXNGUrl)) { Fail "-SearXNGUrl: not a SearXNG address (http(s)://host[:port][/path], with no user, query or fragment): $SearXNGUrl" }
  if ($SearXNG) { Fail '-SearXNG sets up a local SearXNG and -SearXNGUrl names one you run: one or the other' }
}
$askSearXNGUrl = -not $Yes -and -not $SearXNGUrl -and -not $PSBoundParameters.ContainsKey('SearXNG')
$askSearXNG = $askSearXNGUrl -and $dockerReady
if ($askFeatures -or $askLeaveOut -or $askDebugging -or $askPassphrase -or $askSearXNGUrl) {
  Step 'What to build'
  Say '  Answer y or n; Enter takes the capital one. Qwen, Gemma 4 and flint models are always built.'
  if ($askFeatures) {
    $features = @()
    if (Ask 'Email alerts: the server e-mails you when a download or a calibration ends, an engine stops, or it starts?' $true) { $features += 'email' }
    if (Ask 'Web search: the models search the web through a SearXNG you run?' $true) { $features += 'search' }
    if (Ask 'The window app (strata-window): the page in a window of its own, with a tray icon?' $true) { $features += 'window' }
    if (Ask 'The terminal app (strata-tui): the server in a terminal, with a dashboard?' $true) { $features += 'tui' }
  }
  if ($askLeaveOut) {
    $leaveOut = @()
    if (-not (Ask 'GLM-5.3, DeepSeek-V4.1 and MiniMax-M3.1 models (Project Maya''s engines, the largest part of the build)?' $true)) { $leaveOut += 'maya' }
    if (-not (Ask 'Raon-1-mini models (the raon1 engine)?' $true)) { $leaveOut += 'raon1' }
    if (-not (Ask 'Pictures in Qwen chats (the picture encoder, strata-vision)?' $true)) { $leaveOut += 'vision' }
  }
  if ($askDebugging) {
    $Debugging = [switch](Ask 'The Debugging plugin turned on: verbose logs, to find out why a model does not start or calibrate (the plan says what it logs)?' $false)
  }
  if ($askPassphrase) {
    $SigningPassphrase = [switch](Ask 'Seal the plugin signing key this build makes with a passphrase (make asks for it now, and at every later build or plugin you sign)?' $false)
  }
  if ($askSearXNGUrl -and $features -contains 'search') {
    while ($true) {
      $answer = Read-Host '  A SearXNG you already run for web search? Its address (http://host:port), or Enter for none'
      if (-not $answer) { break }
      if (Test-SearXNGUrl $answer.Trim()) { $SearXNGUrl = $answer.Trim(); break }
      Say '  not a SearXNG address: http(s)://host[:port][/path], with no user, query or fragment'
    }
  }
  if ($askSearXNG -and -not $SearXNGUrl -and $features -contains 'search') {
    $SearXNG = [switch](Ask 'A local SearXNG for web search (in Docker Desktop; 127.0.0.1:8888, this PC only)?' $true)
  }
}
if ($Yes -and -not $PSBoundParameters.ContainsKey('SearXNG') -and -not $SearXNGUrl -and $dockerReady -and $features -contains 'search') {
  $SearXNG = [switch]$true
}
if ($SearXNGUrl -and $features -notcontains 'search') {
  Fail "-SearXNGUrl is the Web search plugin's SearXNG: build web search too (-With ...,search)"
}
if ($SearXNG -and $features -notcontains 'search') {
  Fail "-SearXNG sets up the Web search plugin's SearXNG: build web search too (-With ...,search)"
}
if ($SearXNG -and -not $dockerReady) {
  Fail '-SearXNG needs Docker Desktop installed and running (docker.com); nothing was installed or changed.'
}

# --- start with Windows (the owner, 2026-10-31: "even have a windows start option", both ways chosen here). A Windows
# service was the first idea, but one logged on as an administrator's account runs elevated, which the server refuses;
# so both are Task Scheduler tasks with the least privileges. logon: at this user's log-on, strata-window in the tray (it
# uses the server already running or starts its own). startup: when the PC starts, the server itself on the install's
# data folder, as this user whether or not anyone is logged on; Task Scheduler keeps the user's password (the key file
# needs it), which schtasks asks for itself.
if ($Boot -and @('logon', 'startup', 'no') -notcontains $Boot) { Fail "-Boot takes logon, startup or no, not $Boot" }
if (-not $Boot) {
  $Boot = 'no'
  if (-not $Yes) {
    Step 'Start with Windows'
    while ($true) {
      $answer = Read-Host '  Start the server with Windows? l: at your log-on, strata-window in the tray; s: when this PC starts, before anyone logs on (an administrator''s approval and your Windows password, once); Enter: no'
      if (-not $answer) { break }
      if ($answer -match '^(l|logon)$') { $Boot = 'logon'; break }
      if ($answer -match '^(s|startup)$') { $Boot = 'startup'; break }
      Say '  l, s, or Enter for no, please'
    }
  }
}
if ($Boot -eq 'logon' -and $features -notcontains 'window') {
  Fail '-Boot logon opens strata-window in the tray at log-on: build the window too (-With ...,window), or choose -Boot startup'
}

# The task Task Scheduler makes (schtasks /XML), for -Boot logon or startup: the least privileges (an administrator's
# account gets its filtered token), one at a time, no time limit, on batteries too.
function Get-BootTask([string]$kind) {
  $user = [Security.SecurityElement]::Escape([Security.Principal.WindowsIdentity]::GetCurrent().Name)
  $folder = [Security.SecurityElement]::Escape([IO.Path]::GetFullPath($Out))
  if ($kind -eq 'logon') {
    $trigger = "    <LogonTrigger>`n      <Enabled>true</Enabled>`n      <UserId>$user</UserId>`n    </LogonTrigger>"
    $logonType = 'InteractiveToken'
    $command = "`"$folder\strata-window.exe`""
    $arguments = '--tray'
    $restart = ''
  } else {
    $trigger = "    <BootTrigger>`n      <Enabled>true</Enabled>`n    </BootTrigger>"
    $logonType = 'Password'
    $command = "`"$folder\strata-unified-server.exe`""
    $arguments = "--data-dir `"$folder\data`" --no-login-link"
    $restart = "    <RestartOnFailure>`n      <Interval>PT1M</Interval>`n      <Count>3</Count>`n    </RestartOnFailure>`n"
  }
  @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>M-Strata server (install.ps1 -Boot $kind)</Description>
  </RegistrationInfo>
  <Triggers>
$trigger
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>$user</UserId>
      <LogonType>$logonType</LogonType>
      <RunLevel>LeastPrivilege</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
$restart    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>6</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>$command</Command>
      <Arguments>$arguments</Arguments>
      <WorkingDirectory>$folder</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
"@
}
$makeArgs = @()
if ($features.Count -gt 0) { $makeArgs += @('--with', ($features -join ',')) }
if ($leaveOut.Count -gt 0) { $makeArgs += @('--without', ($leaveOut -join ',')) }
if ($Debugging) { $makeArgs += '--debugging' }
# a sealed key goes to make as sealed; one there unsealed is used as it is (make seals only a key it makes)
if ($keySealed -or ($SigningPassphrase -and -not $keyExists)) { $makeArgs += '--signing-passphrase' }
if ($SearXNG) { $makeArgs += @('--searxng-url', 'http://127.0.0.1:8888') }
elseif ($SearXNGUrl) { $makeArgs += @('--searxng-url', $SearXNGUrl) }

# --- 1. what is there ------------------------------------------------------------------------------------------------
Step 'Looking at what this PC has'
if (-not [Environment]::Is64BitOperatingSystem) { Fail 'M-Strata builds on 64-bit Windows 10 or 11 only.' }
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
if (-not $keyExists) {
  if ($SigningPassphrase) { Say "  plugin signing key: $signingKey - made by this build, sealed with a passphrase: make asks for it, twice" }
  else { Say "  plugin signing key: $signingKey - made by this build (protected for this Windows user, no passphrase)" }
} elseif ($keySealed) {
  Say "  plugin signing key: $signingKey, as it is - sealed with a passphrase: make asks for it"
} else {
  Say "  plugin signing key: $signingKey, as it is (no passphrase)"
  if ($SigningPassphrase) { Say '  (-SigningPassphrase seals only a key the build makes: this one is used as it is)' }
}
$searxngDir = Join-Path $parent 'docker\searxng'
if ($SearXNG) {
  # (8888 already taken by something else stops it here; a SearXNG this script set up before is started again)
  $taken = Get-NetTCPConnection -LocalPort 8888 -State Listen -ErrorAction SilentlyContinue
  if ($taken -and -not (Test-Path -LiteralPath (Join-Path $searxngDir 'compose.yaml'))) {
    Fail 'port 8888 on this PC is taken: stop what uses it, or leave the local SearXNG out'
  }
  Say "  SearXNG for web search: $searxngDir (Docker Desktop; 127.0.0.1:8888, this PC only) - the Web search plugin's address from the first start"
} elseif ($SearXNGUrl) {
  Say "  SearXNG for web search: $SearXNGUrl (yours) - the Web search plugin's address from the first start"
}
Say "  make.bat --out $Out $($makeArgs -join ' ')"
if ($Debugging) {
  Say ''
  Say "  -Debugging: the Debugging plugin ships on, so the server logs verbosely, to find out why a model does not"
  Say "  start or calibrate. It logs one line per request (never its text) and the engine's own output, which can"
  Say "  hold what the engine prints of your prompts and the paths of your model files. Run it on your own PC,"
  Say "  keep the server's log private, look through a log before you share it, and turn the plugin off in the"
  Say "  Plugins tab for everyday use. The engine's output alone can be turned off in Settings > Troubleshooting."
}
if ($Boot -eq 'logon') {
  Say "  start with Windows: at your log-on, the task M-Strata opens $Out\strata-window.exe in the tray (it uses the"
  Say '  server already running, or starts its own) - Task Scheduler, as you, with no password or administrator. The task:'
} elseif ($Boot -eq 'startup') {
  Say "  start with Windows: when this PC starts, the task M-Strata runs $Out\strata-unified-server.exe on $Out\data as"
  Say '  you, with the least privileges, whether or not you are logged on. Windows asks for an administrator''s approval to'
  Say '  make it, and schtasks for your Windows password, which Task Scheduler keeps (the server needs it to read its'
  Say '  key). The task:'
}
if ($Boot -ne 'no') { (Get-BootTask $Boot).TrimEnd().Split("`n") | ForEach-Object { Say "    | $($_.TrimEnd())" } }
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

# --- 6. a local SearXNG for web search (the README's "Web search through SearXNG", owner 2026-10-30) -----------------
# Loopback only (127.0.0.1:8888: nothing outside this PC reaches it); its server bound to IPv4 inside the container (the
# image binds IPv6 by default, which fails where Docker has no IPv6); JSON on; the README's extra engines; a secret made
# here, kept when it is set up again.
if ($SearXNG) {
  Step "SearXNG for web search, in $searxngDir"
  New-Item -ItemType Directory -Force -Path (Join-Path $searxngDir 'config'), (Join-Path $searxngDir 'cache') | Out-Null
  $utf8 = New-Object System.Text.UTF8Encoding $false
  $compose = @'
services:
  searxng:
    image: docker.io/searxng/searxng:latest
    container_name: searxng
    restart: unless-stopped
    ports: ["127.0.0.1:8888:8080"]   # this PC only: M-Strata's server reaches it, nothing outside does
    environment: ["GRANIAN_HOST=0.0.0.0"]   # IPv4 inside the container
    volumes: ["./config:/etc/searxng", "./cache:/var/cache/searxng"]
'@
  [IO.File]::WriteAllText((Join-Path $searxngDir 'compose.yaml'), $compose.Replace("`r`n", "`n"), $utf8)
  $settings = Join-Path $searxngDir 'config\settings.yml'
  if (-not (Test-Path -LiteralPath $settings)) {
    $bytes = New-Object byte[] 32
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $secret = -join ($bytes | ForEach-Object { $_.ToString('x2') })
    $text = @"
use_default_settings: true
server:
  secret_key: "$secret"
  limiter: false
  base_url: "http://127.0.0.1:8888/"
search:
  formats: [html, json]   # M-Strata's Web search plugin reads the JSON
engines:
  - name: bing
    disabled: false
  - name: qwant
    disabled: false
  - name: yahoo
    disabled: false
  - name: mojeek
    disabled: false
    inactive: false
"@
    [IO.File]::WriteAllText($settings, $text.Replace("`r`n", "`n"), $utf8)
  }
  Push-Location $searxngDir
  & $dockerExe.Source compose up -d
  $started = $LASTEXITCODE
  Pop-Location
  if ($started -ne 0) { Fail "SearXNG did not start (exit code $started); its message is above" }
  $answered = $false
  for ($i = 0; $i -lt 12 -and -not $answered; $i++) {
    try {
      $reply = Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 -Uri 'http://127.0.0.1:8888/search?q=m-strata&format=json'
      $answered = $reply.Content -match '"results"'
    } catch { Start-Sleep -Seconds 5 }
  }
  if ($answered) { Say '  SearXNG answers on http://127.0.0.1:8888' }
  else { Say '  SearXNG has not answered yet (its first start can take a minute); the Web search plugin uses it once it does' }
}

# --- 7. the build ----------------------------------------------------------------------------------------------------
Step "Building into $Out (the first build compiles the CUDA engine: allow an hour or more)"
& (Join-Path $repo 'make.bat') --out $Out --ggml-dir $ggml --cuda-root $cuda @makeArgs
if ($LASTEXITCODE -ne 0) { Fail "make stopped (exit code $LASTEXITCODE); its message is above" }

# --- 8. start with Windows -------------------------------------------------------------------------------------------
# The task from a UTF-16 file in the install folder (what schtasks /XML reads; kept there, the record of what was made).
# logon: made by this user. startup: a start-up trigger needs an administrator, so schtasks runs with Windows' approval
# prompt, and asks for this user's password itself (/RP *), which Task Scheduler keeps - this script never sees it.
$bootMade = $false
if ($Boot -ne 'no') {
  Step "Start with Windows ($Boot)"
  $taskFile = Join-Path ([IO.Path]::GetFullPath($Out)) 'boot-task.xml'
  [IO.File]::WriteAllText($taskFile, (Get-BootTask $Boot), [Text.Encoding]::Unicode)
  $schtasks = Join-Path $env:SystemRoot 'System32\schtasks.exe'
  if ($Boot -eq 'logon') {
    & $schtasks '/Create', '/TN', 'M-Strata', '/XML', $taskFile, '/F'
    $bootMade = $LASTEXITCODE -eq 0
    $byHand = "schtasks /Create /TN M-Strata /XML `"$taskFile`" /F"
  } else {
    $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    Say '  Windows asks for an administrator''s approval; then type your Windows password in the window that opens.'
    $run = Start-Process -FilePath $schtasks -ArgumentList @('/Create', '/TN', 'M-Strata', '/XML', "`"$taskFile`"", '/RU', "`"$user`"", '/RP', '*', '/F') -Verb RunAs -Wait -PassThru
    $bootMade = $run -and $run.ExitCode -eq 0
    $byHand = "schtasks /Create /TN M-Strata /XML `"$taskFile`" /RU `"$user`" /RP * /F (in an administrator's terminal)"
  }
  if (-not $bootMade) { Say "  The task was not made; the build is done. Make it with: $byHand" }
}

Step 'Done'
Say "  M-Strata is in $Out. Start it with:"
if ($features -contains 'window') { Say "    $Out\strata-window.exe        (a window with the page, and a tray icon)" }
if ($features -contains 'tui') { Say "    $Out\strata-tui.exe           (the same server in this terminal, with a dashboard)" }
Say "    $Out\strata-unified-server.exe  (prints a one-time login link: open it in a browser on this PC)"
if ($SearXNG) { Say "  Web search: SearXNG on http://127.0.0.1:8888 (${searxngDir}: docker compose there stops and starts it)" }
elseif ($SearXNGUrl) { Say "  Web search: your SearXNG at $SearXNGUrl" }
if ($bootMade -and $Boot -eq 'logon') {
  Say '  At your log-on, strata-window opens in the tray (the task M-Strata; schtasks /Delete /TN M-Strata /F removes it).'
} elseif ($bootMade) {
  Say "  The server starts with this PC (the task M-Strata; schtasks /Run /TN M-Strata starts it now, and strata-window and"
  Say "  strata-tui use it). Sign in with $Out\strata.exe key show, or the apps. An administrator's schtasks /Delete /TN"
  Say '  M-Strata /F removes the task.'
}
if (-not $driver) { Say '  No NVIDIA driver was found: install it before starting a model.' }
if ($restartAfter) { Say '  An installer asked for a restart to finish: restart Windows before the first start.' }
