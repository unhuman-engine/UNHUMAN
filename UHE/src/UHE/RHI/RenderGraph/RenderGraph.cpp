#include "uhepch.h"
#include "RenderGraph.h"
#include <sstream>

namespace UHE::RHI
{

namespace
{
std::string JsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else
                {
                    out += c;
                }
        }
    }
    return out;
}

void JsonArrayFromNames(std::ostringstream& out, const std::vector<std::string>& names)
{
    out << '[';
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i != 0)
            out << ", ";
        out << '"' << JsonEscape(names[i]) << '"';
    }
    out << ']';
}
} // namespace

RGRawGraph RenderGraph::DumpRawGraph() const
{
    RGRawGraph raw;
    raw.topologyHash = ComputeTopologyHash();
    raw.errors = m_Builder.Validate();

    const RenderGraphResources& resources = m_Builder.Resources();

    for (const RGTextureSlot& slot : resources.Textures())
    {
        RGDeclaredTexture declared;
        declared.name = slot.name;
        declared.versionCount = slot.currentVersion;
        declared.imported = slot.imported;
        declared.initialState = slot.desc.initialState;
        declared.finalState = slot.desc.finalState;
        raw.textures.push_back(std::move(declared));
    }

    for (const RGBufferSlot& slot : resources.Buffers())
    {
        RGDeclaredBuffer declared;
        declared.name = slot.name;
        declared.versionCount = slot.currentVersion;
        declared.imported = slot.imported;
        raw.buffers.push_back(std::move(declared));
    }

    for (const RGPassSpec& pass : m_Builder.Passes())
    {
        RGDeclaredPass declared;
        declared.name = pass.name;
        declared.type = pass.type;
        declared.queue = pass.queue;
        declared.readCount = static_cast<u32>(pass.reads.size());
        declared.writeCount = static_cast<u32>(pass.writes.size());
        declared.colorCount = static_cast<u32>(pass.colors.size());
        declared.hasDepth = pass.hasDepth;

        // Names carry the version the access was bound to at declaration time —
        // the slot's current version would retroactively rebind reads (§7).
        for (const RGResourceAccess& access : pass.reads)
        {
            declared.readNames.push_back(access.isTexture ? resources.TextureNameAt(access.texture, access.version)
                                                          : resources.BufferNameAt(access.buffer, access.version));
        }
        for (const RGResourceAccess& access : pass.writes)
        {
            declared.writeNames.push_back(access.isTexture ? resources.TextureNameAt(access.texture, access.version)
                                                           : resources.BufferNameAt(access.buffer, access.version));
        }
        raw.passes.push_back(std::move(declared));
    }

    return raw;
}

std::string RenderGraph::DumpRawGraphJson() const
{
    const RGRawGraph raw = DumpRawGraph();
    std::ostringstream out;

    out << "{\n  \"topologyHash\": " << raw.topologyHash << ",\n";
    out << "  \"valid\": " << (raw.HasErrors() ? "false" : "true") << ",\n";

    out << "  \"textures\": [\n";
    for (size_t i = 0; i < raw.textures.size(); ++i)
    {
        const RGDeclaredTexture& texture = raw.textures[i];
        out << "    {\"name\": \"" << JsonEscape(texture.name) << "\", \"versions\": " << texture.versionCount
            << ", \"imported\": " << (texture.imported ? "true" : "false") << ", \"initialState\": \""
            << RGToString(texture.initialState) << "\", \"finalState\": \"" << RGToString(texture.finalState) << "\"}"
            << (i + 1 < raw.textures.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"buffers\": [\n";
    for (size_t i = 0; i < raw.buffers.size(); ++i)
    {
        const RGDeclaredBuffer& buffer = raw.buffers[i];
        out << "    {\"name\": \"" << JsonEscape(buffer.name) << "\", \"versions\": " << buffer.versionCount
            << ", \"imported\": " << (buffer.imported ? "true" : "false") << "}"
            << (i + 1 < raw.buffers.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"passes\": [\n";
    for (size_t i = 0; i < raw.passes.size(); ++i)
    {
        const RGDeclaredPass& pass = raw.passes[i];
        out << "    {\"name\": \"" << JsonEscape(pass.name) << "\", \"type\": \"" << RGToString(pass.type)
            << "\", \"queue\": \"" << RGToString(pass.queue) << "\", \"reads\": ";
        JsonArrayFromNames(out, pass.readNames);
        out << ", \"writes\": ";
        JsonArrayFromNames(out, pass.writeNames);
        out << ", \"colorCount\": " << pass.colorCount << ", \"hasDepth\": " << (pass.hasDepth ? "true" : "false")
            << "}" << (i + 1 < raw.passes.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"errors\": [\n";
    for (size_t i = 0; i < raw.errors.size(); ++i)
    {
        const RGValidationError& error = raw.errors[i];
        out << "    {\"type\": " << static_cast<u32>(error.type) << ", \"pass\": \"" << JsonEscape(error.passName)
            << "\", \"resource\": \"" << JsonEscape(error.resourceName) << "\", \"message\": \""
            << JsonEscape(error.message) << "\"}" << (i + 1 < raw.errors.size() ? "," : "") << "\n";
    }
    out << "  ]\n";

    out << "}\n";
    return out.str();
}

} // namespace UHE::RHI
