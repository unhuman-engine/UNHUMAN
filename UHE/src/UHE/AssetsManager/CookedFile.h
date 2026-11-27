#pragma once

#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE::Cooked
{
// ─── Cooked asset container ─────────────────────────────────────────────
// What the runtime loads instead of interchange formats (glTF, PNG): a
// versioned binary of named sections, each 16-byte aligned, zstd-compressed
// and checked by CRC-32.
//
//   header   magic "UHECOOK\0", format version, byte-order tag, asset type,
//            section count, source hash (what it was cooked from)
//   table    per section: id, codec, offset, stored size, raw size, CRC of raw
//   sections the data, at 16-byte aligned offsets
//
// Values are written in the cooking machine's byte order and the tag records
// it; a reader on a machine of the other order refuses the file (recook it
// for that platform) instead of reading garbage.

inline constexpr u32 kFormatVersion = 1;

/// @brief A four-character code ("MODL", "GEOM") as the u32 the file stores.
constexpr u32 FourCC(const char (&text)[5])
{
    return static_cast<u32>(static_cast<u8>(text[0])) | (static_cast<u32>(static_cast<u8>(text[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(text[2])) << 16) | (static_cast<u32>(static_cast<u8>(text[3])) << 24);
}

/// @brief The four characters of \p code, for messages and the inspector.
[[nodiscard]] UHE_API std::string FourCCName(u32 code);

/// @brief CRC-32 (IEEE) of \p size bytes.
[[nodiscard]] UHE_API u32 Crc32(const void* data, std::size_t size);

/// @brief A stable 64-bit hash (FNV-1a) of a file's bytes; 0 when it cannot be read.
[[nodiscard]] UHE_API u64 HashFile(const std::filesystem::path& path);

/// @brief The same hash of \p size bytes in memory: HashBytes of a file's contents equals HashFile.
[[nodiscard]] UHE_API u64 HashBytes(const void* data, std::size_t size, u64 hash = 1469598103934665603ull);

/// @brief Appends values to a byte buffer in this machine's byte order.
class UHE_API BinaryWriter
{
public:
    template <typename T> void Put(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "Put writes raw bytes");
        PutBytes(&value, sizeof(T));
    }
    void PutBytes(const void* data, std::size_t size);
    void PutString(const std::string& text);
    /// @brief A u64 count, then the elements' raw bytes.
    template <typename T> void PutVector(const std::vector<T>& values)
    {
        static_assert(std::is_trivially_copyable_v<T>, "PutVector writes raw bytes");
        Put<u64>(values.size());
        if (!values.empty())
            PutBytes(values.data(), values.size() * sizeof(T));
    }

    [[nodiscard]] const std::vector<u8>& Data() const { return m_Data; }
    [[nodiscard]] std::vector<u8> Take() { return std::move(m_Data); }

private:
    std::vector<u8> m_Data;
};

/**
 * @brief Reads values back, bounds-checked: reading past the end fails the reader.
 *
 * Every Get after a failure returns a default value, so a parser can read a
 * whole record and check Ok() once instead of after every field.
 */
class UHE_API BinaryReader
{
public:
    BinaryReader(const u8* data, std::size_t size) : m_Data(data), m_Size(size) {}
    explicit BinaryReader(const std::vector<u8>& data) : m_Data(data.data()), m_Size(data.size()) {}

    template <typename T> T Get()
    {
        static_assert(std::is_trivially_copyable_v<T>, "Get reads raw bytes");
        T value{};
        GetBytes(&value, sizeof(T));
        return value;
    }
    bool GetBytes(void* out, std::size_t size);
    std::string GetString();
    /// @brief Reads what PutVector wrote; a count larger than what remains fails the reader.
    template <typename T> bool GetVector(std::vector<T>& out)
    {
        static_assert(std::is_trivially_copyable_v<T>, "GetVector reads raw bytes");
        const u64 count = Get<u64>();
        if (!m_Ok || count > (m_Size - m_Pos) / (sizeof(T) == 0 ? 1 : sizeof(T)))
        {
            m_Ok = false;
            out.clear();
            return false;
        }
        out.resize(static_cast<std::size_t>(count));
        return count == 0 || GetBytes(out.data(), static_cast<std::size_t>(count) * sizeof(T));
    }
    /// @brief A count read from the file, refused when larger than \p limit (a damaged count would allocate forever).
    u64 GetCount(u64 limit);

    /// @brief Bytes not read yet: the bound for a count of records that take at least a byte each.
    [[nodiscard]] u64 Remaining() const { return m_Size - m_Pos; }
    [[nodiscard]] bool Ok() const { return m_Ok; }
    [[nodiscard]] bool AtEnd() const { return m_Pos == m_Size; }
    void Fail() { m_Ok = false; }

private:
    const u8* m_Data = nullptr;
    std::size_t m_Size = 0;
    std::size_t m_Pos = 0;
    bool m_Ok = true;
};

/// @brief How a section is stored.
enum class Codec : u32
{
    Raw = 0,
    Zstd = 1,
};

struct SectionInfo
{
    u32 Id = 0;
    Codec Compression = Codec::Raw;
    u64 Offset = 0;
    u64 StoredSize = 0;
    u64 RawSize = 0;
    u32 Crc = 0; ///< of the raw (decompressed) bytes
};

/// @brief Collects sections and writes the file.
class UHE_API Writer
{
public:
    /// @brief Adds a section; compressed with zstd unless that would not make it smaller.
    void AddSection(u32 id, std::vector<u8> raw, bool compress = true);
    /// @brief Writes header, table and sections; false (with \p error) when the file cannot be written.
    bool Write(const std::filesystem::path& path, u32 assetType, u64 sourceHash, std::string* error = nullptr) const;

private:
    struct Pending
    {
        SectionInfo Info;
        std::vector<u8> Stored;
    };
    std::vector<Pending> m_Sections;
};

/// @brief Opens a cooked file and hands out its sections, verified.
class UHE_API Reader
{
public:
    /// @brief Reads and checks the header and table; false (with \p error) for a damaged, foreign or newer file.
    bool Open(const std::filesystem::path& path, std::string* error = nullptr);
    /// @brief The same from bytes already in memory.
    bool OpenMemory(std::vector<u8> bytes, std::string* error = nullptr);

    [[nodiscard]] u32 AssetType() const { return m_AssetType; }
    [[nodiscard]] u32 Version() const { return m_Version; }
    [[nodiscard]] u64 SourceHash() const { return m_SourceHash; }
    [[nodiscard]] const std::vector<SectionInfo>& Sections() const { return m_Sections; }
    [[nodiscard]] bool HasSection(u32 id) const;

    /// @brief Decompresses section \p id and checks its CRC; false (with \p error) when missing or damaged.
    bool ReadSection(u32 id, std::vector<u8>& out, std::string* error = nullptr) const;

private:
    std::vector<u8> m_File;
    u32 m_AssetType = 0;
    u32 m_Version = 0;
    u64 m_SourceHash = 0;
    std::vector<SectionInfo> m_Sections;
};

} // namespace UHE::Cooked
