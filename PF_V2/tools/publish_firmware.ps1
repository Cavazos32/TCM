# Uso (desde la raíz del repo TCM):
#   .\PF_V2\tools\publish_firmware.ps1
#
# Lee FW_VERSION de PF_V2/PF_LR/Config.h, commit+push si hay cambios,
# crea/actualiza tag v<version> y lo sube → GitHub Actions publica el Release OTA.
param(
  [switch]$SkipCommit
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$config = Join-Path $Root "PF_V2\PF_LR\Config.h"
if (-not (Test-Path $config)) { throw "No encontré $config" }
$m = [regex]::Match((Get-Content $config -Raw), '#define\s+FW_VERSION\s+"([^"]+)"')
if (-not $m.Success) { throw "FW_VERSION no encontrado en Config.h" }
$version = $m.Groups[1].Value.Trim().TrimStart("v", "V")
$tag = "v$version"

Write-Host "Publicar firmware PF_V2 tag=$tag (FW_VERSION=$version)"

if (-not $SkipCommit) {
  $status = git status --porcelain PF_V2 PF_V2/tools .github
  if ($status) {
    git add PF_V2 .github/workflows/pf-v2-ota-release.yml 2>$null
    git add -u PF_V2 2>$null
    git commit -m "PF_V2: release firmware $tag"
  }
  git push origin main
}

git tag -f $tag -m "PF_V2 PF_LR $tag"
git push origin $tag --force

Write-Host ""
Write-Host "Tag $tag enviado. El workflow 'PF_V2 OTA Release' compila y sube el Release."
Write-Host "Ver: https://github.com/Cavazos32/TCM/actions"
