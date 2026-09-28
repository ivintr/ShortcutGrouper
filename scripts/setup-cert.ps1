# setup-cert.ps1 — Generate self-signed certificate for MSIX signing
# Run once as Administrator, then re-run build-sparse.ps1

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
$CertDir = Join-Path $Root "cert"
$CertFile = Join-Path $CertDir "sparse.pfx"

New-Item -ItemType Directory -Path $CertDir -Force | Out-Null

Write-Host "=== Generating Self-Signed Certificate ===" -ForegroundColor Cyan

$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject "CN=ShortcutGrouper" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -KeyExportPolicy Exportable `
    -KeySpec Signature `
    -KeyLength 2048 `
    -KeyAlgorithm RSA `
    -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears(5)

# Пароль НЕ хардкодим: берём из окружения, дефолт — только для локальной разработки.
$CertPassword = if ($env:SPARSE_PFX_PASSWORD) { $env:SPARSE_PFX_PASSWORD } else { "12345" }
if (-not $env:SPARSE_PFX_PASSWORD)
{
    Write-Host "  WARNING: SPARSE_PFX_PASSWORD not set, using default dev password." -ForegroundColor Yellow
    Write-Host "  Set `$env:SPARSE_PFX_PASSWORD before running in CI/shared machines." -ForegroundColor Yellow
}
$password = ConvertTo-SecureString -String $CertPassword -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $CertFile -Password $password

Write-Host "Certificate created: $CertFile" -ForegroundColor Green
Write-Host "Thumbprint: $($cert.Thumbprint)" -ForegroundColor Yellow
Write-Host ""
Write-Host "Now run: .\build-sparse.ps1" -ForegroundColor Yellow
