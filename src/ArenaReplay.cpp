#include "Base32.h"
#include "CharacterDatabase.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "Map.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "Timer.h"
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <zlib.h>
#include "Log.h"

// Liste des opcodes qu’on enregistre (ArenaReplay style)
std::vector<Opcodes> watchList =
{
    SMSG_UPDATE_OBJECT,
    SMSG_COMPRESSED_UPDATE_OBJECT,
    SMSG_DESTROY_OBJECT,
    SMSG_NAME_QUERY_RESPONSE,
    MSG_MOVE_HEARTBEAT,
    MSG_MOVE_JUMP,
    SMSG_MONSTER_MOVE,
    SMSG_SPELL_START,
    SMSG_SPELL_GO,
    SMSG_AURA_UPDATE,
    SMSG_AURA_UPDATE_ALL,
    SMSG_CAST_FAILED,
    SMSG_ATTACKSTART,
    SMSG_ATTACKSTOP,
    SMSG_POWER_UPDATE,
    SMSG_MESSAGECHAT
};

struct PacketRecord { uint32 timestamp; WorldPacket packet; };

struct MatchRecord
{
    uint32 mapId;
    uint32 encounterId;
    uint32 startTime;
    std::deque<PacketRecord> packets;
    std::unordered_map<ObjectGuid, WorldPacket> hasCreate;
};

struct ReplaySession
{
    MatchRecord record;
    uint32 replayStartTime;
    std::string playerGuids;
};

std::unordered_map<uint32, MatchRecord> records;
std::unordered_map<uint64, ReplaySession> loadedReplays;
static std::unordered_set<uint32> activeRecordings;

static constexpr uint32 RAID_BOSS_ID = 36612;


	static bool ContainsGuid(WorldPacket const& packet, ObjectGuid guid)
{
    if (packet.size() < sizeof(uint64))
        return false;

    // On cherche la séquence binaire du GUID dans le buffer du packet
    const char* data = reinterpret_cast<const char*>(packet.contents());
    std::string_view sv(data, packet.size());

    std::string guidBytes(reinterpret_cast<const char*>(&guid), sizeof(uint64));
    return sv.find(guidBytes) != std::string_view::npos;
}



class RaidReplayServerScript : public ServerScript
{
public:
    RaidReplayServerScript() : ServerScript("RaidReplayServerScript", { SERVERHOOK_CAN_PACKET_SEND }) { }

    bool CanPacketSend(WorldSession* session, WorldPacket& packet) override
    {
        if (!session || !session->GetPlayer())
            return true;

        Player* player = session->GetPlayer();
        Map* map = player->GetMap();
        if (!map)
            return true;

        uint32 instanceId = map->GetInstanceId();
        if (instanceId == 0)
            instanceId = player->GetGUID().GetCounter();

        if (activeRecordings.find(instanceId) == activeRecordings.end())
            return true;

        if (records.find(instanceId) == records.end())
            records[instanceId].packets.clear();

        MatchRecord& record = records[instanceId];
        record.mapId = map->GetId();
        record.encounterId = RAID_BOSS_ID;

        // Ignore packets hors watchlist
        if (std::find(watchList.begin(), watchList.end(), packet.GetOpcode()) == watchList.end())
            return true;

        WorldPacket safePacket(packet.GetOpcode(), packet.size());
        if (packet.size() > 0)
            safePacket.append(packet.contents(), packet.size());

        record.packets.push_back({ getMSTime(), safePacket });

        // Capture create packets pour rebuild
        if (packet.GetOpcode() == SMSG_UPDATE_OBJECT || packet.GetOpcode() == SMSG_COMPRESSED_UPDATE_OBJECT)
        {
            ByteBuffer buf;
            buf.append(packet.contents(), packet.size());

            if (packet.GetOpcode() == SMSG_COMPRESSED_UPDATE_OBJECT)
            {
                uint32 realSize;
                buf >> realSize;

                std::vector<uint8> decompressed(realSize);
                uLongf dstSize = realSize;
                if (uncompress(&decompressed[0], &dstSize, buf.contents() + buf.rpos(), buf.size() - buf.rpos()) != Z_OK)
                    return true;

                buf.clear();
                buf.append(&decompressed[0], dstSize);
            }

            uint32 blocks;
            buf >> blocks;

            for (uint32 i = 0; i < blocks; ++i)
            {
                uint8 updateType;
                buf >> updateType;

                if (updateType == UPDATETYPE_CREATE_OBJECT || updateType == UPDATETYPE_CREATE_OBJECT2)
                {
                    ObjectGuid createdGuid;
                    buf >> createdGuid;
                    record.hasCreate[createdGuid] = safePacket;
                }
            }
        }

        return true;
    }
};

class RaidReplayCommandScript : public CommandScript
{
public:
    RaidReplayCommandScript() : CommandScript("RaidReplayCommandScript") { }

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        static Acore::ChatCommands::ChatCommandTable raidReplayRecordCommand =
        {
            { "start",  HandleStart,  SEC_GAMEMASTER, Acore::ChatCommands::Console::No },
            { "stop",   HandleStop,   SEC_GAMEMASTER, Acore::ChatCommands::Console::No },
            { "replay", HandleReplay, SEC_GAMEMASTER, Acore::ChatCommands::Console::No },
			{ "makespec", HandleMakeSpec, SEC_GAMEMASTER, Acore::ChatCommands::Console::No }
        };

        static Acore::ChatCommands::ChatCommandTable raidReplayCommandTable =
        {
            { "raidrecord", raidReplayRecordCommand }
        };

        return raidReplayCommandTable;
    }

    static bool HandleStart(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        Map* map = player->GetMap();

        uint32 instanceId = map->GetInstanceId();
        if (instanceId == 0)
            instanceId = player->GetGUID().GetCounter();

        activeRecordings.insert(instanceId);
        records[instanceId].packets.clear();
        records[instanceId].mapId = map->GetId();
        records[instanceId].encounterId = RAID_BOSS_ID;
        records[instanceId].startTime = getMSTime();

        // 🔥 Resend forcé de tous les create (option 1.5)
        for (auto const& ref : map->GetPlayers())
        {
            if (Player* member = ref.GetSource())
            {
 UpdateData updateData;
member->BuildCreateUpdateBlockForPlayer(&updateData, player);

WorldPacket createPkt;
updateData.BuildPacket(createPkt);
player->GetSession()->SendPacket(&createPkt);

// Sauvegarde aussi dans le record
records[instanceId].packets.push_back({ getMSTime(), createPkt });
records[instanceId].hasCreate[member->GetGUID()] = createPkt;
            }
        }

        handler->PSendSysMessage("📹 Recording démarré pour l’instance %u.", instanceId);
        return true;
    }

    static bool HandleStop(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        Map* map = player->GetMap();

        uint32 instanceId = map->GetInstanceId();
        if (instanceId == 0)
            instanceId = player->GetGUID().GetCounter();
        auto it = records.find(instanceId);
        if (it == records.end() || it->second.packets.empty())
        {
            handler->SendSysMessage("⚠️ Aucun enregistrement trouvé.");
            return false;
        }

        MatchRecord& match = it->second;

        std::string playerGuids;
        if (Group* group = player->GetGroup()) {
            for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next()) {
                if (Player* member = ref->GetSource()) {
                    if (!playerGuids.empty()) playerGuids += ",";
                    playerGuids += std::to_string(member->GetGUID().GetRawValue());
                }
            }
        } else {
            playerGuids = std::to_string(player->GetGUID().GetRawValue());
        }

        ByteBuffer buffer;
        for (auto& pr : match.packets)
        {
            buffer << uint32(pr.packet.size());
            buffer << (pr.timestamp - match.startTime);
            buffer << pr.packet.GetOpcode();
            if (pr.packet.size() > 0)
                buffer.append(pr.packet.contents(), pr.packet.size());
        }

        std::string encoded = Acore::Encoding::Base32::Encode(
            std::vector<uint8>(buffer.contents(), buffer.contents() + buffer.size())
        );

        CharacterDatabase.Execute(
            "INSERT INTO `character_arena_replays` "
            "(`arenaTypeId`, `typeId`, `contentSize`, `contents`, `mapId`, `winnerPlayerGuids`, `loserPlayerGuids`) "
            "VALUES ({}, {}, {}, '{}', {}, '{}', '{}')",
            0,
            match.encounterId,
            buffer.size(),
            encoded,
            match.mapId,
            playerGuids,
            ""
        );

        handler->PSendSysMessage("✅ Recording stoppé et sauvegardé (packets: %u).", (uint32)match.packets.size());

        activeRecordings.erase(instanceId);
        records.erase(it);

        return true;
    }
	
	
static bool HandleMakeSpec(ChatHandler* handler) {
    Player* player = handler->GetSession()->GetPlayer();
    if (!player)
        return false;

    Map* map = player->GetMap();
    player->SetPendingSpectatorForBG(map->GetInstanceId());

    // ⚠️ faut forcer un nouveau worldport pour que le client bascule en spec
    player->TeleportTo(
        map->GetId(),
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ(),
        player->GetOrientation()
    );

    handler->PSendSysMessage("✅ Vous êtes maintenant en mode spectateur pour l’instance %u.", map->GetInstanceId());
    return true;
}


    static bool HandleReplay(ChatHandler* handler, uint32 replayId)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        QueryResult result = CharacterDatabase.Query(
            "SELECT mapId, typeId, contentSize, contents, winnerPlayerGuids, loserPlayerGuids "
            "FROM character_arena_replays WHERE id = {}", replayId);

        if (!result)
        {
            handler->PSendSysMessage("⚠️ Replay %u introuvable.", replayId);
            return false;
        }

        Field* fields = result->Fetch();
        uint32 mapId       = fields[0].Get<uint32>();
        uint32 encounter   = fields[1].Get<uint32>();
        uint32 contentSize = fields[2].Get<uint32>();
        std::string contents   = fields[3].Get<std::string>();
        std::string winnerPlayerGuids = fields[4].Get<std::string>();
        std::string loserPlayerGuids = fields[5].Get<std::string>();
        std::string playerGuids = winnerPlayerGuids;
        if (!loserPlayerGuids.empty())
        {
            if (!playerGuids.empty())
                playerGuids += ",";
            playerGuids += loserPlayerGuids;
        }

        std::vector<uint8> data = *Acore::Encoding::Base32::Decode(contents);
        ByteBuffer buffer;
        buffer.append(&data[0], data.size());

        MatchRecord record;
        record.mapId = mapId;
        record.encounterId = encounter;
        record.startTime = getMSTime();

        while (buffer.rpos() < buffer.size())
        {
            uint32 size, timestamp;
            uint16 opcode;
            buffer >> size;
            buffer >> timestamp;
            buffer >> opcode;

            WorldPacket packet(opcode, size);
            if (size > 0)
            {
                std::vector<uint8> tmp(size, 0);
                buffer.read(&tmp[0], size);
                packet.append(&tmp[0], size);
            }

            record.packets.push_back({ timestamp, packet });
        }

        ReplaySession session;
        session.record = std::move(record);
        session.replayStartTime = getMSTime();
        session.playerGuids = playerGuids;

        loadedReplays[player->GetGUID().GetCounter()] = std::move(session);

        handler->PSendSysMessage("▶️ Replay %u chargé (%u packets).",
                                 replayId, (uint32)loadedReplays[player->GetGUID().GetCounter()].record.packets.size());
        return true;
    }
};

class RaidReplayUpdateScript : public WorldScript
{
public:
    RaidReplayUpdateScript() : WorldScript("RaidReplayUpdateScript", { WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 diff) override
    {
        static std::unordered_map<uint64, uint32> replayTimers;

        for (auto it = loadedReplays.begin(); it != loadedReplays.end(); )
        {
            uint64 viewerGUID = it->first;
            Player* viewer = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(viewerGUID));
            if (!viewer)
            {
                replayTimers.erase(viewerGUID);
                it = loadedReplays.erase(it);
                continue;
            }

            ReplaySession& session = it->second;
            MatchRecord& match = session.record;

            if (match.packets.empty())
            {
                ChatHandler(viewer->GetSession()).PSendSysMessage("Replay terminé.");
                replayTimers.erase(viewerGUID);
                it = loadedReplays.erase(it);
                continue;
            }

            replayTimers[viewerGUID] += diff;

        while (!match.packets.empty() && match.packets.front().timestamp <= replayTimers[viewerGUID])
{
    PacketRecord pr = match.packets.front();
    match.packets.pop_front();

    // ⚠️ Ne jamais rejouer des updates contenant le GUID du viewer,
    // sinon son client "possède" le clone et il perd ses contrôles/langue
    if (ContainsGuid(pr.packet, viewer->GetGUID()))
        continue;

    viewer->GetSession()->SendPacket(&pr.packet);
}

            ++it;
        }
    }
};

void Addmod_arena_replayScripts()
{
    new RaidReplayServerScript();
    new RaidReplayUpdateScript();
    new RaidReplayCommandScript();
}
