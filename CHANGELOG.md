# Changelog

All notable changes to this project are documented here. The format follows Keep a Changelog and the project uses
Semantic Versioning.

## [1.0.1-tofu.3] - 2026-11-22 (Tofu Worldview fork, branch tofu)

### Added

- Hardcore survival for Hardcore alts that may play (`Challenge.AllowHardcore = 1`): flagged for the Tofu playerbots fork (no new fight below `Hardcore.RestHealthPct`, targets at most `Hardcore.MaxLevelsAbove` above, no level-up teleport, no outdoor PvP), the `tofu hardcore` strategy in both engines (eat back to the rest threshold, flee below `Hardcore.FleeHealthPct`), and no `pvp` strategy. Applies while grouped with a real player too.

## [1.0.1-tofu.2] - 2026-11-22 (Tofu Worldview fork, branch tofu)

### Changed

- Challenge modes (read from the character's mod-challenge-modes settings, `AutonomousAltBots.Challenge.*`): a Quest Only alt drops the grind strategy; a Hardcore character cannot be turned on unless `Challenge.AllowHardcore = 1`, and an enrolled character that is on Hardcore is turned off.
- Fixed: a Hardcore alt that died was logged back in every check, then killed and kicked by mod-challenge-modes. Now it is turned off and logged out as soon as it dies, and a character flagged dead is never logged in.

## [1.0.1-tofu.1] - 2026-11-19 (Tofu Worldview fork, branch tofu)

### Changed

- Replaced both integration patches with the Tofu fork of mod-playerbots (`TofuAutonomousAlt.h`); the core is no longer patched. Built against mod-playerbots ecc4a53 and the Playerbot core fork at 32fd347.
- Autonomous alts log in only while at least one real player is in the world and log out `NoPlayerLogoutDelaySeconds` after the last one leaves, whatever `Playerbots.DisabledWithoutRealPlayer` is set to.
- Commands are player level: players manage characters on their own account; GMs and the console manage any character. `.autoalt reload` stays GM only.
- While grouped with a real player who may control it, the alt follows that player and takes commands; it resumes on its own when the player leaves or logs out. `AutonomousAltBots.Independent` is removed.
- Level-up behaviour (paid class-trainer spells, automatic talents, teleport to a zone for the new level) and hunter ammo restocking are provided by the playerbots fork and configured here (`LevelUp.*`, `Restock.*`).
- Fixed: upstream 1.0.1 does not compile against mod-playerbots ecc4a53 (`PlayerbotAI::IsRealPlayer()` no longer exists).
- Removed the patch installers and the upstream CI workflow, which builds against upstream mod-playerbots.

## [Unreleased]

### Changed

- Made `.autoalt` the primary command namespace while retaining `.autonomousalt` as a backward-compatible alias.

## [1.0.1] - 2026-08-03

### Fixed

- Corrected the clean-build workflow's AzerothCore `APPS_BUILD` value from `worldserver` to `world-only`.
- Updated the official checkout and compiler-cache actions to their current Node.js 24 releases.

## [1.0.0] - 2026-08-03

### Added

- Persistent per-character autonomous-alt selection.
- Masterless login and automatic relogin after worldserver restarts.
- GM/console start, stop, enable, disable, status, list, and reload commands.
- Quest, grind, New RPG, and independent-mode strategy management.
- Full off-screen activity override for selected alts.
- Protection from random-bot guild assignment and fixed-level XP flags.
- In-flight login cancellation guard.
- Versioned AzerothCore and mod-playerbots integration patches.
- Cross-platform patch installers and GitHub Actions compile workflow.
