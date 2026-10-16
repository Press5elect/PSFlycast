# PSFlyCast privacy policy

SPDX-FileCopyrightText: 2026 Press5elect

PSFlyCast is a homebrew title I make for a jailbroken PS5. It has no accounts
of its own, no analytics, no telemetry and no advertising. I receive nothing
from it: no crash reports, no usage counts, no identifiers. Everything it
keeps stays on the console, in its title folder.

## What leaves the console, and only when

| What | Where it goes | When |
| --- | --- | --- |
| RetroAchievements: the user name, then the key the server hands back; the hash that identifies the game; unlocks, leaderboard entries and the rich presence line | retroachievements.org | only while Achievements is on and you are signed in |
| Cover pictures | GitHub (raw.githubusercontent.com, libretro-thumbnails): the request names the game's title | when a cover is downloaded |
| Descriptions and release dates | TheGamesDB, asked by Flycast's own scraper | when a game's details are looked up |
| The update check | the GitHub API, this repository's releases | at start (Settings > About, "Look for updates at start-up") and when Check for updates is used |
| An update | GitHub, the release's ZIP | when you choose to update |
| A game's online mode | Flycast's DCNet service, with the name you set under Online | only while a game's online mode is used |
| Netplay | the other console's or computer's address that you type in, directly | only during netplay |
| Network game folders | the SMB server or share you name in `network.cfg` | when that folder is used |

Each request that PSFlyCast makes itself says it comes from PSFlyCast and its
version (its User-Agent: `PSFlyCast/<version> (PlayStation 5) Flycast/<version>`). Nothing else about the
console is sent. What those services do with a request is under their own
policies (RetroAchievements, GitHub, TheGamesDB, DCNet).

## What is kept on the console

- `emu.cfg`: the settings, and with RetroAchievements the user name and the
  key the site gave (never the password). Signing out deletes the key. The
  file is not to be passed on.
- `data/achievements-unsent.txt`: unlocks and leaderboard entries made while
  the server could not be reached, kept until it answers. The requests in it
  carry that key, so the file is for the title alone. It is deleted once
  they are sent.
- `flycast-boot.log`: what happened at start-up and since, including the web
  addresses the update check and the downloads asked for. Look through it
  before you send it to anyone.
- Saves, save states, memory cards, favourites, covers and the other folders
  the manual describes.

Deleting the title folder removes everything PSFlyCast has kept.

## Changes and contact

This file changes with the title, and its history is this repository's
history. Questions: an issue on
[github.com/Press5elect/PSFlycast](https://github.com/Press5elect/PSFlycast/issues).
