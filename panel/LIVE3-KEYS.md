# MPC Live III page: key audit (firmware 3.10.0, virtual Live III, MODEL=acvg)

Method: each control in acvg.json's button table pressed once through `POST /control` (press, 0.45 s, release), from the
main screen after STOP and two MAINs. Screen hashes (`/screen.png`) taken before, 0.45 s into the hold and 1.5 s after
release; `/surface` LEDs compared the same way (the blinking TAP_TEMPO, QLINK_TRIGGER and SET lamps and the meters ignored).
Changed screens were looked at, not just hashed. A first, noisier pass (playback running, modes carried over) was redone
from a clean start. QLINK5_TOUCH..QLINK8_TOUCH were not pressed (the server refuses them). The project in the VM had
only submix tracks, so keys that need a drum, plugin or sample track (track edit, sounds, track mute, note repeat, copy)
show less than they would on a real project: "no visible reaction" means "not seen here", not "dead".

Totals: 105 names in the table; 61 change the screen or show a toast, 26 only move a lamp
(counting the 16 step buttons), 12 show nothing at all, 6 not audited (4 blocked knob touches,
SHIFT, the strip press name).

## Printed labels and the control behind them

Page label (sub-label is the shifted function as printed on Akai's product photo; none of those were tested with Shift) - control - evidence.

| Printed key | Control | Status | Evidence |
|---|---|---|---|
| Set | SET | guess (name only) | no visible reaction |
| 1..16 step buttons | STEP1..STEP16 | confirmed by lamp | page lamps follow the app's step RGB; no screen change on the main view |
| Prev / Next | PREV / NEXT | guess (name only) | LED only |
| Note Repeat | NOTE_REPEAT | guess (name only) | no visible reaction |
| Full Level | FULL_LEVEL | confirmed | opens the User Level dialog |
| 16 Level | SIXTEEN_LEVEL | confirmed by lamp | LED toggles, pad bank LEDs go dark |
| Erase | ERASE | partly (LED only) | lamp changes; needs playback to do more |
| A/E, B/F, C/G, D/H | PAD_BANK_A..D | confirmed | bank LED moves, toast "Bank B" |
| Copy | COPY | guess (name only) | no visible reaction |
| Notes | PAD_PERFORM (was NEXT_SEQ) | changed, confirmed | opens PAD PERFORM on the Notes tab; NEXT_SEQ opens the Next Seq screen instead |
| TC | TC | confirmed | Timing Correct dialog |
| Undo | UNDO | confirmed | toast "Undo: Set Track Mute" |
| Load | BROWSER | confirmed | opens the file browser with a Load button |
| Save | SAVE | confirmed | Save dialog |
| Sample Edit | MODE_SAMPLE_EDIT | confirmed | opens Sample Edit |
| Track Mute | TRACK_MUTE | partly (LED only) | lamp changes; no screen on a submix track |
| Q-Link | QLINK_BANK | confirmed | steps the bank LEDs and the overlay's bank |
| T. Strip | STRIP | partly (LED only) | lamp toggles |
| Menu | MENU | confirmed | opens the main menu while held |
| Main | MAIN | confirmed | back to the main view |
| Shift | SHIFT | latch, not audited | |
| Sounds | SOUNDS | confirmed | Sounds / Track Type view |
| Edit | MODE_PROGRAM_EDIT (was MODE_SEQUENCE_EDIT) | changed, partly | responds (screen "Track edit not available for submix track"); MODE_SEQUENCE_EDIT showed nothing at all |
| Arrange | ARRANGE | confirmed | opens the arrangement |
| Mixer | CHANNEL_MIXER | confirmed | opens the track mixer. MIX, MODE_PROGRAM_MIX and PAD_MIXER open the pad mixer instead |
| Tap Tempo | TAP_TEMPO | guess (name only) | no visible reaction |
| Loop On | LOOP_ON | confirmed | loop button toggles |
| < > | STEP_PREV / STEP_NEXT | confirmed | playhead moves one step |
| Locate | LOCATE | confirmed | Locate dialog |
| << >> | BAR_PREV / BAR_NEXT | confirmed | playhead moves one bar |
| Mic On | MIC_SPEAKER | confirmed | toast "Audio Inputs 1 & 2 set to internal microphone" |
| Rec to Arrange | RECORD_TO_ARRANGE | confirmed | LED and REC ARM chip change |
| Record | RECORD | confirmed | toast "Recording: To Arrangement", record button red |
| Over Dub | OVERDUB | confirmed | toast "Overdubbing: To Arrangement" |
| Stop / Play | STOP / PLAY | confirmed | lamps and playhead |
| Play Start | PLAY_START | confirmed | plays |
| + / - | DATA_PLUS / DATA_MINUS | confirmed | steps the selected item |

Still a guess: Set, Prev/Next, Note Repeat, Copy, Tap Tempo (no visible reaction in the empty submix project), plus the
lamp-only Erase, Track Mute and T. Strip. Not checked at all: every sub-label (the shifted functions), because SHIFT was not
held during presses. Unprinted keys with a real reaction that live under "More keys": NEXT_SEQ (Next Seq screen), MIX and
PAD_MIXER (pad mixer), MODE_PROGRAM_MIX, MODE_SONG, SAMPLER, XYFX, READ_WRITE (automation toast), MUTE, TRACK_NEXT/PREV,
QLINK_PROJECT / PROGRAM / PAD_SCENE / PAD_PARAM / SCREEN (toasts "Q-Links: ..."), LCD_F2..F4, cursor keys.

## Every control

| Control | Page label | Result |
|---|---|---|
| QLINK_BANK | Q-Link | Steps the Q-Link bank: LEDs QLINK_BANK_1..4 move one bank per press; the 4-knob overlay then shows that bank (bank 4 = Sequence Tempo / Length / Loop Start / Loop End) |
| MODE_PROGRAM_MIX | (More keys) | Opens the pad mixer (screen "Pad mixer not available for submix track" on a submix); LEDs MODE_PROGRAM_MIX, MIX, EMULATE_MPC60 |
| STRIP | T. strip | LED only (4 to 5); no toast or screen change seen |
| MODE_SEQUENCE_EDIT | (More keys) | No visible reaction, no LED change (tried on a submix track; may need a drum or plugin track) |
| MODE_SAMPLE_EDIT | Sample edit | Opens Sample Edit (waveform, TRIM view, Stems) |
| MODE_SONG | (More keys) | Opens Song mode; pad bank LEDs change |
| ERASE | Erase | LED only (4 to 5) |
| SET | Set | No visible reaction (its lamp blinks on its own) |
| NOTE_REPEAT | Note repeat | No visible reaction |
| MODE_PROGRAM_EDIT | Edit | Screen "Track edit not available for submix track"; LED MODE_PROGRAM_EDIT 4 to 5 |
| LCD_F2 | (More keys) | Screen changes (selects a track/submix on the main view) |
| LCD_F3 | (More keys) | Screen changes (selects a track/submix) |
| LCD_F4 | (More keys) | Screen changes (selects a track/submix) |
| RECORD_TO_ARRANGE | Rec to arrange | LED 3 to 1 and the REC ARM chip on the main screen changes (record target) |
| MIX | (More keys) | Same as MODE_PROGRAM_MIX: pad mixer |
| MIC_SPEAKER | Mic on | Toast "Audio Inputs 1 & 2 set to internal microphone" |
| PAD_BANK_A | A/E | Bank LED A; toast "Bank A" |
| PAD_BANK_B | B/F | LED A to B; toast "Bank B" |
| PAD_BANK_C | C/G | LED B to C |
| PAD_BANK_D | D/H | LED C to D |
| FULL_LEVEL | Full level | Opens the "User Level" dialog (velocity field) |
| SIXTEEN_LEVEL | 16 level | LED toggles and the four pad-bank LEDs go dark (16 levels) |
| BROWSER | Load | Opens the file browser (Places / Content / Expansions, Load button); LED STEP_SEQ lit |
| NEXT_SEQ | (More keys) | Opens the Next Seq screen (sequence list and 16-pad queue); LED NEXT_SEQ 1 to 3 |
| TRACK_MUTE | Track mute | LED only here (4 to 5); no screen seen on a submix track |
| TC | TC | Opens the Timing Correct dialog; LED 7 to 8 |
| PROGRAM | (More keys) | LED only: PROGRAM / SAMPLE / NO_FILTER lamps rotate as a group |
| SAMPLE | (More keys) | LED only (same group) |
| NO_FILTER | (More keys) | LED only (same group) |
| SHIFT | Shift | Latched on the page; not audited here (re-lights UNDO, STOP, NEXT lamps per LIVE3-CONTROLS.md) |
| SAVE | Save | Opens the Save dialog (Project, Project As, New Project, Track ...); LED 4 to 5 |
| WINDOW | (More keys) | No visible reaction |
| MAIN | Main | Returns to the main view (used as the reset between presses) |
| TAP_TEMPO | Tap tempo | No visible reaction (lamp blinks with tempo on its own) |
| DATA_PLUS | + | Steps the selected item (screen changes) |
| DATA_MINUS | − | Steps the selected item |
| CURSOR_UP | (More keys) | Moves the selection on the main view |
| CURSOR_DOWN | (More keys) | Moves the selection |
| CURSOR_CENTRE | (More keys) | Opens the selected field's list (Clip list) |
| QLINK_PROJECT | (More keys) | Toast "Q-Links: Project 1"; bank LEDs move |
| QLINK_PROGRAM | (More keys) | Toast "Q-Links: Track" over a clip list |
| QLINK_PAD_SCENE | (More keys) | Toast "Q-Links: Pad Scene" |
| QLINK_PAD_PARAM | (More keys) | Toast "Q-Links: Pad Parameter" |
| QLINK_SCREEN | (More keys) | Toast "Q-Links: Screen"; bank LEDs move |
| FOOTSWITCH_2 | (More keys) | No visible reaction |
| TOUCH_STRIP_PRESS | (More keys) | Use TOUCH_STRIP press/move/release instead (verified live: strip segment LED lights, see Verification) |
| CURSOR_RIGHT | (More keys) | Moves the selection right (Punch In highlighted) |
| UNDO | Undo | Toast "Undo: Set Track Mute" |
| STEP_PREV | < | Moves the playhead back one step (position readout changes) |
| STEP_NEXT | > | Moves the playhead forward one step |
| LOCATE | Locate | Opens the Locate dialog (Time / Locators, step and bar arrows, loop, auto record) |
| BAR_PREV | << | Moves the playhead back a bar |
| BAR_NEXT | >> | Moves the playhead forward a bar |
| RECORD | Record | Toast "Recording: To Arrangement", record button red; RECORD and PLAY lamps light |
| LOOP_ON | Loop on | Loop button toggles (blue); LED 1 to 3 |
| READ_WRITE | (More keys) | Toast "Track Automation: Write"; automation icon changes |
| MUTE | (More keys) | Mute chip toggles on the main view; UNDO lamp lights |
| SOLO | (More keys) | No visible reaction |
| F_KEY | (More keys) | LED only (0 to 4/5) |
| OVERDUB | Over dub | Toast "Overdubbing: To Arrangement"; record button red |
| STOP | Stop | Stops playback (STOP lamp 3, PLAY 1); also the reset between presses |
| PLAY | Play | Starts playback (playhead moves); PLAY lamp 3, STOP 1 |
| PLAY_START | Play start | Starts playback; same lamps as PLAY |
| QLINK1_TOUCH | (More keys) | Opens the 4-knob Q-Link overlay while held |
| QLINK2_TOUCH | (More keys) | Overlay while held |
| QLINK3_TOUCH | (More keys) | Overlay while held |
| QLINK4_TOUCH | (More keys) | Overlay while held |
| QLINK5_TOUCH | (More keys) | Not pressed: blocked (hangs the app, see LIVE3-CONTROLS.md) |
| QLINK6_TOUCH | (More keys) | Not pressed: blocked (hangs the app, see LIVE3-CONTROLS.md) |
| QLINK7_TOUCH | (More keys) | Not pressed: blocked (hangs the app, see LIVE3-CONTROLS.md) |
| QLINK8_TOUCH | (More keys) | Not pressed: blocked (hangs the app, see LIVE3-CONTROLS.md) |
| STEP1 | 1 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP2 | 2 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP3 | 3 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP4 | 4 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP5 | 5 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP6 | 6 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP7 | 7 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP8 | 8 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP9 | 9 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP10 | 10 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP11 | 11 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP12 | 12 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP13 | 13 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP14 | 14 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP15 | 15 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| STEP16 | 16 | No screen change on the main view; the step lamp follows the app (STEP1 sets RGB 0x40, LIVE3-CONTROLS.md) |
| NEXT | Next | LED only (1 to 3 while held) |
| PREV | Prev | LED only (1 to 3 while held) |
| NUMERIC_PLUS_MINUS | (More keys) | No visible reaction |
| ARRANGE | Arrange | Opens the arrangement (MRQ / TRK / ARR, Cut Copy Paste ...); LED PAD_ASSIGN |
| QLINK_TRIGGER | (More keys) | No visible reaction (lamp blinks) |
| PAD_PERFORM | Notes | Opens PAD PERFORM on the Notes tab (Chromatic / Notes / Chords / Progressions, scale, Use Aftertouch); LED EMULATE_MPC3000, pad banks change |
| PAD_MIXER | (More keys) | Same again: pad mixer |
| CHANNEL_MIXER | Mixer | Opens the track mixer (8 strips; Volume, Pan & Volume, Sends, Effects, I/O tabs) |
| INSTANCE_MUTE | (More keys) | LED only (1 to 3) |
| INSTANCE_SOLO | (More keys) | No visible reaction |
| TRACK_NEXT | (More keys) | Selects the next track |
| TRACK_PREV | (More keys) | Selects the previous track |
| SOUNDS | Sounds | Opens the Sounds / Track Type view ("Sounds not available for submix track" on a submix; Add Track dialog otherwise) |
| COPY | Copy | No visible reaction (needs a selection) |
| MENU | Menu | Opens the main menu while held (Browser, Track Mixer, Pad Mixer, Sounds, XYFX, Arrange, Next Sequence, Save, Project, Preferences); LED 1 to 3 |
| DELETE_KEY | (More keys) | No visible reaction |
| SAMPLER | (More keys) | Opens the Sampler screen; LED 4 to 5 |
| XYFX | (More keys) | Opens XYFX; LED 4 to 5 |

## Verification of the page (live, 13 Nov 2026)

- Pad hit by position: centre click sent note 0x25 velocity 0x7E (126); a click at the corner sent 0x1B (27).
- PLAY then Space (PLAY lamp 3, so Space sent STOP: PLAY 1 / STOP 3), Space again (PLAY 3), then the STOP key.
- Bank key: lamps QLINK_BANK_1..4 went 0,0,3,0 to 0,0,0,3 and the page's four bank dots and "Bank 4" followed.
- Q-Link 1 dragged with the overlay open (touch 0x54 held, CC 0x10 turns): the top overlay knob (Sequence Tempo) went 128 to 130 bpm.
- Strip drag from 20% to 65%: segment LED TOUCH_STRIP_16 lit (127) and the page's strip bar followed; arrow keys on the focused strip send a touch.
- Strip segments 3-7 and 22 share a CC with the Q-Link mode keys and F_KEY, whose lamps are 1-8, so the page reads those segments only above 16.

## Layout pass against Akai's product photo (13 Nov 2026, late)
Checked against the retail top-down photo (akai-mpc-live-iii-top-down). Key placement already matched. Changed:
printed sub-labels to the photo's wording (User level, To keygrp, Notes = Next seq, Load = Project, Sample edit = Sampler,
Main = Track view, Sounds = Favorites, Tap tempo has none), full step labels, invented group headings removed ("Now"
kept, it is printed), Main volume knob added top left (page playback level only; analog on the unit). "More keys" renamed
"Hidden controls": they are in the app's table but have no key on the unit. Sub-labels are print only, not tested with Shift.
