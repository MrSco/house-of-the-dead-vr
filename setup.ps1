# One-step setup for Windows 10/11: installs the build prerequisites with
# winget, then fetches the pinned dependencies and builds (scripts/setup.py).
#
#   powershell -ExecutionPolicy Bypass -File setup.ps1 [--clang] [--test-extras] [--with-mame]
#
# Installs (skipping what is already there): Git, CMake, Ninja, Python 3 and
# Visual Studio 2022 Build Tools with the C++ workload (MSVC and the Windows
# SDK). Everything is built with MSVC, as HOTDVR.exe is (scripts/build_windows.py:
# /fp:strict, so the PC computes the same game as the Quest, bit for bit).
# --clang adds Visual Studio's Clang tools and builds the tools with clang-cl
# instead. SDL 3 is fetched and built with the project. Put your own ROM set at roms\hotd.zip (or .7z) to have The
# House of the Dead recompiled too (or run scripts/recompile.py --game
# houseofthedead afterwards). docs/getting-started.md has the whole path.

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Error "winget not found. Install 'App Installer' from the Microsoft Store, then run setup.ps1 again."
}

function Install-Package([string]$Id, [string]$Override = '') {
    $listed = winget list --id $Id --exact --accept-source-agreements 2>$null | Select-String -SimpleMatch $Id
    if ($listed) { Write-Host "== $Id already installed"; return }
    Write-Host "== Installing $Id"
    $wingetArgs = @('install', '--id', $Id, '--exact', '--silent', '--accept-package-agreements', '--accept-source-agreements')
    if ($Override) { $wingetArgs += @('--override', $Override) }
    & winget @wingetArgs
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {  # -1978335189: already installed
        Write-Error "winget could not install $Id (exit $LASTEXITCODE)"
    }
}

Install-Package 'Git.Git'
Install-Package 'Kitware.CMake'
Install-Package 'Ninja-build.Ninja'
Install-Package 'Python.Python.3.12'
# --clang: Visual Studio's clang-cl instead of MSVC. The rest goes to setup.py.
$useClang = $args -contains '--clang'
$setupArgs = @($args | Where-Object { $_ -ne '--clang' -and $_ -ne '--msvc' })  # (--msvc: the old switch, now the default)

# The C++ tools (MSVC); with --clang, Clang too (clang-cl and the ClangCL toolset).
$vsComponents = @('Microsoft.VisualStudio.Workload.VCTools')
if ($useClang) {
    $vsComponents += @('Microsoft.VisualStudio.Component.VC.Llvm.Clang',
                       'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset')
}
$vsAdd = ($vsComponents | ForEach-Object { "--add $_" }) -join ' '
Install-Package 'Microsoft.VisualStudio.2022.BuildTools' "--wait --passive --norestart $vsAdd --includeRecommended"

$installerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$vswhere = Join-Path $installerDir 'vswhere.exe'
function Test-VsClang {
    if (-not (Test-Path $vswhere)) { return $false }
    $found = & $vswhere -latest -version '[17,18)' -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang `
        Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset -property installationPath
    return [bool]$found
}

if ($useClang -and -not (Test-VsClang)) {
    # winget leaves an existing Visual Studio 2022 or Build Tools install as it is:
    # add the Clang tools to that version, never a newer side-by-side install.
    # Changing an install needs administrator
    # rights, so the Visual Studio Installer runs elevated (Windows asks).
    $vsPath = if (Test-Path $vswhere) { & $vswhere -latest -version '[17,18)' -products * -property installationPath } else { $null }
    if ($vsPath) {
        Write-Host "== Adding the C++ and Clang tools to $vsPath (Windows will ask for permission)"
        $modify = @('modify', '--installPath', "`"$vsPath`"", '--passive', '--norestart', '--includeRecommended') +
                  ($vsComponents | ForEach-Object { @('--add', $_) })
        try {
            $p = Start-Process -FilePath (Join-Path $installerDir 'setup.exe') -ArgumentList $modify -Verb RunAs -Wait -PassThru
            if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {  # 3010: done, restart suggested
                Write-Warning "The Visual Studio Installer finished with exit code $($p.ExitCode)."
            }
        } catch {
            Write-Warning "The Visual Studio Installer did not run: $($_.Exception.Message)"
        }
    }
    if (-not (Test-VsClang)) {
        Write-Error ("The Clang tools for Visual Studio 2022 are not installed (setup.ps1 --clang).`n" +
            "Add them to Visual Studio 2022 in the Visual Studio Installer: Modify > Individual components > " +
            "'C++ Clang Compiler for Windows' and 'MSBuild support for LLVM (clang-cl) toolset', " +
            "then run setup.ps1 --clang again. Or run setup.ps1 without --clang to build with MSVC.")
    }
}
# setup.py: build with this compiler (and reconfigure a build made with the other).
$env:M2_COMPILER = if ($useClang) { 'clang' } else { 'msvc' }

# Pick up the new tools without opening a new terminal.
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path', 'User')

# Prefer the py launcher: a bare "python" can be the Microsoft Store stub.
$python = (Get-Command py -ErrorAction SilentlyContinue)
if (-not $python) { $python = (Get-Command python -ErrorAction SilentlyContinue) }
if (-not $python) { Write-Error "Python was installed but is not on PATH yet; open a new terminal and run setup.ps1 again." }

& $python.Source scripts\setup.py @setupArgs
exit $LASTEXITCODE
