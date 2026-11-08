# Documents

Every document in hbc-reborn, by who it is for. Start with the
[README](../README.md); the SDKs each have their own README in
[sdk/](../sdk/README.md).

## Using the channel

| Document | What it covers |
| --- | --- |
| [using-hbc.md](using-hbc.md) | The app picker, app badges, your own app order |
| [README: installing](../README.md#installing-the-channel-on-a-wii) | Installing the WAD, and replacing the original Homebrew Channel |

## Writing apps

| Document | What it covers |
| --- | --- |
| [hbc-py.md](hbc-py.md) | Every `hbc.py` command, seeing your app's output, a typical loop |
| [devnet.md](devnet.md) | The developer network's wire format, for other tools |
| [sdk/README.md](../sdk/README.md) | The SDKs: network log, in-app agent, speaker, slot light, Message Board, Miis |
| [mboard/](mboard/README.md) | The Message Board library in depth: every call, the files, the proof on a Wii |
| [mii/](mii/README.md) | Miis: the data, the database, a Mii in a message |

## Contributing

| Document | What it covers |
| --- | --- |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | Building, every test and what it needs, the rules for changes |
| [AGENTS.md](../AGENTS.md) | The rules for agents, and the project skill |
| [review.md](review.md) | The review plan and the open items |
| [history/](history/) | Each release's findings and hardware results |
| [CHANGELOG.md](../CHANGELOG.md) | One line per version |
| [plans/](plans/) | Plans in progress: [the November 2026 reorganization](plans/2026-11-reorg.md), [sdk/mii 2.0](plans/mii-sdk-2.md), [sdk/wc24](plans/wc24.md) |

## Research

Notes on how the Wii works, with their sources. They explain the code; they
are not instructions.

| Document | What it covers |
| --- | --- |
| [mboard/play-log.md](mboard/play-log.md) | How the Wii Menu records play time |
| [mboard/storage.md](mboard/storage.md) | How a Message Board message is stored, and what sets its sender, icon and contents |
| [mii/bodies.md](mii/bodies.md) | The Mii Channel's bodies |
| [mii/rendering.md](mii/rendering.md) | How the Wii draws Miis |
| [research/return-path.md](research/return-path.md) | HBC's reload stub and the return path to an installed channel |
| [design/wii-remote-icons.svg](design/wii-remote-icons.svg) | Wii Remote icon designs |
