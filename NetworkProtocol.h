#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace IslandsMP
{
    // v2: every packet carries the SteamID of the player it describes, because the host
    // relays client packets and Steam reports the host as the sender of relayed data.
    constexpr uint8_t PROTOCOL_VERSION = 6;

    enum class MessageType : uint8_t
    {
        PlayerState = 0x01, // unreliable, 10 Hz
        Appearance  = 0x02, // reliable, on change + every few seconds
        PlayerLeave = 0x03, // reliable, best effort before leaving
        PuzzleCursor = 0x04, // unreliable, ~15 Hz while in a grid puzzle
        Chat         = 0x05, // reliable, UTF-8 text after the header
    };

    enum class AnimState : uint8_t
    {
        Idle    = 0,
        Walking = 1,
        Running = 2,
        Jumping = 3,
        Falling = 4,
        Gliding = 5,
        Crouching = 6,
        CrouchWalking = 7,
        Count
    };

    #pragma pack(push, 1)

    struct MessageHeader
    {
        MessageType type;
        uint8_t version;
        uint64_t origin; // SteamID of the player this packet is about
    };

    struct PlayerStateData
    {
        float posX, posY, posZ; // capsule centre (actor location)
        float yaw;
        float velX, velY, velZ;
        AnimState animState;
        float meshZ; // body mesh height relative to the capsule centre (changes when crouching)
        float lookYaw, lookPitch; // camera (control rotation), degrees
    };

    struct PuzzleCursorData
    {
        uint32_t puzzleHash; // FNV-1a of the puzzle ID string; 0 = not in a puzzle
        float x, y, z;       // cursor point in the sender's puzzle-panel grid frame
    };

    #pragma pack(pop)

    static_assert(sizeof(MessageHeader) == 10, "MessageHeader must be 10 bytes");
    static_assert(sizeof(PlayerStateData) == 41, "PlayerStateData must be 41 bytes");

    constexpr size_t PLAYER_STATE_MSG_SIZE = sizeof(MessageHeader) + sizeof(PlayerStateData);
    constexpr size_t PUZZLE_CURSOR_MSG_SIZE = sizeof(MessageHeader) + sizeof(PuzzleCursorData);

    inline bool ReadHeader(const uint8_t* buf, size_t len, MessageHeader& out)
    {
        if (len < sizeof(MessageHeader)) return false;
        std::memcpy(&out, buf, sizeof(MessageHeader));
        return out.version == PROTOCOL_VERSION;
    }

    inline void WriteHeader(uint8_t* buf, MessageType type, uint64_t origin)
    {
        MessageHeader h{type, PROTOCOL_VERSION, origin};
        std::memcpy(buf, &h, sizeof(h));
    }

    inline MessageType GetMessageType(const uint8_t* buf, size_t len)
    {
        return len < 1 ? static_cast<MessageType>(0) : static_cast<MessageType>(buf[0]);
    }

    // --- Simple byte stream used for variable-size messages -----------------------

    class ByteWriter
    {
    public:
        std::vector<uint8_t> data;

        template<typename T> void Put(const T& v)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(&v);
            data.insert(data.end(), p, p + sizeof(T));
        }
        void PutString(const std::string& s)
        {
            uint16_t n = static_cast<uint16_t>(s.size() > 0xFFFF ? 0xFFFF : s.size());
            Put(n);
            data.insert(data.end(), s.begin(), s.begin() + n);
        }
    };

    class ByteReader
    {
    public:
        ByteReader(const uint8_t* d, size_t n) : m_d(d), m_n(n) {}

        template<typename T> bool Get(T& v)
        {
            if (m_pos + sizeof(T) > m_n) return Fail();
            std::memcpy(&v, m_d + m_pos, sizeof(T));
            m_pos += sizeof(T);
            return true;
        }
        bool GetString(std::string& s)
        {
            uint16_t n = 0;
            if (!Get(n) || m_pos + n > m_n) return Fail();
            s.assign(reinterpret_cast<const char*>(m_d + m_pos), n);
            m_pos += n;
            return true;
        }
        bool Ok() const { return m_ok; }
        size_t Pos() const { return m_pos; }

    private:
        bool Fail() { m_ok = false; return false; }
        const uint8_t* m_d;
        size_t m_n;
        size_t m_pos = 0;
        bool m_ok = true;
    };
}
