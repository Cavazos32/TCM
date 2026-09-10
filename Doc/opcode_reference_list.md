# OpCode Reference List

| # | Byte | Action | Actuator | Module | Ejecuta | Direccion | Descripcion |
|---|---|---|---|---|---|---|---|
| 1 | 0x01 | Search home | ASDA | Motion | HomeASDA() | Recibe | Busca el home |
| 2 | 0x02 | Stop | ASDA | Motion | StopASDA() | Recibe | Detiene ASDA |
| 3 | 0x03 | Servo off | ASDA | Motion | OffASDA() | Recibe | Servo apagado |
| 4 | 0x04 | Servo on | ASDA | Motion | OnASDA() | Recibe | Servo energizado |
| 5 | 0x05 | Move to position | ASDA | Motion | ABSPositionASDA() | Recibe | Mover a posicion definida |
| 6 | 0x06 | Position reached | ASDA | Motion | ReachedASDA() | Manda | Llegue a la posicion |
| 7 | 0x07 | Move to 0 | ASDA | Motion | HpASDA() | Recibe | Setear posicion a 0 e ir |
| 8 | 0x08 | Estatus | ASDA | Motion | GetStatus() | Recibe | Cual es tu estatus? |
| 9 | 0x09 | Init | Motion | Motion | InitState() | Recibey manda | Estoy iniciando |
| 10 | 0x0A | Idle | Motion | Motion | IdleState() | Recibey manda | Estoy esperando |
| 11 | 0x0B | Busy | Motion | Motion | BusyState() | Recibey manda | Estoy trabajando |
| 12 | 0x0C | Error | Motion | Motion | ErrorState() | Recibey manda | Tengo un problema |
| 13 | 0x0D | Stop | Motion | Motion | StoprState() | Recibey manda | Me detuvieron |
| 14 | 0x0E | ReturnStop | Motion | Motion | ReturnState() | Recibey manda | Terminar de completar el mov. Despues del stop |
| 15 | 0x0F | Measure (R) | Encoder | Motion | GetMeasured() | Recibey manda | Lee, define y manda posicion |
| 16 | 0x10 | Set0 (R) | Encoder | Motion | Set0() | Recibe | Resetea cuentas |
| 17 | 0x11 | Error | Encoder | Motion | Error() | Manda | No detecto cambio en lectura |
| 18 | 0x12 | Feed (R) | Feeder | Motion | StartFeedR() | Recibe | Inicia a alimentar lado R |
| 19 | 0x13 | Feed (L) | Feeder | Motion | StartFeedL() | Recibe | Inicia a alimentar lado L |
| 20 | 0x14 | LenghtOk (L) | Feeder | Motion | LenghtOK() | Manda | Target reached or in tolerance |
| 21 | 0x15 | LenghtNG (L) | Feeder | Motion | LenghtNG_L() | Manda | Target not reached, out on tolerances |
| 22 | 0x16 | Reset | Motion | Motion | Reset() | Recibe | Reseteo de errores |
| 23 | 0x17 | Measure (L) | Encoder | Motion | GetMeasured() | Recibey manda | Lee, define y manda posicion |
| 24 | 0x18 | Set0 (L) | Encoder | Motion | Set0() | Recibe | Resetea cuentas |
| 25 | 0x19 | Cutter (R) | Out | PLC | CutterR() | Manda | Activa valvula |
| 26 | 0x1A | Cutter (L) | Out | PLC | CutterL() | Manda | Activa valvula |
| 27 | 0x1B | Gripper | Out | PLC | Gripper() | Manda | Activa valvula |
| 28 | 0x1C | Holder | Out | PLC | Holder() | Manda | Activa valvula |
| 29 | 0x1D | Encoder | Out | PLC | Encoder() | Manda | Activa valvula |
| 30 | 0x1E | Reset | Out | PLC | ResetPLC() | Manda | Resetea error PLC |
| 31 | 0x1F | Cutter Error | IN | PLC | CutterE() | Recibe | Muestra error recibido de PLC |
| 32 | 0x20 | Gripper Error | IN | PLC | GripperE() | Recibe | Muestra error recibido de PLC |
| 33 | 0x21 | Holder Error | IN | PLC | HolderE() | Recibe | Muestra error recibido de PLC |
| 34 | 0x22 | Encoder Error | IN | PLC | EncoderE() | Recibe | Muestra error recibido de PLC |
| 35 | 0x23 | Blower | Out | PLC | BlowerE() | Manda | Activa valvula |
| 36 | 0x24 | Init | Estatus | PLC | InitState() | Recibey manda | Estoy iniciando |
| 37 | 0x25 | Idle | Estatus | PLC | IdleState() | Recibey manda | Estoy esperando |
| 38 | 0x26 | Busy | Estatus | PLC | BusyState() | Recibey manda | Estoy trabajando |
| 39 | 0x27 | Error | Estatus | PLC | ErrorState() | Recibey manda | Tengo un problema |
| 40 | 0x28 | Stop | Estatus | PLC | StoprState() | Recibey manda | Me detuvieron |
| 41 | 0x29 | ReturnStop | Estatus | PLC | ReturnState() | Recibey manda | Terminar de completar el mov. Despues del stop |
| 42 | 0x2A | Start | In | PreFeeder | Start() | Recibe | Inicia proceso |
| 43 | 0x2B | Stop | In | PreFeeder | Stop() | Recibe | Detiene todo proceso |
| 44 | 0x2C | Reset | In | PreFeeder | ResetPF() | Recibe | Resetea errores, estado general. |
| 45 | 0x2D | Buffer Full (R) | Out | PreFeeder | BufferFR() | Manda | Muestra error |
| 46 | 0x2E | Buffer Max (R) | Out | PreFeeder | BufferMR() | Manda | Muestra error |
| 47 | 0x2F | Tensioner (R) | Out | PreFeeder | TensionerR() | Manda | Muestra error |
| 48 | 0x30 | Cilindro (R) | Out | PreFeeder | CilindroR() | Manda | Muestra error |
| 49 | 0x31 | Manguera ausente (R) | Out | PreFeeder | MangueraR() | Manda | Muestra error |
| 50 | 0x32 | Holgura (R) | Out | PreFeeder | HolguraR() | Manda | Muestra error |
| 51 | 0x33 | Buffer Full (L) | Out | PreFeeder | BufferFL() | Manda | Muestra error |
| 52 | 0x34 | Buffer Max (L) | Out | PreFeeder | BufferML() | Manda | Muestra error |
| 53 | 0x35 | Tensioner (L) | Out | PreFeeder | TensionerL() | Manda | Muestra error |
| 54 | 0x36 | Cilindro (L) | Out | PreFeeder | CilindroL() | Manda | Muestra error |
| 55 | 0x37 | Manguera ausente (L) | Out | PreFeeder | MangueraL() | Manda | Muestra error |
| 56 | 0x38 | Holgura (L) | Out | PreFeeder | HolguraL() | Manda | Muestra error |
| 57 | 0x39 | Init | Estatus | PreFeeder | InitState() | Recibey manda | Estoy iniciando |
| 58 | 0x3A | Idle | Estatus | PreFeeder | IdleState() | Recibey manda | Estoy esperando = bloqueo de acciones |
| 59 | 0x3B | Busy | Estatus | PreFeeder | BusyState() | Recibey manda | Estoy trabajando = In production actual |
| 60 | 0x3C | Error | Estatus | PreFeeder | ErrorState() | Recibey manda | Tengo un problema |
| 61 | 0x3D | Stop | Estatus | PreFeeder | StoprState() | Recibey manda | Me detuvieron |
| 62 | 0x3E | ReturnStop | Estatus | PreFeeder | ReturnState() | Recibey manda | Terminar de completar el mov. Despues del stop |
| 63 | 0x3F | Materialist | Estatus | PreFeeder | Materialist () | Recibe | Materialist = Materialist actual |
| 64 | 0x40 | Init | Estatus | Machine | InitState() | Recibe | Green |
| 65 | 0x41 | Start | Estatus | Machine | StartCycle() | Recibe | N/A |
| 66 | 0x42 | Stop | Estatus | Machine | StopCycle() | Recibe | Red |
| 67 | 0x43 | Reset | Estatus | Machine | ResetCycle() | Recibey manda | N/A |
| 68 | 0x44 | Idle | Estatus | Machine | IdleState() | Recibey manda | Green |
| 69 | 0x45 | Busy | Estatus | Machine | BusyState() | Recibey manda | Green |
| 70 | 0x46 | Error | Estatus | Machine | ErrorState() | Recibey manda | Red + Buzzer |
| 71 | 0x47 | FinishParts | Estatus | Machine | LotCompleate() | Manda | Green + Buzzer |
| 72 | 0x48 | ReturnStop | Estatus | Machine | ReturnState() | Recibey manda | N/A |
| 73 | 0x49 | Materialist | Estatus | Machine | Materialist() | Recibey manda | Yellow + Buzzer |
| 74 | 0x4A | LenghtOk (r) | Encoder | Motion | LenghtOK_R() | Manda | Target reached or in tolerance |
| 75 | 0x4B | LenghtNG (r) | Encoder | Motion | LenghtNG_R() | Manda | Target not reached, out on tolerances |
| 76 | 0x4C | Trigger Feed (R) | PreFeeder | PreFeeder | TriggerR() | Recibe | Trigger to feed lado R |
| 77 | 0x4D | Laser (R) | in | Motion | LaserR() | Recibe | Material/fault laser R (GPIO 32) |
| 78 | 0x4E | Laser (L) | in | Motion | LaserL() | Recibe | Material/fault laser L (GPIO 34) |
| 79 | 0x4F | Safety exhaust | Motion | Motion | Exhaust() | Manda | Error desfoga el aire |
| 80 | 0x50 | Pressure Sensor FRL | Andon | Andon | PressureError() | Recibe | Baja / nula presion de aire general |
| 81 | 0x51 | Trigger Feed (L) | PreFeeder | PreFeeder | TriggerL() | Recibe | Trigger to feed lado L |
