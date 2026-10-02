# Arquitectura PreFeeder — post-refactor (2026-10-02)

## COMMUNICATION IMPACT

```text
COMMUNICATION IMPACT
Componente protegido: PreFeeder Master TCP + PF_LR peer TCP/HTTP
Cambio propuesto: Comm/Control/Feeder en PF_LR; Master permanece bridge síncrono
Relación causal: colas FreeRTOS Master causaron regresión de enlace (clase A) → revertidas
Riesgo: PF_LR Comm task mal dimensionada; validar HTML local + status peer
Alternativa: Master con colas (descartada en banco)
```

## Tareas

### Master (ESP32) — bridge síncrono (estable)

| Función | Rol |
|---------|-----|
| `loop()` | HTTP + WiFi + accept HMI + RX/TX L/R + keepalive (mismo modelo pre-colas) |

> **Nota 2026-10-02:** las colas FreeRTOS RX/TX del Master se **revirtieron**. Provocaban flapping TCP, “esperando status”, HTTP `failed to fetch` y heap/mutex contention. El contrato externo (TCP HMI :8768, L/R :8765) no cambia. Separación Comm/Control se mantiene en **PF_LR**.

### PF_LR

| Tarea | Core | Rol |
|-------|------|-----|
| `communicationTask` | 0 | WiFi / peer TCP / HTTP → CmdQueue / triggerQ |
| Control (`loop`) | 1 | AUTO, Servo, DeReeler, Buffer, Safety, Refill |
| `feederTask` | 1 | Solo TFEED (Feeder M2) |

## Holgura

Eliminada del producto (sensor, helper, EXXX E057/E063 de catálogo activo). Bytes 0x32/0x38 reservados.

## TFEED

Independiente de `autoEnabled` / InProcess / `sensorsMotionArmed`. Gates: `systemFault`, Buffer Max, enlace, cola.

## Servo

Dueño lógico Control: `requestServoRun` / `requestServoStopNeutral` / `servoApply`. STOP = PWM 1500 µs continuo.
