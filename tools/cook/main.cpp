// uhe_cook: cooks models and looks inside cooked files (issue #45).
//
//   uhe_cook <model.gltf|.glb> [-o <out.uhemodel>]   cook (default: <model>.uhemodel beside it)
//   uhe_cook --inspect <file.uhemodel>               header, sections, sizes, checksums
//   uhe_cook --diff <a.uhemodel> <b.uhemodel>        which sections differ
//
// Exports cook on their own (uhe_export, File > Export Game); this is for
// looking at one asset, and for a cook in a script.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include "UHE/AssetsManager/CookedFile.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace fs = std::filesystem;
using namespace UHE;

namespace
{
int Usage()
{
    std::fprintf(stderr, "usage: uhe_cook <model.gltf|.glb> [-o <out.uhemodel>]\n"
                         "       uhe_cook --inspect <file.uhemodel>\n"
                         "       uhe_cook --diff <a.uhemodel> <b.uhemodel>\n");
    return 2;
}

bool Open(const fs::path& path, Cooked::Reader& reader)
{
    std::string error;
    if (reader.Open(path, &error))
        return true;
    std::fprintf(stderr, "uhe_cook: %s: %s\n", path.string().c_str(), error.c_str());
    return false;
}

int Inspect(const fs::path& path)
{
    Cooked::Reader reader;
    if (!Open(path, reader))
        return 1;
    std::printf("%s\n  type %s, format %u, source hash %016llx, %zu sections\n", path.string().c_str(),
                Cooked::FourCCName(reader.AssetType()).c_str(), reader.Version(),
                static_cast<unsigned long long>(reader.SourceHash()), reader.Sections().size());
    std::printf("  %-6s %-5s %10s %12s %12s %7s  %s\n", "id", "codec", "offset", "stored", "raw", "ratio", "crc32");
    int damaged = 0;
    for (const Cooked::SectionInfo& info : reader.Sections())
    {
        std::vector<u8> bytes;
        std::string error;
        const bool ok = reader.ReadSection(info.Id, bytes, &error);
        damaged += ok ? 0 : 1;
        std::printf("  %-6s %-5s %10llu %12llu %12llu %6.1f%%  %08x %s\n", Cooked::FourCCName(info.Id).c_str(),
                    info.Compression == Cooked::Codec::Zstd ? "zstd" : "raw",
                    static_cast<unsigned long long>(info.Offset), static_cast<unsigned long long>(info.StoredSize),
                    static_cast<unsigned long long>(info.RawSize),
                    info.RawSize != 0 ? 100.0 * static_cast<double>(info.StoredSize) / static_cast<double>(info.RawSize)
                                      : 100.0,
                    info.Crc, ok ? "" : ("DAMAGED: " + error).c_str());
    }
    if (reader.AssetType() == Cooked::FourCC("MODL"))
    {
        RD3d::ModelLoadOptions options;
        options.upload = false;
        options.keepTextureSources = true;
        RD3d::Model model;
        std::string error;
        if (model.LoadCooked(path, options, &error))
        {
            std::size_t vertices = 0, indices = 0;
            for (const RD3d::Geometry& geometry : model.GetGeometry())
                for (const RD3d::Primitive& prim : geometry.primitive)
                {
                    vertices += prim.vertices.size();
                    indices += prim.indices.size();
                }
            std::printf("  model: %zu geometries (%zu vertices, %zu triangles), %zu meshes, %zu nodes, %zu materials, "
                        "%zu images, %zu skins, %zu clips, %zu lights\n",
                        model.GetGeometry().size(), vertices, indices / 3, model.GetMesh().size(),
                        model.GetNodes().size(), model.GetMaterials().size(), model.GetTextureSources().size(),
                        model.GetSkins().size(), model.GetAnimations().size(), model.GetPunctualLights().size());
        }
        else
        {
            std::printf("  model: does not load (%s)\n", error.c_str());
            ++damaged;
        }
    }
    return damaged == 0 ? 0 : 1;
}

int Diff(const fs::path& a, const fs::path& b)
{
    Cooked::Reader left, right;
    if (!Open(a, left) || !Open(b, right))
        return 1;
    int differences = 0;
    if (left.AssetType() != right.AssetType() || left.Version() != right.Version())
    {
        std::printf("type or format differs: %s v%u vs %s v%u\n", Cooked::FourCCName(left.AssetType()).c_str(),
                    left.Version(), Cooked::FourCCName(right.AssetType()).c_str(), right.Version());
        ++differences;
    }
    if (left.SourceHash() != right.SourceHash())
        std::printf("cooked from different sources\n");
    for (const Cooked::SectionInfo& info : left.Sections())
    {
        const auto other = std::find_if(right.Sections().begin(), right.Sections().end(),
                                        [&](const Cooked::SectionInfo& s) { return s.Id == info.Id; });
        if (other == right.Sections().end())
        {
            std::printf("  %s only in %s\n", Cooked::FourCCName(info.Id).c_str(), a.filename().string().c_str());
            ++differences;
        }
        else if (other->Crc != info.Crc || other->RawSize != info.RawSize)
        {
            std::printf("  %s differs (%llu vs %llu bytes)\n", Cooked::FourCCName(info.Id).c_str(),
                        static_cast<unsigned long long>(info.RawSize), static_cast<unsigned long long>(other->RawSize));
            ++differences;
        }
    }
    for (const Cooked::SectionInfo& info : right.Sections())
    {
        if (!left.HasSection(info.Id))
        {
            std::printf("  %s only in %s\n", Cooked::FourCCName(info.Id).c_str(), b.filename().string().c_str());
            ++differences;
        }
    }
    std::printf(differences == 0 ? "identical content\n" : "%d difference(s)\n", differences);
    return differences == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return Usage();
    const std::string first = argv[1];
    if (first == "--inspect")
        return argc == 3 ? Inspect(argv[2]) : Usage();
    if (first == "--diff")
        return argc == 4 ? Diff(argv[2], argv[3]) : Usage();

    const fs::path source = first;
    fs::path out = RD3d::Model::CookedPathFor(source);
    if (argc == 4 && std::string(argv[2]) == "-o")
        out = argv[3];
    else if (argc != 2)
        return Usage();
    std::string error;
    if (!RD3d::CookModelFile(source, out, error))
    {
        std::fprintf(stderr, "uhe_cook: %s: %s\n", source.string().c_str(), error.c_str());
        return 1;
    }
    std::error_code ec;
    std::printf("uhe_cook: %s -> %s (%ju KiB, source %ju KiB)\n", source.string().c_str(), out.string().c_str(),
                static_cast<std::uintmax_t>(fs::file_size(out, ec) / 1024),
                static_cast<std::uintmax_t>(fs::file_size(source, ec) / 1024));
    return 0;
}
