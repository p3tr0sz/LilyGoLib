// Shared scaffold for the polling "tool" apps (airspace, sentinel, sniffer, deauth,
// beacon, presence, evilportal, fido2). Each of these had independently re-implemented
// the same bookkeeping: an `alive` flag to drop late async work after unmount, a request
// sequence so a slow refresh can't overwrite a newer one, a `busy` flag while a command
// is in flight, a `load → render` refresh, a start/stop poll timer, and an identical
// try/catch/finally around every command. This centralizes all of it so an app only
// declares WHAT to fetch, HOW to draw it, and WHEN to keep polling.
//
//   const app = toolApp({
//     name: 'Airspace',             // shown in error toasts
//     read: () => api.airspace(),   // fetch the next state (may Promise.all several calls)
//     apply: (next) => { state = { ...state, ...next }; },
//     render: () => render(),       // reads app.busy for disabled states
//     onError: (e) => info.replaceChildren(errNode(e)),  // a failed refresh
//     interval: 2000,               // poll cadence in ms; 0 disables polling
//     pollWhen: () => state.active,  // poll only while true; omit to always poll
//   });
//   app.start();                                          // first paint + load + timer
//   // commands run through app.action, which gives every app the same error recovery:
//   const start = () => app.action(() => api.airspaceStart(), 'Monitoring started');
//   return { unmount: () => app.unmount(off) };           // pass any extra cleanup
//
import { toast, errorText } from './ui.js';

export function toolApp({ name, read, apply, render, onError, interval = 0, pollWhen }) {
  let alive = true, busy = false, seq = 0, timer = null;

  const app = {
    get alive() { return alive; },
    get busy() { return busy; },

    // Refresh state from the watch. A newer load — or an unmount — makes this a no-op,
    // so a slow response can never clobber fresher state or a torn-down app.
    async load(showError = true) {
      const mine = ++seq;
      try {
        const next = await read();
        if (!alive || mine !== seq) return;
        apply(next);
        render();
      } catch (e) {
        if (alive && mine === seq && showError && onError) onError(e);
      }
    },

    // Run a command: block the controls, run it, toast on success, then refresh.
    // Uniform recovery — a failed command toasts the error and still refreshes.
    async action(fn, message) {
      busy = true; render();
      try {
        await fn();
        if (message) toast(message);
        await app.load();
      } catch (e) {
        toast(errorText(e, name), 'error');
        await app.load(false);
      } finally {
        busy = false;
        if (alive) render();
      }
    },

    // First paint, initial fetch, and the optional poll timer.
    start() {
      render();
      app.load();
      if (interval) {
        timer = setInterval(() => {
          if (alive && (!pollWhen || pollWhen())) app.load(false);
        }, interval);
      }
    },

    // Tear down. Pass any extra cleanup (event unsubscribes, observers) to run too.
    unmount(...cleanups) {
      alive = false;
      if (timer) { clearInterval(timer); timer = null; }
      for (const c of cleanups) { try { c?.(); } catch (e) { console.error(e); } }
    },
  };
  return app;
}
