/*
 * CycleFlowCopy.ino — COPIA de referencia del flujo de corte TCM
 *
 * NO compilar con el sketch TCM (carpeta aparte).
 * Fuente: TCM/TCM.ino  (loop ~10487, prepareBeforeCut ~8242,
 *         procesarCorte ~8215, cycle ~7775)
 *
 * Cada llamada a función lleva un comentario de qué hace.
 */

// =============================================================================
// ENTRADA DESDE loop()  (TCM.ino ~10487)
// =============================================================================
void loop_DISPATCH_CORTE_SOLO()
{
  // ... wifiService, peerService, safety, etc. (servicios de fondo) ...

  if (cutRequested && !cycleActive)
  {
    cutRequested = false;
    abortPendingPrefeederSettle("dispatch");
    // → cancela settle pendiente del PreFeeder para liberar el Start

    if (prefeederIdleModeActive())
    {
      // → PreFeeder en modo Materialista: no correr máquina
      releasePrefeederSettleHold();
    }
    else if (safetyValidateAtStart() && peerVerifyLinkFastOrPing())
    {
      // safetyValidateAtStart() → poll sensores + debounce; exige safetyAllowsRun()
      // peerVerifyLinkFastOrPing() → enlace TCP PreFeeder L/R OK

      prepareBeforeCut();   // HOME / PLC seguro
      procesarCorte(cutLongitud, cutCantidad);  // mm→steps + cycle()
    }
    else
    {
      // errores E027 sensores / E008 servo CAN / E026 safety / PreFeeder sin enlace
      releasePrefeederSettleHold();
      uiNotify();
    }
  }

  // Fuera del corte: serviceCycle(), serviceServoFeed(), web, CAN...
}

// =============================================================================
// prepareBeforeCut()  (TCM.ino ~8242)
// =============================================================================
void prepareBeforeCut()
{
  stCutters = false;
  stGrippers = false;
  stHolder = true;
  stFgtray = false;
  stReset = false;
  applyPlcOutputs();
  // → escribe salidas PLC (holder/grippers/cutters/…)

  cycleAborted = false;
  cycleActive = true;
  peerSettleHoldInProcess = false;
  peerSyncInProcessFlag();
  // → sincroniza flag “In process” hacia PreFeeder (arma buffer/holgura)
  cyclePaused = false;
  cutRequested = false;
  lastCycleSuccess = false;
  lastCycleTotalMs = 0;
  progressStep = 0;
  progressTotalSteps = 0;
  progressCurrentRep = 0;
  progressTotalReps = 0;

  if (!asdaHomed)
  {
    if (!linearRunTorqueHome())
    {
      // → HOME por torque del lineal ASDA; si falla, no arranca corte
      cycleActive = false;
      peerSettleHoldInProcess = false;
      peerSyncInProcessFlag();
      return;
    }
  }
  else
  {
    cycleStartBackToHome();
    // → arranca movimiento abs a posición 0 (C_BACK)
    waitCycleFinish();
    // → espera hasta que serviceCycle/linear termine (con timeout)
  }

  if (!cycleActive || cycleAborted)
  {
    cycleActive = false;
    peerSettleHoldInProcess = false;
    peerSyncInProcessFlag();
    return;
  }

  progressTotalReps = cutCantidad;
  progressCurrentRep = 0;
  progressTotalSteps = 11;
  cyclePaused = false;
  cycleActive = false;  // se vuelve a armar dentro de cycle()
}

// =============================================================================
// procesarCorte()  (TCM.ino ~8215)
// =============================================================================
void procesarCorte(int longitud, int cantidad)
{
  if (cycleAborted)
  {
    cycleAborted = false;
    cycleActive = false;
    peerSyncInProcessFlag();
    return;
  }

  float effectiveMm = 0.0f;
  uint32_t steps = cutLengthToSteps(longitud, &effectiveMm);
  // → convierte longitud mm (+offset de corte) a pulsos del lineal
  //   (usa linearStepsPerMm, gripper area, cutOffsetMm si aplica)

  if (effectiveMm <= 0.0f)
  {
    cycleActive = false;
    cyclePaused = false;
    lastCycleSuccess = false;
    peerSyncInProcessFlag();
    return;
  }

  cycle(cantidad, steps);
  // → rutina principal del lote
}

// =============================================================================
// cycle() — RUTINA COMPLETA  (TCM.ino ~7775)
// =============================================================================
void cycle(int cycles, uint32_t stepsPerCut)
{
  if (cycleAborted)
  {
    cycleAborted = false;
    cycleActive = false;
    peerSettleHoldInProcess = false;
    peerSyncInProcessFlag();
    return;
  }

  cycleActive = true;
  peerSettleHoldInProcess = false;
  peerSyncInProcessFlag();
  // → Start: arma buffer/holgura en PreFeeder (Production)
  cyclePaused = false;
  cyclePausePending = false;
  cyclePausedAccumMs = 0;
  cyclePauseBeganMs = 0;
  setCyclePauseReason("");
  setCycleFaultReason("");
  setCycleError(E000);
  // → limpia error de ciclo a “sin error”
  feedPhase = FEED_IDLE;
  feedHandoffReady = false;
  feedModeThisCycle = feedMode;
  int32_t feedOffsetStepsThisCycleA = feedOffsetSteps();
  // → offset alimentación L (mm→pasos encoder/servo)
  int32_t feedOffsetStepsThisCycleB = feedOffsetStepsB();
  // → offset alimentación R

  progressTotalSteps = 11;
  progressTotalReps = cycles;
  progressCurrentRep = 0;
  progressStep = 0;
  cycleStartTime = millis();
  totalCompletedTime = 0;
  completedReps = 0;
  lastCycleTotalMs = 0;
  lastCycleSuccess = false;
  repStartTime = millis();
  gripToHomeCancel();
  // → cancela medición grip→HOME pendiente
  prefeederTriggerEveryN = 1;

  // ---- R0: esperar PreFeeder listo (buffer + holgura) ----
  if (!waitPrefeederReadyAtStart())
  {
    // → peerBothAutoArmedForRefill + material ready / timeout
    //   falla → E030 o timeout; prefeederHardStopDisarm()
    if (cycleAborted || !cycleActive) { /* Stop */ return; }
    if (!peerBothAutoArmedForRefill())
      setCycleError(E030, "prefeeder_not_ready");
    cycleActive = false;
    lastCycleSuccess = false;
    prefeederHardStopDisarm("start-fail");
    uiNotify();
    return;
  }

  // ===========================================================================
  // BUCLE POR PIEZA
  // ===========================================================================
  for (int rep = 1; rep <= cycles; rep++)
  {
    if (!cycleActive || cycleAborted) break;

    if (rep == 1)
    {
      if (cycleGateAfterStep("listo")) break;
      // → safety stop / pausa paso-a-paso / Pause UI; true = abortar
    }
    else if (cycleGateAfterStep("rep-start")) break;

    progressCurrentRep = rep;
    progressStep = 0;
    repStartTime = millis();

    // ---- 1. Holder ON (solo 1ª pieza) ----
    if (rep == 1)
    {
      progressStep = 1;
      stHolder = true;
      applyPlcOutputs();
      pausableDelay(D_HOLDER_ON_MS);
      // → delay que respeta Pause (no avanza milis mientras cyclePaused)
      if (cycleGateAfterStep("holder-on")) break;
    }

    // ---- 2. Alimentación ----
    progressStep = 2;
    if (rep == 1)
    {
      runServoFeedStep2();
      // → startServoFeedAsync(false) = beginServoFeed()
      //   (perfil PP, encoder OM reset, FEED_SS_SOLID / BYPASS)
      //   + waitServoFeedStep2() hasta FEED_DONE o error/timeout
      if (cycleGateAfterStep("feed")) break;
      if (!feedThisCycleSucceeded())
      {
        // → true si feed llegó a target (encoder/OM/sensor según modo)
        //   E016 hose / E012 incomplete
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
      if (!omCommitPhase1OrFault())
      {
        // → lee OM tras feed (fase 1); si OM_ENFORCE y falla → error y aborta
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
    }
    else if (feedHandoffReady && feedHoseSensorReadyNow())
    {
      // → material prefetch ya listo + sensor manguera OK → no re-alimentar
      feedHandoffReady = false;
      if (!omCommitPhase1OrFault())
      {
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
    }
    else
    {
      if (feedHandoffReady)
        feedHandoffReady = false;
      if (!feedEnsureReadyAtHome())
      {
        // → waitServoFeedStep2 / reintento runServoFeedStep2 si incompleto
        //   o si latch sin láser → re-alimentar
        lastCycleTotalMs = cycleElapsedMs();
        // → tiempo de ciclo descontando pausas
        lastCycleSuccess = false;
        break;
      }
      if (cycleGateAfterStep("feed-prefetch")) break;
      feedConsumeReadyMaterial();
      // → feedPhase=IDLE, limpia flags prefetch (material “consumido” por la pieza)
      if (!omCommitPhase1OrFault())
      {
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
      if (cycleGateAfterStep("feed-consume")) break;
    }

    // ---- 3. Offset alimentación (opcional) ----
    if (feedOffsetStepsThisCycleA != 0 || feedOffsetStepsThisCycleB != 0)
    {
      feedApplyOffsetMove(feedOffsetStepsThisCycleA, feedOffsetStepsThisCycleB);
      // → movimiento relativo extra L/R en servos feeder (ajuste longitud)
      if (cycleGateAfterStep("offset")) break;
    }

    // ---- 4. Pinzas cierran ----
    progressStep = 3;
    stGrippers = true;
    applyPlcOutputs();
    gripToHomeMarkStart();
    // → marca t0 para estadística tiempo pinzas→HOME
    pausableDelay(D_GRIPPERS_ON_MS);
    if (cycleGateAfterStep("grippers-on")) break;

    asdaEncoderReset();
    // → pone encoder OM a cero (fase 2 solo mide avance lineal)
    asdaOverviewMark("omReset");
    // → marca overview ASDA en este punto del perfil

    // ---- 5. Holder abre + lineal FWD ----
    progressStep = 5;
    stHolder = false;
    applyPlcOutputs();
    pausableDelay(D_HOLDER_OPEN_MS);
    if (cycleGateAfterStep("holder-off")) break;

    cycleStartForwardPulses(stepsPerCut, true);
    // → C_FWD: linearStartAbsSteps(pos+steps); skipDwell=true (sin dwell al llegar)
    waitCycleFinish();
    // → bucle: serviceCycle + linearService hasta idle; timeout → faultStopCycle
    asdaOverviewMark("asdaLinearActuator");
    asdaOverviewMark("omFinal");
    (void)omValidatePhase2AndTotal();
    // → mide OM fase2 + total (fase1+fase2); registra ok/fault internos
    if (!omPieceLengthOkForCut())
    {
      // → si enforce: exige fase2/total OK; si no, siempre true
      lastCycleSuccess = false;
      cycleActive = false;
      break;
    }
    if (cycleGateAfterStep("lineal-fwd")) break;

    pausableDelay(D_LINEAR_DONE_MS);
    if (cycleGateAfterStep("lineal-dwell")) break;

    // ---- 6. Holder cierra (pre-corte) ----
    stHolder = true;
    applyPlcOutputs();
    pausableDelay(D_HOLDER_ON_MS);
    if (cycleGateAfterStep("holder-precut")) break;

    // ---- 7. Corte ----
    progressStep = 7;
    stCutters = true;
    applyPlcOutputs();
    if (!peerBothAllOkForPrefetch())
    {
      // → PreFeeder L+R: peerOk, sin error, autoEnabled, no idleMode
      faultStopCycle("prefeeder_all_ok");
      // → aborta ciclo con razón (safety/outputs/feed halt según implementación)
      break;
    }
    pausableDelay(D_CUTTER_PULSE_MS);
    if (cycleGateAfterStep("cutter-on")) break;

    stCutters = false;
    applyPlcOutputs();
    pausableDelay(D_CUTTER_POST_MS);
    if (cycleGateAfterStep("cutter-off")) break;

    // ---- 8. Prefetch siguiente (paralelo) ----
    if (rep < cycles)
    {
      startServoFeedAsync(true);
      // → beginServoFeed(prefetch=true): alimenta siguiente pieza en background
      //   (serviceServoFeed lo avanza desde loop / waitCycleFinishCoop)
    }

    // ---- 9. Extra / depósito ----
    progressStep = 8;
    {
      uint16_t batchIndex = (uint16_t)((rep - 1) / depositBatchSize + 1);
      uint16_t pieceInBatch = (uint16_t)((rep - 1) % depositBatchSize);
      int32_t extraPulses = depositExtraSignedSteps(batchIndex, pieceInBatch);
      // → pulsos extra (+/−) según lote/bandeja para depositar la pieza

      if (extraPulses > 0)
        cycleStartForwardPulses((uint32_t)extraPulses, false);
        // → FWD con dwellAtDest si está configurado
      else if (extraPulses < 0)
        cycleStartBackwardPulses((uint32_t)(-extraPulses));
        // → BACK relativo

      if (rep >= cycles && feedStep2NeedsWait())
      {
        // → hay feed activo residual en última pieza
        waitServoFeedStep2();
        feedConsumeReadyMaterial();
      }
      if (extraPulses != 0)
        waitCycleFinish();

      asdaOverviewMark("asdaDeposit");
      if (!asdaOverviewHitOk("asdaDeposit"))
      {
        // → overview ASDA no marcó el punto de depósito OK
        setCycleError(E012, "overview_deposit");
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
      if (cycleGateAfterStep("deposit")) break;
    }

    // ---- 10. Pinzas abren + trigger PreFeeder ----
    progressStep = 9;
    stGrippers = false;
    applyPlcOutputs();
    prefeederTriggerAsync("grippers-off");
    // → Tfeed fire-and-forget a PreFeeder L/R (no espera ACK)
    pausableDelay(D_GRIPPER_RELEASE_MS);
    if (cycleGateAfterStep("grippers-off")) break;

    // ---- 11. HOME + handoff ----
    progressStep = 11;
    cycleStartBackToHome();
    // → BACK a pos 0
    if (rep < cycles)
      waitCycleFinishCoop();
      // → espera lineal Y feed prefetch en paralelo (timeouts independientes)
    else
      waitCycleFinish();
    gripToHomeMarkEnd();
    // → cierra medición grip→HOME
    if (cycleGateAfterStep("home")) break;

    {
      uint32_t repTime = millis() - repStartTime;
      totalCompletedTime += repTime;
      completedReps++;
    }

    if (rep < cycles)
    {
      if (feedEnsureReadyAtHome())
      {
        feedConsumeReadyMaterial();
        feedHandoffReady = true;
        // → próxima rep puede saltar el feed si sensor sigue OK
      }
      else
      {
        feedHandoffReady = false;
        if (!cycleActive || cycleAborted) break;
        if (cycleErrorCode == E000 && !feedThisCycleSucceeded())
          setCycleError(E012, "feed_incomplete");
        lastCycleTotalMs = cycleElapsedMs();
        lastCycleSuccess = false;
        break;
      }
    }

    // ---- 12. Asentar + post-pieza ----
    if (!feedHandoffReady)
      pausableDelay(asentarDelayMs);
    if (cycleGateAfterStep("asentar")) break;

    safetyPollAfterPiece();
    // → serviceCANRx + debounce + refreshSafetyError + honor stop
    if (!cycleActive || cycleAborted) break;
    if (cycleGateAfterStep("post-pieza")) break;

    if (rep < cycles)
    {
      peerCheckAfterPieceOrPause();
      // → verifica enlace/estado PreFeeder; puede pausar o abortar
      if (!cycleActive || cycleAborted) break;
      if (cycleGateAfterStep("post-pieza-peer")) break;
      if (!prefeederRequireHolguraAtHome())
        // → exige holgura buffer en HOME antes de siguiente pieza
        break;
    }
  } // for rep

  // ===========================================================================
  // CIERRE DEL LOTE
  // ===========================================================================
  const bool wasAborted = cycleAborted;
  const bool wasPaused = cyclePaused;
  const bool allRepsDone = (cycles > 0) && (completedReps >= cycles);
  const bool lotPiecesDone = !wasAborted && !wasPaused && allRepsDone
                             && cycleFaultReason[0] == '\0';

  if (wasAborted)
    cycleAborted = false;

  gripToHomeCancel();

  if (lotPiecesDone)
  {
    lastCycleTotalMs = cycleElapsedMs();
    if (!lastCycleTotalMs)
      lastCycleTotalMs = millis() - cycleStartTime;
    lastCycleSuccess = true;
    towerNotifyLotComplete();
    // → notifica torre/UI fin de lote OK
  }
  else
  {
    lastCycleSuccess = false;
    if (!lastCycleTotalMs)
      lastCycleTotalMs = cycleElapsedMs();
  }

  if (!wasAborted)
  {
    peerSettleHoldInProcess = true;
    cycleActive = false;
    cyclePaused = false;
    prefeederSettleThenDisarm(allRepsDone ? "fin-lote" : "fin-ciclo");
    // → deja PreFeeder rellenar buffer/holgura y luego desarma In process
    if (lotPiecesDone)
      lastCycleSuccess = true;
  }
  else
  {
    cycleActive = false;
    cyclePaused = false;
    peerSyncInProcessFlag();
  }
}

// =============================================================================
// MAPA RÁPIDO DE FUNCIONES LLAMADAS
// =============================================================================
//
// applyPlcOutputs()              Escribe salidas digitales PLC
// pausableDelay(ms)              Delay respetando Pause del ciclo
// cycleGateAfterStep(name)       Safety + paso-a-paso + Pause UI tras un paso
// waitPrefeederReadyAtStart()    Buffer/holgura listos al Start
// runServoFeedStep2()            Feed bloqueante (begin + wait)
// startServoFeedAsync(prefetch)  Feed no bloqueante (prefetch post-corte)
// waitServoFeedStep2()           Espera fin de feed / timeout
// feedEnsureReadyAtHome()        Garantiza material listo en HOME
// feedConsumeReadyMaterial()     Marca feed IDLE / limpia prefetch
// feedThisCycleSucceeded()       ¿Feed alcanzó target?
// feedApplyOffsetMove(A,B)       Offset extra servos L/R
// omCommitPhase1OrFault()        Valida OM fase 1 post-feed
// omValidatePhase2AndTotal()     OM fase 2 + total en posición de corte
// omPieceLengthOkForCut()        ¿Longitud OK para cortar?
// asdaEncoderReset()             Cero encoder OM
// asdaOverviewMark(tag)          Marca overview ASDA
// asdaOverviewHitOk(tag)         ¿Marca overview OK?
// cycleStartForwardPulses(n,s)   Arranca lineal FWD n pulsos
// cycleStartBackwardPulses(n)    Arranca lineal BACK n pulsos
// cycleStartBackToHome()         Arranca lineal a HOME
// waitCycleFinish()              Espera fin movimiento lineal
// waitCycleFinishCoop()          Espera lineal + feed en paralelo
// depositExtraSignedSteps(b,p)   Pulsos extra de depósito
// peerBothAllOkForPrefetch()     All OK PreFeeder antes de cortar/prefetch
// prefeederTriggerAsync(tag)     Trigger Tfeed al PreFeeder
// prefeederRequireHolguraAtHome() Holgura antes de siguiente pieza
// peerCheckAfterPieceOrPause()   Check enlace post-pieza
// safetyPollAfterPiece()         Safety ligero tras pieza
// safetyValidateAtStart()        Poll seguridad al Start
// faultStopCycle(reason)         Abort por falla clasificada
// prefeederSettleThenDisarm()    Settle buffer y desarme al fin
// prefeederHardStopDisarm()      Desarme duro (fallo Start / Stop)
// towerNotifyLotComplete()       Torre: lote completo
// cutLengthToSteps(mm,&eff)      mm → pulsos lineal
// cycleElapsedMs()               Tiempo de ciclo sin pausas
// gripToHomeMarkStart/End/Cancel Cronometraje pinzas→HOME
// linearRunTorqueHome()          HOME torque ASDA
// peerSyncInProcessFlag()        Sync In process → PreFeeder
// uiNotify()                     Empuja estado a UI web
//
