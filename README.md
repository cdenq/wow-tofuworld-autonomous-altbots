# mod-autonomous-altbots (Tofu Worldview fork)

This is the `tofu` branch, a fork of [Gutwormy/mod-autonomous-altbots](https://github.com/Gutwormy/mod-autonomous-altbots) 1.0.1 for the Tofu Worldview server. Everything below the line describes upstream; where it disagrees with this section, this section wins.

- **Requires** the Tofu fork of mod-playerbots: https://github.com/cdenq/wow-tofuworld-playerbots, branch `tofu` (based on mod-playerbots ecc4a53). It provides `TofuAutonomousAlt.h`. No core patch and no patch installer.
- **Online only with players:** autonomous alts log in only while at least one real player is in the world, and log out `AutonomousAltBots.NoPlayerLogoutDelaySeconds` (60) after the last one leaves. `Playerbots.DisabledWithoutRealPlayer` can stay 1.
- **Commands** (`.autoalt start|stop|status|list <name>`) work for players on characters of their own account; GMs and the console can manage any character. `.autoalt reload` is GM only.
- **Grouping:** a real player who may control the alt (same account, same guild if `Playerbots.AllowGuildBots`, a linked account, or a GM) can invite it; it follows and takes commands like an altbot, then goes back to playing on its own after the player leaves or logs out. Other invites are declined.
- **On level-up** (playerbots fork): learns class-trainer spells and pays the trainer cost, spends talent points in its current tree, and teleports to a zone for its new level (never a city). Class quest spells are granted as for random bots.
- **Hunter ammo:** below `AutonomousAltBots.Restock.AmmoThreshold`, a hunter that can afford ammo walks to the nearest friendly ammo vendor on its map and buys with its own gold; without gold it keeps grinding.
- **Challenge modes:** a Quest Only alt does not grind; Hardcore characters cannot be turned on (`AutonomousAltBots.Challenge.AllowHardcore`), and a Hardcore alt that dies is turned off and logged out for good.
- **Hardcore survival** (with `AllowHardcore = 1`): no new fight below 80% health (it eats first), flee below 30%, targets at most 2 levels above, no level-up teleport, no PvP (`AutonomousAltBots.Hardcore.*`).
- **Parties** (`AutonomousAltBots.Party.Enable = 1`): a party is a fixed set of up to 5 enrolled alts from any accounts (`.autoalt party ...`). Members that are playing group together, the lowest-level member leads, and the rest follow it, teleporting to it on the same map or another map when more than `Party.RegroupDistance` yards away. Followers turn in and accept quests at the NPCs the leader visits. Quests are chosen for the members present: a group quest needs at least its suggested player count present, and an elite quest at least `Party.EliteMinMembers`; a quest already in the log is kept while the full roster would qualify. An alt not in a party never groups with anyone except a real player who may control it. A real player who may control a member can invite just that member out of the party (it rejoins when it leaves that group), or lead the whole party with `.autoalt party join <party>`. A party is all challenge characters (any hard mode except Progression) or none. Known limit: a quest that starts from an item the leader loots does not reach the followers.
- **Dungeons** (`AutonomousAltBots.Dungeon.Enable = 1`, needs parties and the Tofu fork of mod-dungeon-clear, https://github.com/cdenq/wow-tofuworld-dungeon-clear branch `tofu`): a party queues through the Dungeon Finder for the lowest dungeon it has not cleared, with one tank and one healer assigned. A free party smaller than five gets pool bots to fill it; a challenge party goes only as a full five of its own members. mod-dungeon-clear runs the clear (no post-combat resurrection when a member is Hardcore). When the run ends the party is teleported out, regroups, and goes back to questing; once every dungeon in range is cleared it repeats one per level. Members decline outside invites while queued or inside. `.autoalt party list <party>` shows what the party is doing.
- **Self Crafted** (`AutonomousAltBots.SelfCrafted.*`): an alt on the Self Crafted challenge learns its class's profession list (existing professions are kept), pays for ranks and recipes, crafts toward the best upgrade it can make (else a skill-up), and steers toward the materials: mining, skinnable beasts, humanoids for cloth, disenchanting, and prospecting. Gear it cannot wear is disenchanted (enchanters) or sold. `.autoalt status <name>` shows its professions and goal.
- **Restock trips** (`AutonomousAltBots.Hub.Enable = 1`): when bags fill, gear wears out, ammo runs low, a tool is missing, a forge or anvil craft is ready, or a profession rank is waiting, a solo alt or the whole party teleports to the guild hub, each bot walks its own errands (sell and repair, ammo, class trainer, profession trainers, tools, reagents, recipes, forge and anvil), and they teleport back together.
- **Data safety:** enrolled alts are never in the random bot pool, so the random-bot randomize step (which empties bags) never runs on them.

---

# mod-autonomous-altbots

Persistent, independently active alt characters for the AzerothCore Playerbots branch.

The module lets an administrator select ordinary player-created characters and keep them logged in as autonomous
Playerbots. Selected characters continue questing, grinding, travelling, looting, equipping upgrades, and gaining normal
character progression without their owner being logged in or nearby.

## Status

Version **1.0.1** is the current public release. It has been compiled successfully against:

- `mod-playerbots/azerothcore-wotlk`, branch `Playerbot`
- `mod-playerbots/mod-playerbots`, branch `master`

The module depends on two small integration patches included in [`patches/`](patches/README.md). It is not compatible
with stock AzerothCore without the Playerbots fork and `mod-playerbots`.

## Features

- Per-character opt-in; it never takes over every character on an account.
- Persistent desired online/offline state across worldserver restarts.
- Full AI activity even with no real player online or nearby.
- Autonomous quest, grind, loot, travel, mount, and New RPG strategies.
- Keeps normal character gear, inventory, spells, quests, guild, position, and XP progression.
- Protects selected alts from random-bot guild assignment and fixed-level/no-XP handling.
- Rejects random-bot and add-class accounts to avoid lifecycle conflicts.
- Handles an in-flight login when an administrator disables a character.
- GM and console commands for starting, stopping, listing, inspecting, and reloading selections.

## Existing related functionality

This project fills a gap in the current AzerothCore port; it does not claim the general idea is new.

- Current AzerothCore `mod-playerbots` can log alt characters in manually and can auto-login account alts when their
  owner logs in. Those alts remain master-oriented and are not a persistent selected autonomous fleet.
- The CMaNGOS Playerbots lineage has an `always online`/`ToggleAlwaysOnlineChars` concept. That implementation is useful
  prior art, but it is not an installable AzerothCore module and is not present in the current AzerothCore Playerbots
  configuration or source paths.

See [`docs/PRIOR_ART.md`](docs/PRIOR_ART.md) for the search scope and links.

## Requirements

1. [AzerothCore Playerbots core fork](https://github.com/mod-playerbots/azerothcore-wotlk), `Playerbot` branch.
2. [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots), `master` branch.
3. Both integration patches supplied with this repository.
4. `EnablePlayerSettings = 1` in the active `worldserver.conf`.
5. `AiPlayerbot.DisabledWithoutRealPlayer = 0` in the active `playerbots.conf`.

The final setting prevents the random-bot holder from repeatedly logging out all masterless bots when the realm is empty.

## Installation

Clone this repository beside `mod-playerbots`:

```bash
cd /path/to/azerothcore-wotlk/modules
git clone https://github.com/Gutwormy/mod-autonomous-altbots.git
```

Apply the integration patches from the AzerothCore root:

```bash
bash modules/mod-autonomous-altbots/tools/install-patches.sh
```

PowerShell users can run:

```powershell
.\modules\mod-autonomous-altbots\tools\Install-Patches.ps1
```

Then set the two required configuration values, re-run CMake, and rebuild the worldserver. The build installs
`mod_autonomous_altbots.conf.dist`; copy it to `mod_autonomous_altbots.conf` when your deployment does not create active
module configs automatically.

Docker example:

```bash
docker compose build ac-worldserver
docker compose up -d
```

The patch installers support `--check`/`-CheckOnly` and `--reverse`/`-Reverse`. Read
[`patches/README.md`](patches/README.md) before upgrading either upstream repository.

## Commands

Commands require AzerothCore's GM command permission.

| Command | Effect |
|---|---|
| `.autoalt start <character>` | Persistently enable the character and log it in. |
| `.autoalt stop <character>` | Log the character out and keep it offline. |
| `.autoalt enable <character>` | Alias for `start`. |
| `.autoalt disable <character>` | Alias for `stop`. |
| `.autoalt status <character>` | Show desired and current online state. |
| `.autoalt list` | List every enabled autonomous alt. |
| `.autoalt reload` | Reload selections from `character_settings` and parties from `tofu_autoalt_party`. |
| `.autoalt party create <party>` | Create a party (1 to 32 letters, digits, `-`, or `_`). Any player; the creator owns it. |
| `.autoalt party add <party> <character>` | Add an enrolled alt. Same permission as `start`; at most 5 members. |
| `.autoalt party remove <character>` | Take a character out of its party. The character's account, the party owner, or a GM. |
| `.autoalt party disband <party>` | Delete the party. The owner or a GM. |
| `.autoalt party list [party]` | List parties, or one party's members and their state. |
| `.autoalt party join <party>` | Join the party's group and lead it. You must be ungrouped and allowed to control every member playing. |

The original `.autonomousalt` namespace remains available as a backward-compatible alias. Use the complete `.autoalt`
name rather than a shorter abbreviation because AzerothCore also provides an `.autobroadcast` command.

A character logged in through the game client must log out before it can be enabled. An already running ordinary altbot
may be enabled; after its current master-owned session ends, the module logs it back in independently.

## Behaviour and resource use

An autonomous alt bypasses `BotActiveAlone` rotation and SmartScale. This is intentional: the selected character is fully
active even in an empty zone. Select a modest number and monitor world update time, CPU, and memory.

The current Playerbots random manager counts these sessions in its online holder. With a 1,500 random-bot target, enabling
five autonomous alts will normally settle near 1,495 random bots plus five autonomous alts rather than 1,505 total.

`AutonomousAltBots.Independent = 1` removes a real-player master and the `follow`/`stay` strategies. Set it to `0` if a
selected alt should resume ordinary master behaviour while grouped with a real player.

## Persistence and data safety

Only the desired enabled state is stored by this module, under source `mod_autonomous_altbots` in AzerothCore's existing
`character_settings` table. Character progression continues through the normal AzerothCore tables. The module does not
create, delete, reset, randomise, relevel, or regear characters.

Back up the character database before testing a new Playerbots/core revision.

## Compatibility and upgrades

The integration patches intentionally fail their `git apply --check` step when upstream code has moved. Do not force them
through a conflict: inspect and refresh the patches for the new revision. See [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md).

## Licence

GPL-2.0-or-later. See [`LICENSE`](LICENSE).

World of Warcraft is a trademark of Blizzard Entertainment. This project is not affiliated with or endorsed by Blizzard
Entertainment, AzerothCore, CMaNGOS, or the mod-playerbots maintainers.
