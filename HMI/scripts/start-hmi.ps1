# TCM HMI - servidor en ESTA ventana (no cerrar) y abre el navegador
param(
    [int]$Port = 5050,
    [switch]$NoBrowser
)

$ErrorActionPreference = 'Continue'
$HmiRoot = Split-Path $PSScriptRoot -Parent
$Url = "http://127.0.0.1:$Port"
$DistIndex = Join-Path $HmiRoot 'frontend\dist\index.html'

function Test-HmiPort {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $connect = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        $ok = $connect.AsyncWaitHandle.WaitOne(400, $false)
        if (-not $ok) { return $false }
        $client.EndConnect($connect)
        return $true
    } catch {
        return $false
    } finally {
        if ($client.Connected) { $client.Close() }
    }
}

function Get-PythonExe {
    $venvPy = Join-Path $HmiRoot '.venv\Scripts\python.exe'
    if (Test-Path $venvPy) { return $venvPy }
    return 'python'
}

function Open-Browser {
    if (-not $NoBrowser) {
        Start-Process $Url
    }
}

if (Test-HmiPort) {
    Write-Host "TCM HMI ya esta en ejecucion ($Url)"
    Write-Host "cycle.py / app.py NO se recargan: cierra la ventana del servidor y vuelve a iniciar."
    Open-Browser
    Write-Host "Si la pagina esta en blanco: Ctrl+F5"
    Write-Host "Esta ventana no es el servidor; se puede cerrar."
    Read-Host 'Enter para salir'
    exit 0
}

if (-not (Test-Path $DistIndex)) {
    Write-Host 'Compilando UI (frontend)...'
    Push-Location (Join-Path $HmiRoot 'frontend')
    try {
        npm run build
        if ($LASTEXITCODE -ne 0) {
            Write-Error 'npm run build fallo'
            Read-Host 'Enter para cerrar'
            exit 1
        }
    } finally {
        Pop-Location
    }
}

$python = Get-PythonExe
$env:PYTHONUNBUFFERED = '1'

if (-not $NoBrowser) {
    $waitCmd = "for (`$i=0; `$i -lt 60; `$i++) { Start-Sleep -Milliseconds 500; try { `$c = New-Object System.Net.Sockets.TcpClient; `$c.Connect('127.0.0.1',$Port); `$c.Close(); Start-Process '$Url'; break } catch {} }"
    Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -ArgumentList @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-Command', $waitCmd
    )
}

Write-Host "TCM HMI - $Url"
Write-Host 'Deja esta ventana abierta. Cerrarla apaga el HMI.'
Write-Host ''

Set-Location $HmiRoot
& $python -u app.py
$code = $LASTEXITCODE
if ($code -ne 0 -and $null -ne $code) {
    Write-Host ''
    Write-Host "El servidor salio con error $code"
    Read-Host 'Enter para cerrar'
}
exit $code
