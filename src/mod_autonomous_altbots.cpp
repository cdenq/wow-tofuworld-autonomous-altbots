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
 */

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "ObjectAccessor.h"
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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr char SETTING_SOURCE[] = "mod_autonomous_altbots";
constexpr char MODULE_VERSION[] = "1.0.1-tofu.1";
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
};

AutonomousAltConfig g_Config;
std::unordered_set<ObjectGuid::LowType> g_DesiredBots;
std::unordered_map<ObjectGuid::LowType, uint32> g_StopWatchElapsed;
std::unordered_set<ObjectGuid::LowType> g_CaughtUpThisLogin;  // got the login-time spell catch-up
uint32 g_StartupElapsed = 0;
uint32 g_CheckElapsed = 0;
uint32 g_NoPlayerElapsed = 0;
bool g_StartupComplete = false;

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

    TofuAutonomousAlt::Settings settings;
    settings.teleportForLevel = sConfigMgr->GetOption<bool>("AutonomousAltBots.LevelUp.TeleportForLevel", true);
    settings.learnTrainerSpells = sConfigMgr->GetOption<bool>("AutonomousAltBots.LevelUp.LearnTrainerSpells", true);
    settings.autoTalents = sConfigMgr->GetOption<bool>("AutonomousAltBots.LevelUp.AutoTalents", true);
    settings.restock = sConfigMgr->GetOption<bool>("AutonomousAltBots.Restock.Enable", true);
    settings.ammoThreshold = sConfigMgr->GetOption<uint32>("AutonomousAltBots.Restock.AmmoThreshold", 200);
    settings.vendorSearchDistance =
        sConfigMgr->GetOption<float>("AutonomousAltBots.Restock.VendorSearchDistance", 3000.0f);
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

void LogoutAutonomousBot(ObjectGuid guid)
{
    if (sRandomPlayerbotMgr.GetPlayerBot(guid))
        sRandomPlayerbotMgr.LogoutPlayerBot(guid);
}

void ApplyAutonomousStrategies(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    // Grouped with a real player: that player is in charge (follow, commands, trade). Hands off.
    if (GroupedWithRealPlayer(bot))
        return;

    // Left over in a group with no real player online (for example, the leader logged out): leave it.
    if (bot->GetGroup())
        bot->RemoveFromGroup();

    if (botAI->GetMaster())
        botAI->SetMaster(nullptr);

    if (g_Config.quest && !botAI->HasStrategy("quest", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+quest", BOT_STATE_NON_COMBAT);

    if (g_Config.grind && !botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+grind", BOT_STATE_NON_COMBAT);

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
        g_StartupElapsed = 0;
        g_CheckElapsed = 0;
        g_NoPlayerElapsed = 0;
        g_StartupComplete = g_Config.startupDelayMs == 0;
    }

    void OnUpdate(uint32 diff) override
    {
        ProcessStopWatch(diff);

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

        for (ObjectGuid::LowType lowGuid : g_DesiredBots)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
            Player* connected = FindConnectedCharacter(guid);

            if (!connected)
            {
                g_CaughtUpThisLogin.erase(lowGuid);
                TofuAutonomousAlt::ClearBotState(lowGuid);

                // Never log in while no real player is in the world.
                if (playerOnline)
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

            ApplyAutonomousStrategies(connected);
        }
    }

private:
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

class AutonomousAltBotsCommandScript : public CommandScript
{
public:
    AutonomousAltBotsCommandScript() : CommandScript("AutonomousAltBotsCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
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

        if (Player* connected = FindConnectedCharacter(guid); connected && !IsPlayerbot(connected))
        {
            handler->PSendSysMessage("{} is logged in as a player. Log that character out before turning it on.", name);
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

    static bool HandleReload(ChatHandler* handler)
    {
        LoadDesiredBots();
        handler->PSendSysMessage("Reloaded {} autonomous alt selection(s).", g_DesiredBots.size());
        return true;
    }
};
} // namespace

void Addmod_autonomous_altbotsScripts()
{
    new AutonomousAltBotsWorldScript();
    new AutonomousAltBotsCommandScript();
}
