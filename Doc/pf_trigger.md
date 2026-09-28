# Trigger PreFeeder (Tfeed)

El **trigger** es el Tfeed al PreFeeder (`0x4C` derecha / `0x51` izquierda). No es el Feed de Motion ni el *In process ON*.

Va **después del corte** (paso 18: post-corte, antes del depósito). Así el PreFeeder rellena el buffer mientras depósito / despeje / HOME. El 21 solo abre pinzas. El feed Motion de la siguiente es el paso 25 (tras HOME, ASDA=0).

**Interruptor de ciclo** (`pfTriggerEnabled` en `cycle_config.json`, UI Cycle → Material handling): **ON** (default) manda Tfeed tras el corte de cada pieza. **OFF** omite siempre el paso 18; el PreFeeder se queda solo con el helper de holgura. No es `Tfeed (s) = 0` en el HTML de L/R: ese valor se clampa a 0.1 s y el feeder igual arranca.

**Regla de lote:** la 1ª pieza ya trae feed de referencia (el operador lo dejó, o el Tfeed de la última pieza del lote anterior). El Tfeed **tras el corte** de esa 1ª (y de las demás) reponen para la siguiente. La última también **manda**, para que el lote siguiente arranque sin otra referencia.

Si **holgura** está ausente o choca con un Tfeed, holgura va primero: el helper no se corta. El Tfeed de ciclo **no se pierde**: Main lo manda igual y el esclavo lo deja pendiente hasta que termine el helper. El ciclo no manda triggers extra para “arreglar” holgura.

---

## Trigger de ciclo (paso 18, post-corte)

| Acción | Resultado | Por qué |
|---|---|---|
| 1ª pieza del lote (tras el corte) | **Trigger** a `feedSides` | Reponer durante depósito/HOME para la pieza 2 (o para el lote siguiente si N=1). |
| `pfTriggerEnabled` = OFF | **Omite** | El operador quiere solo holgura. No se manda 0x4C/0x51. |
| Piezas intermedias | **Trigger** a `feedSides` | Reponer para la siguiente. Si el interruptor está OFF, omite. |
| Última pieza del lote | **Trigger** a `feedSides` | Deja el feed listo para el lote siguiente. Sin este Tfeed (y con interruptor ON), el próximo lote no tendría referencia. Si OFF, omite. |
| C2 Resume (reinicio de la pieza desde 0) **antes** del Tfeed | **Trigger** | El corte de este intento aún no mandó Tfeed. |
| C2 Resume **después** de haber mandado Tfeed | **Omite** | El Tfeed de esa pieza ya se mandó; no hace falta otro. |
| C3: misma pieza que ya pasó el paso 18 | **Omite** | No vuelve al Tfeed. |
| C3 Resume → pieza siguiente | **Trigger** | Esa pieza entra por el flujo y, tras su corte, manda Tfeed. |
| Pause / Resume normal (sin C2) | **Omite** | Sigue donde iba; no re-manda. Pause/Error desarman PF (`In process OFF` → Idle); Resume rearma In process y espera Buffer Full (como Start), no Tfeed. |
| C1, Stop o aborto del lote | **Omite** | Las piezas que no se ejecutan no reciben Tfeed. |
| Error a mitad de pieza **antes** del Tfeed (sin C2) | **Omite** | El lote se corta; no hay Tfeed en el resto. |
| PreFeeder sin enlace | **Omite** | No hay a quién mandar. |
| Lado no seleccionado (`feedSides` = solo L o solo R) | **Omite** ese lado | El otro sí recibe, si toca trigger. |
| Refill / purga | **Omite** | No es ciclo de pieza. |
| Botones manuales Trigger L / R / L+R | **Trigger** | Fuera del ciclo; el operador lo pide. |

Lote de 1 pieza: tras el corte **manda** Tfeed (referencia para el lote siguiente).

---

## Holgura (prioridad sobre Tfeed)

El helper de holgura vive en el PreFeeder (L/R), no en el paso 18 del ciclo. Usa **sus** settings (RPM, duración, umbral de ausencia). No usa Tfeed.

Holgura ausente = aviso de que algo va mal. Se le hace caso. Si choca con un trigger, o ya sabemos que está ausente, **holgura gana**.

| Situación | Qué ocurre |
|---|---|
| Sensor de holgura **ON** (presente / OK) | El helper no corre. El Tfeed de ciclo, si toca, puede pasar. |
| Sensor **OFF** (ausente) ≥ umbral | Lanza helper holgura con sus settings. El ciclo **no** manda Tfeed para “arreglarlo”. |
| Tfeed llega **mientras** el helper corre | Queda pendiente; arranca al terminar el helper. Holgura no se corta ni se reinicia. |
| Tfeed y holgura coinciden (choque) | **Holgura** primero, luego Tfeed. |
| Holgura ausente en paso 18 | Main manda Tfeed igual; el esclavo lo ejecuta tras el helper. |
| Relleno (Buffer Full **aún no visto**, p. ej. Start esperando Full) | Helper puede correr. **No** enclava E057/E063: aún no hay lazo y la máquina no está produciendo. |
| Ausencia se alarga más allá del helper **después** de haber visto Buffer Full | Falla de holgura (PF), aunque Full luego se apague al producir. No otro Tfeed. |
| Tirón / helper / Tfeed pica el sensor un momento | **No** cancela el timeout. Solo holgura sostenida ≥ falla (s) con feeder idle borra el acumulado. |

El ciclo **no** tiene un Tfeed extra “si falta holgura” (pre-pieza / post-pieza). Eso sería tratar el aviso como si se curara con el mismo trigger de producción.

---

## Lo que no es trigger

| Acción | Qué es |
|---|---|
| Arranque de lote: Start + *In process ON* | Arma sensores. **Espera Buffer Full confirmado** (1ª vez de cada Start) antes de la 1ª pieza. No es Tfeed. |
| Resume máquina (Pause/Error) | Rearma In process. **Espera Buffer Full confirmado** (igual que Start) antes de seguir. No es Tfeed. |
| Continuar ciclo (tras recovery) | El prompt está en Pause (PF Idle). Al confirmar, Busy rearma In process y **espera Buffer Full** (igual que Start/Resume) antes de la siguiente pieza. |
| Paso 3 — Feed / handoff | Alimentación Motion (1ª / recovery; si ya hubo feed post-HOME, omite). |
| Paso 25 — Feed post-HOME (piezas 1…N−1) | Tras ASDA=0: Feed Motion de la siguiente (Tfeed ya fue en el paso 18 de esta pieza). |
| Paso 25 — última pieza | Feed Motion **omitido**. |
| C2 Resume con láser ya ON | Omite el **Feed** Motion. El Tfeed se omite solo si esa pieza ya lo mandó (post-corte). |
| Fin de lote OK | Espera Buffer Full + holgura y *In process OFF*. Sin Tfeed extra. |

---

**Regla corta:** Con `pfTriggerEnabled` ON, Tfeed **tras el corte** de cada pieza (incluida la 1ª y la última). C2 omite solo si esa pieza ya mandó Tfeed. Con OFF, el paso 18 siempre omite. Holgura en curso va primero; el Tfeed espera y no se pierde.
