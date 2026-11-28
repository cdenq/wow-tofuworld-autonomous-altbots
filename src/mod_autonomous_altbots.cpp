/*
 * Copyright (C) 2026 mod-autonomous-altbots contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, either version 2 of the License, or any later version.
 *
 * Tofu Worldview fork (branch `tofu`). Differences from upstream 1.0.1:
 * - Needs the Tofu fork of mod-playerbots (TofuAutonomousAlt.h) instead of the two upstream integration patches;
 *   the core is not patched.
 * - Autonomous alts are online only while at least one real player is in the world.
 * - Players manage characters on their own account; GMs and the console manage any character.
 * - While grouped with a real player, the alt is left alone so it follows that player; it resumes on its own after.
 * - Level-up behaviour (paid class spells, talents, zone teleport) and ammo restocking live in mod-playerbots.
 * - Challenge modes (read from the character's mod-challenge-modes settings): Quest Only alts do not use the grind
 *   strategy; Hardcore alts cannot be turned on (unless AllowHardcore), and a Hardcore alt that dies, or a character
 *   flagged dead, is turned off and logged out for good.
 * - Hardcore survival (with AllowHardcore = 1): the alt is flagged Hardcore for mod-playerbots, gets the
 *   "tofu hardcore" strategy (eat back to the rest threshold, flee below the flee threshold), drops the "pvp" strategy,
 *   and in mod-playerbots picks no new fight below the rest threshold, no target more than MaxLevelsAbove above it,
 *   no level-up teleport, and no outdoor PvP. This holds while it is grouped with a real player too.
 * - Parties (Party.Enable = 1): fixed sets of up to 5 enrolled alts (tables tofu_autoalt_party and
 *   tofu_autoalt_party_member). The online members that are not with a real player are grouped under the
 *   lowest-level member, who plays as a solo alt does; the others follow it, take it as master, and walk with it to
 *   quest givers (mod-playerbots "tofu party"). Group quests and elites go by the members present. A real player who
 *   may control a member can invite it out of the party group; it rejoins when it is free. `.autoalt party join`
 *   puts a real player at the head of the whole party.
 * - Dungeons (Dungeon.Enable = 1, needs Party.Enable): a party queues through the Dungeon Finder for the lowest dungeon
 *   it has not cleared (table tofu_autoalt_party_dungeon), with one tank and one healer assigned. A free party smaller
 *   than five is filled by mod-dungeon-clear (Tofu fork, DungeonQueueFill.TofuParties); a challenge party goes only as
 *   a full five of its own members. mod-dungeon-clear runs the clear and reports the end; the party is teleported
 *   out, regroups, and goes back to questing. Members decline outside invites while queued or inside.
 * - Self Crafted (index SelfCraftedIndex below DisableLevel): the alt learns its class's professions
 *   (SelfCrafted.Professions.<Class>), crafts toward the best upgrade it can make, and steers toward the materials
 *   (mod-playerbots "tofu self crafted"). Gear it cannot wear is disenchanted or sold.
 * - Restock trips (Hub.Enable = 1): when bags fill up, gear wears out, ammo runs low, a tool is missing, a forge or
 *   anvil craft is ready, or a profession rank is waiting, a solo alt or a whole party teleports to the guild hub,
 *   each member does its own errands there (mod-playerbots "tofu hub"), and they teleport back together.
 * - Leaving the hub (Hub.Escape.Enable = 1): an alt that logs in at the guild hub, or stands idle there for IdleSeconds
 *   (its homebind may be the hub), teleports to its saved return point (table tofu_autoalt_return_point, written when
 *   a hub trip or a dungeon queue starts), else to an NPC of a quest in its log, else (not Hardcore) to a zone for its
 *   level. Party followers come along with their leader.
 * - Action bars (KeepActionBars = 1): a character played as a bot gets back, at the next real login, the bars a real
 *   player left on it (every spec), each spell at its current rank (table tofu_autoalt_action_snapshot).
 */

#include "AccountMgr.h"
#include "EquipAction.h"
#include "Timer.h"
#include "GameObject.h"
#include "GuildMgr.h"
#include "Guild.h"
#include "QuestDef.h"
#include "Creature.h"
#include "Trainer.h"
#include "StatsWeightCalculator.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "ItemUsageValue.h"
#include "ChatHelper.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GroupReference.h"
#include "LFGMgr.h"
#include "MapMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Tokenize.h"
#include "Player.h"
#include "PlayerSettings.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "TofuAutonomousAlt.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "DBCStores.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr char SETTING_SOURCE[] = "mod_autonomous_altbots";
constexpr char MODULE_VERSION[] = "1.0.1-tofu.16";
constexpr uint32 SETTING_ENABLED = 0;
constexpr uint32 STOP_WATCH_DURATION_MS = 30000;

struct AutonomousAltConfig
{
    bool enabled = true;
    uint32 startupDelayMs = 30000;
    uint32 checkIntervalMs = 10000;
    uint32 noPlayerLogoutDelayMs = 60000;
    bool quest = true;
    bool grind = true;
    bool newRpg = true;
    bool verboseLogging = false;
    bool keepActionBars = false;  // action bars of a character are restored when a real player takes it back

    // Challenge modes: indexes in the character's player settings under challengeSource (mod-challenge-modes), -1 = off.
    std::string challengeSource = "mod-challenge-modes";
    int32 hardcoreIndex = 0;
    int32 hardcoreDeadIndex = 8;
    int32 questOnlyIndex = 6;
    uint32 challengeDisableLevel = 80;  // challenges end at this level (ChallengeModes DisableLevel)
    bool allowHardcore = false;
    std::vector<int32> restrictingIndexes = { 0, 2, 3, 6, 9 };  // hard modes for the challenge grouping rule

    // Parties.
    bool partyEnable = false;
    float partyRegroupDistance = 300.0f;

    // Self Crafted.
    bool selfCraftedEnable = true;
    int32 selfCraftedIndex = 2;
    std::unordered_map<uint8, std::vector<uint32>> selfCraftedProfessions;  // class -> skill ids, in order

    // Dungeons.
    bool dungeonEnable = false;
    uint32 dungeonMinLevel = 15;
    bool dungeonRepeatPerLevel = true;
    uint32 dungeonRetryCooldownMs = 60 * MINUTE * IN_MILLISECONDS;
    uint32 dungeonClearedCooldownMs = 2 * MINUTE * IN_MILLISECONDS;
    uint32 dungeonQueueTimeoutMs = 10 * MINUTE * IN_MILLISECONDS;
    uint32 dungeonRunTimeoutMs = 90 * MINUTE * IN_MILLISECONDS;

    // Restock trips to the guild hub.
    bool hubEnable = false;
    uint32 hubMaxMs = 5 * MINUTE * IN_MILLISECONDS;
    uint32 hubMinIntervalMs = 20 * MINUTE * IN_MILLISECONDS;

    // Leaving the guild hub: an alt there with nothing to do teleports back to its quests.
    bool hubEscapeEnable = false;
    uint32 hubEscapeIdleMs = 60 * IN_MILLISECONDS;
    bool hubEscapeQuestTarget = true;

    // Multibox: a real player's own altbots in his group copy his quests, training, and vendor visits.
    bool multiboxEnable = false;
    bool multiboxQuests = true;
    bool multiboxTraining = true;
    bool multiboxVendor = true;
    uint32 multiboxAmmoTarget = 1000;
    uint32 multiboxReagentTarget = 20;
    bool multiboxGuildBank = true;
    std::vector<uint8> multiboxGuildBankTabs = { 0, 1 };  // guild bank tabs (0 = first) tried in this order
    uint32 multiboxConsumableKeep = 20;                   // usable consumables kept per item; 0 = no limit
};

// Party activity, kept in memory (world thread).
enum class PartyPhase : uint8
{
    Open,     // open-world questing (ProcessParties manages the group)
    Queued,   // in the Dungeon Finder
    Dungeon,  // inside; mod-dungeon-clear runs the clear
    Leaving,  // teleported out; the group is split back up next check
    Hub       // restock trip at the guild hub
};

struct ReturnPoint
{
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
    bool set = false;
};

struct PartyRun
{
    PartyPhase phase = PartyPhase::Open;
    uint32 since = 0;
    ObjectGuid leader;
    ObjectGuid groupGuid;
    uint32 dungeonId = 0;
    bool hardcore = false;
    bool cleared = false;
    uint32 memberCount = 0;
    uint32 nextDungeonAt = 0;    // getMSTime() before which no new queue (0 = now)
    uint8 lastRepeatLevel = 0;   // lowest member level of the last repeat run
    uint32 lastHubAt = 0;
    ReturnPoint back;
    std::string status = "questing";
};

struct SoloHub
{
    bool active = false;
    uint32 since = 0;
    uint32 lastHubAt = 0;
    ReturnPoint back;
};

constexpr uint32 PARTY_MAX_MEMBERS = 5;
constexpr size_t PARTY_NAME_MAX_LENGTH = 32;

struct Party
{
    uint32 id = 0;
    std::string name;
    uint32 ownerAccount = 0;
    std::vector<ObjectGuid::LowType> members;
    std::unordered_set<uint32> clearedDungeons;  // LFG dungeon ids (tofu_autoalt_party_dungeon)
};

AutonomousAltConfig g_Config;
std::unordered_set<ObjectGuid::LowType> g_DesiredBots;
std::unordered_map<ObjectGuid::LowType, uint32> g_StopWatchElapsed;
std::unordered_set<ObjectGuid::LowType> g_CaughtUpThisLogin;  // got the login-time spell catch-up
uint32 g_StartupElapsed = 0;
uint32 g_CheckElapsed = 0;
uint32 g_NoPlayerElapsed = 0;
bool g_StartupComplete = false;

// Parties, loaded at startup and changed only by commands and character deletion (world thread).
std::map<uint32, Party> g_Parties;
std::unordered_map<ObjectGuid::LowType, uint32> g_PartyOfMember;
// This check's roles: member -> its party leader (the leader maps to itself). Rebuilt every check.
std::unordered_map<ObjectGuid::LowType, ObjectGuid::LowType> g_PartyRole;

// Dungeon runs and hub trips (world thread).
std::unordered_map<uint32, PartyRun> g_PartyRuns;
std::unordered_map<ObjectGuid::LowType, SoloHub> g_SoloHub;
std::unordered_set<ObjectGuid::LowType> g_PartyBusy;  // members of a party that is not Open this check

// Leaving the guild hub (world thread). Return points mirror tofu_autoalt_return_point, so a trip cut short by a
// logout or a restart still knows where the alt was questing.
std::unordered_map<ObjectGuid::LowType, ReturnPoint> g_ReturnPoints;
std::unordered_map<ObjectGuid::LowType, uint32> g_HubIdleSince; // first check it was seen idle at the hub
std::unordered_set<ObjectGuid::LowType> g_RescueCheckedThisLogin; // had its first stranded check since login
std::unordered_set<ObjectGuid::LowType> g_HubStayLogged; // "stays at the hub" logged this login

// Hardcore alts that died since the last update (filled from map threads, handled on the world thread).
std::mutex g_PendingDeathsMutex;
std::unordered_set<ObjectGuid::LowType> g_PendingHardcoreDeaths;

bool PlayerSettingsEnabled()
{
    return sWorld->getBoolConfig(CONFIG_PLAYER_SETTINGS_ENABLED);
}

void LoadConfig()
{
    g_Config.enabled = sConfigMgr->GetOption<bool>("AutonomousAltBots.Enable", true);
    g_Config.startupDelayMs =
        sConfigMgr->GetOption<uint32>("AutonomousAltBots.StartupDelaySeconds", 30) * IN_MILLISECONDS;
    g_Config.checkIntervalMs = std::max<uint32>(
        IN_MILLISECONDS,
        sConfigMgr->GetOption<uint32>("AutonomousAltBots.CheckIntervalSeconds", 10) * IN_MILLISECONDS);
    g_Config.noPlayerLogoutDelayMs =
        sConfigMgr->GetOption<uint32>("AutonomousAltBots.NoPlayerLogoutDelaySeconds", 60) * IN_MILLISECONDS;
    g_Config.quest = sConfigMgr->GetOption<bool>("AutonomousAltBots.Strategy.Quest", true);
    g_Config.grind = sConfigMgr->GetOption<bool>("AutonomousAltBots.Strategy.Grind", true);
    g_Config.newRpg = sConfigMgr->GetOption<bool>("AutonomousAltBots.Strategy.NewRpg", true);
    g_Config.verboseLogging = sConfigMgr->GetOption<bool>("AutonomousAltBots.VerboseLogging", false);
    g_Config.keepActionBars = sConfigMgr->GetOption<bool>("AutonomousAltBots.KeepActionBars", false);

    g_Config.challengeSource =
        sConfigMgr->GetOption<std::string>("AutonomousAltBots.Challenge.SettingSource", "mod-challenge-modes");
    g_Config.hardcoreIndex = sConfigMgr->GetOption<int32>("AutonomousAltBots.Challenge.HardcoreIndex", 0);
    g_Config.hardcoreDeadIndex = sConfigMgr->GetOption<int32>("AutonomousAltBots.Challenge.HardcoreDeadIndex", 8);
    g_Config.questOnlyIndex = sConfigMgr->GetOption<int32>("AutonomousAltBots.Challenge.QuestOnlyIndex", 6);
    g_Config.challengeDisableLevel = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Challenge.DisableLevel", 80);
    g_Config.allowHardcore = sConfigMgr->GetOption<bool>("AutonomousAltBots.Challenge.AllowHardcore", false);
    g_Config.restrictingIndexes.clear();
    std::string const restricting =
        sConfigMgr->GetOption<std::string>("AutonomousAltBots.Challenge.RestrictingIndexes", "0,2,3,6,9");
    for (std::string_view token : Acore::Tokenize(restricting, ',', false))
        if (Optional<int32> index = Acore::StringTo<int32>(token))
            g_Config.restrictingIndexes.push_back(*index);

    g_Config.partyEnable = sConfigMgr->GetOption<bool>("AutonomousAltBots.Party.Enable", false);
    g_Config.partyRegroupDistance = sConfigMgr->GetOption<float>("AutonomousAltBots.Party.RegroupDistance", 300.0f);

    g_Config.selfCraftedEnable = sConfigMgr->GetOption<bool>("AutonomousAltBots.SelfCrafted.Enable", true);
    g_Config.selfCraftedIndex = sConfigMgr->GetOption<int32>("AutonomousAltBots.Challenge.SelfCraftedIndex", 2);
    g_Config.selfCraftedProfessions.clear();
    static std::vector<std::pair<char const*, uint32>> const professionNames = {
        {"alchemy", SKILL_ALCHEMY}, {"blacksmithing", SKILL_BLACKSMITHING}, {"enchanting", SKILL_ENCHANTING},
        {"engineering", SKILL_ENGINEERING}, {"herbalism", SKILL_HERBALISM}, {"inscription", SKILL_INSCRIPTION},
        {"jewelcrafting", SKILL_JEWELCRAFTING}, {"leatherworking", SKILL_LEATHERWORKING}, {"mining", SKILL_MINING},
        {"skinning", SKILL_SKINNING}, {"tailoring", SKILL_TAILORING}};
    static std::vector<std::tuple<uint8, char const*, char const*>> const classDefaults = {
        {CLASS_DEATH_KNIGHT, "DeathKnight", "Mining,Blacksmithing,Jewelcrafting,Engineering"},
        {CLASS_DRUID, "Druid", "Skinning,Leatherworking,Mining,Blacksmithing"},
        {CLASS_HUNTER, "Hunter", "Skinning,Leatherworking,Mining,Engineering"},
        {CLASS_MAGE, "Mage", "Tailoring,Enchanting,Mining,Jewelcrafting"},
        {CLASS_PALADIN, "Paladin", "Mining,Blacksmithing,Jewelcrafting,Engineering"},
        {CLASS_PRIEST, "Priest", "Tailoring,Enchanting,Mining,Jewelcrafting"},
        {CLASS_ROGUE, "Rogue", "Skinning,Leatherworking,Mining,Blacksmithing"},
        {CLASS_SHAMAN, "Shaman", "Skinning,Leatherworking,Mining,Blacksmithing"},
        {CLASS_WARLOCK, "Warlock", "Tailoring,Enchanting,Mining,Jewelcrafting"},
        {CLASS_WARRIOR, "Warrior", "Mining,Blacksmithing,Jewelcrafting,Engineering"}};
    for (auto const& [classId, className, defaultList] : classDefaults)
    {
        std::string const list = sConfigMgr->GetOption<std::string>(
            std::string("AutonomousAltBots.SelfCrafted.Professions.") + className, defaultList);
        std::vector<uint32> skills;
        for (std::string_view token : Acore::Tokenize(list, ',', false))
        {
            std::string name(token);
            name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }),
                       name.end());
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            for (auto const& [professionName, skill] : professionNames)
                if (name == professionName)
                    skills.push_back(skill);
        }
        g_Config.selfCraftedProfessions[classId] = skills;
    }

    g_Config.dungeonEnable = sConfigMgr->GetOption<bool>("AutonomousAltBots.Dungeon.Enable", false);
    g_Config.dungeonMinLevel = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Dungeon.MinLevel", 15);
    g_Config.dungeonRepeatPerLevel = sConfigMgr->GetOption<bool>("AutonomousAltBots.Dungeon.RepeatPerLevel", true);
    g_Config.dungeonRetryCooldownMs =
        sConfigMgr->GetOption<uint32>("AutonomousAltBots.Dungeon.RetryCooldownMinutes", 60) * MINUTE * IN_MILLISECONDS;
    g_Config.dungeonQueueTimeoutMs =
        sConfigMgr->GetOption<uint32>("AutonomousAltBots.Dungeon.QueueTimeoutMinutes", 10) * MINUTE * IN_MILLISECONDS;
    g_Config.dungeonRunTimeoutMs =
        sConfigMgr->GetOption<uint32>("AutonomousAltBots.Dungeon.RunTimeoutMinutes", 90) * MINUTE * IN_MILLISECONDS;

    g_Config.hubEnable = sConfigMgr->GetOption<bool>("AutonomousAltBots.Hub.Enable", false);
    g_Config.hubMaxMs = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Hub.MaxMinutes", 5) * MINUTE * IN_MILLISECONDS;
    g_Config.hubMinIntervalMs =
        sConfigMgr->GetOption<uint32>("AutonomousAltBots.Hub.MinIntervalMinutes", 20) * MINUTE * IN_MILLISECONDS;
    g_Config.hubEscapeEnable = sConfigMgr->GetOption<bool>("AutonomousAltBots.Hub.Escape.Enable", false);
    g_Config.hubEscapeIdleMs =
        sConfigMgr->GetOption<uint32>("AutonomousAltBots.Hub.Escape.IdleSeconds", 60) * IN_MILLISECONDS;
    g_Config.hubEscapeQuestTarget = sConfigMgr->GetOption<bool>("AutonomousAltBots.Hub.Escape.QuestTarget", true);

    g_Config.multiboxEnable = sConfigMgr->GetOption<bool>("AutonomousAltBots.Multibox.Enable", false);
    g_Config.multiboxQuests = sConfigMgr->GetOption<bool>("AutonomousAltBots.Multibox.Quests", true);
    g_Config.multiboxTraining = sConfigMgr->GetOption<bool>("AutonomousAltBots.Multibox.Training", true);
    g_Config.multiboxVendor = sConfigMgr->GetOption<bool>("AutonomousAltBots.Multibox.Vendor", true);
    g_Config.multiboxAmmoTarget = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Multibox.AmmoTarget", 1000);
    g_Config.multiboxReagentTarget = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Multibox.ReagentTarget", 20);
    g_Config.multiboxGuildBank = sConfigMgr->GetOption<bool>("AutonomousAltBots.Multibox.GuildBank", true);
    g_Config.multiboxConsumableKeep = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Multibox.ConsumableKeep", 20);
    g_Config.multiboxGuildBankTabs.clear();
    std::string const bankTabs = sConfigMgr->GetOption<std::string>("AutonomousAltBots.Multibox.GuildBankTabs", "1,2");
    for (std::string_view token : Acore::Tokenize(bankTabs, ',', false))
        if (Optional<uint32> tab = Acore::StringTo<uint32>(token); tab && *tab >= 1 && *tab <= GUILD_BANK_MAX_TABS)
            g_Config.multiboxGuildBankTabs.push_back(static_cast<uint8>(*tab - 1));

    TofuAutonomousAlt::HubSettings hub;
    hub.enable = g_Config.hubEnable;
    hub.mapId = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Hub.Map", 1);
    hub.x = sConfigMgr->GetOption<float>("AutonomousAltBots.Hub.X", 16228.407f);
    hub.y = sConfigMgr->GetOption<float>("AutonomousAltBots.Hub.Y", 16306.691f);
    hub.z = sConfigMgr->GetOption<float>("AutonomousAltBots.Hub.Z", 29.263594f);
    hub.o = sConfigMgr->GetOption<float>("AutonomousAltBots.Hub.O", 6.263572f);
    hub.radius = sConfigMgr->GetOption<float>("AutonomousAltBots.Hub.Radius", 120.0f);
    hub.sellRepairEntry = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Hub.SellRepairEntry", 900003);
    hub.ammoVendorEntry = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Hub.AmmoVendorEntry", 0);
    hub.errandStuckMs = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Hub.ErrandStuckSeconds", 20) * IN_MILLISECONDS;
    TofuAutonomousAlt::SetHubSettings(hub);

    TofuAutonomousAlt::Settings settings;
    settings.teleportForLevel = sConfigMgr->GetOption<bool>("AutonomousAltBots.LevelUp.TeleportForLevel", true);
    settings.learnTrainerSpells = sConfigMgr->GetOption<bool>("AutonomousAltBots.LevelUp.LearnTrainerSpells", true);
    settings.autoTalents = sConfigMgr->GetOption<bool>("AutonomousAltBots.LevelUp.AutoTalents", true);
    settings.restock = sConfigMgr->GetOption<bool>("AutonomousAltBots.Restock.Enable", true);
    settings.ammoThreshold = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Restock.AmmoThreshold", 200);
    settings.vendorSearchDistance =
        sConfigMgr->GetOption<float>("AutonomousAltBots.Restock.VendorSearchDistance", 3000.0f);
    settings.hardcoreRestHealthPct = sConfigMgr->GetOption<float>("AutonomousAltBots.Hardcore.RestHealthPct", 80.0f);
    settings.hardcoreFleeHealthPct = sConfigMgr->GetOption<float>("AutonomousAltBots.Hardcore.FleeHealthPct", 30.0f);
    settings.hardcoreMaxLevelsAbove = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Hardcore.MaxLevelsAbove", 2);
    settings.partyEliteMinMembers = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Party.EliteMinMembers", 3);
    settings.partyPresentRange = sConfigMgr->GetOption<float>("AutonomousAltBots.Party.PresentRange", 100.0f);
    settings.restockBagPct = sConfigMgr->GetOption<float>("AutonomousAltBots.Hub.BagPct", 50.0f);
    settings.restockDurabilityPct = sConfigMgr->GetOption<float>("AutonomousAltBots.Hub.DurabilityPct", 25.0f);
    settings.multiboxEnable = sConfigMgr->GetOption<bool>("AutonomousAltBots.Multibox.Enable", false);
    settings.multiboxTraining = sConfigMgr->GetOption<bool>("AutonomousAltBots.Multibox.Training", true);
    TofuAutonomousAlt::SetSettings(settings);

    // Items no bot ever sells, destroys, or deposits (default: the Pocket Portal).
    std::vector<uint32> keptItems;
    std::string const keepList = sConfigMgr->GetOption<std::string>("AutonomousAltBots.KeepItems", "900000");
    for (std::string_view token : Acore::Tokenize(keepList, ',', false))
        if (Optional<uint32> itemId = Acore::StringTo<uint32>(token); itemId && *itemId)
            keptItems.push_back(*itemId);
    TofuAutonomousAlt::SetKeptItems(keptItems);
}

void LoadDesiredBots()
{
    g_DesiredBots.clear();

    if (!PlayerSettingsEnabled())
    {
        LOG_ERROR(
            "module",
            "mod-autonomous-altbots: EnablePlayerSettings must be set to 1 in worldserver.conf; no alts will be loaded");
        TofuAutonomousAlt::ReplaceAll({});
        return;
    }

    // A plain query (the upstream version added a prepared statement to the core for this).
    if (QueryResult result =
            CharacterDatabase.Query("SELECT guid, data FROM character_settings WHERE source = '{}'", SETTING_SOURCE))
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType guid = fields[0].Get<ObjectGuid::LowType>();
            PlayerSettingVector settings = PlayerSettingsStore::ParseSettingsData(fields[1].Get<std::string>());
            if (settings.size() > SETTING_ENABLED && settings[SETTING_ENABLED].IsEnabled())
                g_DesiredBots.insert(guid);
        } while (result->NextRow());
    }

    TofuAutonomousAlt::ReplaceAll(std::vector<ObjectGuid::LowType>(g_DesiredBots.begin(), g_DesiredBots.end()));
    LOG_INFO("module", "mod-autonomous-altbots: loaded {} persistent character selection(s)", g_DesiredBots.size());
}

// Parties: memory mirrors the two tables; the playerbots registry gets membership and roster sizes.
void PushPartiesToRegistry()
{
    TofuAutonomousAlt::ClearParties();
    for (auto const& [id, party] : g_Parties)
    {
        TofuAutonomousAlt::SetPartyRosterSize(id, static_cast<uint32>(party.members.size()));
        for (ObjectGuid::LowType guid : party.members)
            TofuAutonomousAlt::SetPartyMember(guid, id);
    }
}

void LoadParties()
{
    g_Parties.clear();
    g_PartyOfMember.clear();

    if (QueryResult result = CharacterDatabase.Query("SELECT id, name, owner_account FROM tofu_autoalt_party"))
    {
        do
        {
            Field* fields = result->Fetch();
            Party party;
            party.id = fields[0].Get<uint32>();
            party.name = fields[1].Get<std::string>();
            party.ownerAccount = fields[2].Get<uint32>();
            g_Parties[party.id] = party;
        } while (result->NextRow());
    }

    if (QueryResult result =
            CharacterDatabase.Query("SELECT guid, party_id FROM tofu_autoalt_party_member ORDER BY party_id, guid"))
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType const guid = fields[0].Get<ObjectGuid::LowType>();
            uint32 const partyId = fields[1].Get<uint32>();
            auto itr = g_Parties.find(partyId);
            if (itr == g_Parties.end() || itr->second.members.size() >= PARTY_MAX_MEMBERS)
                continue;
            itr->second.members.push_back(guid);
            g_PartyOfMember[guid] = partyId;
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT party_id, dungeon_id FROM tofu_autoalt_party_dungeon"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto itr = g_Parties.find(fields[0].Get<uint32>());
            if (itr != g_Parties.end())
                itr->second.clearedDungeons.insert(fields[1].Get<uint32>());
        } while (result->NextRow());
    }

    PushPartiesToRegistry();
    LOG_INFO("module", "mod-autonomous-altbots: loaded {} part(ies) with {} member(s)", g_Parties.size(),
             g_PartyOfMember.size());
}

Party* FindPartyByName(std::string_view name)
{
    for (auto& [id, party] : g_Parties)
        if (StringEqualI(party.name, name))
            return &party;
    return nullptr;
}

Party* FindPartyOf(ObjectGuid::LowType guid)
{
    auto itr = g_PartyOfMember.find(guid);
    if (itr == g_PartyOfMember.end())
        return nullptr;
    auto party = g_Parties.find(itr->second);
    return party != g_Parties.end() ? &party->second : nullptr;
}

void RemovePartyMember(ObjectGuid::LowType guid)
{
    if (Party* party = FindPartyOf(guid))
        std::erase(party->members, guid);
    g_PartyOfMember.erase(guid);
    PushPartiesToRegistry();
}

Player* FindConnectedCharacter(ObjectGuid guid)
{
    return ObjectAccessor::FindConnectedPlayer(guid);
}

bool IsPlayerbot(Player* player)
{
    return player && GET_PLAYERBOT_AI(player) != nullptr;
}

// Action bars (KeepActionBars). A bot has no game client, so when it learns a new rank the core's "superseded spell"
// packet goes nowhere and its saved buttons keep the old rank, which a returning player's client no longer knows and
// shows as empty; bots also change the shaman totem bar. So the bars are saved (every spec, from character_action)
// when a non-random character logs in as a bot and no saved copy exists yet, and put back when a real player logs in
// on it, each spell moved to the rank the character now has. The saved copy is deleted at that player's next save.
std::mutex g_RestoredBarsMutex;
std::unordered_set<ObjectGuid::LowType> g_RestoredBars;  // restored this session; the copy goes at the next save

bool IsKnownInSpec(Player* player, uint32 spellId, uint8 spec, bool activeOnly)
{
    PlayerSpellMap const& spells = player->GetSpellMap();
    auto itr = spells.find(spellId);
    if (itr == spells.end() || !itr->second || itr->second->State == PLAYERSPELL_REMOVED)
        return false;

    if (activeOnly && !itr->second->Active)
        return false;

    return itr->second->IsInSpec(spec);
}

// The button's action for this spec now: a spell becomes the character's current rank of it; 0 drops the button
// (a spell the character no longer knows in that spec, which the core would drop at the next load anyway).
uint32 MapActionToCurrentRank(Player* player, uint32 action, uint8 type, uint8 spec)
{
    if (type != ACTION_BUTTON_SPELL)
        return action;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(action);
    if (!spellInfo)
        return 0;

    if (IsKnownInSpec(player, action, spec, true))
        return action;

    if (spellInfo->IsRanked() && !spellInfo->IsStackableWithRanks())
        for (uint32 rank = sSpellMgr->GetLastSpellInChain(action); rank; rank = sSpellMgr->GetPrevSpellInChain(rank))
            if (IsKnownInSpec(player, rank, spec, true))
                return rank;

    return IsKnownInSpec(player, action, spec, false) ? action : 0;
}

// World thread, at a bot's login. Only the first bot login after a real player played the character saves the bars.
void SaveActionBarsForBot(Player* bot)
{
    if (!g_Config.keepActionBars || !bot || !bot->GetSession())
        return;

    ObjectGuid::LowType const lowGuid = bot->GetGUID().GetCounter();
    if (sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()) ||
        sRandomPlayerbotMgr.IsAddclassBot(lowGuid))
        return;

    {
        std::lock_guard lock(g_RestoredBarsMutex);
        g_RestoredBars.erase(lowGuid);
    }

    CharacterDatabase.Execute(
        "INSERT IGNORE INTO tofu_autoalt_action_snapshot (guid, spec, button, action, type) "
        "SELECT guid, spec, button, action, type FROM character_action WHERE guid = {} "
        "AND NOT EXISTS (SELECT 1 FROM tofu_autoalt_action_snapshot s WHERE s.guid = {})",
        lowGuid, lowGuid);
}

// World thread, at a real player's login (after the core sent the stored buttons; they are sent again if changed).
void RestoreActionBarsForPlayer(Player* player)
{
    if (!g_Config.keepActionBars || !player)
        return;

    ObjectGuid::LowType const lowGuid = player->GetGUID().GetCounter();
    QueryResult result = CharacterDatabase.Query(
        "SELECT spec, button, action, type FROM tofu_autoalt_action_snapshot WHERE guid = {}", lowGuid);
    if (!result)
        return;

    // spec -> button -> (action, type)
    std::map<uint8, std::map<uint8, std::pair<uint32, uint8>>> saved;
    do
    {
        Field* fields = result->Fetch();
        uint8 const spec = fields[0].Get<uint8>();
        uint8 const button = fields[1].Get<uint8>();
        if (spec < MAX_TALENT_SPECS && button < MAX_ACTION_BUTTONS)
            saved[spec][button] = { fields[2].Get<uint32>(), fields[3].Get<uint8>() };
    } while (result->NextRow());

    // Active spec: the buttons in memory, saved by the core at the next save.
    uint8 const activeSpec = player->GetActiveSpec();
    std::map<uint8, std::pair<uint32, uint8>> const& wanted = saved[activeSpec];
    uint32 changed = 0;
    for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
    {
        ActionButton const* current = player->GetActionButton(button);
        uint32 action = 0;
        uint8 type = 0;
        auto itr = wanted.find(button);
        if (itr != wanted.end())
        {
            type = itr->second.second;
            action = MapActionToCurrentRank(player, itr->second.first, type, activeSpec);
        }

        if (action)
        {
            if (current && current->GetAction() == action && uint8(current->GetType()) == type)
                continue;

            if (player->addActionButton(button, action, type))
            {
                ++changed;
                continue;
            }
        }

        if (current)
        {
            player->removeActionButton(button);
            ++changed;
        }
    }

    // Other spec: its rows in the database, which the core loads at the next spec switch.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    for (uint8 spec = 0; spec < player->GetSpecsCount() && spec < MAX_TALENT_SPECS; ++spec)
    {
        if (spec == activeSpec)
            continue;

        trans->Append("DELETE FROM character_action WHERE guid = {} AND spec = {}", lowGuid, spec);
        for (auto const& [button, entry] : saved[spec])
            if (uint32 action = MapActionToCurrentRank(player, entry.first, entry.second, spec))
                trans->Append("INSERT INTO character_action (guid, spec, button, action, type) VALUES ({}, {}, {}, {}, {})",
                              lowGuid, spec, button, action, entry.second);
    }
    CharacterDatabase.CommitTransaction(trans);

    if (changed)
        player->SendActionButtons(1);

    {
        std::lock_guard lock(g_RestoredBarsMutex);
        g_RestoredBars.insert(lowGuid);
    }

    LOG_INFO("module", "mod-autonomous-altbots: restored the action bars of {} ({} buttons changed in the active spec)",
             player->GetName(), changed);
}

// Any thread (saves run on map threads too). The bars are now saved by the core, so the copy is no longer needed.
void DropActionBarCopyAfterSave(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;

    ObjectGuid::LowType const lowGuid = player->GetGUID().GetCounter();
    {
        std::lock_guard lock(g_RestoredBarsMutex);
        if (!g_RestoredBars.erase(lowGuid))
            return;
    }

    CharacterDatabase.Execute("DELETE FROM tofu_autoalt_action_snapshot WHERE guid = {}", lowGuid);
}

// A real player is a game client with a character in the world (bot sessions never count; neither does a client
// sitting at character select).
bool AnyRealPlayerInWorld()
{
    for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
    {
        if (!session || session->IsHeadless())
            continue;

        Player* player = session->GetPlayer();
        if (player && player->IsInWorld() && IsRealPlayer(player))
            return true;
    }

    return false;
}

// True when the bot shares a group with a real player who is online.
bool GroupedWithRealPlayer(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && IsRealPlayer(member))
            return true;
    }

    return false;
}

// True when every member slot of the group (online or not) belongs to this party.
bool GroupHoldsOnlyParty(Group* group, uint32 partyId)
{
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        auto itr = g_PartyOfMember.find(slot.guid.GetCounter());
        if (!slot.guid.IsPlayer() || itr == g_PartyOfMember.end() || itr->second != partyId)
            return false;
    }
    return true;
}

// Groups each party's eligible online members under its lowest-level member and records the roles for this check.
// Eligible: enrolled, online as a bot, not with a real player, no pending invite. Fewer than 2 eligible: no party
// group this check (a lone member plays solo).
void ProcessParties(bool active)
{
    g_PartyRole.clear();
    if (!g_Config.partyEnable || !active)
        return;

    for (auto const& [partyId, party] : g_Parties)
    {
        // A party in the Dungeon Finder, a dungeon, or at the hub keeps the group it has.
        auto const run = g_PartyRuns.find(partyId);
        if (run != g_PartyRuns.end() && run->second.phase != PartyPhase::Open)
            continue;

        std::vector<Player*> eligible;
        for (ObjectGuid::LowType lowGuid : party.members)
        {
            if (!g_DesiredBots.contains(lowGuid))
                continue;

            Player* member = FindConnectedCharacter(ObjectGuid::Create<HighGuid::Player>(lowGuid));
            if (!member || !member->IsInWorld() || !IsPlayerbot(member) || member->IsBeingTeleported())
                continue;

            if (member->GetGroupInvite() || GroupedWithRealPlayer(member))
                continue;

            eligible.push_back(member);
        }

        if (eligible.size() < 2)
            continue;

        Player* leader = *std::min_element(eligible.begin(), eligible.end(),
                                           [](Player* a, Player* b)
                                           {
                                               if (a->GetLevel() != b->GetLevel())
                                                   return a->GetLevel() < b->GetLevel();
                                               return a->GetGUID().GetCounter() < b->GetGUID().GetCounter();
                                           });

        // The party group: the leader's group if it holds only this party, else another member's such group, else
        // a new one.
        Group* group = leader->GetGroup();
        if (group && !GroupHoldsOnlyParty(group, partyId))
        {
            leader->RemoveFromGroup();
            group = nullptr;
        }

        if (!group)
        {
            for (Player* member : eligible)
            {
                Group* other = member->GetGroup();
                if (member != leader && other && !other->IsFull() && GroupHoldsOnlyParty(other, partyId))
                {
                    if (other->AddMember(leader))
                        group = other;
                    break;
                }
            }
        }

        if (!group)
        {
            group = new Group();
            if (!group->Create(leader))
            {
                delete group;
                continue;
            }
            sGroupMgr->AddGroup(group);
        }

        if (group->GetLeaderGUID() != leader->GetGUID())
            group->ChangeLeader(leader->GetGUID());

        for (Player* member : eligible)
        {
            if (member == leader || member->GetGroup() == group)
                continue;

            if (member->GetGroup())
                member->RemoveFromGroup();

            if (group->IsFull() || !group->AddMember(member))
                continue;
        }

        for (Player* member : eligible)
            if (member->GetGroup() == group)
                g_PartyRole[member->GetGUID().GetCounter()] = leader->GetGUID().GetCounter();
    }
}

// Moves a follower to its leader when it is on another map or farther than the regroup distance (open world only).
void RegroupFollower(Player* bot, Player* leader)
{
    if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported() || leader->IsInFlight())
        return;

    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->IsInFlight())
        return;

    Map* map = leader->GetMap();
    if (!map || map->Instanceable())
        return;

    if (bot->GetMapId() == leader->GetMapId() && bot->IsWithinDistInMap(leader, g_Config.partyRegroupDistance))
        return;

    bot->TeleportTo(leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                    leader->GetOrientation());
}

void LogoutAutonomousBot(ObjectGuid guid)
{
    if (sRandomPlayerbotMgr.GetPlayerBot(guid))
        sRandomPlayerbotMgr.LogoutPlayerBot(guid);
}

// Challenge state of a character, from its mod-challenge-modes player settings. A challenge counts only below the
// level where mod-challenge-modes turns it off; the dead flag counts at any level.
struct ChallengeState
{
    bool hardcore = false;
    bool hardcoreDead = false;
    bool questOnly = false;
    bool restricting = false;  // any hard mode of the challenge grouping rule (Progression does not count)
    bool selfCrafted = false;
};

ChallengeState MakeChallengeState(PlayerSettingVector const& settings, uint8 level)
{
    auto isOn = [&settings](int32 index)
    { return index >= 0 && static_cast<size_t>(index) < settings.size() && settings[index].value != 0; };

    ChallengeState state;
    bool const active = level < g_Config.challengeDisableLevel;
    state.hardcore = active && isOn(g_Config.hardcoreIndex);
    state.hardcoreDead = isOn(g_Config.hardcoreDeadIndex);
    state.questOnly = active && isOn(g_Config.questOnlyIndex);
    state.selfCrafted = active && isOn(g_Config.selfCraftedIndex);
    for (int32 index : g_Config.restrictingIndexes)
        state.restricting = state.restricting || (active && isOn(index));
    return state;
}

ChallengeState GetChallengeState(Player* player)
{
    if (!player || g_Config.challengeSource.empty())
        return {};

    int32 highest = std::max(
        {g_Config.hardcoreIndex, g_Config.hardcoreDeadIndex, g_Config.questOnlyIndex, g_Config.selfCraftedIndex});
    for (int32 index : g_Config.restrictingIndexes)
        highest = std::max(highest, index);
    PlayerSettingVector settings;
    for (int32 index = 0; index <= highest; ++index)
        settings.push_back(player->GetPlayerSetting(g_Config.challengeSource, index));

    return MakeChallengeState(settings, player->GetLevel());
}

// For a character that is not online: its saved settings row.
ChallengeState GetOfflineChallengeState(ObjectGuid guid)
{
    if (g_Config.challengeSource.empty())
        return {};

    PlayerSettingVector settings;
    if (QueryResult result = CharacterDatabase.Query("SELECT data FROM character_settings WHERE guid = {} AND source = '{}'",
                                                     guid.GetCounter(), g_Config.challengeSource))
        settings = PlayerSettingsStore::ParseSettingsData(result->Fetch()[0].Get<std::string>());

    return MakeChallengeState(settings, sCharacterCache->GetCharacterLevelByGuid(guid));
}

// Why this character may not play on its own, or nullptr when it may.
char const* ChallengeBlockReason(ChallengeState const& state)
{
    if (state.hardcoreDead)
        return "has died on the Hardcore challenge";
    if (state.hardcore && !g_Config.allowHardcore)
        return "is on the Hardcore challenge";
    return nullptr;
}

// Hardcore survival, applied whether or not a real player leads the alt: the playerbots-side flag (rest gate, level
// cap, no teleport, no outdoor PvP) and the "tofu hardcore" strategy in both engines; no "pvp" (attacking enemy
// players). When Hardcore ends (level 80), the strategy is removed again; "pvp" comes back with the next strategy reset.
void ApplyHardcoreSurvival(Player* bot, PlayerbotAI* botAI, bool hardcore)
{
    TofuAutonomousAlt::SetHardcore(bot->GetGUID().GetCounter(), hardcore);

    for (BotState state : { BOT_STATE_COMBAT, BOT_STATE_NON_COMBAT })
    {
        bool const has = botAI->HasStrategy("tofu hardcore", state);
        if (hardcore && !has)
            botAI->ChangeStrategy("+tofu hardcore", state);
        else if (!hardcore && has)
            botAI->ChangeStrategy("-tofu hardcore", state);
    }

    if (hardcore && botAI->HasStrategy("pvp", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-pvp", BOT_STATE_NON_COMBAT);
}

// Self Crafted: the profession list goes to mod-playerbots and the "tofu self crafted" strategy crafts toward the
// alt's goal. Applied while a real player leads it too.
void ApplySelfCrafted(Player* bot, PlayerbotAI* botAI, bool selfCrafted)
{
    selfCrafted = selfCrafted && g_Config.selfCraftedEnable;
    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();

    if (selfCrafted)
    {
        auto const itr = g_Config.selfCraftedProfessions.find(bot->getClass());
        std::vector<uint32> skills = itr != g_Config.selfCraftedProfessions.end() ? itr->second : std::vector<uint32>();
        if (skills.empty())
            skills.push_back(SKILL_MINING);  // an empty list would read as "off"
        bool const first = !TofuAutonomousAlt::IsSelfCrafted(bot);
        TofuAutonomousAlt::SetSelfCrafted(guid, skills);
        if (first)
            TofuAutonomousAlt::LearnProfessions(bot);
    }
    else
        TofuAutonomousAlt::SetSelfCrafted(guid, {});

    bool const has = botAI->HasStrategy("tofu self crafted", BOT_STATE_NON_COMBAT);
    if (selfCrafted && !has)
        botAI->ChangeStrategy("+tofu self crafted", BOT_STATE_NON_COMBAT);
    else if (!selfCrafted && has)
        botAI->ChangeStrategy("-tofu self crafted", BOT_STATE_NON_COMBAT);
}

// At the hub: no master, no questing, grinding, wandering, or following; only the "tofu hub" errands.
void ApplyHubStrategies(PlayerbotAI* botAI)
{
    if (botAI->GetMaster())
        botAI->SetMaster(nullptr);

    for (char const* strategy : { "quest", "grind", "new rpg", "follow", "tofu party", "stay" })
        if (botAI->HasStrategy(strategy, BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy(std::string("-") + strategy, BOT_STATE_NON_COMBAT);

    if (!botAI->HasStrategy("tofu hub", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+tofu hub", BOT_STATE_NON_COMBAT);

    botAI->AllowActivity(ALL_ACTIVITY, true);
}

// In the Dungeon Finder or a dungeon: mod-dungeon-clear leads; no plan of its own and no master.
void ApplyDungeonStrategies(PlayerbotAI* botAI, bool inside)
{
    if (inside && botAI->GetMaster())
        botAI->SetMaster(nullptr);

    for (char const* strategy : { "grind", "new rpg", "tofu party", "stay", "tofu hub" })
        if (botAI->HasStrategy(strategy, BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy(std::string("-") + strategy, BOT_STATE_NON_COMBAT);

    if (inside)
        for (char const* strategy : { "quest", "follow" })
            if (botAI->HasStrategy(strategy, BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy(std::string("-") + strategy, BOT_STATE_NON_COMBAT);

    botAI->AllowActivity(ALL_ACTIVITY, true);
}

// A party follower: the party leader is its master; it follows, fights with the party, accepts and turns in quests
// at the givers the leader visits ("tofu party"), and runs no quest, grind, or new rpg plan of its own.
void ApplyFollowerStrategies(Player* bot, PlayerbotAI* botAI, ObjectGuid::LowType leaderGuid)
{
    Player* leader = FindConnectedCharacter(ObjectGuid::Create<HighGuid::Player>(leaderGuid));
    if (leader && botAI->GetMaster() != leader)
        botAI->SetMaster(leader);

    if (g_Config.quest && !botAI->HasStrategy("quest", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+quest", BOT_STATE_NON_COMBAT);

    for (char const* strategy : { "grind", "new rpg", "stay" })
        if (botAI->HasStrategy(strategy, BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy(std::string("-") + strategy, BOT_STATE_NON_COMBAT);

    for (char const* strategy : { "follow", "tofu party" })
        if (!botAI->HasStrategy(strategy, BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy(std::string("+") + strategy, BOT_STATE_NON_COMBAT);

    botAI->AllowActivity(ALL_ACTIVITY, true);
    RegroupFollower(bot, leader);

    if (g_CaughtUpThisLogin.insert(bot->GetGUID().GetCounter()).second &&
        TofuAutonomousAlt::GetSettings().learnTrainerSpells)
        TofuAutonomousAlt::LearnClassTrainerSpells(bot);
}

void ApplyAutonomousStrategies(Player* bot, ChallengeState const& challenge)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    ApplyHardcoreSurvival(bot, botAI, challenge.hardcore);
    ApplySelfCrafted(bot, botAI, challenge.selfCrafted);
    bool const questOnly = challenge.questOnly;

    // Grouped with a real player: that player is in charge (follow, commands, trade). Hands off.
    if (GroupedWithRealPlayer(bot))
        return;

    ObjectGuid::LowType const self = bot->GetGUID().GetCounter();

    if (TofuAutonomousAlt::IsHubVisitActive(self))
    {
        ApplyHubStrategies(botAI);
        return;
    }

    if (botAI->HasStrategy("tofu hub", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-tofu hub", BOT_STATE_NON_COMBAT);

    if (g_PartyBusy.contains(self))
    {
        ApplyDungeonStrategies(botAI, bot->GetMap() && bot->GetMap()->IsDungeon());
        return;
    }

    auto const role = g_PartyRole.find(self);
    bool const inParty = role != g_PartyRole.end();

    if (inParty && role->second != self)
    {
        ApplyFollowerStrategies(bot, botAI, role->second);
        return;
    }

    // Solo: left over in a group with no real player online (for example, the leader logged out): leave it. A party
    // leader keeps its group.
    if (!inParty && bot->GetGroup())
        bot->RemoveFromGroup();

    if (botAI->GetMaster())
        botAI->SetMaster(nullptr);

    if (botAI->HasStrategy("tofu party", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-tofu party", BOT_STATE_NON_COMBAT);

    if (g_Config.quest && !botAI->HasStrategy("quest", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+quest", BOT_STATE_NON_COMBAT);

    // Quest Only earns no kill XP, so a Quest Only alt spends its time on quests instead of grinding.
    bool const grind = g_Config.grind && !questOnly;
    if (grind && !botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+grind", BOT_STATE_NON_COMBAT);
    else if (!grind && botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);

    if (g_Config.newRpg && !botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+new rpg", BOT_STATE_NON_COMBAT);

    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-stay", BOT_STATE_NON_COMBAT);

    botAI->AllowActivity(ALL_ACTIVITY, true);

    // Once per login: learn any class spells it can already afford (level-ups handle the rest).
    if (g_CaughtUpThisLogin.insert(bot->GetGUID().GetCounter()).second &&
        TofuAutonomousAlt::GetSettings().learnTrainerSpells)
        TofuAutonomousAlt::LearnClassTrainerSpells(bot);
}

void SetDesired(ObjectGuid guid, bool enabled)
{
    PlayerSettingsStore::UpdateSetting(guid.GetCounter(), SETTING_SOURCE, SETTING_ENABLED, enabled ? 1 : 0);

    if (Player* player = FindConnectedCharacter(guid))
        player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_ENABLED, enabled ? 1 : 0);

    TofuAutonomousAlt::SetEnabled(guid.GetCounter(), enabled);

    if (enabled)
    {
        g_DesiredBots.insert(guid.GetCounter());
        g_StopWatchElapsed.erase(guid.GetCounter());
    }
    else
    {
        g_DesiredBots.erase(guid.GetCounter());
        g_CaughtUpThisLogin.erase(guid.GetCounter());
        g_StopWatchElapsed[guid.GetCounter()] = 0;
        LogoutAutonomousBot(guid);
    }
}

// Turns a character off for good because of its challenge state (Hardcore, or died on Hardcore).
void TurnOffForChallenge(ObjectGuid guid, char const* reason)
{
    std::string name;
    sCharacterCache->GetCharacterNameByGuid(guid, name);
    LOG_INFO("module", "mod-autonomous-altbots: {} {}; turned off and logged out", name, reason);
    SetDesired(guid, false);
}

bool IsConsoleOrGM(ChatHandler* handler)
{
    WorldSession* session = handler->GetSession();
    return !session || session->GetSecurity() >= SEC_GAMEMASTER;
}

bool ResolveCharacter(ChatHandler* handler, std::string name, ObjectGuid& guid)
{
    if (!normalizePlayerName(name))
    {
        handler->PSendSysMessage("Invalid character name '{}'.", name);
        return false;
    }

    guid = sCharacterCache->GetCharacterGuidByName(name);
    if (!guid)
    {
        handler->PSendSysMessage("Character '{}' was not found.", name);
        return false;
    }

    // Players may only manage characters on their own account.
    if (!IsConsoleOrGM(handler) &&
        sCharacterCache->GetCharacterAccountIdByGuid(guid) != handler->GetSession()->GetAccountId())
    {
        handler->PSendSysMessage("{} is not on your account.", name);
        return false;
    }

    return true;
}

void SendStatus(ChatHandler* handler, ObjectGuid guid)
{
    std::string name;
    sCharacterCache->GetCharacterNameByGuid(guid, name);
    bool desired = g_DesiredBots.contains(guid.GetCounter());
    Player* connected = FindConnectedCharacter(guid);
    char const* state = !connected ? "offline" : (IsPlayerbot(connected) ? "online as bot" : "online as player");

    handler->PSendSysMessage(
        "Autonomous alt {}: {}, {}, level {}.",
        name,
        desired ? "on" : "off",
        state,
        connected ? connected->GetLevel() : sCharacterCache->GetCharacterLevelByGuid(guid));

    if (connected && IsPlayerbot(connected))
    {
        if (TofuAutonomousAlt::IsHubVisitActive(guid.GetCounter()))
            handler->SendSysMessage("  Restocking at the guild hub.");
        std::string const crafted = TofuAutonomousAlt::DescribeSelfCrafted(connected);
        if (!crafted.empty())
            handler->PSendSysMessage("  {}", crafted);
    }
}


// ---- Dungeon runs and hub trips ----

bool ReadyToTravel(Player* bot)
{
    return bot && bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat() && !bot->IsInFlight() &&
           !bot->IsBeingTeleported() && bot->GetMap() && !bot->GetMap()->Instanceable();
}

ReturnPoint PointOf(Player* bot)
{
    ReturnPoint point;
    point.mapId = bot->GetMapId();
    point.x = bot->GetPositionX();
    point.y = bot->GetPositionY();
    point.z = bot->GetPositionZ();
    point.o = bot->GetOrientation();
    point.set = true;
    return point;
}

void TeleportHome(Player* bot)
{
    bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, bot->GetOrientation());
}

void TeleportBack(Player* bot, ReturnPoint const& back)
{
    if (back.set)
        bot->TeleportTo(back.mapId, back.x, back.y, back.z, back.o);
    else
        TeleportHome(bot);
}

void StartHubVisit(Player* bot)
{
    TofuAutonomousAlt::HubSettings const hub = TofuAutonomousAlt::GetHubSettings();
    TofuAutonomousAlt::SetHubVisit(bot->GetGUID().GetCounter(), true);
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        ApplyHubStrategies(botAI);
    bot->TeleportTo(hub.mapId, hub.x, hub.y, hub.z, hub.o);
}

void ClearReturnPoint(ObjectGuid::LowType guid);

void EndHubVisit(Player* bot, ReturnPoint const& back)
{
    TofuAutonomousAlt::SetHubVisit(bot->GetGUID().GetCounter(), false);
    ClearReturnPoint(bot->GetGUID().GetCounter());
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        if (botAI->HasStrategy("tofu hub", BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy("-tofu hub", BOT_STATE_NON_COMBAT);
    TeleportBack(bot, back);
}

// Whether a trip to the hub is due: something to fix, and not too soon after the last one.
bool HubTripDue(uint32 needs, uint32 lastHubAt, uint32 now)
{
    if (!g_Config.hubEnable || !needs)
        return false;

    // Low ammo goes sooner (GetRestockNeeds reports it only when the hub sells ammo).
    if (needs & TofuAutonomousAlt::RESTOCK_AMMO)
        return !lastHubAt || getMSTimeDiff(lastHubAt, now) >= g_Config.hubMinIntervalMs / 4;

    return !lastHubAt || getMSTimeDiff(lastHubAt, now) >= g_Config.hubMinIntervalMs;
}

bool InHubArea(Player* bot)
{
    TofuAutonomousAlt::HubSettings const hub = TofuAutonomousAlt::GetHubSettings();
    return bot->GetMapId() == hub.mapId && bot->GetExactDist(hub.x, hub.y, hub.z) <= hub.radius;
}

bool PointInHubArea(uint32 mapId, float x, float y, float z)
{
    TofuAutonomousAlt::HubSettings const hub = TofuAutonomousAlt::GetHubSettings();
    if (mapId != hub.mapId)
        return false;
    float const dx = x - hub.x;
    float const dy = y - hub.y;
    float const dz = z - hub.z;
    return dx * dx + dy * dy + dz * dz <= hub.radius * hub.radius;
}

bool IsOpenWorldMap(uint32 mapId)
{
    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    return entry && !entry->Instanceable();
}

// ---- Leaving the guild hub ----

void SaveReturnPoint(ObjectGuid::LowType guid, ReturnPoint const& point)
{
    if (!point.set || !IsOpenWorldMap(point.mapId) || PointInHubArea(point.mapId, point.x, point.y, point.z))
        return;

    g_ReturnPoints[guid] = point;
    CharacterDatabase.Execute(
        "REPLACE INTO tofu_autoalt_return_point (guid, map, x, y, z, o) VALUES ({}, {}, {}, {}, {}, {})", guid,
        point.mapId, point.x, point.y, point.z, point.o);
}

void ClearReturnPoint(ObjectGuid::LowType guid)
{
    if (g_ReturnPoints.erase(guid))
        CharacterDatabase.Execute("DELETE FROM tofu_autoalt_return_point WHERE guid = {}", guid);
}

void LoadReturnPoints()
{
    g_ReturnPoints.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT guid, map, x, y, z, o FROM tofu_autoalt_return_point"))
    {
        do
        {
            Field* fields = result->Fetch();
            ReturnPoint point;
            point.mapId = fields[1].Get<uint16>();
            point.x = fields[2].Get<float>();
            point.y = fields[3].Get<float>();
            point.z = fields[4].Get<float>();
            point.o = fields[5].Get<float>();
            point.set = true;
            g_ReturnPoints[fields[0].Get<ObjectGuid::LowType>()] = point;
        } while (result->NextRow());
    }
    LOG_INFO("module", "mod-autonomous-altbots: loaded {} saved return point(s)", g_ReturnPoints.size());
}

// Quest NPCs: for each quest, the creatures that start and end it, and for each of those creatures one open-world
// spawn in the normal phase outside the hub. Built once (the first time it is needed after startup).
struct SpawnPoint
{
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
};

std::unordered_map<uint32, std::vector<uint32>> g_QuestEnders; // quest id -> creature entries
std::unordered_map<uint32, std::vector<uint32>> g_QuestStarters; // quest id -> creature entries
std::unordered_map<uint32, SpawnPoint> g_QuestNpcSpawn; // creature entry -> a spawn
bool g_QuestIndexBuilt = false;

void BuildQuestNpcIndex()
{
    if (g_QuestIndexBuilt)
        return;
    g_QuestIndexBuilt = true;

    std::unordered_set<uint32> wanted;
    for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
    {
        g_QuestEnders[questId].push_back(entry);
        wanted.insert(entry);
    }
    for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
    {
        g_QuestStarters[questId].push_back(entry);
        wanted.insert(entry);
    }

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!wanted.contains(data.id) || g_QuestNpcSpawn.contains(data.id))
            continue;
        if (!(data.phaseMask & PHASEMASK_NORMAL) || !IsOpenWorldMap(data.mapid))
            continue;
        if (PointInHubArea(data.mapid, data.posX, data.posY, data.posZ))
            continue;

        g_QuestNpcSpawn[data.id] = { data.mapid, data.posX, data.posY, data.posZ, data.orientation };
    }

    LOG_INFO("module", "mod-autonomous-altbots: quest NPC index: {} quest(s) with an ender, {} NPC spawn(s)",
             g_QuestEnders.size(), g_QuestNpcSpawn.size());
}

// The quest NPC to go to: a quest in the log that is complete (its ender) before one that is not (its ender, else its
// starter), nearest the alt's level first. Hostile NPCs and quests too far above a Hardcore alt are skipped.
bool ChooseQuestTarget(Player* bot, bool hardcore, SpawnPoint& target, Quest const*& chosen)
{
    BuildQuestNpcIndex();

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    int32 const botLevel = static_cast<int32>(bot->GetLevel());
    int32 const maxAbove = static_cast<int32>(TofuAutonomousAlt::GetSettings().hardcoreMaxLevelsAbove);
    int32 bestScore = std::numeric_limits<int32>::max();
    chosen = nullptr;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;

        QuestStatus const status = bot->GetQuestStatus(questId);
        if (status != QUEST_STATUS_COMPLETE && status != QUEST_STATUS_INCOMPLETE)
            continue;

        int32 const level = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : botLevel;
        if (hardcore && level > botLevel + maxAbove)
            continue;

        std::vector<uint32> npcs;
        if (auto const itr = g_QuestEnders.find(questId); itr != g_QuestEnders.end())
            npcs = itr->second;
        if (status == QUEST_STATUS_INCOMPLETE)
            if (auto const itr = g_QuestStarters.find(questId); itr != g_QuestStarters.end())
                npcs.insert(npcs.end(), itr->second.begin(), itr->second.end());

        int32 const score = (status == QUEST_STATUS_COMPLETE ? 0 : 1000) + std::abs(level - botLevel);
        if (score >= bestScore)
            continue;

        for (uint32 entry : npcs)
        {
            auto const spawn = g_QuestNpcSpawn.find(entry);
            if (spawn == g_QuestNpcSpawn.end())
                continue;

            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
            if (!creature)
                continue;

            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creature->faction);
            if (faction && botFaction && faction->IsHostileTo(*botFaction))
                continue;

            target = spawn->second;
            chosen = quest;
            bestScore = score;
            break;
        }
    }

    return chosen != nullptr;
}

// Teleports an alt out of the hub: to its saved return point, else to an NPC of a quest in its log, else (not Hardcore)
// to a zone for its level. Returns whether it left.
bool LeaveHub(Player* bot, char const* why)
{
    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    bool const hardcore = GetChallengeState(bot).hardcore;

    if (auto const itr = g_ReturnPoints.find(guid); itr != g_ReturnPoints.end())
    {
        ReturnPoint const back = itr->second;
        ClearReturnPoint(guid);
        if (IsOpenWorldMap(back.mapId) && bot->TeleportTo(back.mapId, back.x, back.y, back.z, back.o))
        {
            LOG_INFO("module", "mod-autonomous-altbots: {} {}; left the hub (return point)", bot->GetName(), why);
            return true;
        }
    }

    SpawnPoint target;
    Quest const* quest = nullptr;
    if (g_Config.hubEscapeQuestTarget && ChooseQuestTarget(bot, hardcore, target, quest) &&
        bot->TeleportTo(target.mapId, target.x, target.y, target.z, target.o))
    {
        LOG_INFO("module", "mod-autonomous-altbots: {} {}; left the hub (quest {} \"{}\")", bot->GetName(), why,
                 quest->GetQuestId(), quest->GetTitle());
        return true;
    }

    if (!hardcore)
    {
        sRandomPlayerbotMgr.TofuTeleportForLevel(bot);
        LOG_INFO("module", "mod-autonomous-altbots: {} {}; left the hub (level zone)", bot->GetName(), why);
        return true;
    }

    if (g_HubStayLogged.insert(guid).second)
        LOG_INFO("module",
                 "mod-autonomous-altbots: {} {}; stays at the hub (Hardcore: no return point, no quest NPC to go to, and "
                 "no level-zone teleport)",
                 bot->GetName(), why);
    return false;
}

// Online members of a party that play as bots and are not with a real player.
std::vector<Player*> OnlinePartyBots(Party const& party)
{
    std::vector<Player*> members;
    for (ObjectGuid::LowType lowGuid : party.members)
    {
        if (!g_DesiredBots.contains(lowGuid))
            continue;
        Player* member = FindConnectedCharacter(ObjectGuid::Create<HighGuid::Player>(lowGuid));
        if (member && member->IsInWorld() && IsPlayerbot(member) && !GroupedWithRealPlayer(member))
            members.push_back(member);
    }
    return members;
}

void ClearRunFlags(std::vector<ObjectGuid::LowType> const& members, ObjectGuid leader)
{
    for (ObjectGuid::LowType guid : members)
    {
        TofuAutonomousAlt::SetDungeonBusy(guid, false);
        TofuAutonomousAlt::SetLfgRoles(guid, 0);
    }
    if (!leader.IsEmpty())
        TofuAutonomousAlt::CancelDungeonClear(leader);
}

// LFG dungeons (normal, not seasonal) that no member is locked out of.
std::vector<lfg::LFGDungeonData const*> CandidateDungeons(std::vector<Player*> const& members, Group* group)
{
    std::vector<lfg::LFGDungeonData const*> candidates;
    for (Player* member : members)
        sLFGMgr->InitializeLockedDungeons(member, group);

    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        LFGDungeonEntry const* entry = sLFGDungeonStore.LookupEntry(i);
        if (!entry)
            continue;

        lfg::LFGDungeonData const* data = sLFGMgr->GetLFGDungeon(entry->ID);
        if (!data || data->type != lfg::LFG_TYPE_DUNGEON || data->difficulty != DUNGEON_DIFFICULTY_NORMAL ||
            data->seasonal)
            continue;

        bool locked = false;
        for (Player* member : members)
            if (sLFGMgr->GetLockedDungeons(member->GetGUID()).contains(data->Entry()))
                locked = true;

        if (!locked)
            candidates.push_back(data);
    }
    return candidates;
}

// Tries to put the grouped party in the Dungeon Finder. Returns true when it is queued.
bool TryQueueDungeon(Party& party, PartyRun& run, Player* leader, std::vector<Player*> const& grouped, uint32 now)
{
    if (!g_Config.dungeonEnable || grouped.size() < 2)
        return false;

    if (run.nextDungeonAt && static_cast<int32>(run.nextDungeonAt - now) > 0)
        return false;

    Group* group = leader->GetGroup();
    if (!group || group->GetLeaderGUID() != leader->GetGUID())
        return false;

    bool restricted = false;
    bool hardcore = false;
    uint8 lowest = 255;
    for (Player* member : grouped)
    {
        if (!ReadyToTravel(member))
            return false;
        ChallengeState const state = GetChallengeState(member);
        restricted = restricted || state.restricting;
        hardcore = hardcore || state.hardcore;
        lowest = std::min<uint8>(lowest, member->GetLevel());
    }

    if (lowest < g_Config.dungeonMinLevel)
    {
        run.status = "questing (dungeons from level " + std::to_string(g_Config.dungeonMinLevel) + ")";
        return false;
    }

    if (restricted && (grouped.size() < PARTY_MAX_MEMBERS || party.members.size() < PARTY_MAX_MEMBERS))
    {
        run.status = "questing (a challenge party goes to dungeons only as a full five)";
        return false;
    }

    // One tank and one healer; everyone else (a second tank or healer too) queues as damage.
    std::vector<std::pair<Player*, uint8>> roles;
    bool tank = false;
    bool healer = false;
    for (Player* member : grouped)
    {
        uint8 role = lfg::PLAYER_ROLE_DAMAGE;
        if (!tank && PlayerbotAI::IsTank(member))
        {
            role = lfg::PLAYER_ROLE_TANK;
            tank = true;
        }
        else if (!healer && PlayerbotAI::IsHeal(member))
        {
            role = lfg::PLAYER_ROLE_HEALER;
            healer = true;
        }
        roles.emplace_back(member, role);
    }

    if (grouped.size() >= PARTY_MAX_MEMBERS && (!tank || !healer))
    {
        run.status = "questing (a full party needs a tank and a healer for the Dungeon Finder)";
        return false;
    }

    std::vector<lfg::LFGDungeonData const*> candidates = CandidateDungeons(grouped, group);
    if (candidates.empty())
    {
        run.status = "questing (no dungeon open to every member)";
        return false;
    }

    lfg::LFGDungeonData const* chosen = nullptr;
    for (lfg::LFGDungeonData const* data : candidates)
        if (!party.clearedDungeons.contains(data->id) && (!chosen || data->minlevel < chosen->minlevel))
            chosen = data;

    if (!chosen)
    {
        if (!g_Config.dungeonRepeatPerLevel || lowest <= run.lastRepeatLevel)
        {
            run.status = "questing (every dungeon in range cleared; repeats at the next level)";
            return false;
        }
        chosen = candidates[urand(0, candidates.size() - 1)];
        run.lastRepeatLevel = lowest;
    }

    uint8 leaderRoles = lfg::PLAYER_ROLE_DAMAGE;
    std::vector<ObjectGuid::LowType> guids;
    for (auto const& [member, role] : roles)
    {
        ObjectGuid::LowType const guid = member->GetGUID().GetCounter();
        guids.push_back(guid);
        TofuAutonomousAlt::SetDungeonBusy(guid, true);
        TofuAutonomousAlt::SetLfgRoles(guid, role);
        if (member == leader)
            leaderRoles = role;
    }
    leaderRoles |= lfg::PLAYER_ROLE_LEADER;

    TofuAutonomousAlt::RequestDungeonClear(leader->GetGUID(), hardcore);

    lfg::LfgDungeonSet dungeons;
    dungeons.insert(chosen->id);
    sLFGMgr->JoinLfg(leader, leaderRoles, dungeons, "");

    if (sLFGMgr->GetState(group->GetGUID()) == lfg::LFG_STATE_NONE &&
        sLFGMgr->GetState(leader->GetGUID()) == lfg::LFG_STATE_NONE)
    {
        ClearRunFlags(guids, leader->GetGUID());
        run.nextDungeonAt = now + 10 * MINUTE * IN_MILLISECONDS;
        run.status = "questing (the Dungeon Finder refused " + chosen->name + ")";
        return false;
    }

    run.phase = PartyPhase::Queued;
    run.since = now;
    run.leader = leader->GetGUID();
    run.groupGuid = group->GetGUID();
    run.dungeonId = chosen->id;
    run.hardcore = hardcore;
    run.cleared = false;
    run.memberCount = static_cast<uint32>(grouped.size());
    run.status = "queued for " + chosen->name;
    LOG_INFO("module", "mod-autonomous-altbots: party {} queued for {} ({} member(s), {} filled)", party.name,
             chosen->name, grouped.size(), grouped.size() < PARTY_MAX_MEMBERS ? "rest" : "none");
    return true;
}

void EndDungeonRun(Party& party, PartyRun& run, bool cleared, char const* why, uint32 now)
{
    run.cleared = cleared;
    for (Player* member : OnlinePartyBots(party))
    {
        if (!member->GetMap() || !member->GetMap()->IsDungeon())
            continue;

        if (member->isDead() && !GetChallengeState(member).hardcore)
        {
            member->ResurrectPlayer(0.5f);
            member->SpawnCorpseBones();
        }
        sLFGMgr->TeleportPlayer(member, true);
    }

    if (cleared)
    {
        party.clearedDungeons.insert(run.dungeonId);
        CharacterDatabase.Execute("REPLACE INTO tofu_autoalt_party_dungeon (party_id, dungeon_id) VALUES ({}, {})",
                                  party.id, run.dungeonId);
    }

    LOG_INFO("module", "mod-autonomous-altbots: party {} dungeon run ended: {}", party.name, why);
    run.phase = PartyPhase::Leaving;
    run.since = now;
    run.status = std::string("leaving the dungeon (") + why + ")";
}

void FinishLeaving(Party& party, PartyRun& run, uint32 now)
{
    for (Player* member : OnlinePartyBots(party))
    {
        if (member->GetGroup())
            member->RemoveFromGroup();  // the fill bots are released when the leader leaves; the party regroups

        // Back at the dungeon entrance: the saved point is no longer needed. At the hub (homebind): it takes them back.
        if (!InHubArea(member) && !(member->GetMap() && member->GetMap()->IsDungeon()))
            ClearReturnPoint(member->GetGUID().GetCounter());
    }

    ClearRunFlags(party.members, run.leader);
    run.nextDungeonAt = now + (run.cleared ? g_Config.dungeonClearedCooldownMs : g_Config.dungeonRetryCooldownMs);
    run.phase = PartyPhase::Open;
    run.since = now;
    run.status = "questing";
}

void ProcessPartyRuns(bool active)
{
    g_PartyBusy.clear();
    uint32 const now = getMSTime();

    for (auto& [partyId, party] : g_Parties)
    {
        PartyRun& run = g_PartyRuns[partyId];
        std::vector<Player*> const online = OnlinePartyBots(party);

        switch (run.phase)
        {
            case PartyPhase::Open:
            {
                if (!active || !g_Config.partyEnable)
                    break;

                // The members grouped under one leader this check (ProcessParties).
                Player* leader = nullptr;
                std::vector<Player*> grouped;
                for (Player* member : online)
                {
                    auto const role = g_PartyRole.find(member->GetGUID().GetCounter());
                    if (role == g_PartyRole.end())
                        continue;
                    grouped.push_back(member);
                    if (role->second == member->GetGUID().GetCounter())
                        leader = member;
                }

                if (!leader || grouped.size() < 2)
                {
                    run.status = online.empty() ? "offline" : "questing";
                    break;
                }

                // Restock first: a trip to the hub for the whole party.
                uint32 needs = 0;
                bool ready = true;
                for (Player* member : grouped)
                {
                    needs |= TofuAutonomousAlt::GetRestockNeeds(member);
                    ready = ready && ReadyToTravel(member);
                }

                if (ready && HubTripDue(needs, run.lastHubAt, now))
                {
                    run.back = PointOf(leader);
                    run.phase = PartyPhase::Hub;
                    run.since = now;
                    run.memberCount = static_cast<uint32>(grouped.size());
                    run.status = "restocking at the guild hub";
                    for (Player* member : grouped)
                    {
                        SaveReturnPoint(member->GetGUID().GetCounter(), run.back);
                        StartHubVisit(member);
                    }
                    break;
                }

                if (TryQueueDungeon(party, run, leader, grouped, now))
                {
                    // Where to go back to if the run ends at the homebind (the hub) or a logout cuts it short.
                    ReturnPoint const back = PointOf(leader);
                    for (Player* member : grouped)
                        SaveReturnPoint(member->GetGUID().GetCounter(), back);
                }
                else if (run.status.rfind("questing", 0) != 0)
                    run.status = "questing";
                break;
            }
            case PartyPhase::Queued:
            {
                Player* leader = FindConnectedCharacter(run.leader);
                Group* group = leader ? leader->GetGroup() : nullptr;
                if (!leader || !group)
                {
                    ClearRunFlags(party.members, run.leader);
                    for (ObjectGuid::LowType member : party.members)
                        ClearReturnPoint(member);
                    run.phase = PartyPhase::Open;
                    run.nextDungeonAt = now + 10 * MINUTE * IN_MILLISECONDS;
                    run.status = "questing (the queue ended: the leader left)";
                    break;
                }

                if (leader->GetMap() && leader->GetMap()->IsDungeon())
                {
                    run.phase = PartyPhase::Dungeon;
                    run.since = now;
                    run.groupGuid = group->GetGUID();
                    run.memberCount = static_cast<uint32>(online.size());
                    lfg::LFGDungeonData const* data = sLFGMgr->GetLFGDungeon(run.dungeonId);
                    run.status = std::string("clearing ") + (data ? data->name : "a dungeon");
                    break;
                }

                bool const inQueue = sLFGMgr->GetState(group->GetGUID()) != lfg::LFG_STATE_NONE ||
                                     sLFGMgr->GetState(leader->GetGUID()) != lfg::LFG_STATE_NONE;
                bool const timedOut = getMSTimeDiff(run.since, now) > g_Config.dungeonQueueTimeoutMs;
                if ((!inQueue && getMSTimeDiff(run.since, now) > 15 * IN_MILLISECONDS) || timedOut)
                {
                    if (inQueue)
                    {
                        sLFGMgr->LeaveLfg(group->GetGUID());
                        sLFGMgr->LeaveAllLfgQueues(leader->GetGUID(), true, group->GetGUID());
                    }
                    ClearRunFlags(party.members, run.leader);
                    for (ObjectGuid::LowType member : party.members)
                        ClearReturnPoint(member);
                    run.phase = PartyPhase::Open;
                    run.nextDungeonAt = now + 10 * MINUTE * IN_MILLISECONDS;
                    run.status = timedOut ? "questing (the queue timed out)" : "questing (the queue ended)";
                }
                break;
            }
            case PartyPhase::Dungeon:
            {
                Player* leader = FindConnectedCharacter(run.leader);
                bool cleared = false;
                if (TofuAutonomousAlt::ConsumeDungeonClearResult(run.groupGuid.GetCounter(), cleared))
                    EndDungeonRun(party, run, cleared, cleared ? "all bosses cleared" : "the clear stopped", now);
                else if (!leader || !leader->GetMap() || !leader->GetMap()->IsDungeon())
                    EndDungeonRun(party, run, false, "the leader left the dungeon", now);
                else if (run.hardcore && online.size() < run.memberCount)
                    EndDungeonRun(party, run, false, "a Hardcore member died", now);
                else if (getMSTimeDiff(run.since, now) > g_Config.dungeonRunTimeoutMs)
                    EndDungeonRun(party, run, false, "the run timed out", now);
                break;
            }
            case PartyPhase::Leaving:
            {
                uint32 const elapsed = getMSTimeDiff(run.since, now);
                bool anyInside = false;
                for (Player* member : online)
                {
                    if (!member->GetMap() || !member->GetMap()->IsDungeon() || member->IsBeingTeleported())
                        continue;
                    anyInside = true;
                    if (elapsed > 20 * IN_MILLISECONDS)
                        TeleportHome(member);
                    else if (elapsed > 10 * IN_MILLISECONDS)
                        member->TeleportToEntryPoint();
                }

                if (!anyInside || elapsed > 40 * IN_MILLISECONDS)
                    FinishLeaving(party, run, now);
                break;
            }
            case PartyPhase::Hub:
            {
                bool allDone = true;
                for (Player* member : online)
                    if (TofuAutonomousAlt::IsHubVisitActive(member->GetGUID().GetCounter()) &&
                        !TofuAutonomousAlt::IsHubVisitDone(member->GetGUID().GetCounter()))
                        allDone = false;

                if (allDone || getMSTimeDiff(run.since, now) > g_Config.hubMaxMs || !active)
                {
                    for (Player* member : online)
                        if (TofuAutonomousAlt::IsHubVisitActive(member->GetGUID().GetCounter()))
                            EndHubVisit(member, run.back);
                    run.phase = PartyPhase::Open;
                    run.since = now;
                    run.lastHubAt = now;
                    run.status = "questing";
                }
                break;
            }
        }

        if (run.phase == PartyPhase::Queued || run.phase == PartyPhase::Dungeon || run.phase == PartyPhase::Leaving)
            for (ObjectGuid::LowType guid : party.members)
                g_PartyBusy.insert(guid);
    }
}

// Alts that play alone this check (not grouped as a party, not busy with one): their own hub trips.
void ProcessSoloHubs(bool active)
{
    uint32 const now = getMSTime();
    for (ObjectGuid::LowType lowGuid : g_DesiredBots)
    {
        SoloHub& hub = g_SoloHub[lowGuid];
        Player* bot = FindConnectedCharacter(ObjectGuid::Create<HighGuid::Player>(lowGuid));
        if (!bot || !bot->IsInWorld() || !IsPlayerbot(bot))
        {
            if (hub.active)
                TofuAutonomousAlt::SetHubVisit(lowGuid, false);
            hub.active = false;
            continue;
        }

        if (hub.active)
        {
            if (TofuAutonomousAlt::IsHubVisitDone(lowGuid) || getMSTimeDiff(hub.since, now) > g_Config.hubMaxMs ||
                !active || GroupedWithRealPlayer(bot))
            {
                EndHubVisit(bot, hub.back);
                hub.active = false;
                hub.lastHubAt = now;
            }
            continue;
        }

        if (!active || g_PartyRole.contains(lowGuid) || g_PartyBusy.contains(lowGuid) || GroupedWithRealPlayer(bot) ||
            TofuAutonomousAlt::IsHubVisitActive(lowGuid))
            continue;

        if (!ReadyToTravel(bot) || !HubTripDue(TofuAutonomousAlt::GetRestockNeeds(bot), hub.lastHubAt, now))
            continue;

        hub.back = PointOf(bot);
        SaveReturnPoint(lowGuid, hub.back);
        hub.active = true;
        hub.since = now;
        StartHubVisit(bot);
    }
}

// An alt that logged in at the hub or inside a dungeon with nothing to do there (a trip or run cut short by a logout),
// or that has stood idle at the hub (its homebind may be the hub), goes back to its quests (Hub.Escape.Enable) or,
// with that off, home.
void RescueStranded(Player* bot)
{
    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    bool const firstCheck = g_RescueCheckedThisLogin.insert(guid).second;
    bool const hubKnown = g_Config.hubEnable || g_Config.hubEscapeEnable;
    bool const inDungeon = bot->GetMap() && bot->GetMap()->IsDungeon();

    if (!hubKnown || !InHubArea(bot))
        g_HubIdleSince.erase(guid);

    if (!ReadyToTravel(bot) && !(inDungeon && bot->IsAlive()))
        return;
    if (bot->IsBeingTeleported() || bot->IsInCombat() || GroupedWithRealPlayer(bot))
        return;
    if (TofuAutonomousAlt::IsHubVisitActive(guid) || g_PartyBusy.contains(guid))
        return;

    if (inDungeon)
    {
        auto const itr = g_ReturnPoints.find(guid);
        if (g_Config.hubEscapeEnable && itr != g_ReturnPoints.end())
        {
            ReturnPoint const back = itr->second;
            ClearReturnPoint(guid);
            if (bot->TeleportTo(back.mapId, back.x, back.y, back.z, back.o))
            {
                LOG_INFO("module", "mod-autonomous-altbots: {} was left in a dungeon; went to its return point",
                         bot->GetName());
                return;
            }
        }
        TeleportHome(bot);
        return;
    }

    if (!hubKnown || !InHubArea(bot))
        return;

    if (!g_Config.hubEscapeEnable)
    {
        if (g_Config.hubEnable)
            TeleportHome(bot);
        return;
    }

    // A party follower is brought along by its leader (RegroupFollower).
    auto const role = g_PartyRole.find(guid);
    if (role != g_PartyRole.end() && role->second != guid)
        return;

    uint32 const now = getMSTime();
    auto const [idle, fresh] = g_HubIdleSince.try_emplace(guid, now);
    if (!firstCheck && (fresh || getMSTimeDiff(idle->second, now) < g_Config.hubEscapeIdleMs))
        return;

    idle->second = now;  // a failed attempt is retried after another IdleSeconds
    LeaveHub(bot, firstCheck ? "logged in at the hub" : "was idle at the hub");
}


// ---- Multibox: a real player's own altbots in his group ----
//
// Quests he is rewarded for are force-completed and rewarded for each of them (no looting or travel; prerequisites and
// level are not checked, race, class, exclusive groups, and repeat timers are); quests he accepts or abandons are
// accepted or abandoned too; opening a trainer teaches them their class spells or the ranks of that profession (only
// professions they already have); opening a vendor makes them sell junk and gear they will not use, repair, and
// restock ammo and class reagents. They never buy gear. The hooks fire on map threads, so they queue events that the
// world thread applies.

enum class MultiboxEventType : uint8
{
    QuestRewarded,
    QuestAbandoned,
    TrainerOpened,
    VendorOpened,
    GuildBankOpened
};

struct MultiboxEvent
{
    MultiboxEventType type;
    ObjectGuid main;
    uint32 id = 0;      // quest id
    ObjectGuid npc;     // trainer, vendor, or guild bank object
};

std::mutex g_MultiboxMutex;
std::vector<MultiboxEvent> g_MultiboxEvents;
std::unordered_map<ObjectGuid, uint32> g_MultiboxBankTimes;  // world thread only

void QueueMultibox(MultiboxEvent const& event)
{
    std::lock_guard lock(g_MultiboxMutex);
    g_MultiboxEvents.push_back(event);
}

bool IsMultiboxMain(Player* player)
{
    return g_Config.multiboxEnable && player && player->GetGroup() && IsRealPlayer(player) &&
           !TofuAutonomousAlt::IsActive(player);
}

std::vector<Player*> MultiboxAltsOf(Player* main)
{
    std::vector<Player*> alts;
    Group* group = main->GetGroup();
    if (!group)
        return alts;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == main || !member->IsInWorld() || !IsPlayerbot(member))
            continue;

        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (memberAI && memberAI->GetMaster() == main && TofuAutonomousAlt::IsMultiboxAlt(member))
            alts.push_back(member);
    }
    return alts;
}

ItemUsage UsageFor(Player* alt, uint32 itemId)
{
    PlayerbotAI* altAI = GET_PLAYERBOT_AI(alt);
    if (!altAI)
        return ITEM_USAGE_NONE;
    return altAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", std::to_string(itemId))->Get();
}

// How much a reward item would raise this alt's stat score (playerbots' weights for its class, spec, and role) over
// what it wears in that slot, by stats alone: any armor type it can wear counts (a caster druid weighs cloth with
// intellect against leather with stamina), unlike playerbots' own gear check, which keeps each class to one armor type.
// 0 when it cannot wear the item (level, proficiency, class, challenge-mode gear rules) or it is no improvement.
float RewardUpgradeGain(Player* alt, PlayerbotAI* altAI, StatsWeightCalculator& calculator, ItemTemplate const* proto)
{
    if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
        return 0.0f;
    if (proto->InventoryType == INVTYPE_BODY || proto->InventoryType == INVTYPE_TABARD)
        return 0.0f;
    if (alt->CanUseItem(proto) != EQUIP_ERR_OK)
        return 0.0f;

    Item* test = Item::CreateItem(proto->ItemId, 1, alt);
    if (!test)
        return 0.0f;
    uint16 dest = 0;
    bool const wearable = altAI->CanEquipItem(NULL_SLOT, dest, test, true, true) == EQUIP_ERR_OK;
    test->RemoveFromUpdateQueueOf(alt);
    delete test;
    if (!wearable)
        return 0.0f;

    uint8 const slot = altAI->FindEquipSlot(proto, NULL_SLOT, true);
    if (slot == NULL_SLOT)
        return 0.0f;

    // Rings, trinkets, and one-handers for a dual wielder replace the weaker of their two slots.
    std::vector<uint8> slots = { slot };
    if (slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_FINGER2)
        slots = { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 };
    else if (slot == EQUIPMENT_SLOT_TRINKET1 || slot == EQUIPMENT_SLOT_TRINKET2)
        slots = { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 };
    else if (slot == EQUIPMENT_SLOT_MAINHAND && alt->CanDualWield() && proto->InventoryType != INVTYPE_2HWEAPON &&
             proto->InventoryType != INVTYPE_WEAPONMAINHAND)
        slots = { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND };

    float worn = -1.0f;
    for (uint8 s : slots)
    {
        Item* current = alt->GetItemByPos(INVENTORY_SLOT_BAG_0, s);
        float const score = current ? calculator.CalculateItem(current->GetEntry(), current->GetItemRandomPropertyId())
                                    : 0.0f;
        if (worn < 0.0f || score < worn)
            worn = score;
    }

    float const gain = calculator.CalculateItem(proto->ItemId) - std::max(worn, 0.0f);
    return gain > 0.0f ? gain : 0.0f;
}

// The reward choice: the biggest stat improvement for this alt, else the one that sells for most.
uint32 PickQuestReward(Player* alt, Quest const* quest, bool& upgrade)
{
    upgrade = false;
    uint32 const choices = quest->GetRewChoiceItemsCount();
    if (!choices)
        return 0;

    PlayerbotAI* altAI = GET_PLAYERBOT_AI(alt);
    StatsWeightCalculator calculator(alt);
    int32 bestUpgrade = -1;
    float bestGain = 0.0f;
    int32 bestValue = -1;
    uint32 bestPrice = 0;

    for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
    {
        uint32 const itemId = quest->RewardChoiceItemId[i];
        ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
        if (!proto)
            continue;

        float const gain = altAI ? RewardUpgradeGain(alt, altAI, calculator, proto) : 0.0f;
        if (gain > bestGain)
        {
            bestUpgrade = static_cast<int32>(i);
            bestGain = gain;
        }

        uint32 const price = proto->SellPrice * std::max<uint32>(quest->RewardChoiceItemCount[i], 1);
        if (bestValue < 0 || price > bestPrice)
        {
            bestValue = static_cast<int32>(i);
            bestPrice = price;
        }
    }

    if (bestUpgrade >= 0)
    {
        upgrade = true;
        return static_cast<uint32>(bestUpgrade);
    }
    return bestValue >= 0 ? static_cast<uint32>(bestValue) : 0;
}

bool HasRoomFor(Player* alt, uint32 itemId, uint32 count)
{
    if (!itemId || !count)
        return true;
    ItemPosCountVec dest;
    return alt->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) == EQUIP_ERR_OK;
}

// Every item in the backpack and the equipped bags (not equipped gear, not the bags themselves).
std::vector<Item*> BagItems(Player* alt)
{
    std::vector<Item*> items;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = alt->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items.push_back(item);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* pBag = alt->GetBagByPos(bag))
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (Item* item = pBag->GetItemByPos(slot))
                    items.push_back(item);
    return items;
}

// Reagents used by the class spells the alt knows.
std::unordered_set<uint32> ClassReagents(Player* alt)
{
    std::unordered_set<uint32> reagents;
    ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(alt->getClass());
    if (!classEntry)
        return reagents;

    for (auto const& [spellId, playerSpell] : alt->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->SpellFamilyName != classEntry->spellfamily)
            continue;
        for (int32 reagent : info->Reagent)
            if (reagent > 0)
                reagents.insert(static_cast<uint32>(reagent));
    }
    return reagents;
}

// A consumable the alt can use (potions, healthstones, food, drink, elixirs, flasks, scrolls), of which it keeps only
// ConsumableKeep per item (conjured items are left alone: they cannot be sold or banked).
bool IsKeptConsumable(Player* alt, Item* item)
{
    ItemTemplate const* proto = item->GetTemplate();
    return proto->Class == ITEM_CLASS_CONSUMABLE && !item->IsConjuredConsumable() &&
           alt->CanUseItem(proto) == EQUIP_ERR_OK;
}

// How many of each kept consumable the alt holds beyond ConsumableKeep.
std::unordered_map<uint32, uint32> ConsumableExcess(Player* alt)
{
    std::unordered_map<uint32, uint32> excess;
    if (!g_Config.multiboxConsumableKeep)
        return excess;

    std::unordered_map<uint32, uint32> held;
    for (Item* item : BagItems(alt))
        if (IsKeptConsumable(alt, item) && !TofuAutonomousAlt::IsKeptItem(item->GetEntry()))
            held[item->GetEntry()] += item->GetCount();
    for (auto const& [itemId, count] : held)
        if (count > g_Config.multiboxConsumableKeep)
            excess[itemId] = count - g_Config.multiboxConsumableKeep;
    return excess;
}

void MultiboxRewardQuest(Player* main, Quest const* quest)
{
    uint32 const questId = quest->GetQuestId();
    for (Player* alt : MultiboxAltsOf(main))
    {
        // Only a quest the alt has in its log (accepted normally, so prerequisites and level were met).
        if (alt->FindQuestSlot(questId) >= MAX_QUEST_LOG_SIZE)
            continue;

        PlayerbotAI* altAI = GET_PLAYERBOT_AI(alt);
        std::string const questName = quest->GetTitle();

        if (quest->GetRewOrReqMoney() < 0 && !alt->HasEnoughMoney(-quest->GetRewOrReqMoney()))
        {
            altAI->TellMaster("I need " + ChatHelper::formatMoney(-quest->GetRewOrReqMoney()) + " to complete " +
                              questName + ".");
            continue;
        }

        bool upgrade = false;
        uint32 const choice = PickQuestReward(alt, quest, upgrade);
        bool room = quest->GetRewChoiceItemsCount() == 0 ||
                    HasRoomFor(alt, quest->RewardChoiceItemId[choice], quest->RewardChoiceItemCount[choice]);
        for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
            room = room && HasRoomFor(alt, quest->RewardItemId[i], quest->RewardItemIdCount[i]);
        if (!room)
        {
            altAI->TellMaster("My bags are full, so I could not complete " + questName + ".");
            continue;
        }

        QuestStatus const status = alt->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_FAILED)
            alt->CompleteQuest(questId);

        // The alt itself stands in for the quest giver (its own spells are cast on itself; mail comes from it).
        alt->RewardQuest(quest, choice, alt, false);

        // Wear the rewards that raise its stats now (the chosen item and any fixed reward items), not at the next
        // gear check, which also holds each class to one armor type and wants 10% better.
        StatsWeightCalculator calculator(alt);
        auto wearIfBetter = [&](ItemTemplate const* proto)
        {
            if (!proto || RewardUpgradeGain(alt, altAI, calculator, proto) <= 0.0f)
                return false;
            EquipAction(altAI).EquipItems({ proto->ItemId });
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item* worn = alt->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (worn && worn->GetEntry() == proto->ItemId)
                    return true;
            }
            return false;
        };

        uint32 const chosenItem = quest->GetRewChoiceItemsCount() ? quest->RewardChoiceItemId[choice] : 0;
        ItemTemplate const* chosenProto = chosenItem ? sObjectMgr->GetItemTemplate(chosenItem) : nullptr;
        bool const chosenWorn = upgrade && wearIfBetter(chosenProto);

        std::string message = "Turned in " + ChatHelper::FormatQuest(quest);
        if (chosenProto)
            message += ", took " + ChatHelper::FormatItem(chosenProto) + (chosenWorn ? " and equipped it" : "");
        for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
        {
            ItemTemplate const* fixed =
                quest->RewardItemId[i] ? sObjectMgr->GetItemTemplate(quest->RewardItemId[i]) : nullptr;
            if (fixed && wearIfBetter(fixed))
                message += ", equipped " + ChatHelper::FormatItem(fixed);
        }
        altAI->TellMaster(message + ".");

        if (g_Config.verboseLogging)
            LOG_INFO("module", "mod-autonomous-altbots: multibox {} completed quest {} with {}", alt->GetName(),
                     questId, main->GetName());
    }
}

void MultiboxAbandonQuest(Player* main, uint32 questId)
{
    for (Player* alt : MultiboxAltsOf(main))
    {
        if (alt->FindQuestSlot(questId) >= MAX_QUEST_LOG_SIZE)
            continue;

        if (!alt->TakeQuestSourceItem(questId, false))
            continue;
        alt->AbandonQuest(questId);
        alt->RemoveActiveQuest(questId);
        alt->RemoveTimedAchievement(ACHIEVEMENT_TIMED_TYPE_QUEST, questId);
    }
}

void MultiboxTrainer(Player* main, Creature* npc)
{
    Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
    if (!trainer)
        return;

    bool const classTrainer = trainer->GetTrainerType() == Trainer::Type::Class;
    uint32 const skill = classTrainer ? 0 : TofuAutonomousAlt::TrainerProfessionSkill(npc);
    if (!classTrainer && !skill)
        return;

    for (Player* alt : MultiboxAltsOf(main))
    {
        uint32 shortfall = 0;
        if (classTrainer)
            TofuAutonomousAlt::LearnClassTrainerSpells(alt, &shortfall);
        else if (alt->HasSkill(skill) || TofuAutonomousAlt::IsSelfCrafted(alt))
            TofuAutonomousAlt::LearnProfessions(alt, npc, &shortfall);  // never starts a new profession
        else
            continue;

        if (shortfall)
            GET_PLAYERBOT_AI(alt)->TellMaster("I need " + ChatHelper::formatMoney(shortfall) + " more to train" +
                                               (classTrainer ? " my spells." : " my profession."));
    }
}

// Buys `units` purchases of a vendor item without walking there (the alt pays the list price).
uint32 MultiboxBuy(Player* alt, VendorItem const* vendorItem, ItemTemplate const* proto, uint32 units, bool& broke)
{
    uint32 bought = 0;
    uint32 const perBuy = std::max<uint32>(proto->BuyCount, 1);
    for (uint32 i = 0; i < units; ++i)
    {
        if (proto->BuyPrice && !alt->HasEnoughMoney(proto->BuyPrice))
        {
            broke = true;
            break;
        }
        if (!HasRoomFor(alt, vendorItem->item, perBuy) || !alt->StoreNewItemInBestSlots(vendorItem->item, perBuy))
            break;
        alt->ModifyMoney(-static_cast<int32>(proto->BuyPrice));
        ++bought;
    }
    return bought;
}

void MultiboxVendor(Player* main, Creature* npc)
{
    uint32 const currentList = main->GetSession()->GetCurrentVendor();
    VendorItemData const* items =
        currentList ? sObjectMgr->GetNpcVendorItemList(currentList) : npc->GetVendorItems();

    for (Player* alt : MultiboxAltsOf(main))
    {
        PlayerbotAI* altAI = GET_PLAYERBOT_AI(alt);
        bool broke = false;

        // Sell grays, and weapons and armor that are soulbound or white quality and that it cannot use or that are no
        // upgrade over what it wears. Unbound green-or-better gear is kept for the guild bank.
        std::vector<Item*> toSell;
        for (Item* item : BagItems(alt))
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto->SellPrice || proto->Class == ITEM_CLASS_QUEST)
                continue;
            if (proto->Quality == ITEM_QUALITY_POOR)
            {
                toSell.push_back(item);
                continue;
            }
            bool const gear = (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
                              proto->InventoryType != INVTYPE_BODY && proto->InventoryType != INVTYPE_TABARD;
            if (!gear || (!item->IsSoulBound() && proto->Quality != ITEM_QUALITY_NORMAL))
                continue;
            ItemUsage const usage = UsageFor(alt, proto->ItemId);
            if (alt->CanUseItem(proto) != EQUIP_ERR_OK || (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE))
                toSell.push_back(item);
        }

        for (Item* item : toSell)
        {
            alt->ModifyMoney(static_cast<int32>(item->GetTemplate()->SellPrice * item->GetCount()));
            alt->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        }

        // Sell usable consumables beyond ConsumableKeep of each (those with no sell price wait for the guild bank).
        for (auto const& [itemId, extra] : ConsumableExcess(alt))
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto || !proto->SellPrice)
                continue;
            alt->DestroyItemCount(itemId, extra, true);
            alt->ModifyMoney(static_cast<int32>(proto->SellPrice * extra));
        }

        // Repair.
        alt->DurabilityRepairAll(true, 1.0f, false);
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = alt->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item && item->GetUInt32Value(ITEM_FIELD_DURABILITY) < item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
            {
                altAI->TellMaster("I do not have enough gold to repair all my gear.");
                break;
            }
        }

        if (!items)
            continue;

        // Restock: the best usable ammo for its ranged weapon, and the reagents its class spells use.
        uint32 const ammoSubClass = TofuAutonomousAlt::AmmoSubClassFor(alt);
        std::unordered_set<uint32> const reagents = ClassReagents(alt);

        VendorItem const* bestAmmo = nullptr;
        ItemTemplate const* bestAmmoProto = nullptr;
        for (VendorItem const* vendorItem : items->m_items)
        {
            if (!vendorItem || vendorItem->ExtendedCost || vendorItem->maxcount)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
            if (!proto || alt->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;

            bool const betterAmmo = !bestAmmoProto || proto->ItemLevel > bestAmmoProto->ItemLevel;
            if (ammoSubClass && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == ammoSubClass &&
                proto->RequiredLevel <= alt->GetLevel() && betterAmmo)
            {
                bestAmmo = vendorItem;
                bestAmmoProto = proto;
            }

            if (reagents.contains(proto->ItemId))
            {
                uint32 const have = alt->GetItemCount(proto->ItemId, false);
                uint32 const target = std::min<uint32>(g_Config.multiboxReagentTarget, proto->GetMaxStackSize());
                if (have < target)
                {
                    uint32 const perBuy = std::max<uint32>(proto->BuyCount, 1);
                    MultiboxBuy(alt, vendorItem, proto, (target - have + perBuy - 1) / perBuy, broke);
                }
            }
        }

        if (bestAmmo)
        {
            uint32 const have = alt->GetItemCount(bestAmmoProto->ItemId, false);
            if (have < g_Config.multiboxAmmoTarget)
            {
                uint32 const perBuy = std::max<uint32>(bestAmmoProto->BuyCount, 1);
                if (MultiboxBuy(alt, bestAmmo, bestAmmoProto,
                                (g_Config.multiboxAmmoTarget - have + perBuy - 1) / perBuy, broke))
                    alt->SetAmmo(bestAmmoProto->ItemId);
            }
        }

        if (broke)
            altAI->TellMaster("I do not have enough gold to restock.");
    }
}

// Deposits every unbound item of white quality or better into the configured guild bank tabs, keeping ammo, class
// reagents, profession tools, quest items, temporary items, consumables it can use, and gear it would equip.
void MultiboxGuildBank(Player* main)
{
    Guild* guild = main->GetGuildId() ? sGuildMgr->GetGuildById(main->GetGuildId()) : nullptr;
    if (!guild || g_Config.multiboxGuildBankTabs.empty())
        return;

    for (Player* alt : MultiboxAltsOf(main))
    {
        PlayerbotAI* altAI = GET_PLAYERBOT_AI(alt);
        if (alt->GetGuildId() != main->GetGuildId())
        {
            altAI->TellMaster("I am not in your guild, so I cannot use the guild bank.");
            continue;
        }

        std::unordered_set<uint32> const reagents = ClassReagents(alt);
        std::unordered_map<uint32, uint32> keepLeft;  // per kept consumable: how many more stay in its bags
        std::vector<std::pair<ObjectGuid, uint32>> toDeposit;  // item, amount to split off (0 = the whole stack)
        for (Item* item : BagItems(alt))
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (TofuAutonomousAlt::IsKeptItem(proto->ItemId))
                continue;

            // Usable consumables: keep ConsumableKeep of each, deposit the rest.
            if (IsKeptConsumable(alt, item) && !item->IsSoulBound() && !item->IsBoundAccountWide())
            {
                if (!g_Config.multiboxConsumableKeep)
                    continue;
                auto [it, fresh] = keepLeft.try_emplace(proto->ItemId, g_Config.multiboxConsumableKeep);
                uint32 const stays = std::min(it->second, item->GetCount());
                it->second -= stays;
                uint32 const extra = item->GetCount() - stays;
                if (extra)
                    toDeposit.emplace_back(item->GetGUID(), extra == item->GetCount() ? 0 : extra);
                continue;
            }

            if (proto->Quality < ITEM_QUALITY_NORMAL || item->IsSoulBound() || item->IsBoundAccountWide() ||
                proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_PROJECTILE || proto->Duration ||
                proto->TotemCategory || item->IsConjuredConsumable() || item->IsRefundable() ||
                reagents.contains(proto->ItemId))
                continue;
            ItemUsage const usage = UsageFor(alt, proto->ItemId);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                continue;
            toDeposit.emplace_back(item->GetGUID(), 0);
        }

        uint32 failed = 0;
        for (auto const& [itemGuid, split] : toDeposit)
        {
            bool stored = false;
            for (uint8 tab : g_Config.multiboxGuildBankTabs)
            {
                Item* item = alt->GetItemByGuid(itemGuid);
                if (!item)
                {
                    stored = true;
                    break;
                }
                uint32 const before = item->GetCount();
                guild->SwapItemsWithInventory(alt, false, tab, NULL_SLOT, item->GetBagSlot(), item->GetSlot(), split);
                Item* after = alt->GetItemByGuid(itemGuid);
                if (!after || (split && after->GetCount() < before))
                {
                    stored = true;
                    break;
                }
            }
            if (!stored)
                ++failed;
        }

        if (failed)
            altAI->TellMaster("I could not deposit " + std::to_string(failed) +
                              " items: the guild bank tabs are full or my rank cannot deposit there.");

        if (g_Config.verboseLogging)
            LOG_INFO("module", "mod-autonomous-altbots: multibox {} deposited {} of {} items in the guild bank",
                     alt->GetName(), toDeposit.size() - failed, toDeposit.size());
    }
}

void ProcessMultibox()
{
    std::vector<MultiboxEvent> events;
    {
        std::lock_guard lock(g_MultiboxMutex);
        events.swap(g_MultiboxEvents);
    }

    for (MultiboxEvent const& event : events)
    {
        Player* main = ObjectAccessor::FindPlayer(event.main);
        if (!IsMultiboxMain(main) || !main->IsInWorld())
            continue;

        switch (event.type)
        {
            case MultiboxEventType::QuestRewarded:
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(event.id))
                    MultiboxRewardQuest(main, quest);
                break;
            case MultiboxEventType::QuestAbandoned:
                MultiboxAbandonQuest(main, event.id);
                break;
            case MultiboxEventType::TrainerOpened:
                if (Creature* npc = ObjectAccessor::GetCreature(*main, event.npc))
                    MultiboxTrainer(main, npc);
                break;
            case MultiboxEventType::VendorOpened:
                if (Creature* npc = ObjectAccessor::GetCreature(*main, event.npc))
                    MultiboxVendor(main, npc);
                break;
            case MultiboxEventType::GuildBankOpened:
            {
                // Once per opening: the client sends the activation again only when the window is reopened.
                uint32& last = g_MultiboxBankTimes[event.main];
                if (last && getMSTimeDiff(last, getMSTime()) < 10 * IN_MILLISECONDS)
                    break;
                if (main->GetGameObjectIfCanInteractWith(event.npc, GAMEOBJECT_TYPE_GUILD_BANK))
                {
                    last = getMSTime();
                    MultiboxGuildBank(main);
                }
                break;
            }
        }
    }
}

class AutonomousAltBotsServerScript : public ServerScript
{
public:
    AutonomousAltBotsServerScript()
        : ServerScript("AutonomousAltBotsServerScript", { SERVERHOOK_CAN_PACKET_SEND, SERVERHOOK_CAN_PACKET_RECEIVE })
    {
    }

    // Any thread: notes that a real player opened the guild bank (CMSG_GUILD_BANKER_ACTIVATE: bank guid, full update).
    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != CMSG_GUILD_BANKER_ACTIVATE || !g_Config.multiboxEnable ||
            !g_Config.multiboxGuildBank || packet.size() < sizeof(uint64))
            return true;

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!IsMultiboxMain(player) || !player->GetGuildId())
            return true;

        MultiboxEvent event;
        event.type = MultiboxEventType::GuildBankOpened;
        event.main = player->GetGUID();
        event.npc = ObjectGuid(packet.read<uint64>(0));
        QueueMultibox(event);
        return true;
    }

    // Any thread: only notes that a real player's trainer or vendor window opened.
    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        uint16 const opcode = packet.GetOpcode();
        if (opcode != SMSG_TRAINER_LIST && opcode != SMSG_LIST_INVENTORY)
            return true;

        if (!g_Config.multiboxEnable || packet.size() < sizeof(uint64))
            return true;

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!IsMultiboxMain(player))
            return true;

        bool const trainer = opcode == SMSG_TRAINER_LIST;
        if ((trainer && !g_Config.multiboxTraining) || (!trainer && !g_Config.multiboxVendor))
            return true;

        MultiboxEvent event;
        event.type = trainer ? MultiboxEventType::TrainerOpened : MultiboxEventType::VendorOpened;
        event.main = player->GetGUID();
        event.npc = ObjectGuid(packet.read<uint64>(0));
        QueueMultibox(event);
        return true;
    }
};

class AutonomousAltBotsWorldScript : public WorldScript
{
public:
    AutonomousAltBotsWorldScript()
        : WorldScript(
              "AutonomousAltBotsWorldScript",
              { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
        LOG_INFO(
            "module",
            "mod-autonomous-altbots v{}: {} (startup delay={}s, check interval={}s, logout {}s after the last player)",
            MODULE_VERSION,
            g_Config.enabled ? "enabled" : "disabled",
            g_Config.startupDelayMs / IN_MILLISECONDS,
            g_Config.checkIntervalMs / IN_MILLISECONDS,
            g_Config.noPlayerLogoutDelayMs / IN_MILLISECONDS);
    }

    void OnStartup() override
    {
        LoadDesiredBots();
        LoadParties();
        LoadReturnPoints();
        if (g_Config.hubEscapeEnable && g_Config.hubEscapeQuestTarget)
            BuildQuestNpcIndex();
        g_StartupElapsed = 0;
        g_CheckElapsed = 0;
        g_NoPlayerElapsed = 0;
        g_StartupComplete = g_Config.startupDelayMs == 0;
    }

    void OnUpdate(uint32 diff) override
    {
        ProcessStopWatch(diff);
        ProcessHardcoreDeaths();
        ProcessMultibox();

        if (!g_Config.enabled || !PlayerSettingsEnabled())
            return;

        if (!g_StartupComplete)
        {
            g_StartupElapsed += diff;
            if (g_StartupElapsed < g_Config.startupDelayMs)
                return;
            g_StartupComplete = true;
        }

        g_CheckElapsed += diff;
        if (g_CheckElapsed < g_Config.checkIntervalMs)
            return;
        uint32 const elapsed = g_CheckElapsed;
        g_CheckElapsed = 0;

        bool const playerOnline = AnyRealPlayerInWorld();
        if (playerOnline)
            g_NoPlayerElapsed = 0;
        else
            g_NoPlayerElapsed += elapsed;

        bool const logoutAll = !playerOnline && g_NoPlayerElapsed >= g_Config.noPlayerLogoutDelayMs;

        // Turned off after the loop, because turning off changes g_DesiredBots.
        std::vector<std::pair<ObjectGuid, char const*>> turnOff;

        ProcessParties(playerOnline && !logoutAll);
        ProcessPartyRuns(playerOnline && !logoutAll);
        ProcessSoloHubs(playerOnline && !logoutAll);

        for (ObjectGuid::LowType lowGuid : g_DesiredBots)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
            Player* connected = FindConnectedCharacter(guid);

            if (!connected)
            {
                g_CaughtUpThisLogin.erase(lowGuid);
                g_RescueCheckedThisLogin.erase(lowGuid);
                g_HubIdleSince.erase(lowGuid);
                g_HubStayLogged.erase(lowGuid);
                TofuAutonomousAlt::ClearBotState(lowGuid);

                // Never log in while no real player is in the world.
                if (!playerOnline)
                    continue;

                // A Hardcore or dead Hardcore character never logs in as an autonomous alt.
                if (char const* reason = ChallengeBlockReason(GetOfflineChallengeState(guid)))
                {
                    turnOff.emplace_back(guid, reason);
                    continue;
                }

                sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
                continue;
            }

            if (!IsPlayerbot(connected))
            {
                if (g_Config.verboseLogging)
                    LOG_DEBUG("module", "mod-autonomous-altbots: {} is played from a game client; skipped",
                              connected->GetName());
                continue;
            }

            if (logoutAll)
            {
                LogoutAutonomousBot(guid);
                continue;
            }

            ChallengeState const state = GetChallengeState(connected);
            if (char const* reason = ChallengeBlockReason(state))
            {
                turnOff.emplace_back(guid, reason);
                continue;
            }

            RescueStranded(connected);
            ApplyAutonomousStrategies(connected, state);
        }

        for (auto const& [guid, reason] : turnOff)
            TurnOffForChallenge(guid, reason);
    }

private:
    // A Hardcore alt that died is logged out before it can release and be resurrected (which mod-challenge-modes
    // answers with a kill and a kick, and which would repeat at every login).
    void ProcessHardcoreDeaths()
    {
        std::unordered_set<ObjectGuid::LowType> deaths;
        {
            std::lock_guard lock(g_PendingDeathsMutex);
            deaths.swap(g_PendingHardcoreDeaths);
        }

        for (ObjectGuid::LowType lowGuid : deaths)
            if (g_DesiredBots.contains(lowGuid))
                TurnOffForChallenge(ObjectGuid::Create<HighGuid::Player>(lowGuid), "died on the Hardcore challenge");
    }

    void ProcessStopWatch(uint32 diff)
    {
        for (auto itr = g_StopWatchElapsed.begin(); itr != g_StopWatchElapsed.end();)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(itr->first);
            if (sRandomPlayerbotMgr.GetPlayerBot(guid))
            {
                sRandomPlayerbotMgr.LogoutPlayerBot(guid);
                itr->second = 0;
            }
            else
            {
                itr->second += diff;
            }

            if (itr->second >= STOP_WATCH_DURATION_MS)
                itr = g_StopWatchElapsed.erase(itr);
            else
                ++itr;
        }
    }
};

class AutonomousAltBotsPlayerScript : public PlayerScript
{
public:
    AutonomousAltBotsPlayerScript()
        : PlayerScript("AutonomousAltBotsPlayerScript",
                       { PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_CAN_GROUP_INVITE, PLAYERHOOK_ON_DELETE_FROM_DB,
                         PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_QUEST_ABANDON, PLAYERHOOK_ON_LOGIN,
                         PLAYERHOOK_ON_SAVE })
    {
    }

    // World thread. A bot session has no socket (headless); a game client's session has one.
    void OnPlayerLogin(Player* player) override
    {
        if (!player || !player->GetSession())
            return;

        if (player->GetSession()->IsHeadless())
            SaveActionBarsForBot(player);
        else
            RestoreActionBarsForPlayer(player);
    }

    void OnPlayerSave(Player* player) override
    {
        DropActionBarCopyAfterSave(player);
    }

    // Map threads: the multibox main's quest changes are queued for the world thread.
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (g_Config.multiboxQuests && quest && IsMultiboxMain(player))
            QueueMultibox({ MultiboxEventType::QuestRewarded, player->GetGUID(), quest->GetQuestId(), {} });
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        if (g_Config.multiboxQuests && IsMultiboxMain(player))
            QueueMultibox({ MultiboxEventType::QuestAbandoned, player->GetGUID(), questId, {} });
    }

    // World thread (CMSG_GROUP_INVITE is thread-unsafe). The core refuses an invite to anyone already in a group, and
    // a party member is always in its party group, so a real player who may control the member takes it out first.
    bool OnPlayerCanGroupInvite(Player* player, std::string& membername) override
    {
        if (!g_Config.partyEnable || !player || !IsRealPlayer(player))
            return true;

        Player* invitee = ObjectAccessor::FindPlayerByName(membername, false);
        if (!invitee || !IsPlayerbot(invitee) || !g_PartyOfMember.contains(invitee->GetGUID().GetCounter()))
            return true;

        // Queued for or inside a dungeon with its party, or on a hub trip: it stays (the core then declines the invite).
        if (TofuAutonomousAlt::IsDungeonBusy(invitee->GetGUID().GetCounter()) ||
            TofuAutonomousAlt::IsHubVisitActive(invitee->GetGUID().GetCounter()))
            return true;

        Group* group = invitee->GetGroup();
        if (!group || group == player->GetGroup() || GroupedWithRealPlayer(invitee))
            return true;

        if (!TofuAutonomousAlt::CanControl(invitee, player))
            return true;  // the bot's own invite handling declines

        invitee->RemoveFromGroup();
        return true;
    }

    // Character GUIDs are reused after a delete, so the party row and the saved action bars go with the character.
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM tofu_autoalt_party_member WHERE guid = {}", guid);
        trans->Append("DELETE FROM tofu_autoalt_action_snapshot WHERE guid = {}", guid);
        trans->Append("DELETE FROM tofu_autoalt_return_point WHERE guid = {}", guid);
        g_ReturnPoints.erase(guid);
        RemovePartyMember(guid);
    }

    // Runs on map threads: only reads the thread-safe registry and queues the character for the world thread.
    void OnPlayerJustDied(Player* player) override
    {
        if (!player || !TofuAutonomousAlt::IsEnabled(player->GetGUID().GetCounter()) || !IsPlayerbot(player))
            return;

        if (!GetChallengeState(player).hardcore)
            return;

        std::lock_guard lock(g_PendingDeathsMutex);
        g_PendingHardcoreDeaths.insert(player->GetGUID().GetCounter());
    }
};

class AutonomousAltBotsCommandScript : public CommandScript
{
public:
    AutonomousAltBotsCommandScript() : CommandScript("AutonomousAltBotsCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        // Parties: anyone may create one; members are added by whoever may turn the character on; the owner or a GM
        // disbands; the owner, the character's account, or a GM removes a member.
        static ChatCommandTable partyCommandTable =
        {
            { "create", HandlePartyCreate, SEC_PLAYER, Console::Yes },
            { "add", HandlePartyAdd, SEC_PLAYER, Console::Yes },
            { "remove", HandlePartyRemove, SEC_PLAYER, Console::Yes },
            { "disband", HandlePartyDisband, SEC_PLAYER, Console::Yes },
            { "list", HandlePartyList, SEC_PLAYER, Console::Yes },
            { "join", HandlePartyJoin, SEC_PLAYER, Console::No },
        };

        // Player level: ResolveCharacter limits players to their own account. Reload is GM only.
        static ChatCommandTable autonomousAltCommandTable =
        {
            { "start", HandleEnable, SEC_PLAYER, Console::Yes },
            { "stop", HandleDisable, SEC_PLAYER, Console::Yes },
            { "enable", HandleEnable, SEC_PLAYER, Console::Yes },
            { "disable", HandleDisable, SEC_PLAYER, Console::Yes },
            { "status", HandleStatus, SEC_PLAYER, Console::Yes },
            { "list", HandleList, SEC_PLAYER, Console::Yes },
            { "reload", HandleReload, SEC_GAMEMASTER, Console::Yes },
            { "party", partyCommandTable },
        };
        static ChatCommandTable commandTable =
        {
            { "autoalt", autonomousAltCommandTable },
            { "autonomousalt", autonomousAltCommandTable },
        };
        return commandTable;
    }

    static bool HandleEnable(ChatHandler* handler, std::string name)
    {
        if (!PlayerSettingsEnabled())
        {
            handler->SendSysMessage(
                "Autonomous altbots require EnablePlayerSettings = 1 in worldserver.conf, followed by a server restart.");
            return false;
        }

        ObjectGuid guid;
        if (!ResolveCharacter(handler, name, guid))
            return false;

        uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
        if (sPlayerbotAIConfig.IsInRandomAccountList(accountId) || sRandomPlayerbotMgr.IsAddclassBot(guid.GetCounter()))
        {
            handler->PSendSysMessage("{} belongs to a managed random-bot account and cannot be enrolled as an alt.", name);
            return false;
        }

        Player* connected = FindConnectedCharacter(guid);
        if (connected && !IsPlayerbot(connected))
        {
            handler->PSendSysMessage("{} is logged in as a player. Log that character out before turning it on.", name);
            return false;
        }

        ChallengeState const state = connected ? GetChallengeState(connected) : GetOfflineChallengeState(guid);
        if (char const* reason = ChallengeBlockReason(state))
        {
            handler->PSendSysMessage("{} {} and cannot play on its own.", name, reason);
            return false;
        }

        SetDesired(guid, true);
        handler->PSendSysMessage(
            "{} is on. It plays on its own whenever at least one player is in the world, and logs out about a minute "
            "after the last player leaves.",
            name);
        return true;
    }

    static bool HandleDisable(ChatHandler* handler, std::string name)
    {
        ObjectGuid guid;
        if (!ResolveCharacter(handler, name, guid))
            return false;

        SetDesired(guid, false);
        handler->PSendSysMessage("{} is off and logs out now if it was playing on its own.", name);
        return true;
    }

    static bool HandleStatus(ChatHandler* handler, std::string name)
    {
        ObjectGuid guid;
        if (!ResolveCharacter(handler, name, guid))
            return false;

        SendStatus(handler, guid);
        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        bool const all = IsConsoleOrGM(handler);
        uint32 const accountId = handler->GetSession() ? handler->GetSession()->GetAccountId() : 0;

        std::vector<ObjectGuid> shown;
        for (ObjectGuid::LowType lowGuid : g_DesiredBots)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
            if (all || sCharacterCache->GetCharacterAccountIdByGuid(guid) == accountId)
                shown.push_back(guid);
        }

        if (shown.empty())
        {
            handler->SendSysMessage(all ? "No autonomous alts are on." : "None of your characters are on.");
            return true;
        }

        handler->PSendSysMessage("Autonomous alts ({}):", shown.size());
        for (ObjectGuid const& guid : shown)
            SendStatus(handler, guid);
        return true;
    }

    static bool PartiesEnabled(ChatHandler* handler)
    {
        if (g_Config.partyEnable)
            return true;
        handler->SendSysMessage("Autonomous alt parties are off (AutonomousAltBots.Party.Enable = 0).");
        return false;
    }

    static bool ValidPartyName(std::string const& name)
    {
        if (name.empty() || name.size() > PARTY_NAME_MAX_LENGTH)
            return false;
        return std::all_of(name.begin(), name.end(),
                           [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
    }

    static Party* GetParty(ChatHandler* handler, std::string const& name)
    {
        Party* party = FindPartyByName(name);
        if (!party)
            handler->PSendSysMessage("There is no party named {}.", name);
        return party;
    }

    static bool IsPartyOwner(ChatHandler* handler, Party const& party)
    {
        return IsConsoleOrGM(handler) || handler->GetSession()->GetAccountId() == party.ownerAccount;
    }

    static ChallengeState StateOf(ObjectGuid guid)
    {
        if (Player* connected = FindConnectedCharacter(guid))
            return GetChallengeState(connected);
        return GetOfflineChallengeState(guid);
    }

    // A party in the Dungeon Finder, a dungeon, or at the hub keeps its members until it is back.
    static bool PartyAway(ChatHandler* handler, Party const& party)
    {
        auto const run = g_PartyRuns.find(party.id);
        if (run == g_PartyRuns.end() || run->second.phase == PartyPhase::Open)
            return false;

        handler->PSendSysMessage("Party {} is busy ({}); try again when it is back.", party.name, run->second.status);
        return true;
    }

    static bool HandlePartyCreate(ChatHandler* handler, std::string name)
    {
        if (!PartiesEnabled(handler))
            return false;

        if (!ValidPartyName(name))
        {
            handler->PSendSysMessage("A party name is 1 to {} letters, digits, '-' or '_'.", PARTY_NAME_MAX_LENGTH);
            return false;
        }

        if (FindPartyByName(name))
        {
            handler->PSendSysMessage("A party named {} already exists.", name);
            return false;
        }

        uint32 const id = g_Parties.empty() ? 1 : g_Parties.rbegin()->first + 1;
        uint32 const account = handler->GetSession() ? handler->GetSession()->GetAccountId() : 0;
        Party party;
        party.id = id;
        party.name = name;
        party.ownerAccount = account;
        g_Parties[id] = party;

        CharacterDatabase.Execute("INSERT INTO tofu_autoalt_party (id, name, owner_account) VALUES ({}, '{}', {})", id,
                                  name, account);
        PushPartiesToRegistry();
        handler->PSendSysMessage("Party {} created. Add members with .autoalt party add {} <name>.", name, name);
        return true;
    }

    static bool HandlePartyAdd(ChatHandler* handler, std::string partyName, std::string name)
    {
        if (!PartiesEnabled(handler))
            return false;

        Party* party = GetParty(handler, partyName);
        if (!party)
            return false;

        ObjectGuid guid;
        if (!ResolveCharacter(handler, name, guid))
            return false;

        ObjectGuid::LowType const lowGuid = guid.GetCounter();
        if (!g_DesiredBots.contains(lowGuid))
        {
            handler->PSendSysMessage("{} is not an autonomous alt. Turn it on first with .autoalt start {}.", name,
                                     name);
            return false;
        }

        if (Party const* current = FindPartyOf(lowGuid))
        {
            handler->PSendSysMessage("{} is already in party {}.", name, current->name);
            return false;
        }

        if (party->members.size() >= PARTY_MAX_MEMBERS)
        {
            handler->PSendSysMessage("Party {} is full ({} members).", party->name, PARTY_MAX_MEMBERS);
            return false;
        }

        // The challenge grouping rule: all members on a hard mode, or none.
        if (!party->members.empty())
        {
            bool const restricted = StateOf(guid).restricting;
            ObjectGuid const firstMember = ObjectGuid::Create<HighGuid::Player>(party->members.front());
            bool const partyRestricted = StateOf(firstMember).restricting;
            if (restricted != partyRestricted)
            {
                handler->PSendSysMessage(
                    "{} cannot join {}: a party is all challenge characters (any hard mode but Progression) or none.",
                    name, party->name);
                return false;
            }
        }

        party->members.push_back(lowGuid);
        g_PartyOfMember[lowGuid] = party->id;
        CharacterDatabase.Execute("REPLACE INTO tofu_autoalt_party_member (guid, party_id) VALUES ({}, {})", lowGuid,
                                  party->id);
        PushPartiesToRegistry();
        handler->PSendSysMessage("{} joined party {} ({} of {}).", name, party->name, party->members.size(),
                                 PARTY_MAX_MEMBERS);
        return true;
    }

    static bool HandlePartyRemove(ChatHandler* handler, std::string name)
    {
        if (!normalizePlayerName(name))
        {
            handler->PSendSysMessage("Invalid character name '{}'.", name);
            return false;
        }

        ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(name);
        Party* party = guid ? FindPartyOf(guid.GetCounter()) : nullptr;
        if (!party)
        {
            handler->PSendSysMessage("{} is not in a party.", name);
            return false;
        }

        bool const ownCharacter = handler->GetSession() &&
                                  sCharacterCache->GetCharacterAccountIdByGuid(guid) ==
                                      handler->GetSession()->GetAccountId();
        if (!ownCharacter && !IsPartyOwner(handler, *party))
        {
            handler->PSendSysMessage("Only the party's owner, {}'s account, or a GM can remove it.", name);
            return false;
        }

        if (PartyAway(handler, *party))
            return false;

        std::string const partyName = party->name;
        RemovePartyMember(guid.GetCounter());
        CharacterDatabase.Execute("DELETE FROM tofu_autoalt_party_member WHERE guid = {}", guid.GetCounter());
        handler->PSendSysMessage("{} left party {}.", name, partyName);
        return true;
    }

    static bool HandlePartyDisband(ChatHandler* handler, std::string partyName)
    {
        Party* party = GetParty(handler, partyName);
        if (!party)
            return false;

        if (!IsPartyOwner(handler, *party))
        {
            handler->SendSysMessage("Only the party's owner or a GM can disband it.");
            return false;
        }

        if (PartyAway(handler, *party))
            return false;

        uint32 const id = party->id;
        std::string const name = party->name;
        for (ObjectGuid::LowType guid : party->members)
            g_PartyOfMember.erase(guid);
        g_Parties.erase(id);
        g_PartyRuns.erase(id);

        CharacterDatabase.Execute("DELETE FROM tofu_autoalt_party_member WHERE party_id = {}", id);
        CharacterDatabase.Execute("DELETE FROM tofu_autoalt_party_dungeon WHERE party_id = {}", id);
        CharacterDatabase.Execute("DELETE FROM tofu_autoalt_party WHERE id = {}", id);
        PushPartiesToRegistry();
        handler->PSendSysMessage("Party {} disbanded.", name);
        return true;
    }

    static bool HandlePartyList(ChatHandler* handler, Optional<std::string> partyName)
    {
        if (partyName)
        {
            Party* party = GetParty(handler, *partyName);
            if (!party)
                return false;

            auto const run = g_PartyRuns.find(party->id);
            handler->PSendSysMessage("Party {} ({} of {}): {}; {} dungeon(s) cleared.", party->name,
                                     party->members.size(), PARTY_MAX_MEMBERS,
                                     run != g_PartyRuns.end() ? run->second.status : "questing",
                                     party->clearedDungeons.size());
            for (ObjectGuid::LowType guid : party->members)
                SendStatus(handler, ObjectGuid::Create<HighGuid::Player>(guid));
            return true;
        }

        if (g_Parties.empty())
        {
            handler->SendSysMessage("There are no autonomous alt parties.");
            return true;
        }

        for (auto const& [id, party] : g_Parties)
        {
            std::string names;
            for (ObjectGuid::LowType guid : party.members)
            {
                std::string memberName;
                sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(guid), memberName);
                names += (names.empty() ? "" : ", ") + memberName;
            }
            handler->PSendSysMessage("{}: {}", party.name, names.empty() ? "(no members)" : names);
        }
        return true;
    }

    static bool HandlePartyJoin(ChatHandler* handler, std::string partyName)
    {
        if (!PartiesEnabled(handler))
            return false;

        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (player->GetGroup())
        {
            handler->SendSysMessage("Leave your group first.");
            return false;
        }

        Party* party = GetParty(handler, partyName);
        if (!party)
            return false;

        if (PartyAway(handler, *party))
            return false;

        std::vector<Player*> members;
        for (ObjectGuid::LowType guid : party->members)
        {
            Player* member = FindConnectedCharacter(ObjectGuid::Create<HighGuid::Player>(guid));
            if (member && member->IsInWorld() && IsPlayerbot(member) && !GroupedWithRealPlayer(member))
                members.push_back(member);
        }

        if (members.empty())
        {
            handler->PSendSysMessage("No member of {} is playing right now.", party->name);
            return false;
        }

        for (Player* member : members)
        {
            if (!TofuAutonomousAlt::CanControl(member, player))
            {
                handler->PSendSysMessage("You may not lead {}.", member->GetName());
                return false;
            }
        }

        if (GetChallengeState(player).restricting != GetChallengeState(members.front()).restricting)
        {
            handler->SendSysMessage("Your character and this party are on opposite sides of the challenge rule.");
            return false;
        }

        Group* group = nullptr;
        for (Player* member : members)
            if (member->GetGroup() && GroupHoldsOnlyParty(member->GetGroup(), party->id))
            {
                group = member->GetGroup();
                break;
            }

        if (!group)
        {
            Player* first = members.front();
            if (first->GetGroup())
                first->RemoveFromGroup();
            group = new Group();
            if (!group->Create(first))
            {
                delete group;
                return false;
            }
            sGroupMgr->AddGroup(group);
        }

        for (Player* member : members)
        {
            if (member->GetGroup() == group)
                continue;
            if (member->GetGroup())
                member->RemoveFromGroup();
            if (!group->IsFull())
                group->AddMember(member);
        }

        if (group->IsFull() || !group->AddMember(player))
        {
            handler->PSendSysMessage("Party {} has no room for you.", party->name);
            return false;
        }

        group->ChangeLeader(player->GetGUID());
        handler->PSendSysMessage("You lead {} now. Leave the group to let it play on its own again.", party->name);
        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadDesiredBots();
        LoadParties();
        handler->PSendSysMessage("Reloaded {} autonomous alt selection(s) and {} part(ies).", g_DesiredBots.size(),
                                 g_Parties.size());
        return true;
    }
};
} // namespace

void Addmod_autonomous_altbotsScripts()
{
    new AutonomousAltBotsWorldScript();
    new AutonomousAltBotsServerScript();
    new AutonomousAltBotsPlayerScript();
    new AutonomousAltBotsCommandScript();
}
