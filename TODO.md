# Later

Work noted for later, with what triggered it. Bigger findings with evidence
go in REVIEW.md.

## A USB keyboard library that works

The USB keyboard (1.14.6, PR 15) works since 1.14.8 only because HBC stopped
using libogc's libwiikeyboard, which failed on the bench Wii:

- It opens only the first boot keyboard interface. The bench's Chicony
  (04f2:0841) lists its silent one first, so it never typed.
- `USBKeyboard_Scan` blocks in IOS until a key report, so a shutdown that waits
  for the reader hangs until a key is pressed.
- The caller must have run `USB_Initialize`, and nothing says so.
- No SET_IDLE, no hot-plug notice, only polling.

`channel/channelapp/source/keyboard.c` works around all four: it reads every
boot keyboard interface with async interrupt requests and cancels them at
shutdown. Either turn that into a full keyboard SDK in `sdk/` (like
`sdk/mboard`: key events with modifiers, repeat and layouts, a host-tested
core), or send libogc a fix PR that makes libwiikeyboard as robust. Then HBC
and other apps can use it.

## More keyboard keys

The keyboard works but needs a lot more bindings to feel right (Monty,
2026-11-02). The HOME menu inside apps does not read it yet.
