<#
.SYNOPSIS
    Signs an unsigned OpenHarmony HAP with your own AppGallery Connect (AGC)
    materials so it can be installed on HarmonyOS 5.0+ (NEXT) devices.

.DESCRIPTION
    Wraps hap-sign-tool (hapsigntoolv2.jar) with SHA256withECDSA local
    signing. Requires:
      - the unsigned HAP produced by the CI workflow (sign_mode=none),
      - a .p12 keystore with its alias and passwords,
      - the application certificate (.cer) and the release provisioning
        profile (.p7b) downloaded from AppGallery Connect.

    DevEco Studio 5 ships everything the script needs; it auto-detects the
    SDK signing tool and the bundled JBR Java. No admin rights required.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/sign_hap_agc.ps1 ^
      -Hap Yosuga-...-unsigned.hap ^
      -Keystore app.p12 -KeystorePassword ******** ^
      -KeyAlias mykey -KeyPassword ******** ^
      -AppCert app.cer -Profile profile.p7b
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Hap,

    [Parameter(Mandatory = $true)]
    [string]$Keystore,

    [Parameter(Mandatory = $true)]
    [string]$KeystorePassword,

    [Parameter(Mandatory = $true)]
    [string]$KeyAlias,

    [Parameter(Mandatory = $true)]
    [string]$KeyPassword,

    [Parameter(Mandatory = $true)]
    [string]$AppCert,

    [Parameter(Mandatory = $true)]
    [string]$Profile,

    [string]$Out,
    [string]$SignToolJar,
    [string]$Java
)

$ErrorActionPreference = 'Stop'

function Assert-File([string]$Path, [string]$What) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$What not found: $Path"
    }
}

foreach ($item in @(
        @{ Path = $Hap;      What = 'Unsigned HAP' },
        @{ Path = $Keystore; What = 'Keystore (.p12)' },
        @{ Path = $AppCert;  What = 'Application certificate (.cer)' },
        @{ Path = $Profile;  What = 'Provisioning profile (.p7b)' })) {
    Assert-File $item.Path $item.What
}

# --- Locate Java (DevEco Studio bundles a JBR) -------------------------
if (-not $Java) {
    $jbrCandidates = @(
        (Join-Path $env:ProgramFiles 'Huawei\DevEco Studio\jbr\bin\java.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Huawei\DevEco Studio\jbr\bin\java.exe')
    )
    foreach ($candidate in $jbrCandidates) {
        if (Test-Path -LiteralPath $candidate) { $Java = $candidate; break }
    }
}
if (-not $Java) {
    $cmd = Get-Command java -ErrorAction SilentlyContinue
    if ($cmd) { $Java = $cmd.Source }
}
if (-not $Java) {
    throw "Java was not found. Install DevEco Studio 5 or pass -Java <path to java.exe>."
}

# --- Locate hap-sign-tool.jar ------------------------------------------
if (-not $SignToolJar) {
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:DEVECO_SDK_HOME) |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    foreach ($root in $roots) {
        $jar = Get-ChildItem -LiteralPath $root -Recurse -Filter 'hap-sign-tool*.jar' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($jar) { $SignToolJar = $jar.FullName; break }
    }
}
if (-not $SignToolJar) {
    throw @"
hap-sign-tool.jar was not found. Install DevEco Studio 5 (it bundles the
OpenHarmony SDK with the signing tool under
"<DevEco Studio>\sdk\default\openharmony\toolchains\lib\"), or download
the OpenHarmony command line tools bundle and pass -SignToolJar:
  https://repo.huaweicloud.com/harmonyos/ohpm/5.0.3/commandline-tools-windows-x64-5.0.3.906.zip
  (the jar is at command-line-tools\sdk\default\openharmony\toolchains\lib\hap-sign-tool.jar)
"@
}

if (-not $Out) {
    $base = [System.IO.Path]::GetFileNameWithoutExtension($Hap)
    $Out = Join-Path (Split-Path -Parent $Hap) ($base + '-signed.hap')
}

Write-Host "==> Signing: $Hap"
Write-Host "    Java:    $Java"
Write-Host "    Tool:    $SignToolJar"
Write-Host "    Out:     $Out"

$signArgs = @(
    '-jar', $SignToolJar, 'sign-app',
    '-keyAlias', $KeyAlias,
    '-signAlg', 'SHA256withECDSA',
    '-mode', 'localSign',
    '-appCertFile', $AppCert,
    '-profileFile', $Profile,
    '-inFile', $Hap,
    '-outFile', $Out,
    '-keystoreFile', $Keystore,
    '-keyPwd', $KeyPassword,
    '-keystorePwd', $KeystorePassword
)
& $Java $signArgs
if ($LASTEXITCODE -ne 0) {
    throw "hap-sign-tool sign-app failed (exit code $LASTEXITCODE)."
}

$verifyArgs = @(
    '-jar', $SignToolJar, 'verify-app',
    '-inFile', $Out,
    '-outCertChain', ($Out + '.certchain.cer'),
    '-outProfile', ($Out + '.profile.p7b')
)
& $Java $verifyArgs
if ($LASTEXITCODE -ne 0) {
    throw "hap-sign-tool verify-app failed (exit code $LASTEXITCODE)."
}

Write-Host ''
Write-Host 'SUCCESS. Signed HAP: ' $Out
Write-Host 'Install on your device with:  hdc install "' $Out '"'
