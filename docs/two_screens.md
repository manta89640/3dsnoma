# The 3DS's two screens

How the port puts the channel on the 3DS's two screens: the top screen is for looking (the room,
the island, the Sound Player's stage), the bottom screen is for touching (every button, list and
question). This is the redesign's foundation (phases P2–P6 of the plan); the pieces are in a dozen
files, collected here.

## One frame, two screens

The channel draws one picture a frame, its 640x456 frame, through the GX FIFO. A scene that wants
something on the bottom screen draws it in the same frame, after `engine::ui::screen::to_bottom(region)`,
and goes back with `to_top()`. Both write a pseudo-command into the FIFO (`runtime/hw/gx/gx.h`
`SCREEN_OPCODE`, 0x51), so the switch lands between the draws in order on the GPU thread.

- `platform/3ds/gx_pica.cpp`: a second render target (240x320, RGB565 into the console's frame
  buffer), cleared white at its first draw in a frame; `set_xform` and the scissor map the given
  rectangle of the frame onto it. Each draw keeps the mapping it was issued under.
- `runtime/hw/gx/raster.cpp` (host): a second EFB; each draw on it is mapped through its own
  rectangle onto the whole buffer, then box-filtered to 320x240 (`gx_host_bottom_screen`,
  `NOMA_DUMP_3DS`), so the host preview matches the 3DS when a frame uses several rectangles.

The 2D projection is always the layouts' 608x456 units (x ±304, y ±228). A *region* is a rectangle
of that space shown on the bottom screen (`engine/ui/screen.h`):

| Region | Rectangle (layout units) | Pixels a unit | Used by |
|---|---|---|---|
| `BOTTOM` | x −304..229, y −228..172 (533x400) | 0.6 | the native scenes (Sound Player, island, test card), the room's panel, the system prompt |
| `FULL` | x −304..304, y −228..228 (608x456) | 0.53 | the channel's own scenes shown whole (“away” scenes, the first start's setup) |

`BOTTOM` is the bottom left of the layouts, where the channel keeps its Home tab and the room its
bar, so the channel's corner parts land in the bottom screen's corner unmoved.

## The 16:9 squeeze and 4:3 layouts

On a 16:9 TV the channel lays its layouts out 760 units wide (the port's 5:3; `app_VideoMode_Set`)
and squeezes the panes laid out for 4:3 by 0.8 (the location adjust, `layout_sDrawInfo` +0x50
bit 0x20), which the TV's (or the top screen's) wider pixels stretch back. The bottom screen's
pixels are square, so:

- `screen::squeeze(false)` around a bottom-screen layout's update and draw (its matrices are worked
  out in both), `squeeze(true)` after;
- a channel scene shown whole on the bottom screen is *made* for 4:3: `screen::layouts_43(true)`
  sets app::VideoMode's layout width to 608 and clears wide before the scene's layouts are made
  (their edge parts are placed from the width as they are made), and `layouts_43(false)` before the
  room is made again. Its cameras are then 4:3 too, which is the bottom screen's shape.

## Who has the bottom screen

`screen::claim(region)` / `release()`: the platform stops the log console drawing there
(`platform_bottom_claim`; the console shows only with `NOMA_DEBUG_CONSOLE=1`, otherwise an unused
bottom screen is white), and the touch screen points into the region (`input_set_touch_region`):
the game's pointer is where the touch is, in layout units (x/304, −y/228), and stays inside the
region. `claimed()` / `claimed_region()` say who has it.

| Who | Region | Where |
|---|---|---|
| The room's panel (its buttons) | `BOTTOM` | `engine/room/panel.cpp`, made by the room scene's ctor, deleted first by its dtor (`runtime/patches/room.cpp`) |
| The room's other screens while they show (“away”) | `FULL` | `engine::room::leaving` at `SceneTransit::Go` (To 0, 5, 6, 7, 8) and `away_for_setup` (the setup's ctor); `back()` in the room's ctor |
| Native scenes | `BOTTOM` | their ctors and dtors (Sound Player, island, test card) |
| The system prompt, when nobody has it | `BOTTOM` | `runtime/patches/overlays.cpp` `app_SystemPrompt_Update` |

## The app's overlays

`app_Application_Draw` is native (`runtime/patches/overlays.cpp`): the scene (on the bottom screen
whole while away, with a white quad on the top screen), the wipe, then the data notice, the system
prompt, the HOME menu and the way out's fade. While a scene has the bottom screen the wipe and the
way out's fade are drawn there too (the wipe through a region centred on its circle). The system
prompt's dialog is moved into the middle of the claimed region (`place`) before its update and
draw, and drawn on the bottom screen without its hand.

## Input

`engine/input/remote.cpp` feeds the game's KPAD from the 3DS's controls:

- touch: the pointer, and A held from its second read (`TAP_READY` 2: the first read puts the
  pointer there) until it lifts, as the Wii pressed at A-down: held arrows repeat (the channel's
  `layout::RepeatButton`s, the lists' tabs); the game's cursor takes a touch's pointer at once,
  skipping the 10 updates a remote's pointer settles for (`layout_Cursor_Update`,
  runtime/patches/overlays.cpp: `input_pointer_from_touch`);
- a touch that begins on a list's rows (`input_set_drag_area`, renewed by the list every frame)
  presses only as a tap at its release; once it is a drag (`input_mark_drag`) it presses nothing,
  the pointer held where it began and taken away when it lets go;
- there is no pointer but the touch's (`pointer_on`): the remote points while a finger is down, for
  the read after it lifts (`LIFT_READS`: a press made at A's release lands where the finger was) and
  while a tap's A is to come, never otherwise; a new region (`input_set_touch_region`) has none; a
  D-pad move of a list's highlight sends it away (`input_pointer_away`), so A presses the highlight;
  a script's own pointer (`P(x;y)`, nudges) is the one exception (`input_script_pointer`);
- on Wuhu Island the jump is the A button's own (`wiimote_a_button_pressed/held`): a touch holds the
  game's A for the bottom screen's menus;
- the circle pad is the D-pad out of stick mode (platform/3ds/main.cpp); only a script's `S(x;y)`
  still nudges a pointer (`WiimoteInput::nudge_x/y`), inside the claimed region too;
- the Wii hand (each `layout::Cursor`'s layouts) shows only for a script's nudged pointer
  (`input_pointer_by_stick`, `layout_Cursor_Update`): never in play on the 3DS;
- a hooked button whose push sends its scene away keeps its pushed look
  (`engine::system::hold_button_look`: the cursor's unfocus and refocus held off; the room's panel
  poses it again after the button's own end of its push), and the room's panel asks for the scene as
  the push starts (`on_push_start`), not after its 11 frames;
- presses between two of the game's reads are kept for the next (`wiimote_take_pressed`);
- a drag on a list scrolls it and coasts when flicked (`engine/ui/drag_scroll.h`).

## Making a bottom-screen part

1. Build it from the channel's layouts (`engine/ui/lyt.h`; `engine/ui/menu`, `confirm`,
   `list_window` for menus, questions and lists), hanging from a layout of its own; place it in
   the region's layout units (`BOTTOM_MIDDLE_X/Y`, `Confirm::DROP` for the dialog's height).
2. Update and draw it between `squeeze(false)` and `squeeze(true)`; draw it between
   `to_bottom(region)` and `to_top()`.
3. Claim the region while it shows; hook its buttons (`engine::system::hook_button`) and get their
   pushes from the HostScene (or `set_push_listener` inside one of the channel's scenes).
4. Guest objects that must last are made at the start only (`host_scene_boot`): the channel's
   heaps are stacks, and a block made mid-scene pins the memory of the scene under it.

`NOMA_DUMP_3DS=1` (host) and `NOMA_SHOT_EVERY` (3DS) show both screens; `NOMA_TEST_CARD=1` shows
the calibration card for text and target sizes.
