# Research scripts

One-off scripts that answered a question once. They stay as the record of how
a finding was made, next to the document that holds the finding. They are not
maintained: paths, profiles and the code they drive may have moved on since.
Read one before running it; most change a Dolphin profile in your home folder.

When a script here becomes something people run again, give it `--help`,
make it safe to import, and move it to [`tools/`](../README.md).

## msgboard/: the Message Board play log (HBC 1.8.9)

How the Wii Menu records play time, worked out in a Dolphin profile with the
real Wii Menu 4.3U. The findings are in
[docs/mboard/play-log.md](../../docs/mboard/play-log.md).

| Script | What it found or checked |
| --- | --- |
| [`playrec_re.py`](msgboard/playrec_re.py) | Snapshots `cdb.vff`, writes a valid `play_rec.dat` and diffs two snapshots: how a play record becomes a log line. |
| [`cdb_add.py`](msgboard/cdb_add.py) | The prototype writer: one title added to today's message, to see the Wii Menu accept it. |
| [`c_then_menu.py`](msgboard/c_then_menu.py) | HBC's C writer and the Wii Menu writing the same day: the Wii Menu merges its record into HBC's message. |
| [`hbc_app_flow.py`](msgboard/hbc_app_flow.py) | HBC's play log with an agent app sent over Wiiload, in Dolphin. |
| [`hbc_named_flow.py`](msgboard/hbc_named_flow.py) | The name the play log gives a Wiiload upload without the agent. |
