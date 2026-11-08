# AI Assistant — design & build plan (2026-11-08)

Roadmap item `assistant` (P5, deferred): *"voice from the watch mic, LLM called by the browser, answer
spoken on the watch"* (`docs/web-roadmap.md`, `docs/web-api.md` §Assistant). This plan takes it off the
shelf. The headline: it needs **no new firmware routes** — it is a web-desktop app plus a secrets slot,
built on tools that already exist.

## Why it's cheap (zero firmware flash)

Everything the assistant needs on the device is already shipped and verified (P4):

| Need | Existing API | Contract |
|---|---|---|
| Capture voice on the watch | `recorder` | `POST /api/recorder/start {max_s?}` → records 16 kHz mono WAV to `/sd`; `stop` → `{path, duration_ms, size}` |
| Get the recording to the browser | `files` | `GET /api/files/content?path=…` |
| Put a reply audio file on the watch | `files` | `POST /api/files` (upload WAV/MP3 to `/sd`) |
| Speak the reply on the watch | `audio.play` | `POST /api/audio/play {path}` (WAV/MP3 on `/sd`/`/flash`); `PATCH /api/audio {volume}` |

The LLM call, speech-to-text (STT) and text-to-speech (TTS) all run **in the browser** (the desktop
plane), exactly as the roadmap specifies. The watch is only the microphone and the speaker. So the
assistant is a new web app (`examples/factory/web/apps/assistant/`) + a settings place for a model
endpoint and key. **No `web_tool_*.cpp`, no new caps, no flash cost.** The `assistant` cap can simply
mean "the recorder + audio.play are present," or we skip a cap and gate the app on `recorder` +
`audio.play`.

## Architecture

```
  watch mic ──recorder──▶ WAV on /sd ──files.content──▶ browser
                                                           │
                                                      [STT] speech→text
                                                           │
                                                      [LLM] Claude Messages API (text→text)
                                                           │
                                                      [TTS] text→audio (WAV/MP3)
                                                           │
  watch speaker ◀──audio.play──  WAV on /sd ◀──files upload──┘
```

The browser orchestrates; the watch is voice I/O. A short text transcript + the conversation live in the
browser (optionally cached to `/sd` later).

## The one new thing: a secrets slot

The LLM/STT/TTS need an endpoint + API key. The design deferred the assistant precisely because "the
desktop has no settings place for those." Options:

- **A — browser `localStorage` (recommended for v1).** A Settings pane in the Assistant app stores
  `{provider, baseUrl, apiKey, model, sttProvider, ttsProvider}` in `localStorage`. Zero firmware change,
  secrets never touch the watch, honors "no firmware routes of its own." Per-browser (not synced across
  devices) — fine for a personal desktop.
- **B — firmware-synced secrets** via a new `/api/settings` extension persisted on flash. Syncs across
  browsers, but adds a firmware route + a place to leak a key off-device. Defer unless multi-device sync
  is wanted.

Recommendation: **A**.

## LLM: Claude by default, endpoint-configurable

- **Transport:** the web desktop is dependency-free vanilla ESM (no bundler), so call the **Messages API
  with `fetch`** (raw HTTP), not an SDK. `POST https://api.anthropic.com/v1/messages`, headers
  `x-api-key`, `anthropic-version: 2023-06-01`, and — because this is a browser —
  `anthropic-dangerous-direct-browser-access: true` (Anthropic's CORS opt-in; verify the exact header
  against live docs at build). The key is in the browser: acceptable for a personal, self-hosted,
  single-user desktop where the user supplies their own key — **state this plainly in the UI**.
- **Model:** default `claude-opus-5-5` is the most capable but heavy for a watch chat. For a snappy, cheap
  assistant, **recommend `claude-haiku-5-5`** ($0.10/$0.50 per MTok, fast) or `claude-sonnet-5-5` for more
  capability — **user's choice**, stored in settings. All are configurable.
- **Provider-neutral option:** allow a custom `baseUrl` + model so a user can point at an OpenAI-compatible
  endpoint instead of Claude. Default path is Claude.
- **Request shape:** adaptive thinking is on by default on these models; keep the request minimal
  (`model`, `max_tokens`, `messages`, a short `system` establishing the watch-assistant persona + brevity
  for TTS). Stream if we show text as it arrives.

## STT / TTS: Claude has no audio I/O — two routes

Claude's Messages API is text-only, so speech needs another mechanism:

- **Route 1 — browser-native Web Speech API (zero key, quickest).** `SpeechRecognition` (live browser mic)
  and `SpeechSynthesis` (speaks in the browser). *Limitation:* it uses the **browser's** mic/speaker, not
  the watch's — so it doesn't meet "voice from the watch, spoken on the watch." Good for a fast demo /
  fallback, and for a "type instead of speak" path.
- **Route 2 — a file-based STT/TTS service (meets the watch-I/O goal).** STT takes the watch's WAV →
  text; TTS takes text → a WAV/MP3 we upload to `/sd` and play via `audio.play`. Needs a configurable
  STT/TTS endpoint + key (e.g. a Whisper-style STT and any TTS that returns audio). This is the real
  "talk to your watch" experience.

Recommendation: support both; ship text-first, then Route 2 for full watch voice.

## Phased build (each slice shippable + verified)

- **P1 — text ↔ text, spoken on the watch.** Assistant app: a settings pane (Route A secrets), a chat UI,
  type a question → Claude → show the answer; optional **TTS → upload → `audio.play`** so the watch speaks
  the reply. Proves key storage, the LLM call, the conversation UI, and watch playback. (If TTS slips,
  P1a is text-only; P1b adds watch speech.)
- **P2 — voice in from the watch mic.** Record via `recorder` → download the WAV → STT → text → the P1
  pipeline. Completes the full loop (watch mic → watch speaker).
- **P3 — polish.** Streaming answers, barge-in/stop (`audio/stop`, `recorder/stop`), conversation history
  on `/sd`, volume via `PATCH /api/audio`, a push-to-talk affordance on the watch screen, cost/usage note.

## Privacy & safety (must be explicit in the UI)

Audio and transcripts **leave the watch → browser → third-party APIs** (LLM, and STT/TTS if Route 2). The
API key lives in the browser (`localStorage`). This is opt-in and disclosed in-app: a first-run notice,
the key field marked as stored locally, and a clear "what gets sent where" line. No audio is retained on
our side beyond the `/sd` recording the user already controls.

## Testing (dual-plane, like the rest)

- **UI** (`web_ui_test.mjs`, Chrome DOM): point the app at a **stub endpoint** (a tiny local server or a
  mock route returning canned `messages` responses) so the flow is deterministic and free — assert
  record→transcribe→answer→play wiring without spending tokens or needing a real key.
- **Mock plane:** the JS mock already serves `recorder`/`audio.play`/`files`; add fake WAV handling so the
  record→download→upload→play path exercises end-to-end against the mock.
- **Live:** one manual smoke against the real Claude API with the user's key (costs a few cents), kept out
  of CI.

## Decisions to confirm before building

1. **v1 scope:** text-first (P1, fastest) vs. full voice immediately (P1+P2)?
2. **Model default:** Haiku 5.5 (cheap/fast, recommended) · Sonnet 5.5 · Opus 5.5?
3. **STT/TTS:** browser-native (zero key, browser mic/speaker) vs. a file-based service (true watch voice)?
4. **Secrets:** browser `localStorage` (recommended) vs. a firmware-synced slot?

Build proceeds like the automation work: small slices, each verified on both planes and committed on `dev`.
