[18:55:42] [MAQUINA] [INFO] Modo prueba en vacío OFF (bypass sensores/encoder)
[18:55:42] [MAQUINA] [INFO] Delays — holderOn=45ms open=20ms grippers=20ms cutter=150ms postCut=25ms linear=20ms dwell=35ms gripRel=50ms asentar=30ms · feedSides=L · refill=56mm asda=-300mm
[18:55:42] [MAQUINA] [ERROR] (0x040) Cycle Start (0x040) length=-265.0 mm qty=1
[18:55:42] [MAQUINA] [INFO] prepareBeforeCut: cutters/grippers safe + Holder/Encoder cerrados
[18:55:42] [MAQUINA] [CMD] ASDA ya en 0 (pos=0.12 mm) — sin MOVE_ZERO al Start
[18:55:42] [MAQUINA] [INFO] PreFeeder: In process ON (ciclo Busy)
[18:55:42] [MAQUINA] [INFO] Delay · Delay Holder ON: 45 ms
[18:55:42] [MAQUINA] [CMD] Feed start lados=L
[18:55:42] [ANDON] [INFO] BusyState
[18:55:43] [MAQUINA] [INFO] Delay · Delay tras cerrar pinzas: 20 ms
[18:55:43] [MAQUINA] [INFO] enc_set0: omitido (lineal TCP no usa OM)
[18:55:43] [MAQUINA] [INFO] Holder+Encoder OFF (abren para lineal)
[18:55:43] [MAQUINA] [INFO] Delay · Delay Holder/Encoder OFF: 20 ms
[18:55:43] [MAQUINA] [INFO] Lineal MOVE TCP modelMm=-265.0 cutOffset→targetAbsMm=257.5 sides=L rpm=3000
[18:55:45] [MAQUINA] [RX] Lineal MOVE TCP OK targetAbsMm=257.5 rpm=3000
[18:55:45] [MAQUINA] [INFO] Delay · Delay antes del corte: 20 ms
[18:55:45] [MAQUINA] [INFO] Holder+Encoder ON (cierran pre-corte)
[18:55:45] [MAQUINA] [INFO] Delay · Delay tras cerrar holder: 45 ms
[18:55:45] [MAQUINA] [INFO] Cortador ON (Set) lados=L
[18:55:45] [MAQUINA] [INFO] Delay · Delay entre Set y Res cortador: 150 ms
[18:55:45] [MAQUINA] [INFO] Cortador OFF (Res) lados=L
[18:55:45] [MAQUINA] [INFO] Delay · Delay post-corte: 25 ms
[18:55:45] [MAQUINA] [CMD] WIP start ref=deposit_target 387.5 mm (post Idle/Reached)
[18:55:45] [MAQUINA] [INFO] Delay · Delay tras depósito: 35 ms
[18:55:45] [MAQUINA] [RX] (0x4C) trigger PreFeeder Tfeed lados=L — R(0x4C)=omit L(0x51)=ok
[18:55:45] [MAQUINA] [INFO] Delay · Delay tras abrir pinzas: 50 ms
[18:55:46] [MAQUINA] [INFO] Despeje post-pinzas → 392.5 mm (+|clearance|=5.0); WIP fin ref=392.5 mm
[18:55:46] [MAQUINA] [INFO] WIP Delivery: fin(grippers)=392.5 → offset soplo=294.5 → inicio(cortador)=98.0 → HOME (0.25s c/u, offset=98.0)
[18:55:47] [MAQUINA] [RX] WIP Delivery move ok @ fin_offset=294.5 mm
[18:55:47] [MAQUINA] [INFO] WIP Delivery blower ON @ fin=294.5 mm hold=0.25s
[18:55:47] [MAQUINA] [RX] WIP Delivery blower OFF ok @ fin=294.5 mm
[18:55:48] [MAQUINA] [RX] WIP Delivery move ok @ inicio=98.0 mm
[18:55:48] [MAQUINA] [INFO] WIP Delivery blower ON @ inicio=98.0 mm hold=0.25s
[18:55:48] [MAQUINA] [RX] WIP Delivery blower OFF ok @ inicio=98.0 mm
[18:55:48] [MAQUINA] [INFO] Delay · Delay asentar: 30 ms
[18:55:49] [MAQUINA] [RX] Pieza 1/1 OK
[18:55:49] [MAQUINA] [INFO] Finish: Holder/Encoder cerrados; cutters/grippers OFF
[18:55:49] [MAQUINA] [INFO] PreFeeder: espera settled antes de Idle · L:buffer≠Full