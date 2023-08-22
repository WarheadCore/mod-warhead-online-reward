/*
 * This file is part of the WarheadCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Chat.h"
#include "ExternalMail.h"
#include "OnlineReward.h"
#include "Player.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;

class OnlineReward_CS : public CommandScript
{
public:
    OnlineReward_CS() : CommandScript("OnlineReward_CS") { }

    [[nodiscard]] ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable vipCommandTable =
        {
            { "add",        HandleOnlineRewardAddCommand,       SEC_ADMINISTRATOR,  Console::Yes },
            { "delete",     HandleOnlineRewardDeleteCommand,    SEC_ADMINISTRATOR,  Console::Yes },
            { "list",       HandleOnlineRewardListCommand,      SEC_ADMINISTRATOR,  Console::Yes },
            { "next",       HandleOnlineRewardNextCommand,      SEC_PLAYER,         Console::No },
            { "reload",     HandleOnlineRewardReloadCommand,    SEC_ADMINISTRATOR,  Console::Yes },
            { "init",       HandleOnlineRewardInitCommand,      SEC_ADMINISTRATOR,  Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "or", vipCommandTable }
        };

        return commandTable;
    }

    static bool HandleOnlineRewardAddCommand(ChatHandler* handler, bool isPerOnline, uint32 secs, uint8 level, std::string_view items, std::optional<std::string_view> reputations)
    {
        if (!secs)
        {
            handler->PSendSysMessage("> Нужно указать количество секунд");
            return true;
        }

        Seconds seconds{ secs };
        auto timeString = Acore::Time::ToTimeString(seconds);
        timeString.append(Acore::StringFormatFmt(" ({})", seconds.count()));

        if (sORMgr->AddReward(sORMgr->GetLastId() + 1, isPerOnline, seconds, level, items, reputations.value_or(""), handler))
            handler->PSendSysMessage("> Награда добавлена");

        return true;
    }

    static bool HandleOnlineRewardDeleteCommand(ChatHandler* handler, uint32 id)
    {
        handler->PSendSysMessage(Acore::StringFormatFmt("> Награда {}была удалена", sORMgr->DeleteReward(id) ? "" : "не ").c_str());
        return true;
    }

    static bool HandleOnlineRewardListCommand(ChatHandler* handler)
    {
        handler->PSendSysMessage("> Список наград за онлайн:");

        std::size_t count{ 0 };

        for (auto const& [id, onlineReward] : sORMgr->GetOnlineRewards())
        {
            handler->PSendSysMessage(Acore::StringFormatFmt("{}. {}. IsPerOnline? {}", ++count, Acore::Time::ToTimeString(onlineReward.RewardTime), onlineReward.IsPerOnline).c_str());

            if (!onlineReward.Items.empty())
            {
                handler->SendSysMessage("-- Предметы:");

                for (auto const& [itemID, itemCount] : onlineReward.Items)
                    handler->PSendSysMessage(Acore::StringFormatFmt("> {}/{}", itemID, itemCount).c_str());
            }

            if (!onlineReward.Reputations.empty())
            {
                handler->SendSysMessage("-- Репутация:");

                for (auto const& [faction, reputation] : onlineReward.Reputations)
                    handler->PSendSysMessage(Acore::StringFormatFmt("> {}/{}", faction, reputation).c_str());
            }

            handler->SendSysMessage("--");
        }

        return true;
    }

    static bool HandleOnlineRewardNextCommand(ChatHandler* handler)
    {
        auto player = handler->GetPlayer();
        if (!player)
            return false;

        Seconds playedTimeSec{ player->GetTotalPlayedTime() };

        for (auto const& [id, onlineReward] : sORMgr->GetOnlineRewards())
            sORMgr->GetNextTimeForReward(player, playedTimeSec, &onlineReward);

        return true;
    }

    static bool HandleOnlineRewardReloadCommand(ChatHandler* handler)
    {
        sORMgr->LoadDBData();
        handler->PSendSysMessage("> Награды перезагружены");
        return true;
    }

    static bool HandleOnlineRewardInitCommand(ChatHandler* handler)
    {
        sORMgr->RewardNow();
        handler->PSendSysMessage("> Инициализирована выдача наград за онлайн");
        return true;
    }
};

class OnlineReward_Player : public PlayerScript
{
public:
    OnlineReward_Player() : PlayerScript("OnlineReward_Player") { }

    void OnLogin(Player* player) override
    {
        sORMgr->AddRewardHistory(player->GetGUID().GetCounter());
    }

    void OnLogout(Player* player) override
    {
        sORMgr->DeleteRewardHistory(player->GetGUID().GetCounter());
    }
};

class OnlineReward_World : public WorldScript
{
public:
    OnlineReward_World() : WorldScript("OnlineReward_World") { }

    void OnAfterConfigLoad(bool reload) override
    {
        sORMgr->LoadConfig(reload);
    }

    void OnStartup() override
    {
        sORMgr->InitSystem();
        sExternalMail->LoadSystem();
    }

    void OnUpdate(uint32 diff) override
    {
        sORMgr->Update(Milliseconds(diff));
        sExternalMail->Update(diff);
    }
};

// Group all custom scripts
void AddSC_OnlineReward()
{
    new OnlineReward_CS();
    new OnlineReward_Player();
    new OnlineReward_World();
}