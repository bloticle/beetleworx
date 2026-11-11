$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$jar = Join-Path $root 'analysis/ffdec/ffdec.jar'
if (!(Test-Path -LiteralPath $jar)) {
    $archive = Join-Path $root 'analysis/ffdec.zip'
    New-Item -ItemType Directory -Path (Split-Path $archive -Parent) -Force | Out-Null
    Invoke-WebRequest -Uri 'https://github.com/jindrapetrik/jpexs-decompiler/releases/download/version26.4.0/ffdec_26.4.0.zip' -OutFile $archive
    $expected = '56CE960AEAFC7CAF034E2052BA49F878A883F7C7CA690DA6EEFFCA9587B5E4B9'
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'JPEXS archive checksum mismatch.' }
    Expand-Archive -LiteralPath $archive -DestinationPath (Split-Path $jar -Parent) -Force
}
if (!(Get-Command java -ErrorAction SilentlyContinue)) { throw 'Java 17 or newer is required to build the in-game PC menu.' }
