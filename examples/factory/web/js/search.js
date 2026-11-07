// Global launchpad search indexes. File walking stays in the browser; the watch only lists folders.
export const SETTINGS_INDEX = [
  { id: 'brightness', title: 'Brightness', section: 'Display', terms: 'backlight screen' },
  { id: 'screen-off-after', title: 'Screen off after', section: 'Display', terms: 'timeout sleep display' },
  { id: 'set-the-watch-clock', title: 'Set the watch clock', section: 'Time', terms: 'time zone date sync browser' },
  { id: 'desktop-version', title: 'Desktop version', section: 'Web desktop', terms: 'update rescue install' },
  { id: 'turn-the-web-server-off-after', title: 'Turn the web server off after', section: 'Web desktop', terms: 'idle timeout battery' },
  { id: 'this-browser', title: 'Log out this browser', section: 'Web desktop', terms: 'session pairing forget' },
  { id: 'all-browsers', title: 'Log out all browsers', section: 'Web desktop', terms: 'sessions pairing forget' },
  { id: 'sd-card', title: 'SD card', section: 'Storage', terms: 'storage space mount card' },
  { id: 'remove-the-card', title: 'Safe to remove SD card', section: 'Storage', terms: 'unmount eject storage' },
  { id: 'internal-flash', title: 'Internal flash', section: 'Storage', terms: 'storage space memory' },
  { id: 'colours', title: 'Colours', section: 'Watch', terms: 'theme accent appearance' },
  { id: 'board', title: 'Board', section: 'Watch', terms: 'device hardware about' },
  { id: 'firmware', title: 'Firmware', section: 'Watch', terms: 'version about' },
  { id: 'restart-the-watch', title: 'Restart the watch', section: 'Watch', terms: 'reboot power' },
];

export function searchSettings(query) {
  const q = query.trim().toLowerCase();
  if (!q) return [];
  return SETTINGS_INDEX.filter((s) => `${s.title} ${s.section} ${s.terms}`.toLowerCase().includes(q)).slice(0, 8);
}

const join = (dir, name) => `${dir === '/' ? '' : dir}/${name}`;

// Bounded breadth-first walk: user data first, desktop internals last. A new query cancels
// further requests through isCurrent(), so stale searches never keep the single-threaded watch busy.
export async function searchFiles(api, query, caps, isCurrent = () => true) {
  const q = query.trim().toLowerCase();
  if (!q || !caps.includes('files')) return { results: [], truncated: false };
  const roots = ['/sd/data', '/sd'];   // caps.includes('files') already guaranteed above
  if (caps.includes('files.flash')) roots.push('/flash');
  const queue = roots.map((path) => ({ path, depth: 0 }));
  const seen = new Set();
  const results = [];
  let visited = 0;
  let truncated = false;

  while (queue.length && results.length < 24 && visited < 120 && isCurrent()) {
    const { path, depth } = queue.shift();
    if (seen.has(path)) continue;
    seen.add(path);
    visited++;
    let entries;
    try { entries = (await api.list(path)).entries || []; }
    catch { continue; }             // absent SD cards and unreadable folders are not search failures
    if (!isCurrent()) break;
    entries.sort((a, b) => Number(b.dir) - Number(a.dir) || a.name.localeCompare(b.name));
    for (const e of entries) {
      if (!e?.name || e.name === '.' || e.name === '..') continue;
      const child = join(path, e.name);
      if (e.name.toLowerCase().includes(q)) {
        results.push({ name: e.name, path: child, dir: !!e.dir, size: e.size || 0 });
        if (results.length >= 24) { truncated = true; break; }
      }
      if (e.dir && depth < 6 && !seen.has(child)) {
        queue.push({ path: child, depth: depth + 1 });
        // Stable sort keeps breadth-first order while deferring desktop implementation files.
        queue.sort((a, b) => Number(a.path === '/sd/web' || a.path === '/sd/web.new') - Number(b.path === '/sd/web' || b.path === '/sd/web.new'));
      }
    }
  }
  if (queue.length && (results.length >= 24 || visited >= 120)) truncated = true;
  return { results, truncated };
}
