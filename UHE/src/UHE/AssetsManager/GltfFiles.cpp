#include "GltfFiles.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>

namespace UHE
{
namespace fs = std::filesystem;

namespace
{
std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
} // namespace

std::optional<std::string> GltfJson(const fs::path& file)
{
    if (Lower(file.extension().string()) != ".glb")
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return std::nullopt;
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    std::ifstream in(file, std::ios::binary);
    std::array<std::uint32_t, 5> header{}; // magic, version, length, chunk length, chunk type
    in.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    // The lengths come from the file: checked against its real size before
    // anything is allocated, so a broken header cannot ask for 4 GB.
    if (!in || ec || header[0] != 0x46546C67u || header[4] != 0x4E4F534Au || header[2] > size ||
        std::uintmax_t{header[3]} > size - std::min<std::uintmax_t>(size, sizeof(header)))
        return std::nullopt;
    std::string json(header[3], '\0');
    in.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (!in)
        return std::nullopt;
    return json;
}

std::string DecodeGltfUri(const std::string& uri)
{
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2])))
        {
            out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else
            out += uri[i];
    }
    return out;
}

std::optional<std::vector<fs::path>> GltfExternalFiles(const fs::path& file)
{
    const std::optional<std::string> json = GltfJson(file);
    if (!json)
        return std::nullopt;
    static const std::regex kUri(R"json("uri"\s*:\s*"([^"]*)")json");
    std::vector<fs::path> files;
    for (std::sregex_iterator it(json->begin(), json->end(), kUri), end; it != end; ++it)
    {
        const std::string uri = (*it)[1].str();
        if (uri.starts_with("data:"))
            continue;
        fs::path path = fs::path(DecodeGltfUri(uri)).lexically_normal();
        if (std::ranges::find(files, path) == files.end())
            files.push_back(std::move(path));
    }
    return files;
}

} // namespace UHE
