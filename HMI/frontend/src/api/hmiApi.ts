import type { BackendSnapshot } from './backendTypes';
import type { CycleConfig } from '../types';

async function api<T>(path: string, opts: RequestInit = {}): Promise<T> {
  const res = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...opts,
  });
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
  return post('/api/start', qty != null ? { qty } : {});
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
    doHome: !!opts?.doHome,
  });
}

export function confirmError() {
  return post('/api/error/confirm');
}

export function resetError(opts?: { confirm?: boolean; doHome?: boolean }) {
  return post('/api/error/reset', {
    confirm: opts?.confirm ?? true,
    doHome: opts?.doHome ?? true,
  });
}

export function cycleMaterialist(on: boolean) {
  return post('/api/cycle/materialist', { on });
}

export function setCycleStepByStep(on: boolean) {
  return post<{ ok: boolean; stepByStep?: boolean; error?: string }>(
    '/api/cycle/step-by-step',
    { on }
  );
}

export function setCycleTrialMode(on: boolean) {
  return post<{ ok: boolean; trialMode?: boolean; error?: string }>(
    '/api/cycle/trial-mode',
    { on }
  );
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

export function motionAction(action: string, extra: Record<string, unknown> = {}) {
  return post('/api/motion', { action, ...extra });
}

export function plcAction(action: string, extra: Record<string, unknown> = {}) {
  return post('/api/plc', { action, ...extra });
}

export function prefeederAction(action: string) {
  return post('/api/prefeeder', { action });
}

export function clearLog(target: 'main' | 'motion' | 'plc' | 'prefeeder' | 'all') {
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
