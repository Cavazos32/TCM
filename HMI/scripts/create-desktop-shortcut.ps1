# Crea acceso directo en el Escritorio → Iniciar HMI.bat
$HmiRoot = Split-Path $PSScriptRoot -Parent
$Target = Join-Path $HmiRoot 'Iniciar HMI.bat'
$Desktop = [Environment]::GetFolderPath('Desktop')
$ShortcutPath = Join-Path $Desktop 'TCM HMI.lnk'

$WshShell = New-Object -ComObject WScript.Shell
$Shortcut = $WshShell.CreateShortcut($ShortcutPath)
$Shortcut.TargetPath = $Target
$Shortcut.WorkingDirectory = $HmiRoot
$Shortcut.Description = 'Iniciar servidor TCM HMI y abrir http://localhost:5050'
$Shortcut.IconLocation = "$env:SystemRoot\System32\shell32.dll,13"
$Shortcut.Save()

Write-Host "Acceso directo creado: $ShortcutPath"
