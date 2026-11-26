#include "UHE/AssetsManager/CookedFile.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <zstd.h>

namespace UHE::Cooked
{
namespace
{
constexpr char kMagic[8] = {'U', 'H', 'E', 'C', 'O', 'O', 'K', '\0'};
constexpr u32 kByteOrderTag = 0x01020304u; ///< reads back as 0x04030201 on the other byte order
constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kTableEntrySize = 40;
constexpr std::size_t kAlignment = 16;
constexpr u32 kMaxSections = 4096;
constexpr int kZstdLevel = 9; ///< level 19 cooked PondDemo in 90 s for a few percent; 9 is the knee

std::size_t AlignUp(std::size_t value)
{
    return (value + kAlignment - 1) / kAlignment * kAlignment;
}

void SetError(std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
}

const std::array<u32, 256>& CrcTable()
{
    static const std::array<u32, 256> table = []
    {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i)
        {
            u32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}
} // namespace

std::string FourCCName(u32 code)
{
    std::string name(4, ' ');
    for (int i = 0; i < 4; ++i)
    {
        const char c = static_cast<char>((code >> (8 * i)) & 0xFFu);
        name[static_cast<std::size_t>(i)] = (c >= 32 && c < 127) ? c : '?';
    }
    return name;
}

u32 Crc32(const void* data, std::size_t size)
{
    const auto& table = CrcTable();
    const auto* bytes = static_cast<const u8*>(data);
    u32 crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i)
        crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

u64 HashFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return 0;
    u64 hash = 1469598103934665603ull;
    std::array<char, 65536> buffer{};
    while (in)
    {
        in.read(buffer.data(), buffer.size());
        const std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i)
            hash = (hash ^ static_cast<u8>(buffer[static_cast<std::size_t>(i)])) * 1099511628211ull;
    }
    return hash;
}

// ── BinaryWriter / BinaryReader ─────────────────────────────────────────────

void BinaryWriter::PutBytes(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const u8*>(data);
    m_Data.insert(m_Data.end(), bytes, bytes + size);
}

void BinaryWriter::PutString(const std::string& text)
{
    Put<u32>(static_cast<u32>(text.size()));
    PutBytes(text.data(), text.size());
}

bool BinaryReader::GetBytes(void* out, std::size_t size)
{
    if (!m_Ok || size > m_Size - m_Pos)
    {
        m_Ok = false;
        std::memset(out, 0, size);
        return false;
    }
    std::memcpy(out, m_Data + m_Pos, size);
    m_Pos += size;
    return true;
}

std::string BinaryReader::GetString()
{
    const u32 size = Get<u32>();
    if (!m_Ok || size > m_Size - m_Pos)
    {
        m_Ok = false;
        return {};
    }
    std::string text(reinterpret_cast<const char*>(m_Data + m_Pos), size);
    m_Pos += size;
    return text;
}

u64 BinaryReader::GetCount(u64 limit)
{
    const u64 count = Get<u64>();
    if (count > limit)
    {
        m_Ok = false;
        return 0;
    }
    return count;
}

// ── Writer ──────────────────────────────────────────────────────────────────

void Writer::AddSection(u32 id, std::vector<u8> raw, bool compress)
{
    Pending section;
    section.Info.Id = id;
    section.Info.RawSize = raw.size();
    section.Info.Crc = Crc32(raw.data(), raw.size());
    section.Info.Compression = Codec::Raw;
    if (compress && !raw.empty())
    {
        std::vector<u8> packed(ZSTD_compressBound(raw.size()));
        const std::size_t size = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), kZstdLevel);
        if (ZSTD_isError(size) == 0 && size < raw.size())
        {
            packed.resize(size);
            section.Stored = std::move(packed);
            section.Info.Compression = Codec::Zstd;
        }
    }
    if (section.Info.Compression == Codec::Raw)
        section.Stored = std::move(raw);
    section.Info.StoredSize = section.Stored.size();
    m_Sections.push_back(std::move(section));
}

bool Writer::Write(const std::filesystem::path& path, u32 assetType, u64 sourceHash, std::string* error) const
{
    BinaryWriter header;
    header.PutBytes(kMagic, sizeof(kMagic));
    header.Put<u32>(kFormatVersion);
    header.Put<u32>(kByteOrderTag);
    header.Put<u32>(assetType);
    header.Put<u32>(static_cast<u32>(m_Sections.size()));
    header.Put<u64>(sourceHash);
    std::vector<u8> file = header.Take();
    file.resize(kHeaderSize, 0);

    // Table, then each section at the next aligned offset.
    std::size_t offset = AlignUp(kHeaderSize + m_Sections.size() * kTableEntrySize);
    BinaryWriter table;
    for (const Pending& section : m_Sections)
    {
        table.Put<u32>(section.Info.Id);
        table.Put<u32>(static_cast<u32>(section.Info.Compression));
        table.Put<u64>(offset);
        table.Put<u64>(section.Info.StoredSize);
        table.Put<u64>(section.Info.RawSize);
        table.Put<u32>(section.Info.Crc);
        table.Put<u32>(0);
        offset = AlignUp(offset + section.Stored.size());
    }
    file.insert(file.end(), table.Data().begin(), table.Data().end());
    for (const Pending& section : m_Sections)
    {
        file.resize(AlignUp(file.size()), 0);
        file.insert(file.end(), section.Stored.begin(), section.Stored.end());
    }

    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    // Written beside and renamed over: a reader never sees half a file.
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        if (!out)
        {
            SetError(error, "cannot write '" + temporary.string() + "'");
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        std::filesystem::remove(temporary, ec);
        SetError(error, "cannot replace '" + path.string() + "'");
        return false;
    }
    return true;
}

// ── Reader ──────────────────────────────────────────────────────────────────

bool Reader::Open(const std::filesystem::path& path, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        SetError(error, "cannot open '" + path.string() + "'");
        return false;
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return OpenMemory(std::move(bytes), error);
}

bool Reader::OpenMemory(std::vector<u8> bytes, std::string* error)
{
    m_File = std::move(bytes);
    m_Sections.clear();
    if (m_File.size() < kHeaderSize || std::memcmp(m_File.data(), kMagic, sizeof(kMagic)) != 0)
    {
        SetError(error, "not a cooked asset");
        return false;
    }
    BinaryReader header(m_File.data() + sizeof(kMagic), kHeaderSize - sizeof(kMagic));
    m_Version = header.Get<u32>();
    const u32 order = header.Get<u32>();
    m_AssetType = header.Get<u32>();
    const u32 count = header.Get<u32>();
    m_SourceHash = header.Get<u64>();
    if (order != kByteOrderTag)
    {
        SetError(error, "cooked for the other byte order; recook it for this platform");
        return false;
    }
    if (m_Version == 0 || m_Version > kFormatVersion)
    {
        SetError(error, "format version " + std::to_string(m_Version) + " is newer than this engine reads (" +
                            std::to_string(kFormatVersion) + "); recook it");
        return false;
    }
    if (count > kMaxSections || kHeaderSize + std::size_t{count} * kTableEntrySize > m_File.size())
    {
        SetError(error, "damaged section table");
        return false;
    }
    BinaryReader table(m_File.data() + kHeaderSize, std::size_t{count} * kTableEntrySize);
    for (u32 i = 0; i < count; ++i)
    {
        SectionInfo info;
        info.Id = table.Get<u32>();
        info.Compression = static_cast<Codec>(table.Get<u32>());
        info.Offset = table.Get<u64>();
        info.StoredSize = table.Get<u64>();
        info.RawSize = table.Get<u64>();
        info.Crc = table.Get<u32>();
        (void)table.Get<u32>();
        const bool known = info.Compression == Codec::Raw || info.Compression == Codec::Zstd;
        if (!known || info.Offset > m_File.size() || info.StoredSize > m_File.size() - info.Offset)
        {
            SetError(error, "section '" + FourCCName(info.Id) + "' lies outside the file or has an unknown codec");
            return false;
        }
        m_Sections.push_back(info);
    }
    return true;
}

bool Reader::HasSection(u32 id) const
{
    return std::any_of(m_Sections.begin(), m_Sections.end(), [id](const SectionInfo& s) { return s.Id == id; });
}

bool Reader::ReadSection(u32 id, std::vector<u8>& out, std::string* error) const
{
    const auto it =
        std::find_if(m_Sections.begin(), m_Sections.end(), [id](const SectionInfo& s) { return s.Id == id; });
    if (it == m_Sections.end())
    {
        SetError(error, "no section '" + FourCCName(id) + "'");
        return false;
    }
    const u8* stored = m_File.data() + it->Offset;
    if (it->Compression == Codec::Raw)
    {
        if (it->StoredSize != it->RawSize)
        {
            SetError(error, "section '" + FourCCName(id) + "' has inconsistent sizes");
            return false;
        }
        out.assign(stored, stored + it->StoredSize);
    }
    else
    {
        // The declared size is checked against what zstd says the frame holds,
        // so a damaged size cannot make this allocate without bound.
        const unsigned long long frame = ZSTD_getFrameContentSize(stored, it->StoredSize);
        if (frame == ZSTD_CONTENTSIZE_ERROR || frame == ZSTD_CONTENTSIZE_UNKNOWN || frame != it->RawSize)
        {
            SetError(error, "section '" + FourCCName(id) + "' is not a valid zstd frame");
            return false;
        }
        out.resize(static_cast<std::size_t>(it->RawSize));
        const std::size_t size = ZSTD_decompress(out.data(), out.size(), stored, it->StoredSize);
        if (ZSTD_isError(size) != 0 || size != it->RawSize)
        {
            SetError(error, "section '" + FourCCName(id) + "' does not decompress");
            return false;
        }
    }
    if (Crc32(out.data(), out.size()) != it->Crc)
    {
        SetError(error, "section '" + FourCCName(id) + "' fails its checksum");
        return false;
    }
    return true;
}

} // namespace UHE::Cooked
