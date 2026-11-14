param(
    [string]$DestinationPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'sdk')
)

$ErrorActionPreference = 'Stop'
$archive = Join-Path ([IO.Path]::GetTempPath()) ('aimp-sdk-' + [Guid]::NewGuid().ToString('N') + '.zip')
try {
    Invoke-WebRequest -Uri 'https://aimp.ru/?do=download.file&id=15' -OutFile $archive
    $expected = 'aa449fed93b93af7880a5580d71265231d048115e837b8d79f0c32aec59537a7'
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
        throw 'AIMP SDK checksum mismatch. Download the pinned SDK 5.40 archive to continue.'
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $DestinationPath -Force
    if (-not (Test-Path -LiteralPath (Join-Path $DestinationPath 'Sources/Cpp/apiPlugin.h') -PathType Leaf)) {
        throw 'AIMP SDK extraction failed: Sources/Cpp/apiPlugin.h is missing.'
    }
    Write-Host 'AIMP SDK 5.40 downloaded, verified and extracted.'
} finally {
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }
}
