/*
 * node_modules/mochitimers/index.js — timer shim over mbjs:os.
 *
 * The MochiBerry engine exposes setTimeout/setInterval/clearTimeout/clearInterval
 * as functions on the builtin `mbjs:os` module, but does NOT promote them to
 * globals. This module wraps them with Node-compatible semantics (args forwarded
 * to the callback, delay coerced to a non-negative number) and installs them as
 * globals so ordinary Node code works unchanged.
 */

import * as os from 'mbjs:os';

function normalizeDelay(delay) {
    const n = Number(delay);
    return Number.isFinite(n) && n > 0 ? n : 0;
}

export function setTimeout(callback, delay = 0, ...args) {
    const fn = typeof callback === 'function' ? callback : () => {};
    return os.setTimeout(() => fn(...args), normalizeDelay(delay));
}

export function clearTimeout(id) {
    return os.clearTimeout(id);
}

export function setInterval(callback, delay = 0, ...args) {
    const fn = typeof callback === 'function' ? callback : () => {};
    return os.setInterval(() => fn(...args), normalizeDelay(delay));
}

export function clearInterval(id) {
    return os.clearInterval(id);
}

/** Install timer functions as globals (idempotent). */
export function installGlobals() {
    if (typeof globalThis.setTimeout !== 'function') globalThis.setTimeout = setTimeout;
    if (typeof globalThis.clearTimeout !== 'function') globalThis.clearTimeout = clearTimeout;
    if (typeof globalThis.setInterval !== 'function') globalThis.setInterval = setInterval;
    if (typeof globalThis.clearInterval !== 'function') globalThis.clearInterval = clearInterval;
}

// Side effect: any app that pulls in the shim layer gets timers.
installGlobals();

export default { setTimeout, clearTimeout, setInterval, clearInterval, installGlobals };
