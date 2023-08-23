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

#include "OnlineReward.h"
#include "Common.h"
#include "Config.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "ExternalMail.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ReputationMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"

namespace
{
    constexpr auto OR_LOCALE_SUBJECT            = 1;
    constexpr auto OR_LOCALE_TEXT               = 2;
    constexpr auto OR_LOCALE_MESSAGE_MAIL       = 3;
    constexpr auto OR_LOCALE_MESSAGE_IN_GAME    = 4;
    constexpr auto OR_LOCALE_NOT_ENOUGH_BAG     = 5;
    constexpr auto OR_LOCALE_NEXT               = 6;

    constexpr std::string_view GetLocaleText(uint32 textId, LocaleConstant localeConstant)
    {
        if (localeConstant != LOCALE_enUS && localeConstant != LOCALE_ruRU)
            localeConstant = LOCALE_enUS;

        switch (textId)
        {
            case OR_LOCALE_SUBJECT:
                return localeConstant == LOCALE_enUS ? "Reward for online {}" : "Награда за онлайн {}";
            case OR_LOCALE_TEXT:
                return localeConstant == LOCALE_enUS ? "Hi, {}!\nYou been playing on our server for over {}. Please accept a gift from us." : "Привет, {}!\nВы играете на нашем сервере уже более {}. Пожалуйста примите подарок";
            case OR_LOCALE_MESSAGE_MAIL:
                return localeConstant == LOCALE_enUS ? "You were rewarded for online ({}). You can get the award at the post office." : "Вы были вознаграждены за онлайн ({}). Получить награду можно на почте.";
            case OR_LOCALE_MESSAGE_IN_GAME:
                return localeConstant == LOCALE_enUS ? "You were rewarded for online ({})." : "Вы были вознаграждены за онлайн ({}).";
            case OR_LOCALE_NOT_ENOUGH_BAG:
                return localeConstant == LOCALE_enUS ? "Not enough room in the bag. Send via mail" : "У вас недостаточно места в сумке, награда будет ждать вас на почте";
            case OR_LOCALE_NEXT:
                return localeConstant == LOCALE_enUS ? "{}. Count: {}. Left: {}" : "{}. Кол-во: {}. Осталось: {}";
            default:
                return "";
        }
    }

    // Default send message
    void SendLocalizePlayerMessage(Player* player, std::string_view fmt, std::string_view message = {})
    {
        for (std::string_view line : Acore::Tokenize(message.empty() ? fmt : Acore::StringFormatFmt(fmt, message), '\n', true))
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL, nullptr, nullptr, line);
            player->SendDirectMessage(&data);
        }
    }

    std::string GetItemNameLocale(uint32 itemID, int8 index_loc /*= DEFAULT_LOCALE*/)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemID);
        ItemLocale const* itemLocale = sObjectMgr->GetItemLocale(itemID);
        std::string name;

        if (itemLocale)
            name = itemLocale->Name[index_loc];

        if (name.empty() && itemTemplate)
            name = itemTemplate->Name1;

        return name;
    }

    std::string GetItemLink(uint32 itemID, int8 index_loc /*= DEFAULT_LOCALE*/)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemID);
        if (!itemTemplate)
            return "";

        std::string name = GetItemNameLocale(itemID, index_loc);
        uint32 color = ItemQualityColors[itemTemplate->Quality];

        return Acore::StringFormatFmt("|c{:08x}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r", color, itemID, name);
    }
}

OnlineRewardMgr* OnlineRewardMgr::instance()
{
    static OnlineRewardMgr instance;
    return &instance;
}

void OnlineRewardMgr::LoadConfig(bool reload)
{
    if (reload)
        scheduler.CancelAll();

    _isEnable = sConfigMgr->GetOption<bool>("OR.Enable", false);
    if (!_isEnable)
        return;

    _isPerOnlineEnable = sConfigMgr->GetOption<bool>("OR.PerOnline.Enable", false);
    _isPerTimeEnable = sConfigMgr->GetOption<bool>("OR.PerTime.Enable", false);
    _isForceMailReward = sConfigMgr->GetOption<bool>("OR.ForceSendMail.Enable", false);
    _maxSameIpCount = sConfigMgr->GetOption<uint32>("OR.MaxSameIpCount", 3);

    if (!_isPerOnlineEnable && !_isPerTimeEnable)
    {
        _isEnable = false;
        LOG_ERROR("module.or", "> In module Online Reward disabled all function. Disable module.");
        //return;
    }

    if (reload)
        ScheduleReward();
}

void OnlineRewardMgr::InitSystem()
{
    if (!_isEnable)
        return;

    LoadDBData();

    // If data empty no need reward players
    if (!_isEnable)
        return;

    ScheduleReward();
}

void OnlineRewardMgr::ScheduleReward()
{
    if (!_isEnable)
        return;

    scheduler.Schedule(30s, [this](TaskContext context)
    {
        RewardPlayers();
        context.Repeat(1min);
    });
}

void OnlineRewardMgr::Update(Milliseconds diff)
{
    if (!_isEnable)
        return;

    scheduler.Update(diff);
    _queryProcessor.ProcessReadyCallbacks();
}

void OnlineRewardMgr::RewardNow()
{
    scheduler.CancelAll();
    RewardPlayers();
    ScheduleReward();
}

void OnlineRewardMgr::LoadDBData()
{
    LOG_INFO("module.or", "Loading online rewards...");

    if (!_rewards.empty())
        _rewards.clear();

    QueryResult result = CharacterDatabase.Query("SELECT `ID`, `IsPerOnline`, `Seconds`, `MinLevel`, `Items`, `Reputations` FROM `wh_online_rewards`");
    if (!result)
    {
        LOG_WARN("module.or", "> DB table `wh_online_rewards` is empty! Disable module");
        LOG_WARN("module.or", "");
        _isEnable = false;
        return;
    }

    for (auto const& row : *result)
    {
        auto id             = row[0].Get<uint32>();
        auto isPerOnline    = row[1].Get<bool>();
        auto seconds        = row[2].Get<int32>();
        auto minLevel       = row[3].Get<uint8>();
        auto items          = row[4].Get<std::string_view>();
        auto reputations    = row[5].Get<std::string_view>();

        AddReward(id, isPerOnline, Seconds(seconds), minLevel, items, reputations);
    }

    if (_rewards.empty())
    {
        LOG_INFO("module.or", ">> Loaded 0 online rewards");
        LOG_WARN("module.or", ">> Disable module");
        LOG_INFO("module.or", "");
        _isEnable = false;
        return;
    }

    LOG_INFO("module.or", ">> Loaded {} online rewards", _rewards.size());
    LOG_INFO("module.or", "");
}

bool OnlineRewardMgr::AddReward(uint32 id, bool isPerOnline, Seconds seconds, uint8 minLevel, std::string_view items, std::string_view reputations, ChatHandler* handler /*= nullptr*/)
{
    auto SendErrorMessage = [handler](std::string_view message)
    {
        LOG_ERROR("module.or", message);

        if (handler)
            handler->SendSysMessage(message);
    };

    if (IsExistReward(id))
    {
        SendErrorMessage(Acore::StringFormatFmt("> OnlineRewardMgr::AddReward: Reward with id {} is exist!", id));
        return false;
    }

    // Start checks
    if (seconds == 0s)
    {
        SendErrorMessage("> OnlineRewardMgr::AddReward: Seconds = 0? Really? Skip...");
        return false;
    }

    if (minLevel == 0 || minLevel > 80)
    {
        SendErrorMessage(Acore::StringFormatFmt("> OnlineRewardMgr::AddReward: Incorrect level: {}", minLevel));
        return false;
    }

    OnlineReward data(id, isPerOnline, seconds, minLevel);
    auto const& itemData = Acore::Tokenize(items, ',', false);
    auto const& reputationsData = Acore::Tokenize(reputations, ',', false);

    if (itemData.empty() && reputationsData.empty())
    {
        SendErrorMessage(Acore::StringFormatFmt("> OnlineRewardMgr::AddReward: Not found rewards. IsPerOnline?: {}. Seconds: {}", isPerOnline, seconds.count()));
        return false;
    }

    if (!itemData.empty())
    {
        // Items
        for (auto const& pairItems : Acore::Tokenize(items, ',', false))
        {
            auto itemTokens = Acore::Tokenize(pairItems, ':', false);
            if (itemTokens.size() != 2)
            {
                SendErrorMessage(Acore::StringFormatFmt("> OnlineRewardMgr::LoadDBData: Error at extract `itemTokens` from '{}'", pairItems));
                continue;
            }

            auto itemID = Acore::StringTo<uint32>(itemTokens.at(0));
            auto itemCount = Acore::StringTo<uint32>(itemTokens.at(1));

            if (!itemID || !itemCount)
            {
                SendErrorMessage(Acore::StringFormat("> OnlineRewardMgr::LoadDBData: Error at extract `itemID` or `itemCount` from '{}'", pairItems));
                continue;
            }

            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(*itemID);
            if (!itemTemplate)
            {
                SendErrorMessage(Acore::StringFormat("> OnlineRewardMgr::LoadDBData: Item with number {} not found. Skip", *itemID));
                continue;
            }

            if (*itemID && !*itemCount)
            {
                SendErrorMessage(Acore::StringFormat("> OnlineRewardMgr::LoadDBData: For item with number {} item count set 0. Skip", *itemID));
                continue;
            }

            data.Items.emplace_back(*itemID, *itemCount);
        }
    }

    if (!reputationsData.empty())
    {
        // Reputations
        for (auto const& pairReputations : Acore::Tokenize(reputations, ',', false))
        {
            auto reputationsTokens = Acore::Tokenize(pairReputations, ':', false);
            if (reputationsTokens.size() != 2)
            {
                SendErrorMessage(Acore::StringFormatFmt("> OnlineRewardMgr::LoadDBData: Error at extract `reputationsTokens` from '{}'", pairReputations));
                continue;
            }

            auto factionID = Acore::StringTo<uint32>(reputationsTokens.at(0));
            auto reputationCount = Acore::StringTo<uint32>(reputationsTokens.at(1));

            if (!factionID || !reputationCount)
            {
                SendErrorMessage(Acore::StringFormatFmt("> OnlineRewardMgr::LoadDBData: Error at extract `factionID` or `reputationCount` from '{}'", pairReputations));
                continue;
            }

            FactionEntry const* factionEntry = sFactionStore.LookupEntry(*factionID);
            if (!factionEntry)
            {
                SendErrorMessage(Acore::StringFormatFmt("> OnlineReward: Not found faction with id {}. Skip", *factionID));
                continue;
            }

            if (factionEntry->reputationListID < 0)
            {
                SendErrorMessage(Acore::StringFormatFmt("> OnlineReward: Faction {} can't have reputation. Skip", *factionID));
                continue;
            }

            if (*reputationCount > static_cast<uint32>(ReputationMgr::Reputation_Cap))
            {
                SendErrorMessage(Acore::StringFormatFmt("> OnlineReward: reputation count {} > repitation cap {}. Skip", *reputationCount, ReputationMgr::Reputation_Cap));
                continue;
            }

            data.Reputations.emplace_back(*factionID, *reputationCount);
        }
    }

    if (data.Items.empty() && data.Reputations.empty())
    {
        SendErrorMessage(Acore::StringFormatFmt("> OnlineRewardMgr::AddReward: Not found rewards after check items and reputations. IsPerOnline?: {}. Seconds: {}", isPerOnline, seconds.count()));
        return false;
    }

    _rewards.emplace(id, data);
    _lastId = id;

    // If add from command - save to db
    if (handler)
    {
        CharacterDatabase.Execute("INSERT INTO `wh_online_rewards` (`ID`, `IsPerOnline`, `Seconds`, `Items`, `Reputations`) VALUES ({}, {:d}, {}, '{}', '{}')",
            id, isPerOnline, seconds.count(), items, reputations);
    }

    if (!_isEnable)
    {
        _isEnable = true;
        ScheduleReward();
    }

    return true;
}

void OnlineRewardMgr::AddRewardHistory(ObjectGuid::LowType lowGuid)
{
    if (!_isEnable)
        return;

    if (IsExistHistory(lowGuid))
        return;

    _queryProcessor.AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormatFmt("SELECT `RewardID`, `RewardedSeconds` FROM `wh_online_rewards_history` WHERE `PlayerGuid` = {}", lowGuid)).
    WithCallback([this, lowGuid](QueryResult result)
    {
        AddRewardHistoryAsync(lowGuid, std::move(result));
    }));
}

void OnlineRewardMgr::DeleteRewardHistory(ObjectGuid::LowType lowGuid)
{
    if (!_isEnable)
        return;

    _rewardHistory.erase(lowGuid);
}

void OnlineRewardMgr::RewardPlayers()
{
    if (!_isEnable)
        return;

    // Empty world, no need reward
    if (!sWorld->GetPlayerCount())
        return;

    ASSERT(_rewardPending.empty());

    LOG_DEBUG("module.or", "> OR: Start rewards players...");

    auto const& sessions = sWorld->GetAllSessions();
    if (sessions.empty())
        return;

    MakeIpCache();

    for (auto const& [accountID, session] : sessions)
    {
        auto player = session->GetPlayer();
        if (!player || !player->IsInWorld())
            continue;

        auto const lowGuid = player->GetGUID().GetCounter();
        Seconds playedTimeSec{ player->GetTotalPlayedTime() };

        for (auto const& [rewardID, reward] : _rewards)
        {
            if (reward.IsPerOnline && !_isPerOnlineEnable)
                continue;

            if (!reward.IsPerOnline && !_isPerTimeEnable)
                continue;

            CheckPlayerForReward(player, playedTimeSec, &reward);
        }
    }

    _ipCache.clear();

    // Send reward
    SendRewards();

    // Save data to DB
    SaveRewardHistoryToDB();

    LOG_DEBUG("module.or", "> OR: End rewards players");
}

void OnlineRewardMgr::SaveRewardHistoryToDB()
{
    if (_rewardHistory.empty())
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // Save data for exist history
    for (auto const& [lowGuid, history] : _rewardHistory)
    {
        // Delete old data
        trans->Append("DELETE FROM `wh_online_rewards_history` WHERE `PlayerGuid` = {}", lowGuid);

        for (auto const& [rewardID, seconds] : history)
        {
             // Insert new data
            trans->Append("INSERT INTO `wh_online_rewards_history` (`PlayerGuid`, `RewardID`, `RewardedSeconds`) VALUES ({}, '{}', {})", lowGuid, rewardID, seconds.count());
        }
    }

    CharacterDatabase.CommitTransaction(trans);
}

OnlineRewardMgr::RewardHistory* OnlineRewardMgr::GetHistory(ObjectGuid::LowType lowGuid)
{
    if (_rewardHistory.empty())
        return nullptr;

    auto const& itr = _rewardHistory.find(lowGuid);
    if (itr == _rewardHistory.end())
        return nullptr;

    return &itr->second;
}

Seconds OnlineRewardMgr::GetHistorySecondsForReward(ObjectGuid::LowType lowGuid, uint32 id)
{
    if (_rewardHistory.empty())
        return 0s;

    auto const& itr = _rewardHistory.find(lowGuid);
    if (itr == _rewardHistory.end())
        return 0s;

    for (auto const& [rewardID, seconds] : itr->second)
        if (rewardID == id)
            return seconds;

    return 0s;
}

void OnlineRewardMgr::SendRewardForPlayer(Player* player, uint32 rewardID)
{
    auto onlineReward = GetOnlineReward(rewardID);
    if (!onlineReward)
        return;

    ChatHandler handler{ player->GetSession() };
    std::string playedTimeSecStr{ Acore::Time::ToTimeString(onlineReward->RewardTime, TimeOutput::Seconds, TimeFormat::FullText) };
    auto localeIndex{ player->GetSession()->GetSessionDbLocaleIndex() };

    auto SendItemsViaMail = [player, onlineReward, playedTimeSecStr, &localeIndex]()
    {
        auto const mailSubject = Acore::StringFormatFmt(GetLocaleText(OR_LOCALE_SUBJECT, localeIndex), playedTimeSecStr);
        auto const MailText = Acore::StringFormatFmt(GetLocaleText(OR_LOCALE_TEXT, localeIndex), player->GetName(), playedTimeSecStr);

        // Send External mail
        for (auto const& [itemID, itemCount] : onlineReward->Items)
            sExternalMail->AddMail(player->GetName(), mailSubject, MailText, itemID, itemCount, 37688);
    };

    if (!onlineReward->Reputations.empty())
    {
        for (auto const& [faction, reputation] : onlineReward->Reputations)
        {
            ReputationMgr& repMgr = player->GetReputationMgr();
            auto const& factionEntry = sFactionStore.LookupEntry(faction);
            if (factionEntry)
            {
                repMgr.SetOneFactionReputation(factionEntry, static_cast<float>(reputation), true);
                repMgr.SendState(repMgr.GetState(factionEntry));
            }
        }
    }

    if (_isForceMailReward && !onlineReward->Items.empty())
    {
        SendItemsViaMail();

        // Send chat text
        SendLocalizePlayerMessage(player, GetLocaleText(OR_LOCALE_MESSAGE_MAIL, localeIndex), playedTimeSecStr);
        return;
    }

    if (!onlineReward->Items.empty())
    {
        for (auto const& [itemID, itemCount] : onlineReward->Items)
        {
            if (!player->AddItem(itemID, itemCount))
            {
                SendItemsViaMail();

                // Send chat text
                SendLocalizePlayerMessage(player, GetLocaleText(OR_LOCALE_NOT_ENOUGH_BAG, localeIndex));
            }
        }
    }

    // Send chat text
    SendLocalizePlayerMessage(player, GetLocaleText(OR_LOCALE_MESSAGE_IN_GAME, localeIndex), playedTimeSecStr);
}

void OnlineRewardMgr::AddHistory(ObjectGuid::LowType lowGuid, uint32 rewardId, Seconds playerOnlineTime)
{
    auto history = GetHistory(lowGuid);
    if (!history)
    {
        _rewardHistory.emplace(lowGuid, RewardHistory{ { rewardId, playerOnlineTime } });
        return;
    }

    for (auto& [rewardID, seconds] : *history)
    {
        if (rewardID == rewardId)
        {
            seconds = playerOnlineTime;
            return;
        }
    }

    history->emplace_back(rewardId, playerOnlineTime);
}

bool OnlineRewardMgr::IsExistHistory(ObjectGuid::LowType lowGuid)
{
    return _rewardHistory.contains(lowGuid);
}

void OnlineRewardMgr::AddRewardHistoryAsync(ObjectGuid::LowType lowGuid, QueryResult result)
{
    if (!result)
        return;

    std::lock_guard<std::mutex> guard(_playerLoadingLock);

    if (_rewardHistory.contains(lowGuid))
    {
        LOG_FATAL("module.or", "> OR: Time to ping @Winfidonarleyan. Code 2");
        _rewardHistory.erase(lowGuid);
    }

    RewardHistory rewardHistory;

    for (auto const& row : *result)
        rewardHistory.emplace_back(row[0].Get<uint32>(), row[1].Get<Seconds>());

    _rewardHistory.emplace(lowGuid, rewardHistory);
    LOG_DEBUG("module.or", "> OR: Added history for player with guid {}", lowGuid);
}

void OnlineRewardMgr::CheckPlayerForReward(Player* player, Seconds playedTime, OnlineReward const* onlineReward)
{
    if (!onlineReward || !player || playedTime == 0s)
        return;

    if (onlineReward->MinLevel > player->GetLevel())
        return;

    auto lowGuid{ player->GetGUID().GetCounter() };

    auto AddToStore = [this, onlineReward, player](ObjectGuid::LowType playerGuid)
    {
        if (!IsNormalIpPlayer(player))
            return;

        auto const& itr = _rewardPending.find(playerGuid);
        if (itr == _rewardPending.end())
        {
            _rewardPending.emplace(playerGuid, RewardPending{ onlineReward->ID });
            return;
        }

        itr->second.emplace_back(onlineReward->ID);
    };

    auto rewardedSeconds = GetHistorySecondsForReward(lowGuid, onlineReward->ID);

    if (onlineReward->IsPerOnline)
    {
        if (playedTime >= onlineReward->RewardTime && rewardedSeconds == 0s)
            AddToStore(lowGuid);
    }
    else
    {
        for (Seconds diffTime{ onlineReward->RewardTime }; diffTime < playedTime; diffTime += onlineReward->RewardTime)
            if (rewardedSeconds < diffTime)
                AddToStore(lowGuid);
    }

    AddHistory(lowGuid, onlineReward->ID, playedTime);
}

void OnlineRewardMgr::GetNextTimeForReward(Player* player, Seconds playedTime, OnlineReward const* onlineReward)
{
    if (!onlineReward || !player || playedTime == 0s)
        return;

    auto lowGuid = player->GetGUID().GetCounter();
    auto localeIndex = player->GetSession()->GetSessionDbLocaleIndex();
    ChatHandler handler(player->GetSession());

    auto PrintReward = [onlineReward, localeIndex, &handler](Seconds seconds)
    {
        for (auto const& [itemId, count] : onlineReward->Items)
        {
            auto item = GetItemLink(itemId, localeIndex);
            auto left = seconds == 0s ? "<at next reward tick>" : Acore::Time::ToTimeString(seconds);
            auto message = Acore::StringFormatFmt(GetLocaleText(OR_LOCALE_NEXT, localeIndex), item, count, left);
            handler.PSendSysMessage(message.c_str());
        }
    };

    auto rewardedSeconds = GetHistorySecondsForReward(lowGuid, onlineReward->ID);

    if (onlineReward->IsPerOnline && _isPerOnlineEnable)
    {
        if (rewardedSeconds != 0s)
            return;

        PrintReward(onlineReward->RewardTime - playedTime);
    }
    else if (!onlineReward->IsPerOnline && _isPerTimeEnable)
    {
        auto next = onlineReward->RewardTime * (rewardedSeconds / onlineReward->RewardTime + 1);

        for (auto diffTime{ next }; next < playedTime; diffTime + onlineReward->RewardTime)
        {
            PrintReward(0s);
            next += onlineReward->RewardTime;
        }

        PrintReward(next - playedTime);
    }
}

void OnlineRewardMgr::SendRewards()
{
    if (_rewardPending.empty())
        return;

    for (auto const& [lowGuid, rewards] : _rewardPending)
    {
        auto player = ObjectAccessor::FindPlayerByLowGUID(lowGuid);
        if (!player)
        {
            LOG_FATAL("module.or", "> OR::RewardPlayers: Try reward non existing player (maybe offline) with guid {}. Skip reward, try next time", lowGuid);
            DeleteRewardHistory(lowGuid);
            continue;
        }

        for (auto const& rewardID : rewards)
            SendRewardForPlayer(player, rewardID);
    }

    _rewardPending.clear();
}

bool OnlineRewardMgr::IsExistReward(uint32 id)
{
    return _rewards.contains(id);
}

bool OnlineRewardMgr::DeleteReward(uint32 id)
{
    auto const& erased = _rewards.erase(id);
    if (!erased)
        return false;

    CharacterDatabase.Execute("DELETE FROM `wh_online_rewards` WHERE `ID` = {}", id);
    return true;
}

OnlineReward const* OnlineRewardMgr::GetOnlineReward(uint32 id)
{
    return Acore::Containers::MapGetValuePtr(_rewards, id);
}

void OnlineRewardMgr::MakeIpCache()
{
    if (!_ipCache.empty())
        _ipCache.clear();

    auto const& sessions = sWorld->GetAllSessions();
    if (sessions.empty())
        return;

    for (auto const& [accId, session] : sessions)
    {
        auto player{ session->GetPlayer() };
        if (!player || !player->IsInWorld())
            continue;

        auto ip{ session->GetRemoteAddress() };

        auto itr = _ipCache.find(ip);
        if (itr == _ipCache.end())
        {
            _ipCache.emplace(ip, std::vector<Player*>{ player });
            continue;
        }

        itr->second.emplace_back(player);
    }

    for (auto& [ip, players] : _ipCache)
    {
        if (players.size() == 1)
            continue;

        std::sort(players.begin(), players.end(), [](Player* player1, Player* player2)
        {
            return player1->GetTotalPlayedTime() > player2->GetTotalPlayedTime();
        });

        players.resize(_maxSameIpCount);
    }
}

bool OnlineRewardMgr::IsNormalIpPlayer(Player* player)
{
    auto ip{ player->GetSession()->GetRemoteAddress() };

    auto itr = _ipCache.find(ip);
    if (itr == _ipCache.end())
        return false;

    auto& players{ itr->second };
    auto exist = std::find(players.begin(), players.end(), player);
    return exist != players.end();
}