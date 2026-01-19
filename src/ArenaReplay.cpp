//
// Created by romain-p on 17/10/2021.
//
#include "ArenaReplayDatabaseConnection.h"
#include "ArenaReplay_loader.h"
#include "ArenaTeamMgr.h"
#include "Base32.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterDatabase.h"
#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <vector>
#include <zlib.h>

std::vector<Opcodes> watchList =
{
        SMSG_NOTIFICATION,
        SMSG_AURA_UPDATE,
        SMSG_WORLD_STATE_UI_TIMER_UPDATE,
        SMSG_COMPRESSED_UPDATE_OBJECT,
        SMSG_AURA_UPDATE_ALL,
        SMSG_NAME_QUERY_RESPONSE,
        SMSG_DESTROY_OBJECT,
        MSG_MOVE_START_FORWARD,
        MSG_MOVE_SET_FACING,
        MSG_MOVE_HEARTBEAT,
        MSG_MOVE_JUMP,
        SMSG_MONSTER_MOVE,
        MSG_MOVE_FALL_LAND,
        SMSG_PERIODICAURALOG,
        SMSG_ARENA_UNIT_DESTROYED,
        MSG_MOVE_START_STRAFE_RIGHT,
        MSG_MOVE_STOP_STRAFE,
        MSG_MOVE_START_STRAFE_LEFT,
        MSG_MOVE_STOP,
        MSG_MOVE_START_BACKWARD,
        MSG_MOVE_START_TURN_LEFT,
        MSG_MOVE_STOP_TURN,
        MSG_MOVE_START_TURN_RIGHT,
        SMSG_SPELL_START,
        SMSG_SPELL_GO,
        CMSG_CAST_SPELL,
        CMSG_CANCEL_CAST,
        SMSG_CAST_FAILED,
        SMSG_SPELL_START,
        SMSG_SPELL_FAILURE,
        SMSG_SPELL_DELAYED,
        SMSG_PLAY_SPELL_IMPACT,
        SMSG_FORCE_RUN_SPEED_CHANGE,
        SMSG_ATTACKSTART,
        SMSG_POWER_UPDATE,
        SMSG_ATTACKERSTATEUPDATE,
        SMSG_SPELLDAMAGESHIELD,
        SMSG_SPELLHEALLOG,
        SMSG_SPELLENERGIZELOG,
        SMSG_SPELLNONMELEEDAMAGELOG,
        SMSG_ATTACKSTOP,
        SMSG_EMOTE,
        SMSG_AI_REACTION,
        SMSG_PET_NAME_QUERY_RESPONSE,
        SMSG_CANCEL_AUTO_REPEAT,
        SMSG_UPDATE_OBJECT,
        SMSG_FORCE_FLIGHT_SPEED_CHANGE,
        SMSG_GAMEOBJECT_QUERY_RESPONSE,
        SMSG_FORCE_SWIM_SPEED_CHANGE,
        SMSG_GAMEOBJECT_DESPAWN_ANIM,
        SMSG_CANCEL_COMBAT,
        SMSG_DISMOUNTRESULT,
        SMSG_MOUNTRESULT,
        SMSG_DISMOUNT,
        CMSG_MOUNTSPECIAL_ANIM,
        SMSG_MOUNTSPECIAL_ANIM,
        SMSG_MIRRORIMAGE_DATA,
        CMSG_MESSAGECHAT,
        SMSG_MESSAGECHAT
};

/*
CMSG_CANCEL_MOUNT_AURA,
CMSG_ALTER_APPEARANCE
SMSG_SUMMON_CANCEL
SMSG_PLAY_SOUND
SMSG_PLAY_SPELL_VISUAL
CMSG_ATTACKSWING
CMSG_ATTACKSTOP*/

struct PacketRecord { uint32 timestamp; WorldPacket packet; uint64 sourceGuid = 0; };
struct MatchRecord {
    BattlegroundTypeId typeId;
    uint8 arenaTypeId;
    uint32 mapId;
    std::deque<PacketRecord> packets;
    std::vector<uint64> participantGuids;
    std::unordered_map<uint64, uint64> guidRemap;
    bool debugLoggedStart = false;
    size_t debugPacketsLogged = 0;
};
struct BgPlayersGuids { std::string alliancePlayerGuids; std::string hordePlayerGuids; };
std::unordered_map<uint32, MatchRecord> records;
std::unordered_map<uint64, MatchRecord> loadedReplays;
std::unordered_map<uint32, uint32> bgReplayIds;
std::unordered_map<uint32, BgPlayersGuids> bgPlayersGuids;

namespace
{
    bool ReplaceGuidInPacket(WorldPacket& packet, uint64 fromGuid, uint64 toGuid);
    bool PacketContainsGuid(WorldPacket const& packet, uint64 guid);

    std::array<uint8, 8> GetGuidBytes(uint64 guid)
    {
        std::array<uint8, 8> bytes{};
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = uint8((guid >> (i * 8)) & 0xFF);

        return bytes;
    }

    uint64 BuildGuidFromBytes(std::array<uint8, 8> const& bytes)
    {
        uint64 value = 0;
        for (size_t i = 0; i < bytes.size(); ++i)
            value |= (uint64(bytes[i]) << (i * 8));

        return value;
    }

    bool ReplayMetadataContainsGuid(MatchRecord const& record, uint64 guid)
    {
        if (std::find(record.participantGuids.begin(), record.participantGuids.end(), guid) != record.participantGuids.end())
            return true;

        for (PacketRecord const& packet : record.packets)
        {
            if (packet.sourceGuid == guid)
                return true;
        }

        return false;
    }

    std::vector<uint8> GetPackedGuidBytes(uint64 guid)
    {
        std::array<uint8, 8> guidBytes = GetGuidBytes(guid);
        uint8 mask = 0;
        std::vector<uint8> packed;
        packed.reserve(9);

        for (uint8 i = 0; i < guidBytes.size(); ++i)
        {
            if (guidBytes[i] == 0)
                continue;

            mask |= (1u << i);
            packed.push_back(guidBytes[i]);
        }

        if (mask == 0)
            return {};

        packed.insert(packed.begin(), mask);
        return packed;
    }

    uint8 GetPackedMask(uint64 guid)
    {
        std::vector<uint8> packed = GetPackedGuidBytes(guid);
        if (packed.empty())
            return 0;

        return packed.front();
    }

    bool ContainsSequence(std::vector<uint8> const& buffer, std::vector<uint8> const& sequence)
    {
        if (sequence.empty() || buffer.size() < sequence.size())
            return false;

        return std::search(buffer.begin(), buffer.end(), sequence.begin(), sequence.end()) != buffer.end();
    }

    bool ReplaceSequence(std::vector<uint8>& buffer, std::vector<uint8> const& from, std::vector<uint8> const& to)
    {
        if (from.empty() || from == to)
            return false;

        bool modified = false;
        for (size_t i = 0; i + from.size() <= buffer.size();)
        {
            if (std::memcmp(buffer.data() + i, from.data(), from.size()) == 0)
            {
                buffer.erase(buffer.begin() + i, buffer.begin() + i + from.size());
                buffer.insert(buffer.begin() + i, to.begin(), to.end());
                i += to.size();
                modified = true;
            }
            else
            {
                ++i;
            }
        }

        return modified;
    }

    template <typename T>
    bool ReadLittleEndian(std::vector<uint8> const& data, size_t& offset, T& value)
    {
        if (offset + sizeof(T) > data.size())
            return false;

        value = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            value |= (T(data[offset + i]) << (i * 8));

        offset += sizeof(T);
        return true;
    }

    template <typename T>
    void WriteLittleEndian(std::vector<uint8>& buffer, T value)
    {
        for (size_t i = 0; i < sizeof(T); ++i)
            buffer.push_back(uint8((value >> (i * 8)) & 0xFF));
    }

    enum class MultipacketCountField : uint8
    {
        None,
        Count16,
        Count32
    };

    enum class MultipacketHeaderOrder : uint8
    {
        OpcodeThenLength,
        LengthThenOpcode
    };

    enum class MultipacketLengthField : uint8
    {
        Length16,
        Length32
    };

    struct MultipacketLayout
    {
        MultipacketCountField countField;
        MultipacketHeaderOrder headerOrder;
        MultipacketLengthField lengthField;
    };

    bool ReadMultipacketLength(std::vector<uint8> const& data, size_t& offset, MultipacketLengthField lengthField, uint32& length)
    {
        if (lengthField == MultipacketLengthField::Length16)
        {
            uint16 value = 0;
            if (!ReadLittleEndian<uint16>(data, offset, value))
                return false;

            length = value;
            return true;
        }

        uint32 value = 0;
        if (!ReadLittleEndian<uint32>(data, offset, value))
            return false;

        length = value;
        return true;
    }

    bool TryParseMultipacketPayload(std::vector<uint8> const& payload, MultipacketLayout layout, std::vector<WorldPacket>& embeddedPackets)
    {
        size_t offset = 0;
        uint32 declaredCount = 0;

        if (layout.countField == MultipacketCountField::Count16)
        {
            uint16 count = 0;
            if (!ReadLittleEndian<uint16>(payload, offset, count))
                return false;

            declaredCount = count;
        }
        else if (layout.countField == MultipacketCountField::Count32)
        {
            uint32 count = 0;
            if (!ReadLittleEndian<uint32>(payload, offset, count))
                return false;

            declaredCount = count;
        }

        while (offset < payload.size())
        {
            uint16 opcode = 0;
            uint32 length = 0;

            if (layout.headerOrder == MultipacketHeaderOrder::OpcodeThenLength)
            {
                if (!ReadLittleEndian<uint16>(payload, offset, opcode))
                    return false;

                if (!ReadMultipacketLength(payload, offset, layout.lengthField, length))
                    return false;
            }
            else
            {
                if (!ReadMultipacketLength(payload, offset, layout.lengthField, length))
                    return false;

                if (!ReadLittleEndian<uint16>(payload, offset, opcode))
                    return false;
            }

            if (payload.size() - offset < length)
                return false;

            WorldPacket embedded(static_cast<Opcodes>(opcode), length);
            if (length > 0)
                embedded.append(payload.data() + offset, length);

            offset += length;
            embeddedPackets.push_back(std::move(embedded));

            if (declaredCount != 0 && embeddedPackets.size() > declaredCount)
                return false;
        }

        if (layout.countField != MultipacketCountField::None && embeddedPackets.size() != declaredCount)
            return false;

        return offset == payload.size();
    }

    bool BuildMultipacketPayload(MultipacketLayout layout, std::vector<WorldPacket> const& embeddedPackets, std::vector<uint8>& output)
    {
        output.clear();

        if (layout.countField == MultipacketCountField::Count16)
        {
            if (embeddedPackets.size() > std::numeric_limits<uint16>::max())
                return false;

            WriteLittleEndian<uint16>(output, static_cast<uint16>(embeddedPackets.size()));
        }
        else if (layout.countField == MultipacketCountField::Count32)
        {
            WriteLittleEndian<uint32>(output, static_cast<uint32>(embeddedPackets.size()));
        }

        for (WorldPacket const& embedded : embeddedPackets)
        {
            uint32 length = static_cast<uint32>(embedded.size());
            if (layout.lengthField == MultipacketLengthField::Length16 && length > std::numeric_limits<uint16>::max())
                return false;

            auto writeLength = [&](uint32 len)
            {
                if (layout.lengthField == MultipacketLengthField::Length16)
                    WriteLittleEndian<uint16>(output, static_cast<uint16>(len));
                else
                    WriteLittleEndian<uint32>(output, len);
            };

            if (layout.headerOrder == MultipacketHeaderOrder::OpcodeThenLength)
            {
                WriteLittleEndian<uint16>(output, static_cast<uint16>(embedded.GetOpcode()));
                writeLength(length);
            }
            else
            {
                writeLength(length);
                WriteLittleEndian<uint16>(output, static_cast<uint16>(embedded.GetOpcode()));
            }

            if (length > 0)
                output.insert(output.end(), embedded.contents(), embedded.contents() + length);
        }

        return true;
    }

    bool MultipacketPayloadContainsGuid(std::vector<uint8> const& payload, uint64 guid)
    {
        std::array<MultipacketCountField, 3> countFields = {
            MultipacketCountField::None,
            MultipacketCountField::Count16,
            MultipacketCountField::Count32
        };
        std::array<MultipacketHeaderOrder, 2> headerOrders = {
            MultipacketHeaderOrder::OpcodeThenLength,
            MultipacketHeaderOrder::LengthThenOpcode
        };
        std::array<MultipacketLengthField, 2> lengthFields = {
            MultipacketLengthField::Length16,
            MultipacketLengthField::Length32
        };

        for (MultipacketCountField countField : countFields)
        {
            for (MultipacketHeaderOrder headerOrder : headerOrders)
            {
                for (MultipacketLengthField lengthField : lengthFields)
                {
                    MultipacketLayout layout{ countField, headerOrder, lengthField };
                    std::vector<WorldPacket> embeddedPackets;
                    if (!TryParseMultipacketPayload(payload, layout, embeddedPackets))
                        continue;

                    for (WorldPacket const& embedded : embeddedPackets)
                    {
                        if (PacketContainsGuid(embedded, guid))
                            return true;
                    }
                }
            }
        }

        return false;
    }

    bool RewriteMultipacketPacket(WorldPacket& packet, uint64 fromGuid, uint64 toGuid)
    {
        size_t packetSize = packet.size();
        if (packetSize == 0)
            return false;

        uint8 const* contents = packet.contents();
        if (!contents)
            return false;

        std::vector<uint8> payload(contents, contents + packetSize);

        std::array<MultipacketCountField, 3> countFields = {
            MultipacketCountField::None,
            MultipacketCountField::Count16,
            MultipacketCountField::Count32
        };
        std::array<MultipacketHeaderOrder, 2> headerOrders = {
            MultipacketHeaderOrder::OpcodeThenLength,
            MultipacketHeaderOrder::LengthThenOpcode
        };
        std::array<MultipacketLengthField, 2> lengthFields = {
            MultipacketLengthField::Length16,
            MultipacketLengthField::Length32
        };


        for (MultipacketCountField countField : countFields)
        {
            for (MultipacketHeaderOrder headerOrder : headerOrders)
            {
                for (MultipacketLengthField lengthField : lengthFields)
                {
                    MultipacketLayout layout{ countField, headerOrder, lengthField };
                    std::vector<WorldPacket> embeddedPackets;
                    if (!TryParseMultipacketPayload(payload, layout, embeddedPackets))
                        continue;

                    bool modified = false;
                    for (WorldPacket& embedded : embeddedPackets)
                        modified |= ReplaceGuidInPacket(embedded, fromGuid, toGuid);

                    if (!modified)
                        continue;

                    std::vector<uint8> rebuilt;
                    if (!BuildMultipacketPayload(layout, embeddedPackets, rebuilt))
                        continue;

                    WorldPacket updated(packet.GetOpcode(), rebuilt.size());
                    updated.append(rebuilt.data(), rebuilt.size());
                    packet = std::move(updated);
                    return true;
                }
            }
        }

        return false;
    }

    bool Decompress(std::vector<uint8> const& input, std::vector<uint8>& output)
    {
        if (input.size() <= sizeof(uint32))
            return false;

        uint32 decompressedSize;
        std::memcpy(&decompressedSize, input.data(), sizeof(uint32));
        if (decompressedSize == 0)
            return false;

        output.resize(decompressedSize);

        z_stream stream;
        std::memset(&stream, 0, sizeof(stream));
        stream.next_in = const_cast<Bytef*>(reinterpret_cast<Bytef const*>(input.data() + sizeof(uint32)));
        stream.avail_in = static_cast<uInt>(input.size() - sizeof(uint32));
        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());

        int ret = inflateInit(&stream);
        if (ret != Z_OK)
            return false;

        ret = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (ret != Z_STREAM_END)
            return false;

        output.resize(stream.total_out);
        return true;
    }

    uint64 GenerateGhostGuid(uint64 originalGuid, std::unordered_set<uint64> const& usedGuids)
    {
        auto originalBytes = GetGuidBytes(originalGuid);
        uint8 originalMask = GetPackedMask(originalGuid);
        std::vector<size_t> nonZeroIndices;
        nonZeroIndices.reserve(originalBytes.size());
        for (size_t i = 0; i < originalBytes.size(); ++i)
        {
            if (originalBytes[i] != 0)
                nonZeroIndices.push_back(i);
        }

        if (nonZeroIndices.empty())
            return originalGuid;

        auto BuildCandidate = [&](std::vector<uint16> const& offsets)
        {
            std::array<uint8, 8> candidateBytes = originalBytes;
            for (size_t i = 0; i < nonZeroIndices.size(); ++i)
            {
                size_t byteIndex = nonZeroIndices[i];
                uint16 base = originalBytes[byteIndex];
                uint16 offset = offsets[i];
                uint16 value = static_cast<uint16>(((base - 1u + offset) % 255u) + 1u);
                candidateBytes[byteIndex] = static_cast<uint8>(value);
            }

            return BuildGuidFromBytes(candidateBytes);
        };

        std::vector<uint16> offsets(nonZeroIndices.size(), 0);
        while (true)
        {
            size_t position = 0;
            while (position < offsets.size())
            {
                if (offsets[position] < 254)
                {
                    ++offsets[position];
                    break;
                }

                offsets[position] = 0;
                ++position;
            }

            if (position == offsets.size())
                break;

            bool allZero = std::all_of(offsets.begin(), offsets.end(), [](uint16 value) { return value == 0; });
            if (allZero)
                continue;

            uint64 candidate = BuildCandidate(offsets);
            if (candidate == 0 || candidate == originalGuid)
                continue;

            if (GetPackedMask(candidate) != originalMask)
                continue;

            if (usedGuids.find(candidate) == usedGuids.end())
                return candidate;
        }

        uint64 candidate = originalGuid;
        do
        {
            ++candidate;
        } while (candidate == 0 || usedGuids.find(candidate) != usedGuids.end() || GetPackedMask(candidate) != originalMask);

        return candidate;
    }

    bool ContainsGuidSequences(std::vector<uint8> const& payload, uint64 guid, bool allowRaw)
    {
        if (payload.empty() || guid == 0)
            return false;

        auto bytesArray = GetGuidBytes(guid);
        std::vector<uint8> raw(bytesArray.begin(), bytesArray.end());
        std::vector<uint8> packed = GetPackedGuidBytes(guid);

        if (allowRaw && ContainsSequence(payload, raw))
            return true;

        if (!packed.empty() && ContainsSequence(payload, packed))
            return true;

        return false;
    }

    bool ReplaceGuidSequences(std::vector<uint8>& payload, uint64 fromGuid, uint64 toGuid, bool allowRaw)
    {
        if (payload.empty())
            return false;

        auto fromBytesArray = GetGuidBytes(fromGuid);
        auto toBytesArray = GetGuidBytes(toGuid);
        std::vector<uint8> fromBytes(fromBytesArray.begin(), fromBytesArray.end());
        std::vector<uint8> toBytes(toBytesArray.begin(), toBytesArray.end());
        std::vector<uint8> fromPacked = GetPackedGuidBytes(fromGuid);
        std::vector<uint8> toPacked = GetPackedGuidBytes(toGuid);

        bool modified = false;
        if (allowRaw)
            modified |= ReplaceSequence(payload, fromBytes, toBytes);

        if (!fromPacked.empty() && !toPacked.empty())
        {
            if (fromPacked.size() == toPacked.size() && fromPacked.front() == toPacked.front())
            {
                modified |= ReplaceSequence(payload, fromPacked, toPacked);
            }
            else
            {
                LOG_WARN("modules", "ArenaReplay: packed GUID shape mismatch (from mask {:02X} size {}, to mask {:02X} size {}), skipping packed replacement",
                    fromPacked.front(),
                    fromPacked.size(),
                    toPacked.front(),
                    toPacked.size());
            }
        }

        return modified;
    }

    bool Compress(std::vector<uint8> const& input, std::vector<uint8>& output)
    {
        if (input.empty())
            return false;

        uint32 uncompressedSize = static_cast<uint32>(input.size());
        uLongf maxCompressedSize = compressBound(uncompressedSize);
        output.resize(sizeof(uint32) + maxCompressedSize);

        std::memcpy(output.data(), &uncompressedSize, sizeof(uint32));

        z_stream stream;
        std::memset(&stream, 0, sizeof(stream));
        stream.next_in = const_cast<Bytef*>(reinterpret_cast<Bytef const*>(input.data()));
        stream.avail_in = static_cast<uInt>(input.size());
        stream.next_out = output.data() + sizeof(uint32);
        stream.avail_out = static_cast<uInt>(maxCompressedSize);

        int ret = deflateInit(&stream, Z_BEST_SPEED);
        if (ret != Z_OK)
            return false;

        ret = deflate(&stream, Z_FINISH);
        deflateEnd(&stream);
        if (ret != Z_STREAM_END)
            return false;

        output.resize(sizeof(uint32) + stream.total_out);
        return true;
    }

    bool RewriteCompressedPacket(WorldPacket& packet, uint64 fromGuid, uint64 toGuid)
    {
        size_t packetSize = packet.size();
        if (packetSize == 0)
            return false;

        uint8 const* contents = packet.contents();
        if (!contents)
            return false;

        std::vector<uint8> buffer(contents, contents + packetSize);
        std::vector<uint8> decompressed;

        if (!Decompress(buffer, decompressed))
            return false;

        if (!ReplaceGuidSequences(decompressed, fromGuid, toGuid, false))
            return false;

        std::vector<uint8> recompressed;
        if (!Compress(decompressed, recompressed))
            return false;

        WorldPacket updated(SMSG_COMPRESSED_UPDATE_OBJECT, recompressed.size());
        if (!recompressed.empty())
            updated.append(recompressed.data(), recompressed.size());

        packet = std::move(updated);
        return true;
    }

    bool RewriteRawPacket(WorldPacket& packet, uint64 fromGuid, uint64 toGuid)
    {
        size_t packetSize = packet.size();
        if (packetSize == 0)
            return false;

        uint8 const* contents = packet.contents();
        if (!contents)
            return false;

        std::vector<uint8> buffer(contents, contents + packetSize);
        bool allowRaw = packet.GetOpcode() != SMSG_UPDATE_OBJECT;
        if (!ReplaceGuidSequences(buffer, fromGuid, toGuid, allowRaw))
            return false;

        WorldPacket updated(packet.GetOpcode(), buffer.size());
        updated.append(buffer.data(), buffer.size());
        packet = std::move(updated);
        return true;
    }

    bool PacketContainsGuid(WorldPacket const& packet, uint64 guid)
    {
        if (guid == 0)
            return false;

        size_t packetSize = packet.size();
        if (packetSize == 0)
            return false;

        uint8 const* contents = packet.contents();
        if (!contents)
            return false;

        std::vector<uint8> buffer(contents, contents + packetSize);

        if (packet.GetOpcode() == SMSG_COMPRESSED_UPDATE_OBJECT)
        {
            std::vector<uint8> decompressed;
            if (!Decompress(buffer, decompressed))
            {
                LOG_WARN("modules", "ArenaReplay: failed to decompress SMSG_COMPRESSED_UPDATE_OBJECT while searching for guid {}", guid);
                return false;
            }

            return ContainsGuidSequences(decompressed, guid, false);
        }

        if (packet.GetOpcode() == SMSG_UPDATE_OBJECT)
            return ContainsGuidSequences(buffer, guid, false);

        if (packet.GetOpcode() == SMSG_MULTIPLE_PACKETS)
        {
            if (MultipacketPayloadContainsGuid(buffer, guid))
                return true;

            return ContainsGuidSequences(buffer, guid, true);
        }

        return ContainsGuidSequences(buffer, guid, true);
    }

    bool ReplaceGuidInPacket(WorldPacket& packet, uint64 fromGuid, uint64 toGuid)
    {
        if (fromGuid == toGuid)
            return false;

        if (packet.GetOpcode() == SMSG_MULTIPLE_PACKETS)
        {
            if (RewriteMultipacketPacket(packet, fromGuid, toGuid))
                return true;
        }

        if (packet.GetOpcode() == SMSG_COMPRESSED_UPDATE_OBJECT)
            return RewriteCompressedPacket(packet, fromGuid, toGuid);

        return RewriteRawPacket(packet, fromGuid, toGuid);
    }

    std::unordered_set<uint64> BuildUsedGuidSet(MatchRecord const& record)
    {
        std::unordered_set<uint64> used;
        used.insert(record.participantGuids.begin(), record.participantGuids.end());
        for (PacketRecord const& packet : record.packets)
            used.insert(packet.sourceGuid);

        return used;
    }

    void RemapReplayGuids(MatchRecord& record)
    {
        if (record.participantGuids.empty())
            return;

        std::unordered_set<uint64> usedGuids = BuildUsedGuidSet(record);
        std::unordered_map<uint64, uint64> remap;
        remap.reserve(record.participantGuids.size());

        for (uint64 guid : record.participantGuids)
        {
            if (guid == 0)
                continue;

            if (remap.find(guid) != remap.end())
                continue;

            uint64 ghostGuid = GenerateGhostGuid(guid, usedGuids);
            usedGuids.insert(ghostGuid);
            remap.emplace(guid, ghostGuid);
        }

        for (uint64& guid : record.participantGuids)
        {
            auto it = remap.find(guid);
            if (it != remap.end())
                guid = it->second;
        }

        record.guidRemap = std::move(remap);

        LOG_INFO("modules", "ArenaReplay: remapped {} participant GUIDs ({} packets) to ghost GUIDs",
            record.guidRemap.size(),
            record.packets.size());

        for (PacketRecord& packet : record.packets)
        {
            auto sourceIt = record.guidRemap.find(packet.sourceGuid);
            if (sourceIt != record.guidRemap.end())
                packet.sourceGuid = sourceIt->second;

            for (auto const& entry : record.guidRemap)
                ReplaceGuidInPacket(packet.packet, entry.first, entry.second);
        }
    }
}

class ArenaReplayServerScript : public ServerScript
{
public:
    ArenaReplayServerScript() : ServerScript("ArenaReplayServerScript", {
        SERVERHOOK_CAN_PACKET_SEND
        }) {
    }

    bool CanPacketSend(WorldSession* session, WorldPacket& packet) override
    {
        if (session == nullptr || session->GetPlayer() == nullptr)
            return true;

        Battleground* bg = session->GetPlayer()->GetBattleground();

        if (!bg)
            return true;

        const bool isReplay = bgReplayIds.find(bg->GetInstanceID()) != bgReplayIds.end();

        // ignore packet when no bg or casual games
        if (isReplay)
            return true;

        // ignore packets until arena started
        if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS)
            return true;

        // record packets from 1 player of each team
        // iterate just in case a player leaves and used as reference
        for (auto it : bg->GetPlayers())
        {
            if (it.second->GetBgTeamId() == session->GetPlayer()->GetBgTeamId())
            {
                if (it.second->GetGUID() != session->GetPlayer()->GetGUID())
                    return true;
                else
                    break;
            }
        }

        // ignore packets not in watch list
        if (std::find(watchList.begin(), watchList.end(), packet.GetOpcode()) == watchList.end())
            return true;

        if (records.find(bg->GetInstanceID()) == records.end())
            records[bg->GetInstanceID()].packets.clear();
        MatchRecord& record = records[bg->GetInstanceID()];

        uint32 timestamp = bg->GetStartTime();
        record.typeId = bg->GetBgTypeID();
        record.arenaTypeId = bg->GetArenaType();
        record.mapId = bg->GetMapId();
        // push back packet inside queue of matchId 0
        record.packets.push_back({ timestamp, /* copy */ WorldPacket(packet), session->GetPlayer()->GetGUID().GetRawValue() });
        return true;
    }
};

class ArenaReplayArenaScript : public ArenaScript {
public:
    ArenaReplayArenaScript() : ArenaScript("ArenaReplayArenaScript", {
        ARENAHOOK_ON_BEFORE_CHECK_WIN_CONDITION
        }) {
    }

    bool OnBeforeArenaCheckWinConditions(Battleground* const bg) override {
        const bool isReplay = bgReplayIds.find(bg->GetInstanceID()) != bgReplayIds.end();

        // if isReplay then return false to exit from check condition
        return !isReplay;
    }
};

class ArenaReplayBGScript : public BGScript
{
public:
    ArenaReplayBGScript() : BGScript("ArenaReplayBGScript", {
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_UPDATE,
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER,
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END
        }) {
    }

    void OnBattlegroundUpdate(Battleground* bg, uint32 /* diff */) override
    {
        const bool isReplay = bgReplayIds.find(bg->GetInstanceID()) != bgReplayIds.end();
        if (!isReplay)
            return;

        if (!bg->isArena() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveBattlegrounds", true))
            return;

        if (!bg->isRated() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveUnratedArenas", true))
            return;

        uint32 replayId = bgReplayIds.at(bg->GetInstanceID());

        int32 startDelayTime = bg->GetStartDelayTime();
        if (startDelayTime > 1000) // reduces StartTime only when watching Replay
        {
            bg->SetStartDelayTime(1000);
            bg->SetStartTime(bg->GetStartTime() + (startDelayTime - 1000));
        }

        if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS)
            return;

        // retrieve arena replay data
        auto it = loadedReplays.find(replayId);
        if (it == loadedReplays.end())
            return;

        MatchRecord& match = it->second;
        if (!match.debugLoggedStart)
        {
            LOG_INFO("modules", "ArenaReplay: replay {} starting on bg instance {} map {} arenaType {} packets {} participants {} startTime {}",
                replayId,
                bg->GetInstanceID(),
                match.mapId,
                match.arenaTypeId,
                match.packets.size(),
                match.participantGuids.size(),
                bg->GetStartTime());
            match.debugLoggedStart = true;
        }

        // if replay ends or spectator left > free arena replay data and/or kick player
        if (match.packets.empty() || bg->GetPlayers().empty())
        {
            loadedReplays.erase(it);

            if (!bg->GetPlayers().empty())
                bg->GetPlayers().begin()->second->LeaveBattleground(bg);

            return;
        }

        //send replay data to spectator
        const uint64 observerRealGuid = bg->GetPlayers().empty() ? 0 : bg->GetPlayers().begin()->second->GetGUID().GetRawValue();
        bool observerIsParticipant = false;
        uint64 observerGhostGuid = observerRealGuid;
        auto remapIt = match.guidRemap.find(observerRealGuid);
        if (remapIt != match.guidRemap.end())
        {
            observerGhostGuid = remapIt->second;
            observerIsParticipant = true;
        }

        while (!match.packets.empty() && match.packets.front().timestamp <= bg->GetStartTime())
        {
            if (bg->GetPlayers().empty())
                break;

            PacketRecord const& packetRecord = match.packets.front();
            if (observerIsParticipant && packetRecord.sourceGuid != 0 && packetRecord.sourceGuid == observerGhostGuid)
            {
                if (match.debugPacketsLogged < 50)
                {
                    LOG_INFO("modules", "ArenaReplay: skipping packet opcode {} size {} ts {} sourceGuid {} (matches observer ghost guid {} real {})",
                        packetRecord.packet.GetOpcode(),
                        packetRecord.packet.size(),
                        packetRecord.timestamp,
                        packetRecord.sourceGuid,
                        observerGhostGuid,
                        observerRealGuid);
                    ++match.debugPacketsLogged;
                }
                match.packets.pop_front();
                continue;
            }

            WorldPacket const* myPacket = &packetRecord.packet;
            Player* replayer = bg->GetPlayers().begin()->second;
            if (match.debugPacketsLogged < 50)
            {
                LOG_INFO("modules", "ArenaReplay: sending packet opcode {} size {} ts {} sourceGuid {} to observer guid {}",
                    myPacket->GetOpcode(),
                    myPacket->size(),
                    packetRecord.timestamp,
                    packetRecord.sourceGuid,
                    observerRealGuid);
                ++match.debugPacketsLogged;
            }
            replayer->GetSession()->SendPacket(myPacket);
            match.packets.pop_front();
        }
    }

    void OnBattlegroundAddPlayer(Battleground* bg, Player* player) override
    {
        if (!player)
            return;

        if (player->IsSpectator())
            return;

        if (!bg->isArena() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveBattlegrounds", true))
            return;

        if (!bg->isRated() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveUnratedArenas", true))
            return;

        if (bgPlayersGuids.find(bg->GetInstanceID()) == bgPlayersGuids.end())
        {
            BgPlayersGuids playerguids;
            bgPlayersGuids[bg->GetInstanceID()] = playerguids;
        }

        std::string playerGuid = std::to_string(player->GetGUID().GetRawValue());
        TeamId bgTeamId = player->GetBgTeamId();

        if (bgTeamId == TEAM_ALLIANCE)
        {
            if (!bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids.empty())
                bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids += ", ";

            bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids += playerGuid;
        }
        else
        {
            if (!bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids.empty())
                bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids += ", ";

            bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids += playerGuid;
        }
    }

    void OnBattlegroundEnd(Battleground* bg, TeamId winnerTeamId) override {

        if (!bg->isArena() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveBattlegrounds", true))
            return;

        if (!bg->isRated() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveUnratedArenas", true))
            return;

        const bool isReplay = bgReplayIds.find(bg->GetInstanceID()) != bgReplayIds.end();

        // only saves if arena lasted at least X secs (StartDelayTime is included - 60s StartDelayTime + X StartTime)
        uint32 ValidArenaDuration = sConfigMgr->GetOption<uint32>("ArenaReplay.ValidArenaDuration", 75) * IN_MILLISECONDS;
        bool ValidArena = (bg->GetStartTime()) >= ValidArenaDuration || sConfigMgr->GetOption<uint32>("ArenaReplay.ValidArenaDuration", 75) == 0;

        // save replay when a bg ends
        if (!isReplay && ValidArena)
        {
            saveReplay(bg, winnerTeamId);
            return;
        }

        bgReplayIds.erase(bg->GetInstanceID());
        bgPlayersGuids.erase(bg->GetInstanceID());
    }

    void saveReplay(Battleground* bg, TeamId winnerTeamId)
    {
        // retrieve replay data
        auto it = records.find(bg->GetInstanceID());
        if (it == records.end())
            return;

        MatchRecord& match = it->second;

        /** serialize arena replay data **/
        ArenaReplayByteBuffer buffer;
        uint32 headerSize;
        uint32 timestamp;
        for (auto const& packetRecord : match.packets)
        {
            headerSize = packetRecord.packet.size(); //header 4Bytes packet size
            timestamp = packetRecord.timestamp;

            const bool hasSourceGuid = packetRecord.sourceGuid != 0;
            uint32 sizeWithFlag = headerSize;
            if (hasSourceGuid)
                sizeWithFlag |= 0x80000000u;

            buffer << sizeWithFlag; // 4 bytes
            buffer << timestamp; // 4 bytes
            buffer << packetRecord.packet.GetOpcode(); // 2 bytes

            if (hasSourceGuid)
                buffer << packetRecord.sourceGuid; // 8 bytes

            if (headerSize > 0)
                buffer.append(packetRecord.packet.contents(), packetRecord.packet.size()); // headerSize bytes
        }

        uint32 teamWinnerRating = 0;
        uint32 teamLoserRating = 0;
        uint32 teamWinnerMMR = 0;
        uint32 teamLoserMMR = 0;
        std::string teamWinnerName;
        std::string teamLoserName;
        std::string winnerGuids;
        std::string loserGuids;

        if (winnerTeamId == TEAM_ALLIANCE)
        {
            winnerGuids = bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids;
            loserGuids = bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids;
        }
        else
        {
            loserGuids = bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids;
            winnerGuids = bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids;
        }

        for (const auto& playerPair : bg->GetPlayers())
        {
            Player* player = playerPair.second;
            if (!player || player->IsSpectator())
                continue;

            std::string playerGuid = std::to_string(player->GetGUID().GetRawValue());
            TeamId bgTeamId = player->GetBgTeamId();
            ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(bg->GetArenaTeamIdForTeam(bgTeamId));
            uint32 arenaTeamId = bg->GetArenaTeamIdForTeam(bgTeamId);
            TeamId teamId = static_cast<TeamId>(arenaTeamId);
            uint32 teamMMR = bg->GetArenaMatchmakerRating(teamId);

            if (bgTeamId == winnerTeamId)
            {
                getTeamInformation(bg, team, teamWinnerName, teamWinnerRating);
                teamWinnerMMR = teamMMR;
            }
            else // Loss
            {
                getTeamInformation(bg, team, teamLoserName, teamLoserRating);
                teamLoserMMR = teamMMR;
            }

            // Send replay ID to player after a game end
            uint32 replayfightid = 0;
            QueryResult qResult = CharacterDatabase.Query("SELECT MAX(`id`) AS max_id FROM `character_arena_replays`");
            if (qResult)
            {
                do
                {
                    replayfightid = qResult->Fetch()[0].Get<uint32>();
                } while (qResult->NextRow());
            }
            ChatHandler(player->GetSession()).PSendSysMessage("Replay saved. Match ID: {}", replayfightid + 1);
        }

        const uint8 ARENA_TYPE_3V3_SOLO_QUEUE = sConfigMgr->GetOption<uint8>("ArenaReplay.3v3soloQ.ArenaType", 4);
        if (bg->isArena() && (!bg->isRated() || bg->GetArenaType() == ARENA_TYPE_3V3_SOLO_QUEUE))
        {
            teamWinnerName = GetTeamName(winnerGuids);
            teamLoserName = GetTeamName(loserGuids);
        }
        else if (!bg->isArena())
        {
            teamWinnerName = "Battleground";
            teamLoserName = "Battleground";
        }

        // // if loser has a negative value. the uint variable could return this (wrong) value
        // if (teamLoserMMR >= 4294967286)
        //     teamLoserMMR=0;

        // if (teamWinnerMMR >= 4294967286)
        //     teamWinnerMMR=0;

        // temporary code until the issue is not properly fixed
        teamLoserMMR = 0;
        teamWinnerMMR = 0;

        CharacterDatabase.Execute("INSERT INTO `character_arena_replays` "
            //   1             2            3            4          5          6                  7                    8
            "(`arenaTypeId`, `typeId`, `contentSize`, `contents`, `mapId`, `winnerTeamName`, `winnerTeamRating`, `winnerTeamMMR`, "
            //    9                10                 11                 12                 13
            "`loserTeamName`, `loserTeamRating`, `loserTeamMMR`, `winnerPlayerGuids`, `loserPlayerGuids`) "

            "VALUES ({}, {}, {}, \"{}\", {}, '{}', {}, {}, '{}', {}, {}, \"{}\", \"{}\")",
            //       1   2    3     4    5    6    7   8    9    10  11    12      13

            uint32(match.arenaTypeId), // 1
            uint32(match.typeId),      // 2
            buffer.size(),             // 3
            Acore::Encoding::Base32::Encode(buffer.contentsAsVector()), // 4
            bg->GetMapId(),    // 5
            teamWinnerName,    // 6
            teamWinnerRating,  // 7
            teamWinnerMMR,     // 8
            teamLoserName,     // 9
            teamLoserRating,   // 10
            teamLoserMMR,      // 11
            winnerGuids,       // 12
            loserGuids         // 13
        );

        records.erase(it);
    }

private:
    void getTeamInformation(Battleground* bg, ArenaTeam* team, std::string& teamName, uint32& teamRating) {
        if (bg->isRated() && team)
        {
            if (team->GetId() < 0xFFF00000)
            {
                teamName = team->GetName();
                teamRating = team->GetRating();
            }
        }
    }

    std::string GetTeamName(std::string listPlayerGuids) {
        std::string teamName;
        std::stringstream ssPlayerGuids(listPlayerGuids);

        std::vector<std::string> playerGuids;
        std::string playerGuid;
        while (std::getline(ssPlayerGuids, playerGuid, ','))
            playerGuids.push_back(playerGuid);

        for (const std::string& guid : playerGuids)
        {
            uint64 _guid = std::stoi(guid);
            CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByGuid(ObjectGuid(_guid));
            if (playerData)
                teamName += playerData->Name + " ";
        }

        // truncate last character if space
        if (!teamName.empty() && teamName.substr(teamName.size() - 1, teamName.size()) == " ") {
            teamName.pop_back();
        }

        return teamName;
    }
};

enum ReplayGossips
{
    REPLAY_LATEST_2V2 = 1,
    REPLAY_LATEST_3V3 = 2,
    REPLAY_LATEST_5V5 = 3,
    REPLAY_LATEST_3V3SOLO = 4,
    REPLAY_LATEST_1V1 = 5,
    REPLAY_MATCH_ID = 6,
    REPLAY_LIST_BY_PLAYERNAME = 7,
    MY_FAVORITE_MATCHES = 8,
    REPLAY_TOP_2V2_ALLTIME = 9,
    REPLAY_TOP_3V3_ALLTIME = 10,
    REPLAY_TOP_5V5_ALLTIME = 11,
    REPLAY_TOP_3V3SOLO_ALLTIME = 12,
    REPLAY_TOP_1V1_ALLTIME = 13,
    REPLAY_MOST_WATCHED_ALLTIME = 14
};

class ReplayGossip : public CreatureScript
{
public:

    ReplayGossip() : CreatureScript("ReplayGossip") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!sConfigMgr->GetOption<bool>("ArenaReplay.Enable", true))
        {
            ChatHandler(player->GetSession()).SendSysMessage("Arena Replay disabled!");
            return true;
        }

        const bool isArena1v1Enabled = sConfigMgr->GetOption<bool>("ArenaReplay.1v1.Enable", false);
        const bool isArena3v3soloQEnabled = sConfigMgr->GetOption<bool>("ArenaReplay.3v3soloQ.Enable", false);

        if (isArena1v1Enabled)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 1v1 games of the last 30 days", GOSSIP_SENDER_MAIN, REPLAY_LATEST_1V1);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 2v2 games of the last 30 days", GOSSIP_SENDER_MAIN, REPLAY_LATEST_2V2);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 3v3 games of the last 30 days", GOSSIP_SENDER_MAIN, REPLAY_LATEST_3V3);

        if (isArena3v3soloQEnabled)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 3v3 Solo games of the last 30 days", GOSSIP_SENDER_MAIN, REPLAY_LATEST_3V3SOLO);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 5v5 games of the last 30 days", GOSSIP_SENDER_MAIN, REPLAY_LATEST_5V5);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Replay a Match ID", GOSSIP_SENDER_MAIN, REPLAY_MATCH_ID, "", 0, true);             // maybe add command .replay 'replayID' aswell
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Replay list by player name", GOSSIP_SENDER_MAIN, REPLAY_LIST_BY_PLAYERNAME, "", 0, true); // to do: show a list, showing games with type, teamname and teamrating
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "My favorite matches", GOSSIP_SENDER_MAIN, MY_FAVORITE_MATCHES);                   // To do: somehow show teamName/TeamRating/Classes (it's a different db table)

        if (isArena1v1Enabled)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 1v1 games of all time", GOSSIP_SENDER_MAIN, REPLAY_TOP_1V1_ALLTIME);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 2v2 games of all time", GOSSIP_SENDER_MAIN, REPLAY_TOP_2V2_ALLTIME);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 3v3 games of all time", GOSSIP_SENDER_MAIN, REPLAY_TOP_3V3_ALLTIME);

        if (isArena3v3soloQEnabled)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 3v3 Solo games of all time", GOSSIP_SENDER_MAIN, REPLAY_TOP_3V3SOLO_ALLTIME);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay top 5v5 games of all time", GOSSIP_SENDER_MAIN, REPLAY_TOP_5V5_ALLTIME);


        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay most watched games of all time", GOSSIP_SENDER_MAIN, REPLAY_MOST_WATCHED_ALLTIME);  // To Do: show arena type + watchedTimes, maybe hide team name
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());

        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /* sender */, uint32 action) override
    {
        const uint8 ARENA_TYPE_1v1 = sConfigMgr->GetOption<uint8>("ArenaReplay.1v1.ArenaType", 1);
        const uint8 ARENA_TYPE_3V3_SOLO_QUEUE = sConfigMgr->GetOption<uint8>("ArenaReplay.3v3soloQ.ArenaType", 4);

        player->PlayerTalkClass->ClearMenus();
        switch (action)
        {
        case REPLAY_LATEST_2V2:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysLast30Days(player, creature, ARENA_TYPE_2v2);
            break;
        case REPLAY_LATEST_3V3:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysLast30Days(player, creature, ARENA_TYPE_3v3);
            break;
        case REPLAY_LATEST_5V5:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysLast30Days(player, creature, ARENA_TYPE_5v5);
            break;
        case REPLAY_LATEST_3V3SOLO:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysLast30Days(player, creature, ARENA_TYPE_3V3_SOLO_QUEUE);
            break;
        case REPLAY_LATEST_1V1:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysLast30Days(player, creature, ARENA_TYPE_1v1);
            break;
        case REPLAY_TOP_2V2_ALLTIME:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysAllTime(player, creature, ARENA_TYPE_2v2);
            break;
        case REPLAY_TOP_3V3_ALLTIME:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysAllTime(player, creature, ARENA_TYPE_3v3);
            break;
        case REPLAY_TOP_5V5_ALLTIME:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysAllTime(player, creature, ARENA_TYPE_5v5);
            break;
        case REPLAY_TOP_3V3SOLO_ALLTIME:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysAllTime(player, creature, ARENA_TYPE_3V3_SOLO_QUEUE);
            break;
        case REPLAY_TOP_1V1_ALLTIME:
            player->PlayerTalkClass->SendCloseGossip();
            ShowReplaysAllTime(player, creature, ARENA_TYPE_1v1);
            break;
        case REPLAY_MOST_WATCHED_ALLTIME:
            player->PlayerTalkClass->SendCloseGossip();
            ShowMostWatchedReplays(player, creature);
            break;
        case MY_FAVORITE_MATCHES:
            player->PlayerTalkClass->SendCloseGossip();
            ShowSavedReplays(player, creature);
            break;
        case GOSSIP_ACTION_INFO_DEF: // "Back"
            OnGossipHello(player, creature);
            break;

        default:
            if (action >= GOSSIP_ACTION_INFO_DEF + 30) // Replay selected arenas (intid >= 30)
                return replayArenaMatch(player, action - (GOSSIP_ACTION_INFO_DEF + 30));
        }

        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* /* creature */, uint32 /* sender */, uint32 action, const char* code) override
    {
        if (!code)
        {
            CloseGossipMenuFor(player);
            return false;
        }

        // Forbidden: ', %, and , (' causes crash when using 'Replay list by player name')
        std::string inputCode = std::string(code);
        if (inputCode.find('\'') != std::string::npos ||
            inputCode.find('%') != std::string::npos ||
            inputCode.find(',') != std::string::npos ||
            inputCode.length() > 50 ||
            inputCode.empty())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Invalid input.");
            CloseGossipMenuFor(player);
            return false;
        }

        switch (action)
        {
        case REPLAY_MATCH_ID:
        {
            uint32 replayId;
            try
            {
                replayId = std::stoi(code);
            }
            catch (...)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Invalid Match ID.");
                CloseGossipMenuFor(player);
                return false;
            }

            return replayArenaMatch(player, replayId);
        }
        case REPLAY_LIST_BY_PLAYERNAME:
        {
            CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByName(std::string(code));
            if (!playerData)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("No player found with the name: {}", std::string(code));
                CloseGossipMenuFor(player);
                return false;
            }

            std::string playerGuidStr = std::to_string(playerData->Guid.GetRawValue());

            QueryResult result = CharacterDatabase.Query("SELECT id FROM character_arena_replays WHERE winnerPlayerGuids LIKE '%{}%' OR loserPlayerGuids LIKE '%{}%'", playerGuidStr, playerGuidStr);
            if (result)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Replays found for player: {}", std::string(code));
                do
                {
                    Field* fields = result->Fetch();
                    uint32 replayId = fields[0].Get<uint32>();
                    ChatHandler(player->GetSession()).PSendSysMessage("Replay ID: {}", replayId);
                    //AddGossipItemFor(player,) // to do: add gossips with the replays
                } while (result->NextRow());

                CloseGossipMenuFor(player);
                return true;
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage("No replays found for player: {}", std::string(code));
                CloseGossipMenuFor(player);
                return false;
            }
        }
        case MY_FAVORITE_MATCHES:
        {
            try
            {
                uint32 NumberTyped = std::stoi(code);
                FavoriteMatchId(player->GetGUID().GetCounter(), NumberTyped);
                return true;
            }
            catch (...)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Invalid Match ID.");
                CloseGossipMenuFor(player);
                return false;
            }
        }
        }

        return false;
    }

private:

    std::string GetClassIconById(uint8 id)
    {
        switch (id)
        {
        case CLASS_WARRIOR:
            return "|TInterface\\icons\\inv_sword_27";
        case CLASS_PALADIN:
            return "|TInterface\\icons\\inv_hammer_01";
        case CLASS_HUNTER:
            return "|TInterface\\icons\\inv_weapon_bow_07";
        case CLASS_ROGUE:
            return "|TInterface\\icons\\inv_throwingknife_04";
        case CLASS_PRIEST:
            return "|TInterface\\icons\\inv_staff_30";
        case CLASS_DEATH_KNIGHT:
            return "|TInterface\\icons\\spell_deathknight_classicon";
        case CLASS_SHAMAN:
            return "|TInterface\\icons\\inv_jewelry_talisman_04";
        case CLASS_MAGE:
            return "|TInterface\\icons\\inv_staff_13";
        case CLASS_WARLOCK:
            return "|TInterface\\icons\\spell_nature_drowsy";
        case CLASS_DRUID:
            return "|TInterface\\icons\\inv_misc_monsterclaw_04";
        default:
            return "";
        }
    }

    std::string GetRaceIconById(uint8 id, uint8 gender) {
        const std::string gender_icon = gender == GENDER_MALE ? "male" : "female";
        switch (id) {
        case RACE_HUMAN:
            return "|TInterface/ICONS/achievement_character_human_" + gender_icon;
        case RACE_ORC:
            return "|TInterface/ICONS/achievement_character_orc_" + gender_icon;
        case RACE_DWARF:
            return "|TInterface/ICONS/achievement_character_dwarf_" + gender_icon;
        case RACE_NIGHTELF:
            return "|TInterface/ICONS/achievement_character_nightelf_" + gender_icon;
        case RACE_UNDEAD_PLAYER:
            return "|TInterface/ICONS/achievement_character_undead_" + gender_icon;
        case RACE_TAUREN:
            return "|TInterface/ICONS/achievement_character_tauren_" + gender_icon;
        case RACE_GNOME:
            return "|TInterface/ICONS/achievement_character_gnome_" + gender_icon;
        case RACE_TROLL:
            return "|TInterface/ICONS/achievement_character_troll_" + gender_icon;
        case RACE_BLOODELF:
            return "|TInterface/ICONS/achievement_character_bloodelf_" + gender_icon;
        case RACE_DRAENEI:
            return "|TInterface/ICONS/achievement_character_draenei_" + gender_icon;
        default:
            return "";
        }
    }

    std::string GetPlayersIconTexts(std::string playerGuids) {
        std::string iconsTextTeam;
        std::vector<std::string> playerGuidsTeam1;

        std::stringstream ssPlayerGuids(playerGuids);
        std::string item;

        while (std::getline(ssPlayerGuids, item, ','))
            playerGuidsTeam1.push_back(item);

        for (const std::string& guid : playerGuidsTeam1)
        {
            uint64 _guid = std::stoi(guid);
            CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByGuid(ObjectGuid(_guid));
            if (playerData)
            {
                iconsTextTeam += GetClassIconById(playerData->Class) + ":14:14:05:00|t|r";
                iconsTextTeam += GetRaceIconById(playerData->Race, playerData->Sex) + ":14:14:05:00|t|r ";
            }
        }

        if (!iconsTextTeam.empty() && iconsTextTeam.back() == '\n')
            iconsTextTeam.pop_back();

        return iconsTextTeam;
    }

    void AppendPlayerGuidsFromList(std::vector<uint64>& guids, std::string const& guidList)
    {
        if (guidList.empty())
            return;

        std::stringstream ss(guidList);
        std::string entry;
        while (std::getline(ss, entry, ','))
        {
            auto begin = entry.find_first_not_of(" \t\n\r");
            if (begin == std::string::npos)
                continue;

            auto end = entry.find_last_not_of(" \t\n\r");
            if (end == std::string::npos)
                continue;

            std::string trimmed = entry.substr(begin, end - begin + 1);
            if (trimmed.empty())
                continue;

            try
            {
                guids.push_back(std::stoull(trimmed));
            }
            catch (...)
            {
                continue;
            }
        }
    }

    struct ReplayInfo
    {
        uint32 matchId;

        std::string winnerPlayerGuids;
        std::string winnerTeamName;
        uint32 winnerTeamRating;

        std::string loserPlayerGuids;
        std::string loserTeamName;
        uint32 loserTeamRating;

        //uint32 winnerMMR;
        //uint32 loserMMR;
    };

    std::string GetGossipText(ReplayInfo info) {
        std::string iconsTextTeam1 = GetPlayersIconTexts(info.winnerPlayerGuids);
        std::string iconsTextTeam2 = GetPlayersIconTexts(info.loserPlayerGuids);

        std::string coloredWinnerTeamName = "|cff33691E" + info.winnerTeamName + "|r";
        std::string LoserTeamName = info.loserTeamName;

        std::string gossipText = ("[" + std::to_string(info.matchId) + "] (" +
            std::to_string(info.winnerTeamRating) + ")" +
            iconsTextTeam1 + "" +
            " '" + coloredWinnerTeamName + "'" +
            "\n vs (" + std::to_string(info.loserTeamRating) + ")" +
            iconsTextTeam2 + "" +
            " '" + LoserTeamName + "'");

        return gossipText;
    }

    void ShowReplaysAllTime(Player* player, Creature* creature, uint8 arenaTypeId)
    {
        auto matchInfos = loadReplaysAllTimeByArenaType(arenaTypeId);
        ShowReplays(player, creature, matchInfos);
    }

    std::vector<ReplayInfo> loadReplaysAllTimeByArenaType(uint8 arenaTypeId)
    {
        std::vector<ReplayInfo> records;
        QueryResult result = CharacterDatabase.Query("SELECT id, winnerTeamName, winnerTeamRating, winnerPlayerGuids, loserTeamName, loserTeamRating, loserPlayerGuids FROM character_arena_replays WHERE arenaTypeId = {} ORDER BY winnerTeamRating DESC LIMIT 20", arenaTypeId);

        if (!result)
            return records;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                return records;

            ReplayInfo info;
            info.matchId = fields[0].Get<uint32>();
            info.winnerTeamName = fields[1].Get<std::string>();
            info.winnerTeamRating = fields[2].Get<uint32>();
            info.winnerPlayerGuids = fields[3].Get<std::string>();
            info.loserTeamName = fields[4].Get<std::string>();
            info.loserTeamRating = fields[5].Get<uint32>();
            info.loserPlayerGuids = fields[6].Get<std::string>();

            records.push_back(info);
        } while (result->NextRow());

        return records;
    }

    void ShowReplaysLast30Days(Player* player, Creature* creature, uint8 arenaTypeId)
    {
        auto matchInfos = loadReplaysLast30Days(arenaTypeId);
        ShowReplays(player, creature, matchInfos);
    }

    std::vector<ReplayInfo> loadReplaysLast30Days(uint8 arenaTypeId)
    {
        std::vector<ReplayInfo> records;

        std::time_t now = std::time(nullptr);
        std::tm* tmNow = std::localtime(&now);
        tmNow->tm_mday -= 30;
        std::mktime(tmNow);

        std::stringstream ss;
        ss << std::put_time(tmNow, "%Y-%m-%d %H:%M:%S");
        std::string thirtyDaysAgo = ss.str();

        // Only show games that are 30 days old
        QueryResult result = CharacterDatabase.Query(
            "SELECT id, winnerTeamName, winnerTeamRating, winnerPlayerGuids, loserTeamName, loserTeamRating, loserPlayerGuids, timestamp "
            "FROM character_arena_replays "
            "WHERE arenaTypeId = {} AND timestamp >= '{}' "
            "ORDER BY id DESC LIMIT 20", arenaTypeId, thirtyDaysAgo.c_str());

        if (!result)
            return records;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                return records;

            ReplayInfo info;
            info.matchId = fields[0].Get<uint32>();
            info.winnerTeamName = fields[1].Get<std::string>();
            info.winnerTeamRating = fields[2].Get<uint32>();
            info.winnerPlayerGuids = fields[3].Get<std::string>();
            info.loserTeamName = fields[4].Get<std::string>();
            info.loserTeamRating = fields[5].Get<uint32>();
            info.loserPlayerGuids = fields[6].Get<std::string>();

            records.push_back(info);
        } while (result->NextRow());

        return records;
    }

    void ShowMostWatchedReplays(Player* player, Creature* creature)
    {
        auto matchInfos = loadMostWatchedReplays();
        ShowReplays(player, creature, matchInfos);
    }

    void ShowReplays(Player* player, Creature* creature, std::vector<ReplayInfo> matchInfos) {
        if (matchInfos.empty())
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, "No replays found.", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        else
        {
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "[Replay ID] (Team Rating) 'Team Name'\n----------------------------------------------", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF); // Back to Main Menu
            for (const auto& info : matchInfos)
            {
                const std::string gossipText = GetGossipText(info);
                const uint32 actionOffset = GOSSIP_ACTION_INFO_DEF + 30;
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, gossipText, GOSSIP_SENDER_MAIN, actionOffset + info.matchId);
            }
        }

        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Back", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
    }

    std::vector<ReplayInfo> loadMostWatchedReplays()
    {
        std::vector<ReplayInfo> records;
        QueryResult result = CharacterDatabase.Query(
            "SELECT id, winnerTeamName, winnerTeamRating, winnerPlayerGuids, loserTeamName, loserTeamRating, loserPlayerGuids "
            "FROM character_arena_replays "
            "ORDER BY timesWatched DESC, winnerTeamRating DESC "
            "LIMIT 28");

        if (!result)
            return records;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                return records;

            ReplayInfo info;
            info.matchId = fields[0].Get<uint32>();
            info.winnerTeamName = fields[1].Get<std::string>();
            info.winnerTeamRating = fields[2].Get<uint32>();
            info.winnerPlayerGuids = fields[3].Get<std::string>();
            info.loserTeamName = fields[4].Get<std::string>();
            info.loserTeamRating = fields[5].Get<uint32>();
            info.loserPlayerGuids = fields[6].Get<std::string>();

            records.push_back(info);
        } while (result->NextRow());

        return records;
    }

    void ShowSavedReplays(Player* player, Creature* creature, bool firstPage = true)
    {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Favorite a Match ID", GOSSIP_SENDER_MAIN, MY_FAVORITE_MATCHES, "", 0, true);

        std::string sortOrder = (firstPage) ? "ASC" : "DESC";
        QueryResult result = CharacterDatabase.Query("SELECT replay_id FROM character_saved_replays WHERE character_id = " + std::to_string(player->GetGUID().GetCounter()) + " ORDER BY id " + sortOrder + " LIMIT 29");
        if (!result)
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, "No saved replays found.", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        else
        {
            do
            {
                Field* fields = result->Fetch();
                if (!fields)
                    break;

                uint32 matchId = fields[0].Get<uint32>();
                const uint32 actionOffset = GOSSIP_ACTION_INFO_DEF + 30;
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay match " + std::to_string(matchId), GOSSIP_SENDER_MAIN, actionOffset + matchId);

            } while (result->NextRow());

            if (firstPage) // to do
            {
                //AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Next Page", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);
            }
        }
        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Back", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
    }

    void FavoriteMatchId(uint64 playerGuid, uint32 code)
    {
        // Need to check if the match exists in character_arena_replays, then insert in character_saved_replays
        QueryResult result = CharacterDatabase.Query("SELECT id FROM character_arena_replays WHERE id = " + std::to_string(code));
        if (result)
        {
            std::string query = "INSERT INTO character_saved_replays (character_id, replay_id) VALUES (" + std::to_string(playerGuid) + ", " + std::to_string(code) + ")";
            CharacterDatabase.Execute(query.c_str());

            if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(playerGuid)))
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Replay match ID {} saved.", code);
                CloseGossipMenuFor(player);
            }
        }
        else
        {
            if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(playerGuid)))
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Replay match ID {} does not exist.", code);
                CloseGossipMenuFor(player);
            }
        }
    }

    bool replayArenaMatch(Player* player, uint32 replayId)
    {
        auto handler = ChatHandler(player->GetSession());

        if (player->InBattlegroundQueue())
        {
            handler.PSendSysMessage("Can't be queued for arena or bg.");
            return false;
        }

        if (!loadReplayDataForPlayer(player, replayId))
        {
            CloseGossipMenuFor(player);
            return false;
        }

        MatchRecord record = loadedReplays[player->GetGUID().GetCounter()];

        Battleground* bg = sBattlegroundMgr->CreateNewBattleground(record.typeId, GetBattlegroundBracketByLevel(record.mapId, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)), record.arenaTypeId, false);
        if (!bg)
        {
            handler.PSendSysMessage("Couldn't create arena map!");
            handler.SetSentErrorMessage(true);
            return false;
        }

        bgReplayIds[bg->GetInstanceID()] = player->GetGUID().GetCounter();
        player->SetPendingSpectatorForBG(bg->GetInstanceID());
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
        handler.PSendSysMessage("Replay ID {} begins.", replayId);

        return true;
    }

    bool loadReplayDataForPlayer(Player* p, uint32 matchId)
    {
        QueryResult result = CharacterDatabase.Query("SELECT id, arenaTypeId, typeId, contentSize, contents, mapId, timesWatched, winnerPlayerGuids, loserPlayerGuids FROM character_arena_replays WHERE id = {}", matchId);
        if (!result)
        {
            ChatHandler(p->GetSession()).PSendSysMessage("Replay data not found.");
            CloseGossipMenuFor(p);
            return false;
        }

        Field* fields = result->Fetch();
        if (!fields)
        {
            ChatHandler(p->GetSession()).PSendSysMessage("Replay data not found.");
            CloseGossipMenuFor(p);
            return false;
        }

        MatchRecord record;
        if (!fields[7].IsNull())
            AppendPlayerGuidsFromList(record.participantGuids, fields[7].Get<std::string>());

        if (!fields[8].IsNull())
            AppendPlayerGuidsFromList(record.participantGuids, fields[8].Get<std::string>());

        deserializeMatchData(record, fields);

        // Update 'timesWatched' of a Replay +1 everytime someone watches it
        uint32 timesWatched = fields[6].Get<uint32>();
        timesWatched++;
        CharacterDatabase.Execute("UPDATE character_arena_replays SET timesWatched = {} WHERE id = {}", timesWatched, matchId);

        RemapReplayGuids(record);

        loadedReplays[p->GetGUID().GetCounter()] = std::move(record);
        return true;
    }

    void deserializeMatchData(MatchRecord& record, Field* fields)
    {
        record.arenaTypeId = uint8(fields[1].Get<uint32>());
        record.typeId = BattlegroundTypeId(fields[2].Get<uint32>());
        auto encodedData = Acore::Encoding::Base32::Decode(fields[4].Get<std::string>());
        if (!encodedData)
            return;

        record.mapId = uint32(fields[5].Get<uint32>());
        ByteBuffer buffer;
        if (!encodedData->empty())
            buffer.append(encodedData->data(), encodedData->size());

        /** deserialize replay binary data **/
        uint32 packedPacketSize;
        uint32 packetTimestamp;
        uint16 opcode;
        while (buffer.rpos() < buffer.size())
        {
            if (buffer.size() - buffer.rpos() < sizeof(uint32))
                break;

            buffer >> packedPacketSize;
            bool hasSourceGuid = (packedPacketSize & 0x80000000u) != 0;
            uint32 packetSize = packedPacketSize & 0x7FFFFFFFu;

            if (buffer.size() - buffer.rpos() < sizeof(uint32) + sizeof(uint16))
                break;

            buffer >> packetTimestamp;
            buffer >> opcode;

            uint64 sourceGuid = 0;
            if (hasSourceGuid)
            {
                if (buffer.size() - buffer.rpos() < sizeof(uint64))
                    break;

                buffer >> sourceGuid;
            }

            if (buffer.size() - buffer.rpos() < packetSize)
                break;

            WorldPacket packet(opcode, packetSize);

            if (packetSize > 0)
            {
                std::vector<uint8> tmp(packetSize, 0);
                buffer.read(&tmp[0], packetSize);
                packet.append(&tmp[0], packetSize);
            }

            record.packets.push_back({ packetTimestamp, packet, sourceGuid });
        }
    }
};

class ConfigLoaderArenaReplay : public WorldScript
{
public:
    ConfigLoaderArenaReplay() : WorldScript("config_loader_arena_replay", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
        }) {
    }
    virtual void OnAfterConfigLoad(bool /*Reload*/) override
    {
        DeleteOldReplays();
    }

private:
    void DeleteOldReplays()
    {
        // delete all the replays older than X days
        const auto days = sConfigMgr->GetOption<uint32>("ArenaReplay.DeleteReplaysAfterDays", 30);
        if (days > 0)
        {
            std::string addition = "";

            const bool deleteSavedReplays = sConfigMgr->GetOption<bool>("ArenaReplay.DeleteSavedReplays", false);

            if (!deleteSavedReplays)
                addition = "AND `id` NOT IN (SELECT `replay_id` FROM `character_saved_replays`)";

            const auto query = "DELETE FROM `character_arena_replays` WHERE `timestamp` < (NOW() - INTERVAL " + std::to_string(days) + " DAY) " + addition;
            CharacterDatabase.Execute(query);

            if (deleteSavedReplays)
                CharacterDatabase.Execute("DELETE FROM `character_saved_replays` WHERE `replay_id` NOT IN (SELECT `id` FROM `character_arena_replays`)");
        }
    }
};

void AddArenaReplayScripts()
{
    new ConfigLoaderArenaReplay();
    new ArenaReplayServerScript();
    new ArenaReplayBGScript();
    new ArenaReplayArenaScript();
    new ReplayGossip();
}
