# build-setup.ps1 — assemble Setup.exe via built-in IExpress (no extra tools).
# Payload: binaries + MSIX + cert + installer scripts. Output: project root.

param(
    [string]$Configuration = "Release",
    [string]$Version = "1.0.1.0"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
$BuildDir = Join-Path $Root "build\bin\$Configuration"
$PayloadDir = Join-Path $Root "build\setup-payload"
$InstallerDir = Join-Path $Root "installer"
$SetupExe = Join-Path $Root "ShortcutGrouper-Setup-$Version.exe"

foreach ($f in @("GroupManager.exe", "ShellExtension.dll", "ShortcutGrouper.msix"))
{
    if (!(Test-Path (Join-Path $BuildDir $f)))
    {
        throw "Missing $f in $BuildDir. Build the project and run build-sparse.ps1 first."
    }
}
$certFile = Join-Path $Root "cert\sparse.pfx"
if (!(Test-Path $certFile)) { throw "Missing certificate: $certFile. Run setup-cert.ps1 first." }

Write-Host "=== Building Setup.exe ===" -ForegroundColor Cyan

# ---- Payload ----
if (Test-Path $PayloadDir) { Remove-Item $PayloadDir -Recurse -Force }
New-Item -ItemType Directory -Path $PayloadDir -Force | Out-Null
Copy-Item (Join-Path $BuildDir "GroupManager.exe") $PayloadDir
Copy-Item (Join-Path $BuildDir "ShellExtension.dll") $PayloadDir
Copy-Item (Join-Path $BuildDir "ShortcutGrouper.msix") $PayloadDir
Copy-Item $certFile (Join-Path $PayloadDir "sparse.pfx")
Copy-Item (Join-Path $InstallerDir "Install.ps1") $PayloadDir
Copy-Item (Join-Path $InstallerDir "Uninstall.ps1") $PayloadDir
Copy-Item (Join-Path $InstallerDir "install.bat") $PayloadDir
$files = Get-ChildItem $PayloadDir -File | Sort-Object Name
Write-Host "Payload ($($files.Count) files): $($files.Name -join ', ')"

# ---- SED ----
$sedPath = Join-Path $PayloadDir "setup.sed"
$fileLines = @()
$srcLines = @()
for ($i = 0; $i -lt $files.Count; $i++)
{
    # ВАЖНО: только имя файла! IExpress склеивает его с каталогом SourceFiles0.
    $fileLines += "FILE$i=`"$($files[$i].Name)`""
    $srcLines += "%FILE$i%="
}
$sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=%DisplayLicense%
FinishMessage=%FinishMessage%
TargetName=%TargetName%
FriendlyName=%FriendlyName%
AppLaunched=%AppLaunched%
PostInstallCmd=%PostInstallCmd%
AdminQuietInstCmd=%AdminQuietInstCmd%
UserQuietInstCmd=%UserQuietInstCmd%
SourceFiles=SourceFiles
[Strings]
InstallPrompt=
DisplayLicense=
FinishMessage=Installation finished.
TargetName=$SetupExe
FriendlyName=Shortcut Grouper $Version Setup
AppLaunched=cmd.exe /c install.bat
PostInstallCmd=<none>
AdminQuietInstCmd=
UserQuietInstCmd=
$($fileLines -join "`r`n")
[SourceFiles]
SourceFiles0=$PayloadDir
[SourceFiles0]
$($srcLines -join "`r`n")
"@
[IO.File]::WriteAllText($sedPath, $sed)

# ---- Build (Start-Process: через & exit-код iexpress теряется) ----
if (Test-Path $SetupExe) { Remove-Item $SetupExe -Force }
$iex = Start-Process "$env:SystemRoot\System32\iexpress.exe" -ArgumentList "/N `"$sedPath`"" -Wait -PassThru -NoNewWindow
if ($iex.ExitCode -ne 0) { throw "iexpress failed with exit code $($iex.ExitCode)" }
if (!(Test-Path $SetupExe)) { throw "Setup.exe was not created." }

Write-Host ""
Write-Host "=== Done: $SetupExe ($((Get-Item $SetupExe).Length) bytes) ===" -ForegroundColor Green
