# T-Watch Ultra: UX guidelines

Rules for the factory app's watch UI (`FACTORY_WATCH_UI`), taken from the platform guides and research listed under "Sources". Each rule says where it comes from and what it means in pixels on this screen. `watch-ui-roadmap.md` tracks the work; this file is what the work is checked against.

## This screen in the sources' units

The T-Watch Ultra panel is 502 × 410 px at about 315 ppi: 40 × 33 mm, 0.081 mm a pixel.

The Apple Watch Ultra (49 mm) has the same pixels turned sideways, 410 × 502 at 2 px a point and about 338 ppi. (That size is from Apple's device specifications, not from the pages listed below.) So Apple's watchOS numbers carry over almost directly: **1 Apple point ≈ 1.86 px here** (0.150 mm). This file rounds to that.

| Source value | Physical | Here |
|---|---|---|
| Apple 44 × 44 pt control (default) | 6.6 mm | 82 px |
| Apple 28 × 28 pt control (minimum) | 4.2 mm | 52 px |
| NN/g 1 × 1 cm target | 10 mm | 124 px |
| Qt 48 px target (on its own displays) | – | 48 px is a floor for *their* pixels, not ours |
| Apple watchOS body text, 17 pt (default on 44–49 mm) | 2.55 mm | 32 px |
| Apple watchOS caption 1, 16 pt | 2.4 mm | 30 px |
| Apple watchOS minimum text, 12 pt | 1.8 mm | 22 px |
| Wear OS ambient: 10 px from the edge | – | 10 px (their pixels are similar) |

Montserrat's sizes in LVGL are its line spacing, not a cap height like Apple's points, so compare by eye on the simulator too, not only by number.

## Principles

### 1. Glance first

- **1.1 A visit lasts seconds.** Show the one thing the person came for at once; anything else is one scroll or one tap away. Apple: "quick, glanceable, single-screen interactions"; Wear OS: "complete tasks on the watch within seconds"; NN/g: "just a few seconds"; UXDI: "Anything more requires too much from the user."
- **1.2 Most important first.** Top of the page, biggest type (Apple *Layout*, Garmin "front and center", Qt "inverted pyramid").
- **1.3 Distil, don't truncate.** Pick what matters and say it in a headline, not a paragraph (NN/g, Smashing "newspaper headline"). A cut-off label is a failure, not a fit; if a value can be long, give it its own page.
- **1.4 One or two jobs per app.** Leave long, complex work (logs, analysis, configuration) to another device or the web page (Wear OS, Garmin mobile app settings, Smashing "Would I rather do this on my phone?").
- **1.5 Watch screens are for observing.** Keep interaction to a minimum (Garmin "watches are best when they are used to observe").

### 2. Navigation

- **2.1 Shallow.** Face → app list → app → one sub-page is the normal depth. Everything within 3–4 interactions (Garmin); "minimize the depth of hierarchy" (Apple).
- **2.2 Vertical.** Pages and lists scroll up and down (Apple, Garmin "stick with the up/down page loop model"). No sideways carousels inside apps.
- **2.3 One back gesture, everywhere.** Swipe right goes back on every page; no app changes it (Garmin "avoid modifying this behavior", NN/g "swipe ambiguity"). Exceptions are the ones the roadmap lists (sliders, keyboards, horizontal content), and those pages say so.
- **2.4 Few gestures, all of them standard.** Tap, vertical scroll, swipe right, long press. Long press is never the only way to do something (NN/g: force touch had no signifier).
- **2.5 New pages slide in from the right and leave to the right** (Garmin *Views*).
- **2.6 Lists of up to about seven items**; group longer ones (Garmin *Menus*). Show the current choice as the row's value and open a picker for it (Garmin, and the roadmap's picker rule).

### 3. Touch targets and layout

- **3.1 Every tap target at least 82 px tall** (Apple's 44 pt). 52 px (28 pt) is the hard minimum, for secondary controls only. A whole row is one target.
- **3.2 Primary actions are full-width pills** (Apple "prefer buttons that span the width of the screen"). At most two text buttons or three icon buttons side by side, and two side by side only on a page that doesn't scroll (Apple *Layout*, watchOS).
- **3.3 One or two prominent buttons per screen** (Apple *Buttons*; NN/g "few and big", "1–2 targets per screen").
- **3.4 Space between targets.** About 22 px around a control with a visible edge, 45 px around one without (Apple's 12 / 24 pt).
- **3.5 The visible shape is the target.** People aim at what they see; padding an invisible hit area doesn't help much (NN/g). A small icon that acts needs a visible disc or pill of target size.
- **3.6 Every control shows that it's pressed** (Apple "Always include a press state").
- **3.7 Group with space and cards**, align to one left edge, indent only to show "belongs to" (Apple *Layout*). Titles in a column of rows start at the same x.
- **3.8 Fixed headers stay small.** A page title is one line; nothing else is pinned over content (NN/g: "fat sticky headings" took a quarter of the screen).
- **3.9 Keep 24 px from the screen edge** (`UI_SCREEN_MARGIN`; the corners are rounded). Ambient content stays 10 px or more from the edge (Wear OS).

### 4. Type

- **4.1 Body text is about 32 px; nothing that has to be read is under 22 px** (Apple watchOS body 17 pt default, minimum 12 pt). Here (since phase 8): `UI_FONT_BODY` is 28 px, `UI_FONT_SMALL` 24 px, `UI_FONT_CAPTION` and `UI_FONT_TINY` 22 px, and the 10–16 px sizes apps ask for directly draw at 22 px. Body stays under 32 px so a title and a value still share one 88 px row.
- **4.2 At most three sizes in one area**, about 1.4× apart (Qt).
- **4.3 Numbers that matter are big** (Garmin uses a separate number font; the face and level pages already do).
- **4.4 Short text.** Labels of one to three words; messages under about 35 characters a line (NN/g's BBC headlines). Text that must be long goes on its own page that scrolls.
- **4.5 Thin weights only at large sizes** (Apple *Typography*).

### 5. Colour and contrast

- **5.1 Text contrast at least 4.5 : 1, aim for 7 : 1 on small text** (Apple *Accessibility* and *Dark Mode*, WCAG AA). Bold or large text: 3 : 1. Secondary grey is `#A0A0A0` on the watch: 5.5 : 1 or more on every preset's card (`#888` was 4.0 on Mono). Accents on their card: Mint 9.1, Ocean 4.8, Sunset 5.9, Violet 5.7 (lightened to `#B577FF`; `#AA55FF` was 4.4), Cherry 4.7, Lime 7.7, Amber 8.4, Mono 14.4. Check every theme preset, not only Mint.
- **5.2 Light text on black** (Garmin "focus on light-on-dark themes" for LCD/AMOLED; less power).
- **5.3 Colour is never the only signal.** Pair it with text, an icon or a shape (Qt: "colour, shape, and text"; Apple *Feedback*). Signal strength in colour also shows the number.
- **5.4 Background colour only when it means something**, and never full-screen on a page that stays up long, such as a workout or music (Apple watchOS *Color*).
- **5.5 No large solid white areas** (Wear OS: burn-in and power).

### 6. Controls

- **6.1 Choose from a list, don't type** (Garmin "use text entry sparingly"; NN/g; roadmap phase 7). Defaults that work without setup (Garmin: "Don't require the user to use mobile app settings before they can use your app").
- **6.2 Levels: big −/+ or a slider on its own page**, never a small slider in a scrolling list (NN/g: "Sliders have their moment of glory with smartwatches"; roadmap phase 5).
- **6.3 Toggles for on/off, pickers for one of many, checkboxes for several** (Garmin *Menus*).
- **6.4 Confirm only what can't be undone**, and say exactly what will happen as a yes/no question (Garmin *Confirmations*; Apple *Feedback*: "don't warn people when data loss is the expected result"). Here: hold to confirm.
- **6.5 Credentials and long text come from the phone when possible** (Garmin mobile authentication). For Wi-Fi this is the BLE provisioning item on the roadmap.

### 7. Feedback

- **7.1 Every tap answers within about 100 ms**: press state at once, the result shortly after (Apple *Buttons*; Qt's Doherty threshold is 400 ms for the whole response).
- **7.2 Say when something can't be done, and why** (Apple *Feedback*).
- **7.3 Confirm only significant successes**; people expect actions to work (Apple).
- **7.4 Status sits next to what it describes**, not in a pop-up (Apple *Feedback*).
- **7.5 Don't make people watch a spinner.** Apple: "Avoid displaying an indeterminate progress indicator" on the watch. Show progress with a number or a bar, or let the person leave and tell them when it's done.
- **7.6 Don't time out things people have to read.** Messages that need reading close on a tap, not a timer (Apple *Accessibility*, cognitive: "Minimize use of time-boxed interface elements"). A short toast for something the person just did is fine.

### 8. Haptics and alerts

- **8.1 One meaning per vibration, used the same way everywhere.** Apple's set is a model: notification, success, failure, retry, start, stop, click. A tap's buzz is not a success buzz.
- **8.2 Don't overuse it**; a wrist buzz is stronger than a pocket buzz (Apple, Smashing). Haptics can be turned off.
- **8.3 Haptics go with what's on screen**, not instead of it (Apple: complement other feedback; Smashing: vibration for one kind of signal, a screen glow for another).
- **8.4 Interrupt only for urgent, personal things** (Smashing "worth getting tapped on the wrist for"; Apple notifications). Let the person choose which alerts buzz.
- **8.5 Haptics confirm start, stop, lap and similar** without the person looking (Wear OS).

### 9. Power, always-on and privacy

- **9.1 Screen time is the biggest cost you control** (Wear OS power table: screen on is "High"). Don't keep the screen on longer than needed.
- **9.2 No endless animation.** If a loop is needed, pause between loops for at least as long as the loop (Wear OS). Animations stop on a dimmed or always-on screen.
- **9.3 Always-on: show only what matters, keep it where it was.** At most 15 % of pixels lit, no solid fills, outlines instead of filled buttons, values updated about once a minute or "--" (Wear OS *Always on*; Apple *Always On*).
- **9.4 Ease motion to rest instead of stopping it dead** when the screen dims (Apple).
- **9.5 Hide private data on a screen others can see** (Apple *Always On*; UXDI). Passwords show as dots.
- **9.6 Radios cost more than the screen.** Scan when asked or on a slow timer, and say when data is old ("12 s ago"; Wear OS tiles: "indicate when it was last updated").
- **9.7 Work offline and say so** when a network is missing (Wear OS; Garmin "does not assume you are in a connected environment").

### 10. Motion

- **10.1 Motion follows the finger** (Apple *Accessibility*: "Tracking animations directly with people's gestures") and explains where things went (slide in from the right = deeper).
- **10.2 Little bounce, no blinking.** Tighten springs, avoid flashing and fast loops (Apple *Accessibility*). Text and cursors don't move unless the person moved them (see the keyboard fix in the roadmap's phase 7 polish).

### 11. Testing

- **11.1 Check every screen in the simulator** against this file before calling it done (`watch-ui-roadmap.md` → Testing).
- **11.2 Then on the wrist, while walking**, outdoors and in bright light (Wear OS "Test your designs in situations that involve user movement"; Smashing: cold or wet fingers).
- **11.3 With people who didn't build it** (UXDI).

## Review checklist

For a new or changed screen:

1. What's the one thing it shows or does? Is that at the top, in the biggest type? (1.1, 1.2)
2. Can it be reached from the face in four interactions or fewer? (2.1)
3. Does swipe right go back? (2.3)
4. Is every tap target at least 82 px tall (52 px for secondary), with a visible shape and a press state? (3.1, 3.5, 3.6)
5. Is any text that must be read under 22 px? Is body text near 32 px? (4.1)
6. Does all text reach 4.5 : 1 contrast on every theme preset? (5.1)
7. Is any meaning shown by colour alone? (5.3)
8. Is anything typed that could be picked instead? (6.1)
9. Is there a spinner with no number, or a message that closes on a timer? (7.5, 7.6)
10. Does anything animate, blink or move while the person isn't touching it? (9.2, 10.2)

## Sources

Read on 2026-11-05. Apple's pages were read from their DocC JSON (`developer.apple.com/tutorials/data/design/human-interface-guidelines/<page>.json`) and Garmin's through headless Chrome, because both build the page text with JavaScript.

- Apple Human Interface Guidelines: [Designing for watchOS](https://developer.apple.com/design/human-interface-guidelines/designing-for-watchos), [Layout](https://developer.apple.com/design/human-interface-guidelines/layout), [Typography](https://developer.apple.com/design/human-interface-guidelines/typography), [Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons), [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility), [Color](https://developer.apple.com/design/human-interface-guidelines/color), [Dark Mode](https://developer.apple.com/design/human-interface-guidelines/dark-mode), [Always On](https://developer.apple.com/design/human-interface-guidelines/always-on), [Feedback](https://developer.apple.com/design/human-interface-guidelines/feedback), [Playing haptics](https://developer.apple.com/design/human-interface-guidelines/playing-haptics).
- Wear OS: [Principles of Wear OS development](https://developer.android.com/training/wearables/principles), [Design principles](https://developer.android.com/design/ui/wear/guides/foundations/design-principles), [Principles for adding value](https://developer.android.com/design/ui/wear/guides/foundations/adaptive-design/principles-adding-value), [Getting started](https://developer.android.com/design/ui/wear/guides/m2-5/foundations/getting-started), [Tiles principles](https://developer.android.com/design/ui/wear/guides/m2-5/surfaces/tiles-principles), [Conserve power](https://developer.android.com/training/wearables/apps/power), [Always-on](https://developer.android.com/training/wearables/always-on). These pages give no numbers for target size, type or contrast.
- Garmin Connect IQ [User Experience Guidelines](https://developer.garmin.com/connect-iq/user-experience-guidelines/): overview, Key Design Principles, Designing Workflows and Interactions, Visual Design and Product Personalities, Views, Menus, Confirmations.
- NN/g: [The Apple Watch: User-Experience Appraisal](https://www.nngroup.com/articles/smartwatch/) (2015), [Samsung Galaxy Gear](https://www.nngroup.com/articles/samsung-watch/) (2013).
- UX Design Institute: [Designing for wearables: 7 rules](https://www.uxdesigninstitute.com/blog/designing-for-wearables/) (2023).
- Smashing Magazine: [How To Design For Smartwatches And Wearables](https://www.smashingmagazine.com/2015/02/designing-for-smartwatches-wearables/) (2015), [Intimate And Interruptive: Designing For The Power Of Apple Watch](https://www.smashingmagazine.com/2015/10/intimate-interruptive-designing-power-apple-watch/) (2015).
- Qt: [Introducing Qt's GUI Design Skill](https://www.qt.io/blog/introducing-qt-gui-design-skill-design-for-developers-in-agentic-workflows) (2026). The constraint tables it mentions are in the skill itself, which wasn't read.
- Material Design: no IoT or embedded guide was found, so it isn't used.

The NN/g and Smashing pieces are about first-generation watches; their numbers are kept only where the platform guides agree.
