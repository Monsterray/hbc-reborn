# Using the channel

Features of the channel itself, for the person holding the Wii Remote.

## App picker navigation

Hold a D-pad direction to repeat movement in the app picker. In Options
(1), **Grid edges: Turn page** makes Left and Right at an edge switch
pages, keeping the same row where possible. **Wrap rows** keeps the original
movement between rows. Clickable page arrows remain available for pointer
users. The choice is saved with the channel's settings.

## App badges

Options (1) > **App badges: On** marks apps in the list and the grid with a
pill in the tile's top-right corner: a blue **New** on an app you have never
run from HBC, and an amber **Updated** (with a bar along the tile's top edge)
on one whose `boot.dol`, `boot.elf`, `meta.xml` or `icon.png` changed, in
size or date, since you last ran it.

Only a launch from HBC's own app list or app page records a run and clears
the badge. An app sent with Wiiload, USBGecko or the developer network, or
started some other way, does not.

HBC keeps the record on the device the apps are on, one text file each:
`sd:/hbc/apps_seen.txt` for apps on the SD card, `usb:/hbc/apps_seen.txt` for
a USB drive (and likewise for the other devices). It has a line for each app:
the app's folder name, a tab, then the size and modified time of the four
files:

    Wii64	boot.dol=1318128@1762104532 boot.elf=- meta.xml=412@1762104532 icon.png=9211@1762104532

The file is read once when the list is scanned, and written only when
something changed: a launch, or apps that were removed. It is never written
to the Wii's NAND. Delete it to mark every app on that device as new again.
If the device is read-only or fails, the badges simply do not persist. With
badges off (the default) HBC neither reads nor writes the file, so turning
them on for the first time shows every app as new.

A theme can colour the pills with `badge_new` and `badge_updated` in its
`theme.xml`, each with a `<fill>` and a `<text>` colour (red, green, blue and
alpha, 0 to 255, like `font_color`). The defaults are a blue fill `#4fa3e0`
with text `#062a44` for New, and an amber fill `#e8b04a` with text `#3a2604`
for Updated. The text is 12 px; a font entry targeting `badge` changes it.

## Your own app order

Options > Sort applications by: **Custom** lists apps by the `<sort_id>`
number in their `meta.xml`, lowest first. Apps without one follow, by name,
and apps with the same number go by name. Any whole number works, negative
too:

```xml
<app version="1">
  <name>WiiXplorer</name>
  <sort_id>10</sort_id>
  ...
</app>
```

Edit the files on the card, or let `hbc.py order` do it from the PC:

```sh
python3 tools/hbc.py order sd:/apps                                # the order now
python3 tools/hbc.py order sd:/apps wiixplorer usbloader_gx nintendont
```

The second gives those three 10, 20 and 30 and leaves the rest as they are;
steps of 10 leave room to put an app between two others by hand. An app's
update that brings its own `meta.xml` drops its number, so run `order`
again afterwards.
