//
// Arena replay module using AzerothCore-friendly hooks.
//

#include "ArenaReplay_loader.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "Chat.h"
#include "CharacterDatabase.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    struct PacketRecord
    {
        uint32 timestamp = 0;
        WorldPacket packet;
    };

    struct MatchRecord
    {
        BattlegroundTypeId typeId = BattlegroundTypeId::BATTLEGROUND_AA;
        uint8 arenaTypeId = 0;
        uint32 mapId = 0;
        std::deque<PacketRecord> packets;
    };

    constexpr uint8 kArenaType2v2 = 2;
    constexpr uint8 kArenaType3v3 = 3;
    constexpr uint8 kArenaType5v5 = 5;

    uint8 ResolveArenaTypeId(Battleground const* bg)
    {
        if (!bg)
            return 0;

        uint8 arenaType = bg->GetArenaType();

        if (arenaType)
            return arenaType;

        switch (bg->GetMaxPlayersPerTeam())
        {
            case kArenaType2v2:
                return kArenaType2v2;
            case kArenaType3v3:
                return kArenaType3v3;
            case kArenaType5v5:
                return kArenaType5v5;
            default:
                break;
        }

        return arenaType;
    }

    class ReplayRecorder
    {
    public:
        explicit ReplayRecorder(Battleground* bg)
        {
            if (!bg)
                return;

            _match.typeId = bg->GetBgTypeID();
            _match.arenaTypeId = ResolveArenaTypeId(bg);
            _match.mapId = bg->GetMapId();
        }

        void Record(uint32 timestamp, WorldPacket const& packet)
        {
            PacketRecord record;
            record.timestamp = timestamp;
            record.packet = WorldPacket(packet);
            _match.packets.push_back(std::move(record));
        }

        MatchRecord Release()
        {
            return std::move(_match);
        }

    private:
        MatchRecord _match;
    };

    std::unordered_map<uint32, std::unique_ptr<ReplayRecorder>> s_activeRecorders;
    std::unordered_map<uint32, MatchRecord> s_activeReplays;

    constexpr std::array<Opcodes, 6> kTrackedOpcodes =
    {
        SMSG_MONSTER_MOVE,
        SMSG_MOVE_KNOCK_BACK,
        SMSG_SPELL_START,
        SMSG_SPELL_GO,
        SMSG_PLAY_SPELL_VISUAL,
        SMSG_AURA_UPDATE
    };

    bool IsTrackedOpcode(Opcodes opcode)
    {
        return std::find(kTrackedOpcodes.begin(), kTrackedOpcodes.end(), opcode) != kTrackedOpcodes.end();
    }

    bool IsDesignatedRecorder(Battleground const* bg, Player const* player)
    {
        if (!bg || !player)
            return false;

        for (auto const& itr : bg->GetPlayers())
        {
            Player const* member = itr.second;
            if (!member)
                continue;

            if (member->GetBgTeamId() != player->GetBgTeamId())
                continue;

            if (member->GetGUID() == player->GetGUID())
                return true;
        }

        return false;
    }

    void NotifyPlayersReplaySaved(Battleground* bg, uint32 replayId)
    {
        if (!bg)
            return;

        for (auto const& pair : bg->GetPlayers())
        {
            if (Player* player = pair.second)
                ChatHandler(player->GetSession()).PSendSysMessage("Replay saved. Match ID: %u", replayId);
        }
    }

    uint8 HexNibble(char c)
    {
        if (c >= '0' && c <= '9')
            return uint8(c - '0');

        if (c >= 'a' && c <= 'f')
            return uint8(10 + (c - 'a'));

        if (c >= 'A' && c <= 'F')
            return uint8(10 + (c - 'A'));

        return 0xFF;
    }

    std::string ToHex(uint8 const* data, size_t size)
    {
        static constexpr char kHexDigits[] = "0123456789ABCDEF";
        std::string encoded;
        if (!data || size == 0)
            return encoded;

        encoded.reserve(size * 2);

        for (size_t i = 0; i < size; ++i)
        {
            uint8 byte = data[i];
            encoded.push_back(kHexDigits[byte >> 4]);
            encoded.push_back(kHexDigits[byte & 0xF]);
        }

        return encoded;
    }

    std::vector<uint8> FromHex(std::string const& data)
    {
        std::vector<uint8> decoded;
        if (data.size() % 2 != 0)
            return decoded;

        decoded.reserve(data.size() / 2);
        for (size_t i = 0; i < data.size(); i += 2)
        {
            uint8 high = HexNibble(data[i]);
            uint8 low = HexNibble(data[i + 1]);

            if (high == 0xFF || low == 0xFF)
            {
                decoded.clear();
                return decoded;
            }

            decoded.push_back((high << 4) | low);
        }

        return decoded;
    }

    void SaveReplay(Battleground* bg, MatchRecord&& match)
    {
        if (!bg || match.packets.empty())
            return;

        ByteBuffer buffer;
        for (PacketRecord& record : match.packets)
        {
            uint32 packetSize = record.packet.size();
            buffer << packetSize;
            buffer << record.timestamp;
            buffer << static_cast<uint16>(record.packet.GetOpcode());

            if (packetSize > 0)
                buffer.append(record.packet.contents(), record.packet.size());
        }

        uint32 contentSize = uint32(buffer.size());
        std::string encoded = ToHex(static_cast<uint8 const*>(buffer.contents()), buffer.size());

        CharacterDatabase.Execute(
            "INSERT INTO character_arena_replays (arenaTypeId, typeId, contentSize, contents, mapId) VALUES ({}, {}, {}, '{}', {})",
            uint32(match.arenaTypeId), uint32(match.typeId), contentSize, encoded, match.mapId);

        uint32 replayId = 0;
        if (QueryResult result = CharacterDatabase.Query("SELECT MAX(`id`) AS max_id FROM `character_arena_replays`"))
            replayId = result->Fetch()[0].Get<uint32>();

        NotifyPlayersReplaySaved(bg, replayId);
    }

    void HandleReplayTick(Battleground* bg)
    {
        if (!bg)
            return;

        auto it = s_activeReplays.find(bg->GetInstanceID());
        if (it == s_activeReplays.end())
            return;

        int32 startDelayTime = bg->GetStartDelayTime();
        if (startDelayTime > 1000)
        {
            bg->SetStartDelayTime(1000);
            bg->SetStartTime(bg->GetStartTime() + (startDelayTime - 1000));
        }

        MatchRecord& match = it->second;
        if (match.packets.empty() || bg->GetPlayers().empty())
        {
            s_activeReplays.erase(it);

            if (!bg->GetPlayers().empty())
                bg->GetPlayers().begin()->second->LeaveBattleground(bg);

            return;
        }

        while (!match.packets.empty() && match.packets.front().timestamp <= bg->GetStartTime())
        {
            if (bg->GetPlayers().empty())
                break;

            Player* viewer = bg->GetPlayers().begin()->second;
            if (!viewer)
                break;

            viewer->GetSession()->SendPacket(&match.packets.front().packet);
            match.packets.pop_front();
        }
    }

    std::vector<uint32> LoadRecentReplays(uint8 arenaTypeId)
    {
        std::vector<uint32> resultIds;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT id FROM character_arena_replays WHERE arenaTypeId IN ({}, 0) ORDER BY id DESC LIMIT 10",
                uint32(arenaTypeId)))
        {
            do
            {
                Field* fields = result->Fetch();
                if (!fields)
                    break;

                resultIds.push_back(fields[0].Get<uint32>());
            }
            while (result->NextRow());
        }

        return resultIds;
    }

    std::vector<uint32> LoadSavedReplays(uint64 playerGuid, bool ascending)
    {
        std::vector<uint32> resultIds;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT replay_id FROM character_saved_replays WHERE character_id = {} ORDER BY id {} LIMIT 29",
                uint32(playerGuid), ascending ? "ASC" : "DESC"))
        {
            do
            {
                Field* fields = result->Fetch();
                if (!fields)
                    break;

                resultIds.push_back(fields[0].Get<uint32>());
            }
            while (result->NextRow());
        }

        return resultIds;
    }
}

class ArenaReplayServerScript : public ServerScript
{
public:
    ArenaReplayServerScript() : ServerScript("ArenaReplayServerScript") { }

    bool CanPacketSend(WorldSession* session, WorldPacket& packet) override
    {
        if (!session)
            return true;

        Player* player = session->GetPlayer();
        if (!player)
            return true;

        Battleground* bg = player->GetBattleground();
        if (!bg || !bg->isArena())
            return true;

        if (s_activeReplays.find(bg->GetInstanceID()) != s_activeReplays.end())
            return true;

        if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS)
            return true;

        if (!IsTrackedOpcode(static_cast<Opcodes>(packet.GetOpcode())))
            return true;

        auto recorder = s_activeRecorders.find(bg->GetInstanceID());
        if (recorder == s_activeRecorders.end())
            return true;

        if (!IsDesignatedRecorder(bg, player))
            return true;

        recorder->second->Record(bg->GetStartTime(), packet);
        return true;
    }
};

class ArenaReplayBGScript : public BGScript
{
public:
    ArenaReplayBGScript() : BGScript("ArenaReplayBGScript") { }

    void OnBattlegroundStart(Battleground* bg) override
    {
        if (!bg || !bg->isArena())
            return;

        if (s_activeReplays.find(bg->GetInstanceID()) != s_activeReplays.end())
            return;

        s_activeRecorders[bg->GetInstanceID()] = std::make_unique<ReplayRecorder>(bg);
    }

    void OnBattlegroundEnd(Battleground* bg, TeamId /*winnerTeamId*/) override
    {
        if (!bg)
            return;

        auto it = s_activeRecorders.find(bg->GetInstanceID());
        if (it != s_activeRecorders.end())
        {
            SaveReplay(bg, it->second->Release());
            s_activeRecorders.erase(it);
        }

        s_activeReplays.erase(bg->GetInstanceID());
    }

    void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override
    {
        HandleReplayTick(bg);
    }
};

class ReplayGossip : public CreatureScript
{
public:
    ReplayGossip() : CreatureScript("ReplayGossip") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay 2v2 Matches", GOSSIP_SENDER_MAIN, 1);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay 3v3 Matches", GOSSIP_SENDER_MAIN, 2);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay 5v5 Matches", GOSSIP_SENDER_MAIN, 3);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Replay a Match ID", GOSSIP_SENDER_MAIN, 0, "Enter the Match ID", 0, true);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Favorite Matches", GOSSIP_SENDER_MAIN, 4);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        player->PlayerTalkClass->ClearMenus();

        switch (action)
        {
            case 1:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplayList(player, creature, LoadRecentReplays(kArenaType2v2));
                break;
            case 2:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplayList(player, creature, LoadRecentReplays(kArenaType3v3));
                break;
            case 3:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplayList(player, creature, LoadRecentReplays(kArenaType5v5));
                break;
            case 4:
                player->PlayerTalkClass->SendCloseGossip();
                ShowSavedReplays(player, creature, true);
                break;
            case GOSSIP_ACTION_INFO_DEF + 1:
                player->PlayerTalkClass->SendCloseGossip();
                ShowSavedReplays(player, creature, false);
                break;
            case GOSSIP_ACTION_INFO_DEF:
                OnGossipHello(player, creature);
                break;
            default:
                if (action >= GOSSIP_ACTION_INFO_DEF + 10)
                    return replayArenaMatch(player, action - (GOSSIP_ACTION_INFO_DEF + 10));
                break;
        }

        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action, const char* code) override
    {
        if (action == 0)
        {
            if (!code)
                return false;

            CloseGossipMenuFor(player);

            try
            {
                uint32 replayId = static_cast<uint32>(std::stoul(code));
                return replayArenaMatch(player, replayId);
            }
            catch (...) // invalid input
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Invalid Match ID.");
                return false;
            }
        }
        else if (action == 5)
        {
            if (!code)
                return false;

            CloseGossipMenuFor(player);

            try
            {
                uint32 replayId = static_cast<uint32>(std::stoul(code));
                BookmarkMatch(player->GetGUID().GetCounter(), replayId);
                return true;
            }
            catch (...)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Invalid Match ID.");
                return false;
            }
        }

        return false;
    }

private:
    void ShowReplayList(Player* player, Creature* creature, std::vector<uint32> const& matchIds)
    {
        uint32 base = GOSSIP_ACTION_INFO_DEF + 10;

        if (matchIds.empty())
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, "No replays found.", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        else
        {
            for (uint32 id : matchIds)
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay match " + std::to_string(id), GOSSIP_SENDER_MAIN, base + id);
        }

        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Back", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
    }

    void ShowSavedReplays(Player* player, Creature* creature, bool firstPage)
    {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Bookmark a Match ID", GOSSIP_SENDER_MAIN, 5, "Enter the Match ID", 0, true);

        auto matchIds = LoadSavedReplays(player->GetGUID().GetCounter(), firstPage);
        uint32 base = GOSSIP_ACTION_INFO_DEF + 10;

        if (matchIds.empty())
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, "No saved replays found.", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        else
        {
            for (uint32 id : matchIds)
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay match " + std::to_string(id), GOSSIP_SENDER_MAIN, base + id);
        }

        if (firstPage)
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Next Page", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);

        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Back", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
    }

    void BookmarkMatch(uint64 playerGuid, uint32 replayId)
    {
        CharacterDatabase.Execute("INSERT IGNORE INTO character_saved_replays (character_id, replay_id) VALUES ({}, {})",
            uint32(playerGuid), replayId);
    }

    bool replayArenaMatch(Player* player, uint32 replayId)
    {
        ChatHandler handler(player->GetSession());

        std::optional<MatchRecord> record = loadReplayData(replayId);
        if (!record)
        {
            handler.PSendSysMessage("Replay data not found.");
            return false;
        }

        Battleground* bg = sBattlegroundMgr->CreateNewBattleground(record->typeId,
            GetBattlegroundBracketByLevel(record->mapId, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)),
            record->arenaTypeId, false);

        if (!bg)
        {
            handler.PSendSysMessage("Couldn't create arena map!");
            handler.SetSentErrorMessage(true);
            return false;
        }

        player->SetPendingSpectatorForBG(bg->GetInstanceID());
        s_activeReplays[bg->GetInstanceID()] = std::move(*record);
        bg->StartBattleground();

        BattlegroundTypeId bgTypeId = bg->GetBgTypeID();
        TeamId teamId = Player::TeamIdForRace(player->getRace());
        uint32 queueSlot = 0;
        WorldPacket data;

        player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId, queueSlot, true, false, TEAM_NEUTRAL);
        player->SetEntryPoint();
        sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bgTypeId);
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime(), bg->GetArenaType(), teamId);
        player->GetSession()->SendPacket(&data);
        handler.PSendSysMessage("Replay ID %u begins.", replayId);
        return true;
    }

    std::optional<MatchRecord> loadReplayData(uint32 matchId)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT id, arenaTypeId, typeId, contentSize, contents, mapId FROM character_arena_replays WHERE id = {}",
            matchId);

        if (!result)
            return std::nullopt;

        Field* fields = result->Fetch();
        if (!fields)
            return std::nullopt;

        MatchRecord record;
        deserializeMatchData(record, fields);
        return record;
    }

    void deserializeMatchData(MatchRecord& record, Field* fields)
    {
        record.arenaTypeId = uint8(fields[1].Get<uint32>());
        record.typeId = BattlegroundTypeId(fields[2].Get<uint32>());
        uint32 contentSize = fields[3].Get<uint32>();
        std::vector<uint8> data = FromHex(fields[4].Get<std::string>());
        record.mapId = fields[5].Get<uint32>();

        ByteBuffer buffer;
        size_t appendSize = std::min<size_t>(contentSize, data.size());
        if (appendSize > 0)
            buffer.append(data.data(), appendSize);

        while (buffer.rpos() < buffer.size())
        {
            if (buffer.size() - buffer.rpos() < sizeof(uint32) + sizeof(uint32) + sizeof(uint16))
                break;

            uint32 packetSize;
            uint32 packetTimestamp;
            uint16 opcode;
            buffer >> packetSize;
            buffer >> packetTimestamp;
            buffer >> opcode;

            if (buffer.size() - buffer.rpos() < packetSize)
                break;

            WorldPacket packet(opcode, packetSize);
            if (packetSize > 0)
            {
                std::vector<uint8> tmp(packetSize);
                buffer.read(tmp.data(), packetSize);
                packet.append(tmp.data(), packetSize);
            }

            record.packets.push_back({ packetTimestamp, std::move(packet) });
        }
    }
};

void AddArenaReplayScripts()
{
    new ArenaReplayServerScript();
    new ArenaReplayBGScript();
    new ReplayGossip();
}
