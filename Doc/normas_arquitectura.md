# TCM — Normas de arquitectura

Documento **normativo** del proyecto. Define las reglas que rigen diseño, comunicación entre módulos, estados, errores, documentación y cambios de código.

No es un catálogo (opcodes, EXXX, GPIO). Esos viven en:

| Fuente | Contenido |
|--------|-----------|
| `Doc/TCM - D.xlsx` | Opcodes, Error list, arquitectura de referencia |
| `Doc/gpio_list_updated.md` | Pines GPIO por módulo |
| Headers `*States.h` / HMI | Enums y bytes en firmware / Main |

Si algo no encaja aquí, **se actualiza este documento antes** de improvisar en código.

---

## 1. Arquitectura general

### Módulos

El sistema es modular: **Motion**, **PLC**, **PreFeeder**, **Andon**, orquestados por **HMI / Main** (ciclo + UI).

- Cada esclavo ejecuta su dominio (mover, válvulas, buffer, torre).
- **Main** decide el lote, la política ante fallos y qué estado de máquina ven los demás.
- Un módulo **no** necesita el detalle interno de otro: solo comandos y estados que le afecten.

### Regla A1 — Responsabilidad acotada

Cada módulo solo conoce lo necesario para su función.  
Ejemplo: PreFeeder no recibe “el ASDA no llegó”; recibe Stop / ErrorState / Idle según lo que Main publique.

### Regla A2 — Main interpreta la política; el EXXX es identidad compartida

La **política** C1/C2/C3 (stop all, pause, finish step, secuencias de salida) la aplica **solo HMI / Main**.  
El **código EXXX** es la identidad del fallo del módulo: el esclavo lo genera y Main lo muestra.  
Entre esclavos no se reenvía el EXXX; se alinean por **estado**.

---

## 2. Comunicación: flip-flop, no sondeo

### Regla C1 — Evento Set / Res

Estados, errores y condiciones relevantes **no** se preguntan en bucle (“¿sigues en error?”).  
Se trata como **flip-flop**:

| Señal | Significado |
|-------|-------------|
| **Set** | Un evento activa la condición (sensor, timeout, comando fallido, enlace caído, cambio de estado). |
| **Res** | Reset / comando explícito desde Main limpia la condición. |

Mientras no haya **Set**, no hay acción obligatoria. No inventar polling de “por si acaso”.

**Excepción de enlace:** el heartbeat TCP (`ping`/`pong` o keepalive) solo verifica que el socket vive.  
No es sondeo de sensores ni de estado de aplicación. Los sensores/válvulas se publican por **evento** (cambio) o status push del esclavo.

### Regla C2 — Opcode = contrato

Todo comando, estado o error de detalle que cruce TCP lleva **opcode** acordado en el Excel / Opcodes.  
No se inventan bytes sueltos en un `.ino` sin fila en la fuente de verdad.

### Regla C3 — PLC: válvulas por impulso (KEEP Set/Res)

En el esclavo PLC, cada comando de válvula (`on` / `off`) genera **un pulso** en el GPIO; la salida **no** queda enclavada en el ESP.  
El enclavado lo hace el PLC neumático (KEEP: Set / Res).  
- Querer **ON** (Set) → un pulso (solo si el estado lógico cambia a on).  
- Querer **OFF** (Res) → otro pulso (solo si cambia a off).  
- **Boot / Reset / All Off** = todas OFF (incluido holder). El holder lo activa rutina o manual.  
- Ancho de pulso: `VALVE_PULSE_MS` (100 ms) para que el KEEP lea bien.  
No pulsar si el estado lógico ya coincide (un pulso de más invertiría el KEEP).  
El estado lógico (JSON/status/UI) refleja la posición pretendida; el pin físico solo es impulso.  
HMI/ciclo siguen enviando `on: true|false` como hasta ahora.

**Excepción Blower:** no es KEEP. El pin queda **ON (nivel)** durante `durationSec` (ajustable en UI PLC) y luego **OFF** automático.

---

## 3. Estados

### Qué es un estado

Opcode de condición del módulo o de la máquina: Init, Idle, Busy, Error, Stop, Return (y en máquina: Start, Reset, Finish, Materialist, …).

Informa **qué está pasando** sin explicar el detalle del fallo.

### Regla E1 — Estado ≠ EXXX

ErrorState (`0x0C` / `0x27` / `0x3C` / `0x46`, etc.) es **estado**, no entrada del catálogo EXXX.  
La UI no muestra “E###: módulo en falla genérica” cuando existe (o debió existir) el detalle concreto.

### Regla E2 — Propagación por estado

Cuando Main debe alinear al resto: publica estado de máquina (p. ej. Error `0x46`, Stop) y/o comandos Stop/Reset de módulo.  
No reenvía el EXXX a todos los esclavos.

---

## 4. Errores

### Clasificación

Los errores de detalle se clasifican en **C1**, **C2** y **C3**. La clase define la reacción de Main y la **única** secuencia de salida válida.

| Clase | Al Set (activar) | Salida |
|-------|------------------|--------|
| **C1** | Stop inmediato a **todos** los módulos | Secuencia C1 fija |
| **C2** | Pausar; **no** lanzar el siguiente step | Secuencia C2 fija |
| **C3** | Terminar el paso en curso de forma segura, luego detener | Secuencia C3 fija |

### Forma en UI

**`EXXX: Module, Descripción`**

Misma cadena (o al menos el mismo **EXXX** + descripción oficial) en:

- HMI / Main  
- HTML local del módulo (p. ej. IP del PreFeeder, Motion, PLC)

### Regla R0 — Un EXXX, un nombre, todas las pantallas

Cada módulo es dueño de **sus** errores EXXX (los de su dominio en el Excel).  
Si ocurre un fallo en PreFeeder y abres la IP/HTML de ese módulo, debes ver **el mismo EXXX** (y la misma descripción de catálogo) que verías en Main — no un alias, slug legacy, ni otro texto inventado (“Buffer Full” vs `E052: Pre-Feeder, Buffer sin relleno`).

- Prohibido: dos nombres distintos para el mismo opcode/fallo (uno en web local, otro en HMI).  
- Permitido: en la web local mostrar solo los EXXX **de ese módulo**; Main muestra cualquier EXXX que reciba.  
- El HTML local no inventa catálogo paralelo: usa el mismo código/descripción que Error list.
- Firmware local debe exponer en JSON al menos `exxx` + `ui` (`EXXX: Module, Descripción`) para que la web del módulo no reinvente textos.

### Regla R1 — Fuente de verdad del catálogo

Errores oficiales = hoja **Error list** (+ Opcodes) en `Doc/TCM - D.xlsx`.  
Deben tener opcode, enum, handler y clase. Secuencia de códigos según el Excel.

### Regla R2 — Altas y bajas

- Alta: siguiente EXXX + byte asignado en Excel → enum en el módulo → catálogo HMI **y** UI local del módulo si tiene HTML.  
- Baja: marcado **Eliminar** → no implementar / quitar del código (Main y HTML local).  
- No crear EXXX solo para “estado genérico”.

### Regla R3 — Alcance del detalle en red

Detalle EXXX por TCP → **solo Main/HMI** (no se reenvía a otros esclavos).  
En la **propia** web del módulo sí se muestra su EXXX activo (misma identidad).  
Resto de módulos en red → estados / stop / reset.

### Regla R4 — Salida fija (no variable)

Salir de un error **solo** con la secuencia de su clase.  
No hay caminos distintos por EXXX, por módulo ni por atajo de operador.  
Cambiar un paso u orden = cambiar **este documento** primero.

#### Secuencia C1

1. Ya hubo stop a todos (Error/Stop de máquina).  
2. Corregir causa física si aplica.  
3. UI muestra EXXX y exige **confirmación**.  
4. Operador confirma.  
5. **Reset** (Res): limpia latch + reset de módulos.  
6. **Validar** estados coherentes.  
7. **Homing general** obligatorio.  
8. Idle / aceptar Start.

Prohibido: Reset sin confirmación; Start sin home; saltar validación.

#### Secuencia C2

1. Proceso pausado; no hubo siguiente step.  
2. Corregir causa si aplica.  
3. **Reset** (Res).  
4. **Resume**.  
5. Proceso desde **step 0**.

Prohibido: Resume sin Reset; continuar “donde iba”.

#### Secuencia C3

1. Paso en curso terminó seguro; luego pausa/detención.  
2. Corregir causa si aplica.  
3. **Reset** (Res).  
4. **Resume**.  
5. **Reintentar el proceso actual** (no home C1 ni “desde step 0” C2).

Prohibido: cortar el paso seguro con atajos; tratar C3 como C1 o C2 sin cambiar esta norma.

Orden resumido:

- **C1:** Confirmación → Reset → Validar → Homing → Idle/Start  
- **C2:** Reset → Resume → desde step 0  
- **C3:** (paso seguro OK) → Reset → Resume → reintentar proceso  

---

## 5. Andon / torre

### Regla T1

Andon refleja **estado de máquina** (`0x40`–`0x49`), no el listado EXXX.  
Un sensor propio de Andon (p. ej. presión) puede reportar EXXX a Main; la torre sigue lo que Main mande como estado.

---

## 6. Documentación y GPIO

### Regla D1

Pines y dirección I/O: `Doc/gpio_list_updated.md` (y Excel I-O si aplica).  
No hardcodear pines “porque sí” sin alinear la doc.

### Regla D2

Opcodes y errores: Excel primero; código después.  
Docs derivados (`errores_por_modulo.md`, etc.) no sustituyen la norma ni el Excel.

---

## 7. Código y mantenimiento

### Regla M1 — Dónde se define el contrato

Opcodes/enums de cada módulo en su `*States.h` (o equivalente HMI).  
Política C1/C2/C3 y flip-flop de error en Main/HMI.

### Regla M2 — Sin ruido de depuración

No agregar mensajes de debug (logs verbosos, spam, prints de sondeo) salvo pedido explícito.  
No dejar comentarios de debug / restos de prueba en archivos salvo que se requiera.

### Regla M3 — Cambios normativos

Todo cambio de arquitectura (comunicación, clases, secuencias de salida, alcance de detalle) se refleja **aquí** primero.  
Un PR de error nuevo: Excel → enum/opcode → HMI → clase coherente. Sin eso, no es oficial.

### Regla M4 — Norma ↔ archivos alineados

Cada norma vigente debe reflejarse en los **archivos necesarios**, no solo en este documento.

| Tipo de norma | Debe quedar alineado en |
|---------------|-------------------------|
| Contrato opcode / estado / EXXX | Excel (`Doc/TCM - D.xlsx`) + `*States.h` / HMI + HTML local del módulo si aplica |
| Política C1/C2/C3 y Set/Res | HMI/Main (`error_policy`, ciclo, catálogo) |
| GPIO / I-O | `Doc/gpio_list_updated.md` (+ Excel I-O) y firmware del módulo |
| Comportamiento de esclavo | `.ino` / headers del módulo afectado |
| Resumen para el agente | `.cursor/rules/tcm-arquitectura.mdc` (si cambia una prohibición dura) |

Prohibido: documentar una norma nueva o cambiada y dejar código, Excel o UI local en el comportamiento anterior.  
Al cerrar un cambio normativo: listar qué archivos se actualizaron para cumplirla.

---

## 8. Resumen

| Tema | Norma corta |
|------|-------------|
| Quién manda el lote / política | Main / HMI |
| Cómo se avisa sin detalle | Opcode de estado |
| Cómo se dice qué falló | EXXX (mismo en Main y HTML local del módulo) |
| Quién aplica C1/C2/C3 | Solo Main |
| Cómo se activa/limpia | Flip-flop Set / Res — no polling |
| PLC válvulas | Pulso ON / pulso OFF — sin enclavado de GPIO |
| Cómo se sale de un fallo | Secuencia fija C1 / C2 / C3 |
| De dónde salen códigos | Excel + GPIO doc |
| Debug | Solo si se pide |
| Norma nueva/cambiada | Reflejar en todos los archivos necesarios (M4) |

Si una decisión no cabe en esta tabla, se actualiza **este documento** antes que el código.
