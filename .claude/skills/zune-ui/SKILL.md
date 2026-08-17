---
name: zune-ui
description: Build MUI/Zune GUIs for AROS that look designed rather than assembled. Use when writing or reviewing any Zune application, window, or custom class in this repository - covers the layout vocabulary, the rules that separate a polished app from a default one, and the AROS-specific traps.
---

# Zune UI

Zune is AROS's reimplementation of MUI. Everything here was verified against
this tree, not against MUI documentation; the header is
`Developer/include/libraries/mui.h` in a built distribution, and the shortest
complete real app is
`external/aros/workbench/network/WirelessManager/wpa_supplicant/wpa_gui_amiga.c`
(236 lines, scan a list and act on a selection - structurally the same problem
as most tools here).

## The one idea everything else follows from

**MUI is a constraint layout, not a canvas.** You declare a tree of nested
groups and each object reports its own minimum, default and maximum size,
derived from the *user's* font and the *user's* frame settings. The layout
engine then solves it for the window the user dragged out.

So the single biggest tell of an amateur Zune app is **hardcoded pixels**. An
app that says "this field is 120 wide" looks fine on the author's machine and
broken on a user who picked a 16-point font. An app that never says it looks
right on both, and got there with less code.

The corollary that matters for "premium": on this platform polish is mostly
*restraint*. The user has already chosen their frames, spacing, fonts and pens
in MUI prefs. Overriding those does not look richer, it looks foreign. What you
control is **structure, grouping, alignment, and what the app says.**

## Layout vocabulary

Objects are built from macros that open with a class and close with `End`.
Attributes are `Tag, value` pairs; `Child, obj` adds to a group.

```c
Object *app = ApplicationObject,
    MUIA_Application_Title, (IPTR) "BTScan",
    SubWindow, window = WindowObject,
        MUIA_Window_Title, (IPTR) "BTScan",
        WindowContents, VGroup,
            Child, (IPTR) list,
            Child, (IPTR) buttons,
            End,
        End,
    End;
```

**Groups** — `VGroup`, `HGroup`, `ColGroup(n)`, `RowGroup(n)`, `PageGroup`,
and the `...V` variants (`VGroupV`, `ColGroupV(n)`) which are virtual groups
inside a `ScrollgroupObject`. `RegisterGroup(titles)` is the tab control.

**Frames** — one per object, never two: `NoFrame`, `ButtonFrame`, `TextFrame`,
`StringFrame`, `ReadListFrame`, `InputListFrame`, `PropFrame`, `SliderFrame`,
`GaugeFrame`, `VirtualFrame`, `GroupFrame`, `GroupFrameT("Title")`.
`GroupFrameT` also sets `MUIA_Background, MUII_GroupBack`, which is why titled
sections look inset for free.

**Spacing** — `HVSpace` (a `Rectangle` that eats slack), `HSpace(n)`,
`VSpace(n)`, `HBar(n)`, `VBar(n)`, `HCenter(obj)`, `VCenter(obj)`,
`GroupSpacing(n)`, `InnerSpacing(h,v)`.

**Buttons and labels** — `SimpleButton("Scan")`, `ImageButton(label, path)`,
`CoolImageButton(label, image)`, `PopButton(img)`. Labels come in three depths
that exist to line up with their partner's frame: `Label()` for an unframed
object, `Label1()` for a single frame (a button), `Label2()` for a double frame
(a string gadget). Using the wrong one is the classic half-pixel misalignment
in a form.

**Text styling** inside any text string, via `MUIA_Text_PreParse` or inline:
`MUIX_L` `MUIX_C` `MUIX_R` (justify), `MUIX_N` `MUIX_B` `MUIX_I` `MUIX_U`
(style), `MUIX_PT` (text pen) `MUIX_PH` (highlight pen). Use the pen macros
rather than naming a colour - they follow the user's palette.

**Images** — `DtpicObject` with `MUIA_Dtpic_Name` loads through datatypes.
This distribution builds `png.datatype` and `png.library`, so a PNG works
directly; check `Classes/DataTypes/` before assuming a format.

## The rules that make it look designed

1. **Never set a pixel size.** No `MUIA_Width`, no `MUIA_FixWidth`, on anything
   that holds text. Size comes from content and font. The exception is a decoration
   whose size is intrinsic, like an image.

2. **Give growth to exactly one thing.** In a window with a list and some
   controls, the list should absorb the slack and the controls should not.
   `MUIA_Weight` on the list, and let the buttons keep their natural height.
   A window where everything stretches equally looks unresolved.

3. **Group by meaning, and title the groups.** `GroupFrameT("Discovered
   devices")` costs one line and does more for perceived quality than any
   amount of colour. Two or three titled regions beat a flat stack of gadgets.

4. **Align labels in a two-column grid.** `ColGroup(2)` with `Label2("Name")`
   then the gadget, repeated. The labels right-align themselves; the column
   finds one width for all of them. Hand-placing labels in `HGroup`s is what
   produces ragged forms.

5. **Push buttons to one edge deliberately.** `HGroup` with `Child, HVSpace`
   before the buttons right-aligns them; `HVSpace` on both sides centres them.
   Buttons stretched across the full width look like a dialog from 1988.

6. **Every button gets a `MUIA_ControlChar`,** and the label spells it with
   `MUIA_Text_HiChar` or an underscore convention. Keyboard access is not a
   nicety on this platform.

7. **Every non-obvious control gets `MUIA_ShortHelp`.** Bubble help is cheap and
   it is the difference between a tool and a toy.

8. **`MUIA_Window_ID`** makes the window remember its position and size between
   runs. A four-character ID, unique per window. Users notice instantly when it
   is missing and never consciously notice when it is there.

9. **Ship a menustrip.** At minimum Project→About / About MUI / Quit, wired with
   `MUIM_Application_AboutMUI` and `MUIV_Application_ReturnID_Quit`. An app
   without a menu reads as a demo.

10. **Ship an icon.** `MUIA_Application_DiskObject, GetDiskObject(...)` gives
    the app its iconified appearance and its AppIcon.

11. **Disable what cannot be used** with `MUIA_Disabled` rather than letting it
    fail. A Connect button that is live with nothing selected is a bug the user
    has to discover.

12. **Say what is happening.** One `TextObject` with `TextFrame` as a status
    line, updated at each state change, is worth more than a progress bar that
    lies.

## Wiring behaviour

Notifications connect an attribute change to a method call - no polling:

```c
DoMethod(button, MUIM_Notify, MUIA_Pressed, FALSE,
         (IPTR) app, 3, MUIM_CallHook, (IPTR) &scan_hook, NULL);
```

Hooks on AROS are set up with `HookEntry` as the entry and your function as the
sub-entry (needs `<clib/alib_protos.h>`):

```c
static IPTR ScanFunc(struct Hook *hook, Object *caller, void *data);
struct Hook scan_hook = { .h_Entry = HookEntry, .h_SubEntry = (HOOKFUNC) ScanFunc };
```

A list wants a display hook that fills `columns[]`, and is asked for the header
row by being called with a NULL entry:

```c
static IPTR DisplayFunc(struct Hook *h, STRPTR *columns, STRPTR *entry)
{
    if (entry == NULL) { columns[0] = "Name"; columns[1] = "Signal"; }
    else               { columns[0] = entry[0]; columns[1] = entry[1]; }
    return TRUE;
}
```

with `MUIA_List_Format` describing the columns - `"WEIGHT=500 BAR,"` gives the
first column five times the slack and a separator after it - and
`MUIA_List_Title, TRUE` to show the header at all.

## The event loop, and the trap in it

The idiomatic loop is:

```c
while ((sigs & SIGBREAKF_CTRL_C) == 0
    && DoMethod(app, MUIM_Application_NewInput, (IPTR) &sigs)
       != MUIV_Application_ReturnID_Quit)
{
    if (sigs != 0)
        sigs = Wait(sigs | SIGBREAKF_CTRL_C);
}
```

`NewInput` hands back the signal mask MUI wants waited on. **To react to
anything outside MUI - a device, a timer, another task - OR your own signal
into that `Wait` and test it on the way out.** Do not add a second loop and do
not poll with a timeout; both make the UI stutter and both are visible.

```c
sigs = Wait(sigs | SIGBREAKF_CTRL_C | mysig);
if (sigs & mysig) refresh_from_the_other_task();
```

## AROS traps

- **Cast pointers to `IPTR` in the tag lists.** The macros are varargs. On
  32-bit m68k it happens to work without the cast, which is exactly why the
  omission survives until someone builds for 64-bit.
- **`MUIA_Application_Iconified, TRUE` before opening** starts the app
  iconified. `WirelessManager` does this because it is a daemon; a tool the user
  launched should not.
- **`MUI_DisposeObject(app)` disposes the whole tree.** Do not free children.
- **List entries are yours.** `MUIM_List_InsertSingle` stores what you gave it
  unless you supplied a construct/destruct hook; whatever you allocated for a
  row you free, and `MUIM_List_Clear` does not do it for you.
- **Check `MUIA_List_Active` against `MUIV_List_Active_Off`** before acting on a
  selection.
- **A very large image is a real cost here.** Decoding a 1500×1000 truecolour
  PNG through datatypes on a JIT'd m68k is not free; scale decorative art down
  to the size it will actually be drawn at, offline, and ship that.

## Before calling a layout done

- Resize the window to its minimum and to full screen. Nothing should overlap,
  clip, or leave one region absurdly large.
- Switch MUI prefs to a much larger font. If anything breaks, a size was
  hardcoded.
- Tab through every control. If focus skips something interactive, it is
  missing an input mode.
- Look at it with nothing selected and with nothing found. Empty states are
  where unpolished apps show it.
