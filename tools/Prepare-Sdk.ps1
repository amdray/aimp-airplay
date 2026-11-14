param(
    [string]$DestinationPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'sdk')
)

$ErrorActionPreference = 'Stop'
$archive = Join-Path ([IO.Path]::GetTempPath()) ('aimp-sdk-' + [Guid]::NewGuid().ToString('N') + '.zip')
try {
    Invoke-WebRequest -Uri 'https://aimp.ru/?do=download.file&id=15' -OutFile $archive
    $expected = '5ecf7225f522a4c2de45b7b958d0edb4363b218caccfd141289bd5e675bb50d1'
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
