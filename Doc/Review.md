[11:06:30] [PLC] [INFO] CMD Cutter L byte=0x1A (26) setOut=CUTTER_L → ON
[11:06:30] [PLC] [INFO] CMD Cutter L byte=0x1A (26) setOut=CUTTER_L → OFF
[11:06:41] [MAQUINA] [INFO] Delays — holderOn=120ms open=60ms grippers=60ms cutter=70ms postCut=60ms linear=60ms dwell=100ms gripRel=200ms asentar=30ms · feedSides=L
[11:06:41] [MAQUINA] [ERROR] (0x040) Cycle Start (0x040) length=-255.0 mm qty=1
[11:06:41] [MAQUINA] [INFO] prepareBeforeCut: cutters/grippers safe + Holder/Encoder cerrados
[11:06:41] [MAQUINA] [INFO] PreFeeder: ignorado (debug bypass)
[11:06:41] [MAQUINA] [INFO] Delay · Delay Holder ON: 120 ms
[11:06:41] [MAQUINA] [CMD] Feed start lados=L
[11:06:41] [ANDON] [INFO] BusyState
[11:06:42] [MAQUINA] [INFO] Delay · Delay tras cerrar pinzas: 60 ms
[11:06:42] [MAQUINA] [CMD] enc_set0: omitido (Stage2 RESET OM en Motion)
[11:06:42] [MAQUINA] [INFO] Holder/Encoder: se mantienen cerrados (sin abrir mid-pieza)
[11:06:42] [MAQUINA] [INFO] Delay · Delay (holder se mantiene): 60 ms
[11:06:42] [MAQUINA] [CMD] Stage2 START modelMm=-255.0 → targetAbsMm=255.0 sides=L; deposit abs sigue 255.0 mm
[11:06:43] [MAQUINA] [RX] Stage2 phase=WAIT_REACHED_1 gen=2
[11:06:43] [MAQUINA] [INFO] Stage2 phase=SETTLE_1 gen=2
[11:06:44] [MAQUINA] [RX] Stage2 phase=WAIT_REACHED_2 gen=2
[11:06:44] [MAQUINA] [INFO] Stage2 phase=SETTLE_2 gen=2
[11:06:45] [MAQUINA] [ERROR] Stage2 phase=DONE_NG gen=2
[11:06:45] [MAQUINA] [ERROR] Stage2 NG gen=2 piece=310.0 target=255.0 apPct=80.0 apMm=204.0 fast=3000 fine=800 omL=178.5 omR=178.5 avg=178.5 rem=76.5 fOmL=222.5 fOmR=222.5 fAvg=222.5 fault=STAGE2: fuera de ventana final
[11:06:45] [MAQUINA] [ERROR] Stage2 NG — STAGE2: fuera de ventana final
[11:06:45] [MAQUINA] [INFO] Finish: Holder/Encoder cerrados; cutters/grippers OFF
[11:06:45] [ANDON] [ERROR] ErrorState