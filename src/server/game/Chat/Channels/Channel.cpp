/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Channel.h"
#include "AccountMgr.h"
#include "ChannelAppenders.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "ChatPackets.h"
#include "DB2Stores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SocialMgr.h"
#include "StringConvert.h"
#include "World.h"
#include "WorldSession.h"
#include <sstream>

Channel::Channel(ObjectGuid const& guid, uint32 channelId, uint32 team /*= 0*/, AreaTableEntry const* zoneEntry /*= nullptr*/) :
    _isDirty(false),
    _nextActivityUpdateTime(0),
    _announceEnabled(false),                                               // no join/leave announces
    _ownershipEnabled(false),                                              // no ownership handout
    _isOwnerInvisible(false),
    _channelFlags(CHANNEL_FLAG_GENERAL),                                   // for all built-in channels
    _channelId(channelId),
    _channelTeam(team),
    _channelGuid(guid),
    _zoneEntry(zoneEntry)
{
    ChatChannelsEntry const* channelEntry = sChatChannelsStore.AssertEntry(channelId);
    if (channelEntry->GetFlags().HasFlag(ChatChannelFlags::AllowItemLinks))     // for trade channel
        _channelFlags |= CHANNEL_FLAG_TRADE;

    if (channelEntry->GetFlags().HasFlag(ChatChannelFlags::LinkedChannel))      // for city only channels
        _channelFlags |= CHANNEL_FLAG_CITY;

    if (channelEntry->GetFlags().HasFlag(ChatChannelFlags::LookingForGroup))    // for LFG channel
        _channelFlags |= CHANNEL_FLAG_LFG;
    else                                                                        // for all other channels
        _channelFlags |= CHANNEL_FLAG_NOT_LFG;
}

Channel::Channel(ObjectGuid const& guid, std::string const& name, uint32 team /*= 0*/, std::string const& banList) :
    _isDirty(false),
    _nextActivityUpdateTime(0),
    _announceEnabled(true),
    _ownershipEnabled(true),
    _isOwnerInvisible(false),
    _channelFlags(CHANNEL_FLAG_CUSTOM),
    _channelId(0),
    _channelTeam(team),
    _channelGuid(guid),
    _channelName(name),
    _zoneEntry(nullptr)
{
    for (std::string_view guid : Trinity::Tokenize(banList, ' ', false))
    {
        // legacy db content might not have 0x prefix, account for that
        if (guid.size() > 2 && guid.substr(0, 2) == "0x")
            guid.remove_suffix(2);

        Optional<uint64> high = Trinity::StringTo<uint64>(guid.substr(0, 16), 16);
        Optional<uint64> low = Trinity::StringTo<uint64>(guid.substr(16, 16), 16);
        if (!high || !low)
            continue;

        ObjectGuid banned;
        banned.SetRawValue(*high, *low);
        if (!banned)
            continue;

        TC_LOG_DEBUG("chat.system", "Channel({}) loaded player {} into bannedStore", name, banned.ToString());
        _bannedStore.insert(banned);
    }
}

Channel::~Channel() = default;

void Channel::GetChannelName(std::string& channelName, uint32 channelId, LocaleConstant locale, AreaTableEntry const* zoneEntry)
{
    if (channelId)
    {
        ChatChannelsEntry const* channelEntry = sChatChannelsStore.AssertEntry(channelId);
        if (channelEntry->GetFlags().HasFlag(ChatChannelFlags::ZoneBased))
        {
            if (channelEntry->GetFlags().HasFlag(ChatChannelFlags::LinkedChannel))
                zoneEntry = ChannelMgr::SpecialLinkedArea;

            channelName = ChatHandler::PGetParseString(channelEntry->Name[locale], ASSERT_NOTNULL(zoneEntry)->AreaName[locale]);
        }
        else
            channelName = channelEntry->Name[locale];
    }
}

std::string Channel::GetName(LocaleConstant locale /*= DEFAULT_LOCALE*/) const
{
    std::string result = _channelName;
    Channel::GetChannelName(result, _channelId, locale, _zoneEntry);

    return result;
}

void Channel::UpdateChannelInDB()
{
    time_t const now = GameTime::GetGameTime();
    if (_isDirty)
    {
        std::ostringstream banlist;
        for (ObjectGuid const& guid : _bannedStore)
            banlist << guid.ToHexString() << ' ';

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHANNEL);
        stmt->setString(0, _channelName);
        stmt->setUInt32(1, _channelTeam);
        stmt->setBool(2, _announceEnabled);
        stmt->setBool(3, _ownershipEnabled);
        stmt->setString(4, _channelPassword);
        stmt->setString(5, banlist.str());
        CharacterDatabase.Execute(stmt);
    }
    else if (_nextActivityUpdateTime <= now)
    {
        if (!_playersStore.empty())
        {
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHANNEL_USAGE);
            stmt->setString(0, _channelName);
            stmt->setUInt32(1, _channelTeam);
            CharacterDatabase.Execute(stmt);
        }
    }
    else
        return;

    _isDirty = false;
    _nextActivityUpdateTime = now + urand(1 * MINUTE, 6 * MINUTE) * std::max(1u, sWorld->getIntConfig(CONFIG_PRESERVE_CUSTOM_CHANNEL_INTERVAL));
}

void Channel::JoinChannel(Player* player, std::string const& pass)
{
    ObjectGuid const& guid = player->GetGUID();
    if (IsOn(guid))
    {
        // Do not send error message for built-in channels
        if (!IsConstant())
        {
            PlayerAlreadyMemberAppend appender(guid);
            ChannelNameBuilder<PlayerAlreadyMemberAppend> builder(this, appender);
            SendToOne(builder, guid);
        }
        return;
    }

    if (IsBanned(guid))
    {
        BannedAppend appender;
        ChannelNameBuilder<BannedAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (!CheckPassword(pass))
    {
        WrongPasswordAppend appender;
        ChannelNameBuilder<WrongPasswordAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (HasFlag(CHANNEL_FLAG_LFG) &&
        sWorld->getBoolConfig(CONFIG_RESTRICTED_LFG_CHANNEL) &&
        AccountMgr::IsPlayerAccount(player->GetSession()->GetSecurity()) && //FIXME: Move to RBAC
        player->GetGroup())
    {
        NotInLFGAppend appender;
        ChannelNameBuilder<NotInLFGAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    player->JoinedChannel(this);

    if (_announceEnabled && !player->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL))
    {
        JoinedAppend appender(guid);
        ChannelNameBuilder<JoinedAppend> builder(this, appender);
        SendToAll(builder);
    }

    bool newChannel = _playersStore.empty();
    if (newChannel)
        _nextActivityUpdateTime = 0; // force activity update on next channel tick

    PlayerInfo& playerInfo = _playersStore[guid];
    playerInfo.SetInvisible(!player->isGMVisible());

    /*
    YouJoinedAppend appender;
    ChannelNameBuilder<YouJoinedAppend> builder(this, appender);
    SendToOne(builder, guid);
    */

    auto builder = [&](LocaleConstant locale)
    {
        LocaleConstant localeIdx = sWorld->GetAvailableDbcLocale(locale);

        Trinity::PacketSenderOwning<WorldPackets::Channel::ChannelNotifyJoined>* notify = new Trinity::PacketSenderOwning<WorldPackets::Channel::ChannelNotifyJoined>();
        //notify->Data.ChannelWelcomeMsg = "";
        notify->Data.ChatChannelID = _channelId;
        //notify->Data.InstanceID = 0;
        notify->Data._ChannelFlags = _channelFlags;
        notify->Data._Channel = GetName(localeIdx);
        notify->Data.ChannelGUID = _channelGuid;
        notify->Data.Write();
        return notify;
    };

    SendToOne(builder, guid);

    JoinNotify(player);

    // Custom channel handling
    if (!IsConstant())
    {
        // If the channel has no owner yet and ownership is allowed, set the new owner.
        // or if the owner was a GM with .gm visible off
        // don't do this if the new player is, too, an invis GM, unless the channel was empty
        if (_ownershipEnabled && (newChannel || !playerInfo.IsInvisible()) && (_ownerGuid.IsEmpty() || _isOwnerInvisible))
        {
            _isOwnerInvisible = playerInfo.IsInvisible();

            SetOwner(guid, !newChannel && !_isOwnerInvisible);
            playerInfo.SetModerator(true);
        }
    }
}

void Channel::LeaveChannel(Player* player, bool send, bool suspend)
{
    ObjectGuid const& guid = player->GetGUID();
    if (!IsOn(guid))
    {
        if (send)
        {
            NotMemberAppend appender;
            ChannelNameBuilder<NotMemberAppend> builder(this, appender);
            SendToOne(builder, guid);
        }
        return;
    }

    player->LeftChannel(this);

    if (send)
    {
        /*
        YouLeftAppend appender;
        ChannelNameBuilder<YouLeftAppend> builder(this, appender);
        SendToOne(builder, guid);
        */

        auto builder = [&](LocaleConstant locale)
        {
            LocaleConstant localeIdx = sWorld->GetAvailableDbcLocale(locale);

            Trinity::PacketSenderOwning<WorldPackets::Channel::ChannelNotifyLeft>* notify = new Trinity::PacketSenderOwning<WorldPackets::Channel::ChannelNotifyLeft>();
            notify->Data.Channel = GetName(localeIdx);
            notify->Data.ChatChannelID = _channelId;
            notify->Data.Suspended = suspend;
            notify->Data.Write();
            return notify;
        };

        SendToOne(builder, guid);
    }

    PlayerInfo& info = _playersStore.at(guid);
    bool changeowner = info.IsOwner();
    _playersStore.erase(guid);

    if (_announceEnabled && !player->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL))
    {
        LeftAppend appender(guid);
        ChannelNameBuilder<LeftAppend> builder(this, appender);
        SendToAll(builder);
    }

    LeaveNotify(player);

    if (!IsConstant())
    {
        // If the channel owner left and there are still playersStore inside, pick a new owner
        // do not pick invisible gm owner unless there are only invisible gms in that channel (rare)
        if (changeowner && _ownershipEnabled && !_playersStore.empty())
        {
            PlayerContainer::iterator itr;
            for (itr = _playersStore.begin(); itr != _playersStore.end(); ++itr)
            {
                if (!itr->second.IsInvisible())
                    break;
            }

            if (itr == _playersStore.end())
                itr = _playersStore.begin();

            ObjectGuid const& newowner = itr->first;
            itr->second.SetModerator(true);

            SetOwner(newowner);

            // if the new owner is invisible gm, set flag to automatically choose a new owner
            if (itr->second.IsInvisible())
                _isOwnerInvisible = true;
        }
    }
}

void Channel::KickOrBan(Player const* player, std::string const& badname, bool ban)
{
    ObjectGuid const& good = player->GetGUID();

    if (!IsOn(good))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, good);
        return;
    }

    PlayerInfo& info = _playersStore.at(good);
    if (!info.IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        NotModeratorAppend appender;
        ChannelNameBuilder<NotModeratorAppend> builder(this, appender);
        SendToOne(builder, good);
        return;
    }

    Player* bad = ObjectAccessor::FindConnectedPlayerByName(badname);
    ObjectGuid const& victim = bad ? bad->GetGUID() : ObjectGuid::Empty;
    if (!bad || !victim || !IsOn(victim))
    {
        PlayerNotFoundAppend appender(badname);
        ChannelNameBuilder<PlayerNotFoundAppend> builder(this, appender);
        SendToOne(builder, good);
        return;
    }

    bool changeowner = _ownerGuid == victim;

    if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR) && changeowner && good != _ownerGuid)
    {
        NotOwnerAppend appender;
        ChannelNameBuilder<NotOwnerAppend> builder(this, appender);
        SendToOne(builder, good);
        return;
    }

    if (ban && !IsBanned(victim))
    {
        _bannedStore.insert(victim);
        _isDirty = true;

        if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL))
        {
            PlayerBannedAppend appender(good, victim);
            ChannelNameBuilder<PlayerBannedAppend> builder(this, appender);
            SendToAll(builder);
        }
    }
    else if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL))
    {
        PlayerKickedAppend appender(good, victim);
        ChannelNameBuilder<PlayerKickedAppend> builder(this, appender);
        SendToAll(builder);
    }

    _playersStore.erase(victim);
    bad->LeftChannel(this);

    if (changeowner && _ownershipEnabled && !_playersStore.empty())
    {
        info.SetModerator(true);
        SetOwner(good);
    }
}

void Channel::UnBan(Player const* player, std::string const& badname)
{
    ObjectGuid const& good = player->GetGUID();

    if (!IsOn(good))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, good);
        return;
    }

    PlayerInfo& info = _playersStore.at(good);
    if (!info.IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        NotModeratorAppend appender;
        ChannelNameBuilder<NotModeratorAppend> builder(this, appender);
        SendToOne(builder, good);
        return;
    }

    Player* bad = ObjectAccessor::FindConnectedPlayerByName(badname);
    ObjectGuid victim = bad ? bad->GetGUID() : ObjectGuid::Empty;

    if (victim.IsEmpty() || !IsBanned(victim))
    {
        PlayerNotFoundAppend appender(badname);
        ChannelNameBuilder<PlayerNotFoundAppend> builder(this, appender);
        SendToOne(builder, good);
        return;
    }

    _bannedStore.erase(victim);

    PlayerUnbannedAppend appender(good, victim);
    ChannelNameBuilder<PlayerUnbannedAppend> builder(this, appender);
    SendToAll(builder);

    _isDirty = true;
}

void Channel::Password(Player const* player, std::string const& pass)
{
    ObjectGuid const& guid = player->GetGUID();

    if (!IsOn(guid))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    PlayerInfo& info = _playersStore.at(guid);
    if (!info.IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        NotModeratorAppend appender;
        ChannelNameBuilder<NotModeratorAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    _channelPassword = pass;

    PasswordChangedAppend appender(guid);
    ChannelNameBuilder<PasswordChangedAppend> builder(this, appender);
    SendToAll(builder);

    _isDirty = true;
}

void Channel::SetMode(Player const* player, std::string const& p2n, bool mod, bool set)
{
    ObjectGuid const& guid = player->GetGUID();

    if (!IsOn(guid))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    PlayerInfo& info = _playersStore.at(guid);
    if (!info.IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        NotModeratorAppend appender;
        ChannelNameBuilder<NotModeratorAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (guid == _ownerGuid && p2n == player->GetName() && mod)
        return;

    Player* newp = ObjectAccessor::FindConnectedPlayerByName(p2n);
    ObjectGuid victim = newp ? newp->GetGUID() : ObjectGuid::Empty;

    if (!newp || victim.IsEmpty() || !IsOn(victim) ||
        (player->GetTeam() != newp->GetTeam() &&
        (!player->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL) ||
        !newp->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL))))
    {
        PlayerNotFoundAppend appender(p2n);
        ChannelNameBuilder<PlayerNotFoundAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (_ownerGuid == victim && _ownerGuid != guid)
    {
        NotOwnerAppend appender;
        ChannelNameBuilder<NotOwnerAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (mod)
        SetModerator(newp->GetGUID(), set);
    else
        SetMute(newp->GetGUID(), set);
}

void Channel::SetInvisible(Player const* player, bool on)
{
    auto itr = _playersStore.find(player->GetGUID());
    if (itr == _playersStore.end())
        return;

    itr->second.SetInvisible(on);

    // we happen to be owner too, update flag
    if (_ownerGuid == player->GetGUID())
        _isOwnerInvisible = on;
}

void Channel::SetOwner(Player const* player, std::string const& newname)
{
    ObjectGuid const& guid = player->GetGUID();

    if (!IsOn(guid))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR) && guid != _ownerGuid)
    {
        NotOwnerAppend appender;
        ChannelNameBuilder<NotOwnerAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    Player* newp = ObjectAccessor::FindConnectedPlayerByName(newname);
    ObjectGuid victim = newp ? newp->GetGUID() : ObjectGuid::Empty;

    if (!newp || !victim || !IsOn(victim) ||
        (player->GetTeam() != newp->GetTeam() &&
        (!player->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL) ||
        !newp->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL))))
    {
        PlayerNotFoundAppend appender(newname);
        ChannelNameBuilder<PlayerNotFoundAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    PlayerInfo& info = _playersStore.at(victim);
    info.SetModerator(true);
    SetOwner(victim);
}

void Channel::SendWhoOwner(Player const* player)
{
    ObjectGuid const& guid = player->GetGUID();
    if (IsOn(guid))
    {
        ChannelOwnerAppend appender(this, _ownerGuid);
        ChannelNameBuilder<ChannelOwnerAppend> builder(this, appender);
        SendToOne(builder, guid);
    }
    else
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
    }
}

void Channel::List(Player const* player)
{
    ObjectGuid const& guid = player->GetGUID();
    if (!IsOn(guid))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    std::string channelName = GetName(player->GetSession()->GetSessionDbcLocale());
    TC_LOG_DEBUG("chat.system", "SMSG_CHANNEL_LIST {} Channel: {}",
        player->GetSession()->GetPlayerInfo(), channelName);

    WorldPackets::Channel::ChannelListResponse list;
    list._Display = true; /// always true?
    list._Channel = channelName;
    list._ChannelFlags = GetFlags();

    uint32 gmLevelInWhoList = sWorld->getIntConfig(CONFIG_GM_LEVEL_IN_WHO_LIST);

    list._Members.reserve(_playersStore.size());
    for (PlayerContainer::value_type const& i : _playersStore)
    {
        Player* member = ObjectAccessor::FindConnectedPlayer(i.first);

        // PLAYER can't see MODERATOR, GAME MASTER, ADMINISTRATOR characters
        // MODERATOR, GAME MASTER, ADMINISTRATOR can see all
        if (member &&
            (player->GetSession()->HasPermission(rbac::RBAC_PERM_WHO_SEE_ALL_SEC_LEVELS) ||
             member->GetSession()->GetSecurity() <= AccountTypes(gmLevelInWhoList)) &&
            member->IsVisibleGloballyFor(player))
        {
            list._Members.emplace_back(i.first, *member->m_playerData->VirtualPlayerRealm, i.second.GetFlags());
        }
    }

    player->SendDirectMessage(list.Write());
}

void Channel::Announce(Player const* player)
{
    ObjectGuid const& guid = player->GetGUID();

    if (!IsOn(guid))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    PlayerInfo const& playerInfo = _playersStore.at(guid);
    if (!playerInfo.IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        NotModeratorAppend appender;
        ChannelNameBuilder<NotModeratorAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    _announceEnabled = !_announceEnabled;

    WorldPackets::Channel::ChannelNotify notify;
    if (_announceEnabled)
    {
        AnnouncementsOnAppend appender(guid);
        ChannelNameBuilder<AnnouncementsOnAppend> builder(this, appender);
        SendToAll(builder);
    }
    else
    {
        AnnouncementsOffAppend appender(guid);
        ChannelNameBuilder<AnnouncementsOffAppend> builder(this, appender);
        SendToAll(builder);
    }

    _isDirty = true;
}

void Channel::Say(ObjectGuid const& guid, std::string const& what, uint32 lang) const
{
    if (what.empty())
        return;

    // TODO: Add proper RBAC check
    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL))
        lang = LANG_UNIVERSAL;

    if (!IsOn(guid))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    PlayerInfo const& playerInfo = _playersStore.at(guid);
    if (playerInfo.IsMuted())
    {
        MutedAppend appender;
        ChannelNameBuilder<MutedAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    Player* player = ObjectAccessor::FindConnectedPlayer(guid);

    auto builder = [&](LocaleConstant locale)
    {
        LocaleConstant localeIdx = sWorld->GetAvailableDbcLocale(locale);

        Trinity::PacketSenderOwning<WorldPackets::Chat::Chat>* packet = new Trinity::PacketSenderOwning<WorldPackets::Chat::Chat>();
        packet->Data.ChannelGUID = _channelGuid;
        if (player)
            packet->Data.Initialize(CHAT_MSG_CHANNEL, Language(lang), player, player, what, 0, GetName(localeIdx));
        else
        {
            packet->Data.Initialize(CHAT_MSG_CHANNEL, Language(lang), nullptr, nullptr, what, 0, GetName(localeIdx));
            packet->Data.SenderGUID = guid;
            packet->Data.TargetGUID = guid;
        }

        packet->Data.Write();

        return packet;
    };

    SendToAll(builder, !playerInfo.IsModerator() ? guid : ObjectGuid::Empty,
        !playerInfo.IsModerator() && player ? player->GetSession()->GetAccountGUID() : ObjectGuid::Empty);
}

void Channel::AddonSay(ObjectGuid const& guid, std::string const& prefix, std::string const& what, bool isLogged) const
{
    if (what.empty())
        return;

    if (!IsOn(guid))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    PlayerInfo const& playerInfo = _playersStore.at(guid);
    if (playerInfo.IsMuted())
    {
        MutedAppend appender;
        ChannelNameBuilder<MutedAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    Player* player = ObjectAccessor::FindConnectedPlayer(guid);

    auto builder = [&](LocaleConstant locale)
    {
        LocaleConstant localeIdx = sWorld->GetAvailableDbcLocale(locale);

        Trinity::PacketSenderOwning<WorldPackets::Chat::Chat>* packet = new Trinity::PacketSenderOwning<WorldPackets::Chat::Chat>();
        packet->Data.ChannelGUID = _channelGuid;
        if (player)
            packet->Data.Initialize(CHAT_MSG_CHANNEL, isLogged ? LANG_ADDON_LOGGED : LANG_ADDON, player, player, what, 0, GetName(localeIdx), DEFAULT_LOCALE, prefix);
        else
        {
            packet->Data.Initialize(CHAT_MSG_CHANNEL, isLogged ? LANG_ADDON_LOGGED : LANG_ADDON, nullptr, nullptr, what, 0, GetName(localeIdx), DEFAULT_LOCALE, prefix);
            packet->Data.SenderGUID = guid;
            packet->Data.TargetGUID = guid;
        }

        packet->Data.Write();

        return packet;
    };

    SendToAllWithAddon(builder, prefix, !playerInfo.IsModerator() ? guid : ObjectGuid::Empty,
        !playerInfo.IsModerator() && player ? player->GetSession()->GetAccountGUID() : ObjectGuid::Empty);
}

void Channel::Invite(Player const* player, std::string const& newname)
{
    ObjectGuid const& guid = player->GetGUID();

    if (!IsOn(guid))
    {
        NotMemberAppend appender;
        ChannelNameBuilder<NotMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    Player* newp = ObjectAccessor::FindConnectedPlayerByName(newname);
    if (!newp || !newp->isGMVisible())
    {
        PlayerNotFoundAppend appender(newname);
        ChannelNameBuilder<PlayerNotFoundAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (IsBanned(newp->GetGUID()))
    {
        PlayerInviteBannedAppend appender(newname);
        ChannelNameBuilder<PlayerInviteBannedAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (newp->GetTeam() != player->GetTeam() &&
        (!player->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL) ||
        !newp->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL)))
    {
        InviteWrongFactionAppend appender;
        ChannelNameBuilder<InviteWrongFactionAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (IsOn(newp->GetGUID()))
    {
        PlayerAlreadyMemberAppend appender(newp->GetGUID());
        ChannelNameBuilder<PlayerAlreadyMemberAppend> builder(this, appender);
        SendToOne(builder, guid);
        return;
    }

    if (!newp->GetSocial()->HasIgnore(guid, player->GetSession()->GetAccountGUID()))
    {
        InviteAppend appender(guid);
        ChannelNameBuilder<InviteAppend> builder(this, appender);
        SendToOne(builder, newp->GetGUID());
    }

    PlayerInvitedAppend appender(newp->GetName());
    ChannelNameBuilder<PlayerInvitedAppend> builder(this, appender);
    SendToOne(builder, guid);
}

void Channel::SetOwner(ObjectGuid const& guid, bool exclaim)
{
    if (!_ownerGuid.IsEmpty())
    {
        // [] will re-add player after it possible removed
        auto itr = _playersStore.find(_ownerGuid);
        if (itr != _playersStore.end())
            itr->second.SetOwner(false);
    }

    _ownerGuid = guid;
    if (!_ownerGuid.IsEmpty())
    {
        uint8 oldFlag = GetPlayerFlags(_ownerGuid);
        auto itr = _playersStore.find(_ownerGuid);
        if (itr == _playersStore.end())
            return;

        itr->second.SetModerator(true);
        itr->second.SetOwner(true);

        ModeChangeAppend appender(_ownerGuid, oldFlag, GetPlayerFlags(_ownerGuid));
        ChannelNameBuilder<ModeChangeAppend> builder(this, appender);
        SendToAll(builder);

        if (exclaim)
        {
            OwnerChangedAppend ownerAppender(_ownerGuid);
            ChannelNameBuilder<OwnerChangedAppend> ownerBuilder(this, ownerAppender);
            SendToAll(ownerBuilder);
        }

        _isDirty = true;
    }
}

void Channel::SilenceAll(Player const* /*player*/, std::string const& /*name*/)
{
}

void Channel::UnsilenceAll(Player const* /*player*/, std::string const& /*name*/)
{
}

void Channel::DeclineInvite(Player const* /*player*/)
{
}

void Channel::JoinNotify(Player const* player)
{
    ObjectGuid const& guid = player->GetGUID();

    if (IsConstant())
    {
        auto builder = [&](LocaleConstant locale)
        {
            LocaleConstant localeIdx = sWorld->GetAvailableDbcLocale(locale);

            Trinity::PacketSenderOwning<WorldPackets::Channel::UserlistAdd>* userlistAdd = new Trinity::PacketSenderOwning<WorldPackets::Channel::UserlistAdd>();
            userlistAdd->Data.AddedUserGUID = guid;
            userlistAdd->Data._ChannelFlags = GetFlags();
            userlistAdd->Data.UserFlags = GetPlayerFlags(guid);
            userlistAdd->Data.ChannelID = GetChannelId();
            userlistAdd->Data.ChannelName = GetName(localeIdx);
            userlistAdd->Data.Write();
            return userlistAdd;
        };

        SendToAllButOne(builder, guid);
    }
    else
    {
        auto builder = [&](LocaleConstant locale)
        {
            LocaleConstant localeIdx = sWorld->GetAvailableDbcLocale(locale);

            Trinity::PacketSenderOwning<WorldPackets::Channel::UserlistUpdate>* userlistUpdate = new Trinity::PacketSenderOwning<WorldPackets::Channel::UserlistUpdate>();
            userlistUpdate->Data.UpdatedUserGUID = guid;
            userlistUpdate->Data._ChannelFlags = GetFlags();
            userlistUpdate->Data.UserFlags = GetPlayerFlags(guid);
            userlistUpdate->Data.ChannelID = GetChannelId();
            userlistUpdate->Data.ChannelName = GetName(localeIdx);
            userlistUpdate->Data.Write();
            return userlistUpdate;
        };

        SendToAll(builder);
    }
}

void Channel::LeaveNotify(Player const* player)
{
    ObjectGuid const& guid = player->GetGUID();

    auto builder = [&](LocaleConstant locale)
    {
        LocaleConstant localeIdx = sWorld->GetAvailableDbcLocale(locale);

        Trinity::PacketSenderOwning<WorldPackets::Channel::UserlistRemove>* userlistRemove = new Trinity::PacketSenderOwning<WorldPackets::Channel::UserlistRemove>();
        userlistRemove->Data.RemovedUserGUID = guid;
        userlistRemove->Data._ChannelFlags = GetFlags();
        userlistRemove->Data.ChannelID = GetChannelId();
        userlistRemove->Data.ChannelName = GetName(localeIdx);
        userlistRemove->Data.Write();
        return userlistRemove;
    };

    if (IsConstant())
        SendToAllButOne(builder, guid);
    else
        SendToAll(builder);
}

void Channel::SetModerator(ObjectGuid const& guid, bool set)
{
    if (!IsOn(guid))
        return;

    PlayerInfo& playerInfo = _playersStore.at(guid);
    if (playerInfo.IsModerator() != set)
    {
        uint8 oldFlag = playerInfo.GetFlags();
        playerInfo.SetModerator(set);

        ModeChangeAppend appender(guid, oldFlag, playerInfo.GetFlags());
        ChannelNameBuilder<ModeChangeAppend> builder(this, appender);
        SendToAll(builder);
    }
}

void Channel::SetMute(ObjectGuid const& guid, bool set)
{
    if (!IsOn(guid))
        return;

    PlayerInfo& playerInfo = _playersStore.at(guid);
    if (playerInfo.IsMuted() != set)
    {
        uint8 oldFlag = playerInfo.GetFlags();
        playerInfo.SetMuted(set);

        ModeChangeAppend appender(guid, oldFlag, playerInfo.GetFlags());
        ChannelNameBuilder<ModeChangeAppend> builder(this, appender);
        SendToAll(builder);
    }
}

template <class Builder>
void Channel::SendToAll(Builder& builder, ObjectGuid const& guid, ObjectGuid const& accountGuid) const
{
    Trinity::LocalizedDo<Builder> localizer(builder);

    for (PlayerContainer::value_type const& i : _playersStore)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(i.first))
            if (guid.IsEmpty() || !player->GetSocial()->HasIgnore(guid, accountGuid))
                localizer(player);
}

template <class Builder>
void Channel::SendToAllButOne(Builder& builder, ObjectGuid const& who) const
{
    Trinity::LocalizedDo<Builder> localizer(builder);

    for (PlayerContainer::value_type const& i : _playersStore)
        if (i.first != who)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(i.first))
                localizer(player);
}

template <class Builder>
void Channel::SendToOne(Builder& builder, ObjectGuid const& who) const
{
    Trinity::LocalizedDo<Builder> localizer(builder);

    if (Player* player = ObjectAccessor::FindConnectedPlayer(who))
        localizer(player);
}

template <class Builder>
void Channel::SendToAllWithAddon(Builder& builder, std::string const& addonPrefix, ObjectGuid const& guid /*= ObjectGuid::Empty*/,
    ObjectGuid const& accountGuid /*= ObjectGuid::Empty*/) const
{
    Trinity::LocalizedDo<Builder> localizer(builder);

    for (PlayerContainer::value_type const& i : _playersStore)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(i.first))
            if (player->GetSession()->IsAddonRegistered(addonPrefix) && (guid.IsEmpty() || !player->GetSocial()->HasIgnore(guid, accountGuid)))
                localizer(player);
}
