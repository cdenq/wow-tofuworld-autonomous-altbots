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
 */

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GroupReference.h"
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

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr char SETTING_SOURCE[] = "mod_autonomous_altbots";
constexpr char MODULE_VERSION[] = "1.0.1-tofu.4";
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
};

constexpr uint32 PARTY_MAX_MEMBERS = 5;
constexpr size_t PARTY_NAME_MAX_LENGTH = 32;

struct Party
{
    uint32 id = 0;
    std::string name;
    uint32 ownerAccount = 0;
    std::vector<ObjectGuid::LowType> members;
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
    TofuAutonomousAlt::SetSettings(settings);
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
    for (int32 index : g_Config.restrictingIndexes)
        state.restricting = state.restricting || (active && isOn(index));
    return state;
}

ChallengeState GetChallengeState(Player* player)
{
    if (!player || g_Config.challengeSource.empty())
        return {};

    int32 highest = std::max({g_Config.hardcoreIndex, g_Config.hardcoreDeadIndex, g_Config.questOnlyIndex});
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
    bool const questOnly = challenge.questOnly;

    // Grouped with a real player: that player is in charge (follow, commands, trade). Hands off.
    if (GroupedWithRealPlayer(bot))
        return;

    ObjectGuid::LowType const self = bot->GetGUID().GetCounter();
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
}

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
        g_StartupElapsed = 0;
        g_CheckElapsed = 0;
        g_NoPlayerElapsed = 0;
        g_StartupComplete = g_Config.startupDelayMs == 0;
    }

    void OnUpdate(uint32 diff) override
    {
        ProcessStopWatch(diff);
        ProcessHardcoreDeaths();

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

        for (ObjectGuid::LowType lowGuid : g_DesiredBots)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
            Player* connected = FindConnectedCharacter(guid);

            if (!connected)
            {
                g_CaughtUpThisLogin.erase(lowGuid);
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
                       { PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_CAN_GROUP_INVITE, PLAYERHOOK_ON_DELETE_FROM_DB })
    {
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

        Group* group = invitee->GetGroup();
        if (!group || group == player->GetGroup() || GroupedWithRealPlayer(invitee))
            return true;

        if (!TofuAutonomousAlt::CanControl(invitee, player))
            return true;  // the bot's own invite handling declines

        invitee->RemoveFromGroup();
        return true;
    }

    // Character GUIDs are reused after a delete, so the party row goes with the character.
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM tofu_autoalt_party_member WHERE guid = {}", guid);
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

        uint32 const id = party->id;
        std::string const name = party->name;
        for (ObjectGuid::LowType guid : party->members)
            g_PartyOfMember.erase(guid);
        g_Parties.erase(id);

        CharacterDatabase.Execute("DELETE FROM tofu_autoalt_party_member WHERE party_id = {}", id);
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

            handler->PSendSysMessage("Party {} ({} of {}):", party->name, party->members.size(), PARTY_MAX_MEMBERS);
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
    new AutonomousAltBotsPlayerScript();
    new AutonomousAltBotsCommandScript();
}
