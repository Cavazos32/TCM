# TCM HMI — Flask + React

Interfaz maestro en Python (Flask) + React/Vite para los módulos de máquina vía TCP JSON.

| Módulo | IP:puerto por defecto |
|--------|------------------------|
| Motion | `10.10.32.20:8767` |
| PLC | `10.10.32.50:8766` |
| PreFeeder | `10.10.32.100:8768` |

## Estructura

```
HMI/
├── app.py              # Servidor Flask, API REST, SSE, sirve UI compilada
├── state.py            # Estado + lógica de enlace
├── cycle.py            # Secuencia de ciclo
├── motion.py           # Cliente TCP Motion
├── plc.py              # Cliente TCP PLC
├── prefeeder.py        # Cliente TCP PreFeeder
├── config/
│   ├── models.json
│   └── cycle_config.json
├── frontend/           # UI React (Vite + Tailwind) — fuente
│   └── dist/           # Build de producción (`npm run build`)
├── scripts/
│   ├── start-hmi.ps1
│   └── create-desktop-shortcut.ps1
├── deploy/
│   └── tcm-hmi.service # systemd (Raspberry Pi)
├── Iniciar HMI.bat     # Acceso directo Windows
└── requirements.txt
```

## Requisitos

- Python 3.10+
- Node.js 18+ (build/desarrollo de la UI)
- Módulos firmware con servidor TCP en la misma red

## Instalación

```bash
cd HMI
python3 -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -r requirements.txt

cd frontend
npm install
npm run build
```

## Ejecutar

```bash
cd HMI
python app.py
```

Abrir: **http://localhost:5050**

### Acceso directo (Windows)

Doble clic en **`Iniciar HMI.bat`**: inicia el servidor en una ventana nueva y abre el navegador. Si el servidor ya corre, solo abre la web.

Para un icono en el Escritorio:

```powershell
cd HMI
powershell -ExecutionPolicy Bypass -File scripts/create-desktop-shortcut.ps1
```

Crea **`TCM HMI.lnk`** en el Escritorio.

### Desarrollo UI (hot reload)

Terminal 1 — backend:
```bash
cd HMI && python app.py
```

Terminal 2 — frontend:
```bash
cd HMI/frontend && npm run dev
```

Abrir: **http://localhost:3000** (proxy `/api` → `:5050`).

## Funciones de la UI

- **Máquina**: Start/Stop/Resume, modelo, progreso de lote, log
- **Cycle**: secuencia 27 pasos, delays, presets, guardar en `config/cycle_config.json`
- **Motion**: ASDA B3, encoder, feeder, offsets
- **PLC**: válvulas, reset, all off
- **PreFeeder**: start/stop/reset/materialist, sensores L/R
- **Ajustes** (☰): tema, idioma ES/EN, enlaces TCP

Variables: `HMI_HOST` (default `0.0.0.0`), `HMI_PORT` (default `5050`).

## Raspberry Pi

1. Copiar `HMI/` a la RPi.
2. Ajustar IPs en `motion.py`, `plc.py`, `prefeeder.py`.
3. `npm run build` en `frontend/`, luego `python app.py`.
4. Arranque automático: `deploy/tcm-hmi.service`.
