# Changelog

All notable changes to this project are documented here. The format follows Keep a Changelog and the project uses
Semantic Versioning.

## [1.0.1-tofu.11] - 2026-11-23 (Tofu Worldview fork, branch tofu)

### Changed

- Multibox reward choice: by stats alone, as the biggest gain in stat score over what is worn in that slot (the weaker ring or trinket slot), for any armor type the bot can wear, so a caster druid takes cloth with intellect over leather with stamina. Challenge-mode gear rules still apply. Else the choice that sells for most.

## [1.0.1-tofu.10] - 2026-11-23 (Tofu Worldview fork, branch tofu)

### Changed

- Multibox vendor: bots also sell white-quality weapons and armor (bound or not) that they cannot use or that are no upgrade.

## [1.0.1-tofu.9] - 2026-11-23 (Tofu Worldview fork, branch tofu)

### Changed

- Multibox guild bank: bots keep consumables they can use (potions, healthstones, food, drink, elixirs, flasks, scrolls).

## [1.0.1-tofu.8] - 2026-11-23 (Tofu Worldview fork, branch tofu)

### Changed

- Multibox quests: accepts are no longer copied (bots get quests by sharing, which the playerbots fork now accepts). Turn-ins whisper the quest and the reward taken, and an upgrade is equipped at once.

## [1.0.1-tofu.7] - 2026-11-23 (Tofu Worldview fork, branch tofu)

### Changed

- Multibox quests: bots copy an accepted quest only when they could take it from the quest giver (level, prerequisites, and the other normal rules), and are completed and rewarded only for quests in their own log.
- Multibox vendor: bots sell grays and soulbound weapons and armor they cannot use or that are no upgrade; unbound items are no longer sold.

### Added

- Multibox guild bank (`AutonomousAltBots.Multibox.GuildBank`, `.GuildBankTabs`, default tabs 1 and 2): when the main opens the guild bank, his altbots in his guild deposit every unbound item of white quality or better, keeping ammo, class reagents, profession tools, quest items, temporary items, and gear they would equip. No gold.

## [1.0.1-tofu.6] - 2026-11-23 (Tofu Worldview fork, branch tofu)

### Added

- Multibox (`AutonomousAltBots.Multibox.*`, off by default): a real player's own altbots in his group copy him. When he is rewarded for a quest, each of them is completed and rewarded for it at once (any objective type; prerequisites and level skipped; race, class, exclusive group, and repeat timers kept), taking the biggest upgrade among the reward choices, else the one that sells for most. Quests he accepts or abandons are accepted or abandoned too. They train class spells at level-up and at class trainers he opens, and profession ranks only for professions they already have; at vendors he opens they sell junk and unused gear, repair, and buy ammo and class reagents, never gear. They whisper him when short of gold, bag space, or quest log room. Needs playerbots fork `TofuAutonomousAlt::IsMultiboxAlt`.

## [1.0.1-tofu.5] - 2026-11-22 (Tofu Worldview fork, branch tofu)

### Added

- Dungeons for parties (`AutonomousAltBots.Dungeon.*`): Dungeon Finder queue for the lowest uncleared dungeon (table `tofu_autoalt_party_dungeon`), one tank and one healer assigned, fill for free parties smaller than five and full-five-only for challenge parties (mod-dungeon-clear Tofu fork), the clear run by mod-dungeon-clear, teleport out and regroup at the end, a retry cooldown after a failed run, one repeat run per level once everything in range is cleared, and outside invites declined while queued or inside.
- Self Crafted (`AutonomousAltBots.SelfCrafted.*`, `Challenge.SelfCraftedIndex`): class profession lists learned and paid for, craft goals (best craftable upgrade, else a skill-up), material steering, disenchanting and selling of gear the alt cannot wear; `.autoalt status` shows professions and the goal.
- Restock trips to the guild hub (`AutonomousAltBots.Hub.*`) for solo alts and whole parties: sell, repair, ammo, class and profession training, tools, reagents, recipes, and forge and anvil crafts, then back to where they were.

## [1.0.1-tofu.4] - 2026-11-22 (Tofu Worldview fork, branch tofu)

### Added

- Parties (`AutonomousAltBots.Party.*`, `.autoalt party create|add|remove|disband|list|join`): fixed sets of up to 5 enrolled alts that group while playing, led by the lowest-level member. Followers follow and regroup by teleport, turn in and accept quests at the leader's NPCs, and quests are filtered by the members present (playerbots fork). A real player who may control a member can invite it out of the party or lead the whole party. Parties respect the challenge grouping rule (`Challenge.RestrictingIndexes`). New tables `tofu_autoalt_party` and `tofu_autoalt_party_member` in acore_characters, created by the module SQL; a deleted character leaves its party.

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
