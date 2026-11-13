# MPC XL page: key audit (firmware 3.10.0, virtual XL)

Method: each control pressed once through `POST /control` (press, 0.5 s, release) from the main screen, then
the screen hash (`/screen.png`) and `/surface` LEDs compared 1.2 s later. Keys flagged "no visible reaction"
were re-run with a 0.35 s held capture and a visual check. Many keys need context the empty project does not
have (a selected pad, a loaded sample, a recorded sequence), so "no visible reaction" means "not seen here",
not "dead". The TAP_TEMPO and QLINK_TRIGGER LEDs blink on their own and were ignored.

| Control | Page label | Result |
|---|---|---|
| TRACKS / OUTPUTS / RETURNS / SUBMIXES | Tracks / Outputs / Returns / Submixes | works: switches the track type (LED follows; TRACKS was already active so no change the first time) |
| TRACK_DEC / TRACK_INC | Trk - / Trk + | works: selects previous / next track |
| RECORD_ARM | Rec arm | works: toggles track record arm LED |
| TRACK_FX | Track FX | works: opens the track's insert effects |
| MUTE | Mute | works: toggles track mute (LED) |
| SOLO | Solo | no visible reaction (nothing soloable selected) |
| QLINK | Q-Link | works: opens the Q-Link screen |
| USB_SELECT | USB select (right wing) | works: toast "USB Select 5-6" |
| RECORD_TO_ARRANGE | Arrange / Clip (under Loop on) | works: REC ARM chip switches SEQ to CLIP, SAMPLE LED changes; printed key confirmed from the product photo |
| SET | Set | no visible reaction |
| STEQ_SEQ_AUDITION | Audition | no visible reaction (LED only blinks) |
| STEP_PREV / STEP_NEXT | Previous / Next | STEP_PREV: screen changes (page step); STEP_NEXT: screen changes |
| PAD_BANK_A / B / C / D | A / B / C / D | works: bank LED moves, pads change |
| SOUNDS | Sounds | works: sounds view (toast "not available for submix track" on a submix) |
| FAVOURITES | Favourites | works: favourite plugin presets screen |
| SAVE | Save | no visible reaction within 2 s (project is untitled; unverified) |
| BROWSER | Browse | works: opens browser |
| TOUCHFX | Touch FX | works: toast "Strip: Touch FX" |
| STRIP | Touch strip | works: toast "Strip: Notes", LED toggles |
| STEM | Stem | works: toast "Stem not available when no sample is selected" |
| CHOP | Chop | works: opens sample chop |
| MODE_SAMPLE_EDIT | Sample edit | works: opens sample edit |
| GRID | Grid edit | works: opens Grid view |
| TRACK_MUTE | Track mute | works: opens Track Mute |
| PAD_MUTE | Pad mute | works: opens Pad Mute ("not available for output track" on an output track) |
| ARRANGE | Arrange | works: opens arrangement |
| MATRIX | Matrix | works: opens clip matrix |
| CHANNEL_MIXER | Mixer | works: opens track mixer |
| MODE_PROGRAM_EDIT | Track edit | works: opens track edit |
| FULL_LEVEL | Full level | works: LED toggles, no screen change |
| SIXTEEN_LEVEL | 16 levels | works: pad banks go dark, LED toggles |
| NEXT_SEQ | Next seq | works: next sequence mode screen/LED |
| ERASE | Erase | LED only (needs playback) |
| NOTE_REPEAT | Note repeat | no visible reaction |
| SHIFT, SHIFT2 | Shift (both latch) | held: no screen change on its own; SHIFT2 shows a brief change while held |
| PAD_PERFORM | Notes (printed "Notes / Pad ctrl") | works: opens Pad Perform (chord/note modes) |
| SAMPLER (added) | Sampler | works: opens Sampler (shows "Disk space low" dialog in this VM) |
| XYFX (added) | XY FX | works: opens XYFX screen |
| TAP_TEMPO | Tap tempo | no visible reaction |
| COPY | Copy | no visible reaction (needs a selection) |
| UNDO | Undo | works: toast "Undo: Set Track Mute" |
| MENU | Menu | works: opens the main menu |
| MAIN | Main | works: returns to the main screen |
| NUMERIC_0..9, ENTER | 0-9, Enter | work: screen changes (numeric entry); `+/-` shows a change while held |
| DATA_PLUS / DATA_MINUS | + / - | works: steps the selected item (MUTE LED flips) |
| METRONOME | Metro | works: toast "Metronome: On/Off", top bar icon |
| TC | TC on/off | works: TC LED and screen |
| QUANTIZE | Quantize | no visible reaction |
| LOOP_ON | Loop on | works: loop button state |
| AUTOMATION | Read / write | works: toast "Global Automation: Read" |
| PREV / NEXT / LOCATE / BAR_PREV / BAR_NEXT | < / > / Locate / << / >> | work: playhead and screen change |
| RECORD / OVERDUB / STOP / PLAY / PLAY_START | Rec / Overdub / Stop / Play / Play start | work: PLAY LED lights while running, STOP LED lights when idle; RECORD and OVERDUB light REC/PLAY LEDs |
| CURSOR_UP/DOWN/LEFT/RIGHT/CENTRE | arrows, centre | work: move selection in the screen |
| STEP1..STEP16 | step buttons under the knobs | no visible reaction on the main screen (they need a step-sequencer or drum-sequencer context) |
| QLINK 1..16 turn | knobs | works: Q-Link value moves (see page notes) |
| DATA_WHEEL turn | wheel | works |
| TOUCH_STRIP | strip | works: toast follows strip mode |
| PAD 1..16 | pads | works: sound plays; velocity and pressure |

Not on the page, from acvc.json: DELETE_KEY (no visible reaction), PAD_BANK_SELECT (no reaction),
QLINK_TRIGGER (no reaction), FOOTSWITCH_1/2 (no reaction), TOUCH_STRIP_PRESS (the strip), QLINK1..17_TOUCH (the knobs).

Labels: corrected against the product photo on 13 Nov 2026 (see the layout pass below). PRE_POST is now on the page (right wing, LED only).

Meters: an early pass saw them stay at 0. Later runs showed VU_* and MAIN_OUTPUT_* move on pad hits and AUDIO_IN_1/2 with input (README-GEN2.md, Audio). The page shows the main-out meter once a level arrives.

## Layout pass against the product photo (13 Nov 2026, late)
Added: right wing (Gain 1, Gain 2, 3/4 rec gain, Dir/main as greyed analog knobs; Main volume = page playback level;
USB select and Pre/post keys moved there). Relabelled to the print: AUTOMATION = "Read / write", PAD_PERFORM = "Notes",
RECORD_TO_ARRANGE = "Arrange / Clip" (under Loop on). Red shifted labels and the 16 step labels are copied from the photo
and are labels only: Shift + key was not tested per key. Not drawn: the Rear/Front, +48V and Stereo/Mono switches, the
wing meters. Two shifted labels were unreadable in the photo and left off (Full level, 16 levels, left Arrange).
