# Trigger PreFeeder (Tfeed)

El **trigger** es el Tfeed al PreFeeder (`0x4C` derecha / `0x51` izquierda). No es el Feed de Motion ni el *In process ON*.

Va en el **paso 3** de la pieza (no en el 22). El 22 solo abre pinzas. El 21 es prefetch de alimentación Motion.

**Interruptor de ciclo** (`pfTriggerEnabled` en `cycle_config.json`, UI Cycle → Material handling): **ON** (default) manda Tfeed entre piezas. **OFF** omite siempre el paso 3; el PreFeeder se queda solo con el helper de holgura. No es `Tfeed (s) = 0` en el HTML de L/R: ese valor se clampa a 0.1 s y el feeder igual arranca.

**Regla de lote:** la 1ª pieza ya trae feed de referencia (el operador lo dejó, o el Tfeed de la última pieza del lote anterior). Por eso la 1ª **omite**. Las demás, incluida la última, **mandan** Tfeed si el interruptor está ON: así el lote siguiente puede arrancar sin otra referencia.

Si **holgura** está ausente o choca con un Tfeed, holgura gana siempre. Es aviso de que algo va mal: se le hace caso y se prioriza su recuperación (helper, con sus settings). El ciclo no “arregla” holgura mandando otro trigger.

---

## Trigger de ciclo (paso 3)

| Acción | Resultado | Por qué |
|---|---|---|
| 1ª pieza del lote (inicio de proceso, paso 3) | **Omite** | El operador ya dejó longitud de referencia; el feed de esa pieza es el adecuado. Mandar Tfeed aquí es un trigger de más. |
| `pfTriggerEnabled` = OFF | **Omite** | El operador quiere solo holgura. No se manda 0x4C/0x51. |
| Piezas intermedias | **Trigger** a `feedSides` | Reponer para la siguiente. Si el interruptor está OFF, omite. |
| Última pieza del lote | **Trigger** a `feedSides` | Deja el feed listo para el lote siguiente. Sin este Tfeed (y con interruptor ON), el próximo lote no tendría referencia. Si OFF, omite. |
| C2 Resume (reinicio de la pieza desde 0) | **Omite** | El Tfeed de esa pieza ya se mandó; no hace falta otro. |
| C3: misma pieza que ya pasó el paso 3 | **Omite** | No vuelve al paso 3. |
| C3 Resume → pieza siguiente | **Trigger** | Esa pieza entra por el paso 3 (si no es la 1ª del lote). |
| Pause / Resume normal (sin C2) | **Omite** | Sigue donde iba; no re-manda. Pause/Error desarman PF (`In process OFF` → Idle); Resume rearma In process y espera Buffer Full (como Start), no Tfeed. |
| C1, Stop o aborto del lote | **Omite** | Las piezas que no se ejecutan no reciben Tfeed. |
| Error a mitad de pieza (sin C2) | **Omite** | El lote se corta; no hay Tfeed en el resto. |
| PreFeeder sin enlace | **Omite** | No hay a quién mandar. |
| Lado no seleccionado (`feedSides` = solo L o solo R) | **Omite** ese lado | El otro sí recibe, si toca trigger. |
| Refill / purga | **Omite** | No es ciclo de pieza. |
| Botones manuales Trigger L / R / L+R | **Trigger** | Fuera del ciclo; el operador lo pide. |

Lote de 1 pieza: es la 1ª → **omite** (la referencia la deja el lote anterior o el operador).

---

## Holgura (prioridad sobre Tfeed)

El helper de holgura vive en el PreFeeder (L/R), no en el paso 3 del ciclo. Usa **sus** settings (RPM, duración, umbral de ausencia). No usa Tfeed.

Holgura ausente = aviso de que algo va mal. Se le hace caso. Si choca con un trigger, o ya sabemos que está ausente, **holgura gana**.

| Situación | Qué ocurre |
|---|---|
| Sensor de holgura **ON** (presente / OK) | El helper no corre. El Tfeed de ciclo, si toca, puede pasar. |
| Sensor **OFF** (ausente) ≥ umbral | Lanza helper holgura con sus settings. El ciclo **no** manda Tfeed para “arreglarlo”. |
| Tfeed llega **mientras** el helper corre | Se descarta el trigger. Holgura no se corta ni se reinicia. |
| Tfeed y holgura coinciden (choque) | **Holgura**. |
| Holgura ausente y el ciclo “querría” Tfeed (paso 3, C2, etc.) | **Holgura**. No se fuerza trigger. |
| Relleno (Buffer Full **OFF**, p. ej. Start esperando Full) | Helper puede correr. **No** enclava E057/E063: aún no hay lazo y la máquina no está produciendo. |
| Ausencia se alarga más allá del helper **y** Buffer Full ya está ON | Falla de holgura (PF), no otro Tfeed. |

El ciclo **no** tiene un Tfeed extra “si falta holgura” (pre-pieza / post-pieza). Eso sería tratar el aviso como si se curara con el mismo trigger de producción.

---

## Lo que no es trigger

| Acción | Qué es |
|---|---|
| Arranque de lote: Start + *In process ON* | Arma sensores. **Espera Buffer Full confirmado** (1ª vez de cada Start) antes de la 1ª pieza. No es Tfeed. |
| Resume máquina (Pause/Error) | Rearma In process. **Espera Buffer Full confirmado** (igual que Start) antes de seguir. No es Tfeed. |
| Paso 4 — Feed / handoff | Alimentación Motion. |
| Paso 21 — Prefetch (piezas 1…N−1) | Feed Motion en paralelo. |
| Paso 21 — última pieza | Prefetch Motion **omitido**. |
| C2 Resume con láser ya ON | Omite el **Feed** Motion. El Tfeed también se omite (ya se mandó). |
| Fin de lote OK | Espera Buffer Full + holgura y *In process OFF*. Sin Tfeed extra. |

---

**Regla corta:** Con `pfTriggerEnabled` ON, Tfeed en paso 3 de cada pieza **excepto** la 1ª del lote y excepto C2 (ya se mandó). La última sí manda, para el lote siguiente. Con OFF, el paso 3 siempre omite. Holgura ausente o en curso **siempre** gana al trigger.
