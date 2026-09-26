import type { BackendSnapshot } from './backendTypes';
import type { CycleConfig } from '../types';

async function api<T>(path: string, opts: RequestInit = {}): Promise<T> {
  const res = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...opts,
  });
  const ct = res.headers.get('content-type') || '';
  if (!ct.includes('application/json')) {
    throw new Error(
      `API ${path}: expected JSON, got ${res.status} ${ct || 'unknown'}`
    );
  }
  return res.json() as Promise<T>;
}

export function post<T>(path: string, body: Record<string, unknown> = {}): Promise<T> {
  return api(path, { method: 'POST', body: JSON.stringify(body) });
}

export function getState(): Promise<BackendSnapshot> {
  return api('/api/state');
}

export function reconnectNetwork() {
  return post<{ ok: boolean; links: Record<string, boolean> }>(
    '/api/network/reconnect'
  );
}

export function selectModel(index: number) {
  return post('/api/model', { index });
}

export function setMmRpm(mm: number, rpm: number) {
  return post('/api/motion/mm-rpm', { mm, rpm });
}

export function startCycle(qty?: number) {
  return post<{ ok: boolean; error?: string }>('/api/start', qty != null ? { qty } : {});
}

export function stopMachine() {
  return post('/api/stop');
}

export function resumeMachine() {
  return post('/api/resume');
}

export function pauseCycle() {
  return post('/api/cycle/pause');
}

export function resetCycle(opts?: { confirm?: boolean; doHome?: boolean }) {
  return post('/api/cycle/reset', {
    confirm: !!opts?.confirm,
    doHome: opts?.doHome ?? false,
  });
}

export function machineHome() {
  return post<{
    ok: boolean;
    error?: string;
    asdaZero?: boolean;
    encSet0R?: boolean;
    encSet0L?: boolean;
    allOff?: boolean;
  }>('/api/machine/home');
}

export function resetError(opts?: { confirm?: boolean; doHome?: boolean }) {
  return post('/api/error/reset', {
    confirm: opts?.confirm ?? true,
    doHome: opts?.doHome ?? false,
  });
}

export function cycleMaterialist(on: boolean) {
  return post('/api/cycle/materialist', { on });
}

export function cycleBusy(on: boolean) {
  return post('/api/cycle/busy', { on });
}

export function setCycleStepByStep(on: boolean) {
  return post<{ ok: boolean; stepByStep?: boolean; error?: string }>(
    '/api/cycle/step-by-step',
    { on }
  );
}

export function startCycleRefill(opts?: { mm?: number; asdaMm?: number }) {
  const body: Record<string, unknown> = {};
  if (opts?.mm != null) body.mm = opts.mm;
  if (opts?.asdaMm != null) body.asdaMm = opts.asdaMm;
  return post<{ ok: boolean; error?: string }>('/api/cycle/refill', body);
}

export function confirmCycleRefill(ok: boolean = true) {
  return post<{ ok: boolean; error?: string }>('/api/cycle/refill/confirm', { ok });
}

export function confirmRecoveryReview(ok: boolean = true) {
  return post<{ ok: boolean; error?: string }>('/api/cycle/recovery/review', { ok });
}

export function retryCycleRefill(opts?: { mm?: number }) {
  const body: Record<string, unknown> = {};
  if (opts?.mm != null) body.mm = opts.mm;
  return post<{ ok: boolean; error?: string }>('/api/cycle/refill/retry', body);
}

export function getCycleConfig() {
  return api<{ ok: boolean; config: CycleConfig }>('/api/cycle/config');
}

export function setCycleConfig(config: Partial<CycleConfig>) {
  return post<{ ok: boolean; config: CycleConfig }>(
    '/api/cycle/config',
    config as Record<string, unknown>
  );
}

export function reloadCycleConfigFromDisk() {
  return post<{ ok: boolean; config: CycleConfig }>('/api/cycle/config/reload', {});
}

export function getAppConfig() {
  return api<{ ok: boolean; config: { andonBuzzerMute: boolean } }>('/api/app/config');
}

export function setAppConfig(config: { andonBuzzerMute?: boolean }) {
  return post<{ ok: boolean; config: { andonBuzzerMute: boolean } }>(
    '/api/app/config',
    config as Record<string, unknown>
  );
}

export function unlockDebugMode(password: string) {
  return fetch('/api/debug/unlock', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ password }),
  }).then(async (res) => {
    if (!res.ok) {
      return {
        ok: false as const,
        error: 'server_unavailable' as const,
        status: res.status,
      };
    }
    try {
      const data = (await res.json()) as { ok?: boolean; error?: string | null };
      return {
        ok: !!data.ok,
        error: data.ok ? null : ((data.error as string) || 'invalid_password'),
        status: res.status,
      };
    } catch {
      return {
        ok: false as const,
        error: 'server_unavailable' as const,
        status: res.status,
      };
    }
  });
}

export function motionAction(action: string, extra: Record<string, unknown> = {}) {
  return post('/api/motion', { action, ...extra });
}

export function plcAction(action: string, extra: Record<string, unknown> = {}) {
  return post('/api/plc', { action, ...extra });
}

export function prefeederAction(
  action: string,
  extra: Record<string, unknown> = {}
) {
  return post<{ ok: boolean; error?: string; pulseS?: number }>('/api/prefeeder', { action, ...extra });
}

export function andonAction(
  action: string,
  extra: Record<string, unknown> = {}
) {
  return post<{ ok: boolean; error?: string }>('/api/andon', { action, ...extra });
}

export function clearLog(
  target: 'main' | 'motion' | 'plc' | 'prefeeder' | 'andon' | 'all'
) {
  return post('/api/log/clear', { target });
}

export function setEncPoll(enable: boolean) {
  return post('/api/motion/enc-poll', { enable });
}

export function getFeedOffset() {
  return api<{ ok: boolean; feedOffsetMmL?: number; feedOffsetMmR?: number }>(
    '/api/motion/feed-offset'
  );
}

export function setFeedOffset(feedOffsetMmL: number, feedOffsetMmR: number) {
  return post('/api/motion/feed-offset', { feedOffsetMmL, feedOffsetMmR });
}
