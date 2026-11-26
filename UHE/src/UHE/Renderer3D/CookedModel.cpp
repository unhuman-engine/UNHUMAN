// Model::SaveCooked / LoadCooked: the loaded model as a cooked binary
// (UHE/AssetsManager/CookedFile.h), so a game loads it without parsing glTF.
//
// Every field is written explicitly, in declaration order, rather than as a
// memory image: the structs hold std::string, std::vector and Ref<> members.
// Changing what a section holds is a format change: bump kModelVersion, and
// the loader refuses the older cook (recook) instead of misreading it.

#include "uhepch.h"
#include <array>
#include "LoadModel.h"
#include "UHE/AssetsManager/CookedFile.h"

namespace UHE::RD3d
{
namespace
{
using Cooked::BinaryReader;
using Cooked::BinaryWriter;
using Cooked::FourCC;

constexpr u32 kAssetModel = FourCC("MODL");
constexpr u32 kSectionInfo = FourCC("INFO");
constexpr u32 kSectionTextures = FourCC("TEXS");
constexpr u32 kSectionMaterials = FourCC("MATS");
constexpr u32 kSectionGeometry = FourCC("GEOM");
constexpr u32 kSectionScene = FourCC("SCNE");
constexpr u32 kSectionAnimation = FourCC("ANIM");

/// The model sections' own layout version, inside the container's.
constexpr u32 kModelVersion = 1;

constexpr std::size_t kSlotCount = static_cast<std::size_t>(MaterialTextureSlot::Count);

/// Each MaterialTextureSlot's texture field, in slot order.
std::array<Ref<Texture2D>*, kSlotCount> SlotTextures(Material& m)
{
    MaterialExtensions& e = m.Extensions;
    return {&m.AlbedoTexture,
            &m.MetallicRoughnessTexture,
            &m.NormalTexture,
            &m.OcclusionTexture,
            &m.EmissiveTexture,
            &e.ClearcoatTexture,
            &e.ClearcoatRoughnessTexture,
            &e.ClearcoatNormalTexture,
            &e.SpecularTexture,
            &e.SpecularColorTexture,
            &e.SheenColorTexture,
            &e.SheenRoughnessTexture,
            &e.TransmissionTexture,
            &e.ThicknessTexture,
            &e.IridescenceTexture,
            &e.IridescenceThicknessTexture,
            &e.AnisotropyTexture,
            &e.DiffuseTransmissionTexture,
            &e.DiffuseTransmissionColorTexture};
}

// ── Materials ───────────────────────────────────────────────────────────────

void PutSampler(BinaryWriter& w, const RHI::SamplerDesc& s)
{
    w.Put(s.magFilter);
    w.Put(s.minFilter);
    w.Put(s.wrapS);
    w.Put(s.wrapT);
    w.Put(s.maxAnisotropy);
    w.Put(s.maxLod);
    w.Put(s.colorSpace);
}

RHI::SamplerDesc GetSampler(BinaryReader& r)
{
    RHI::SamplerDesc s;
    s.magFilter = r.Get<RHI::SamplerDesc::Filter>();
    s.minFilter = r.Get<RHI::SamplerDesc::Filter>();
    s.wrapS = r.Get<RHI::SamplerDesc::Wrap>();
    s.wrapT = r.Get<RHI::SamplerDesc::Wrap>();
    s.maxAnisotropy = r.Get<u8>();
    s.maxLod = r.Get<f32>();
    s.colorSpace = r.Get<RHI::SamplerDesc::ColorSpace>();
    return s;
}

void PutMaterial(BinaryWriter& w, const Material& m, const MaterialTextureSources& sources)
{
    for (const i32 source : sources)
        w.Put(source);
    w.Put(m.BaseColorFactor);
    w.Put(m.MetallicFactor);
    w.Put(m.RoughnessFactor);
    w.Put(m.NormalScale);
    w.Put(m.OcclusionStrength);
    w.Put(m.EmissiveFactor);
    w.Put(m.Alpha);
    w.Put(m.AlphaCutoff);
    w.Put(static_cast<u8>(m.DoubleSided));
    for (const UVTransform& t : m.UVTransforms)
    {
        w.Put(t.cosRotation);
        w.Put(t.sinRotation);
        w.Put(t.scale);
        w.Put(t.offset);
        w.Put(t.texCoordSet);
        w.Put(static_cast<u8>(t.HasTransform));
    }
    const MaterialExtensions& e = m.Extensions;
    w.Put(e.EmissiveStrength);
    w.Put(e.IOR);
    w.Put(e.ClearcoatFactor);
    w.Put(e.ClearcoatRoughnessFactor);
    w.Put(e.ClearcoatNormalScale);
    w.Put(static_cast<u8>(e.HasClearcoat));
    w.Put(e.SpecularFactor);
    w.Put(e.SpecularColorFactor);
    w.Put(static_cast<u8>(e.HasSpecular));
    w.Put(e.SheenColorFactor);
    w.Put(e.SheenRoughnessFactor);
    w.Put(static_cast<u8>(e.HasSheen));
    w.Put(e.TransmissionFactor);
    w.Put(e.ThicknessFactor);
    w.Put(e.AttenuationDistance);
    w.Put(e.AttenuationColor);
    w.Put(static_cast<u8>(e.HasTransmission));
    w.Put(e.IridescenceFactor);
    w.Put(e.IridescenceIOR);
    w.Put(e.IridescenceThicknessMinimum);
    w.Put(e.IridescenceThicknessMaximum);
    w.Put(static_cast<u8>(e.HasIridescence));
    w.Put(e.AnisotropyStrength);
    w.Put(e.AnisotropyRotation);
    w.Put(static_cast<u8>(e.HasAnisotropy));
    w.Put(e.DiffuseTransmissionFactor);
    w.Put(e.DiffuseTransmissionColor);
    w.Put(static_cast<u8>(e.HasDiffuseTransmission));
    w.Put(static_cast<u8>(e.Unlit));
    w.Put(e.TransmissionBlend);
}

void GetMaterial(BinaryReader& r, Material& m, MaterialTextureSources& sources)
{
    for (i32& source : sources)
        source = r.Get<i32>();
    m.BaseColorFactor = r.Get<glm::vec4>();
    m.MetallicFactor = r.Get<f32>();
    m.RoughnessFactor = r.Get<f32>();
    m.NormalScale = r.Get<f32>();
    m.OcclusionStrength = r.Get<f32>();
    m.EmissiveFactor = r.Get<glm::vec3>();
    m.Alpha = r.Get<AlphaMode>();
    m.AlphaCutoff = r.Get<f32>();
    m.DoubleSided = r.Get<u8>() != 0;
    for (UVTransform& t : m.UVTransforms)
    {
        t.cosRotation = r.Get<f32>();
        t.sinRotation = r.Get<f32>();
        t.scale = r.Get<glm::vec2>();
        t.offset = r.Get<glm::vec2>();
        t.texCoordSet = r.Get<u32>();
        t.HasTransform = r.Get<u8>() != 0;
    }
    MaterialExtensions& e = m.Extensions;
    e.EmissiveStrength = r.Get<f32>();
    e.IOR = r.Get<f32>();
    e.ClearcoatFactor = r.Get<f32>();
    e.ClearcoatRoughnessFactor = r.Get<f32>();
    e.ClearcoatNormalScale = r.Get<f32>();
    e.HasClearcoat = r.Get<u8>() != 0;
    e.SpecularFactor = r.Get<f32>();
    e.SpecularColorFactor = r.Get<glm::vec3>();
    e.HasSpecular = r.Get<u8>() != 0;
    e.SheenColorFactor = r.Get<glm::vec3>();
    e.SheenRoughnessFactor = r.Get<f32>();
    e.HasSheen = r.Get<u8>() != 0;
    e.TransmissionFactor = r.Get<f32>();
    e.ThicknessFactor = r.Get<f32>();
    e.AttenuationDistance = r.Get<f32>();
    e.AttenuationColor = r.Get<glm::vec3>();
    e.HasTransmission = r.Get<u8>() != 0;
    e.IridescenceFactor = r.Get<f32>();
    e.IridescenceIOR = r.Get<f32>();
    e.IridescenceThicknessMinimum = r.Get<f32>();
    e.IridescenceThicknessMaximum = r.Get<f32>();
    e.HasIridescence = r.Get<u8>() != 0;
    e.AnisotropyStrength = r.Get<f32>();
    e.AnisotropyRotation = r.Get<f32>();
    e.HasAnisotropy = r.Get<u8>() != 0;
    e.DiffuseTransmissionFactor = r.Get<f32>();
    e.DiffuseTransmissionColor = r.Get<glm::vec3>();
    e.HasDiffuseTransmission = r.Get<u8>() != 0;
    e.Unlit = r.Get<u8>() != 0;
    e.TransmissionBlend = r.Get<BlendApproach>();
}

// ── Geometry and scene ──────────────────────────────────────────────────────

/// The fields of a Primitive other than its data and GPU handles.
void PutPrimitiveHeader(BinaryWriter& w, const Primitive& p)
{
    w.Put<u64>(p.materialIndex);
    w.Put(static_cast<u8>(p.hasVertexColor));
    w.Put<u64>(p.geometryIndex);
    w.Put(static_cast<u8>(p.usesSharedGeometry));
    w.Put(p.IndexCount);
}

void GetPrimitiveHeader(BinaryReader& r, Primitive& p)
{
    p.materialIndex = static_cast<size_t>(r.Get<u64>());
    p.hasVertexColor = r.Get<u8>() != 0;
    p.geometryIndex = static_cast<size_t>(r.Get<u64>());
    p.usesSharedGeometry = r.Get<u8>() != 0;
    p.IndexCount = r.Get<u32>();
}

template <typename Track> void PutTrack(BinaryWriter& w, const Track& t)
{
    w.Put(t.TargetBoneID);
    w.Put(t.Interpolation);
    w.PutVector(t.Keyframes);
    w.PutVector(t.InTangents);
    w.PutVector(t.OutTangents);
}

template <typename Track> void GetTrack(BinaryReader& r, Track& t)
{
    t.TargetBoneID = r.Get<i32>();
    t.Interpolation = r.Get<InterpolationMode>();
    r.GetVector(t.Keyframes);
    r.GetVector(t.InTangents);
    r.GetVector(t.OutTangents);
}

template <typename Track> void PutTracks(BinaryWriter& w, const std::vector<Track>& tracks)
{
    w.Put<u64>(tracks.size());
    for (const Track& t : tracks)
        PutTrack(w, t);
}

template <typename Track> void GetTracks(BinaryReader& r, std::vector<Track>& tracks)
{
    tracks.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
    for (Track& t : tracks)
        GetTrack(r, t);
}

void SetError(std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
}
} // namespace

bool CookModelFile(const std::filesystem::path& source, const std::filesystem::path& cooked, std::string& error)
{
    ModelLoadOptions options;
    options.keepTextureSources = true;
    options.upload = false;
    Model model;
    if (!model.loadModel(source, options))
    {
        error = "it does not load";
        return false;
    }
    return model.SaveCooked(cooked, Cooked::HashFile(source), &error);
}

std::filesystem::path Model::CookedPathFor(const std::filesystem::path& source)
{
    return source.string() + ".uhemodel";
}

bool Model::SaveCooked(const std::filesystem::path& path, u64 sourceHash, std::string* error) const
{
    if (!m_Options.keepTextureSources)
    {
        SetError(error, "the model was not loaded with keepTextureSources: its images are not available to store");
        return false;
    }
    Cooked::Writer file;

    {
        BinaryWriter w;
        w.Put(kModelVersion);
        w.Put(static_cast<u8>(m_HasTransparentMaterials));
        w.Put(static_cast<u8>(m_HasUnsupportedExtensions));
        w.Put<u64>(m_UnsupportedExtensionNames.size());
        for (const std::string& name : m_UnsupportedExtensionNames)
            w.PutString(name);
        file.AddSection(kSectionInfo, w.Take(), false);
    }
    {
        // Encoded images stay as the source carried them: PNG and JPEG are
        // already compressed, so the section is stored without zstd.
        BinaryWriter w;
        w.Put<u64>(m_TextureSources.size());
        for (const TextureSource& texture : m_TextureSources)
        {
            w.PutString(texture.Name);
            PutSampler(w, texture.Sampler);
            w.PutVector(texture.Bytes);
        }
        file.AddSection(kSectionTextures, w.Take(), false);
    }
    {
        BinaryWriter w;
        w.Put<u64>(m_LoadedMaterials.size());
        MaterialTextureSources none;
        none.fill(-1);
        for (std::size_t i = 0; i < m_LoadedMaterials.size(); ++i)
            PutMaterial(w, m_LoadedMaterials[i],
                        i < m_MaterialTextureSources.size() ? m_MaterialTextureSources[i] : none);
        file.AddSection(kSectionMaterials, w.Take());
    }
    {
        BinaryWriter w;
        w.Put<u64>(m_Geometry.size());
        for (const Geometry& geometry : m_Geometry)
        {
            w.PutString(geometry.name);
            w.Put(geometry.boundsMin);
            w.Put(geometry.boundsMax);
            w.Put(static_cast<u8>(geometry.hasBounds));
            w.Put<u64>(geometry.primitive.size());
            for (const Primitive& prim : geometry.primitive)
            {
                PutPrimitiveHeader(w, prim);
                w.PutVector(prim.vertices);
                w.PutVector(prim.indices);
            }
        }
        file.AddSection(kSectionGeometry, w.Take());
    }
    {
        BinaryWriter w;
        w.Put<u64>(m_LoadedMeshes.size());
        for (const Mesh& mesh : m_LoadedMeshes)
        {
            w.PutString(mesh.name);
            w.Put<u64>(mesh.nodeIndex);
            w.Put<u64>(mesh.geometryIndex);
            w.Put(mesh.LocalTransform);
            w.Put(mesh.skinIndex);
            w.Put(mesh.boundsMin);
            w.Put(mesh.boundsMax);
            w.Put(static_cast<u8>(mesh.hasBounds));
            w.Put(static_cast<u8>(mesh.ownsGeometryBuffers));
            w.Put<u64>(mesh.primitive.size());
            for (const Primitive& prim : mesh.primitive)
                PutPrimitiveHeader(w, prim);
        }
        w.Put<u64>(m_Nodes.size());
        for (const ModelNode& node : m_Nodes)
        {
            w.PutString(node.Name);
            w.Put(node.Parent);
            w.PutVector(node.Children);
            w.Put(node.MeshIndex);
            w.Put(node.Translation);
            w.Put(node.Rotation);
            w.Put(node.Scale);
            w.Put(node.LightIndex);
            w.Put(node.WorldTransform);
        }
        w.PutVector(m_RootNodes);
        w.Put<u64>(m_NodeToMesh.size());
        for (const auto& [node, mesh] : m_NodeToMesh)
        {
            w.Put<i32>(node);
            w.Put<i32>(mesh);
        }
        w.Put<u64>(m_PunctualLights.size());
        for (const PunctualLight& light : m_PunctualLights)
        {
            w.Put(light.type);
            w.Put(light.Color);
            w.Put(light.Intensity);
            w.Put(light.Range);
            w.Put(light.InnerConeAngle);
            w.Put(light.OuterConeAngle);
            w.PutString(light.Name);
        }
        file.AddSection(kSectionScene, w.Take());
    }
    {
        BinaryWriter w;
        w.Put<u64>(m_Skeleton.Bones.size());
        for (const Bone& bone : m_Skeleton.Bones)
        {
            w.PutString(bone.Name);
            w.Put(bone.ID);
            w.Put(bone.ParentID);
            w.Put(bone.InverseBindMatrix);
            w.Put(bone.LocalTransform);
        }
        w.PutVector(m_Skeleton.JointNodes);
        w.Put(m_Skeleton.RootBoneID);
        w.Put<u64>(m_Skins.size());
        for (const Skin& skin : m_Skins)
        {
            w.PutString(skin.Name);
            w.PutVector(skin.JointNodes);
            w.PutVector(skin.InverseBindMatrices);
            w.PutVector(skin.RestPalette);
        }
        w.Put<u64>(m_Animations.size());
        for (const AnimationClip& clip : m_Animations)
        {
            w.PutString(clip.Name);
            w.Put(clip.Duration);
            w.Put(clip.Loop);
            PutTracks(w, clip.PositionTracks);
            PutTracks(w, clip.RotationTracks);
            PutTracks(w, clip.ScaleTracks);
            w.Put<u64>(clip.Events.size());
            for (const AnimationEvent& event : clip.Events)
            {
                w.Put(event.Time);
                w.PutString(event.Name);
            }
        }
        file.AddSection(kSectionAnimation, w.Take());
    }
    return file.Write(path, kAssetModel, sourceHash, error);
}

bool Model::LoadCooked(const std::filesystem::path& path, const ModelLoadOptions& options, std::string* error)
{
    Cooked::Reader file;
    std::string why;
    if (!file.Open(path, &why))
    {
        SetError(error, why);
        return false;
    }
    if (file.AssetType() != kAssetModel)
    {
        SetError(error, "a cooked " + Cooked::FourCCName(file.AssetType()) + ", not a model");
        return false;
    }

    Destroy();
    m_Options = options;
    const auto fail = [&](const std::string& message)
    {
        Destroy();
        SetError(error, "'" + path.filename().string() + "': " + message);
        return false;
    };
    std::vector<u8> bytes;

    if (!file.ReadSection(kSectionInfo, bytes, &why))
        return fail(why);
    {
        BinaryReader r(bytes);
        const u32 version = r.Get<u32>();
        if (version != kModelVersion)
            return fail("model layout " + std::to_string(version) + ", this engine reads " +
                        std::to_string(kModelVersion) + "; recook it");
        m_HasTransparentMaterials = r.Get<u8>() != 0;
        m_HasUnsupportedExtensions = r.Get<u8>() != 0;
        m_UnsupportedExtensionNames.resize(static_cast<std::size_t>(r.GetCount(1024)));
        for (std::string& name : m_UnsupportedExtensionNames)
            name = r.GetString();
        if (!r.Ok())
            return fail("damaged INFO section");
    }

    if (!file.ReadSection(kSectionTextures, bytes, &why))
        return fail(why);
    std::vector<Ref<Texture2D>> textures;
    {
        BinaryReader r(bytes);
        const u64 count = r.GetCount(r.Remaining());
        for (u64 i = 0; i < count && r.Ok(); ++i)
        {
            TextureSource source;
            source.Name = r.GetString();
            source.Sampler = GetSampler(r);
            r.GetVector(source.Bytes);
            if (!r.Ok())
                break;
            // Decoded from the stored image, as the glTF path decodes the file's.
            textures.push_back(
                options.upload ? Texture2D::CreateFromMemory(source.Bytes.data(), source.Bytes.size(), source.Sampler)
                               : nullptr);
            if (options.keepTextureSources)
                m_TextureSources.push_back(std::move(source));
        }
        if (!r.Ok())
            return fail("damaged TEXS section");
    }

    if (!file.ReadSection(kSectionMaterials, bytes, &why))
        return fail(why);
    {
        BinaryReader r(bytes);
        const u64 count = r.GetCount(r.Remaining());
        m_LoadedMaterials.resize(static_cast<std::size_t>(count));
        m_MaterialTextureSources.resize(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < m_LoadedMaterials.size() && r.Ok(); ++i)
        {
            GetMaterial(r, m_LoadedMaterials[i], m_MaterialTextureSources[i]);
            const auto slots = SlotTextures(m_LoadedMaterials[i]);
            for (std::size_t slot = 0; slot < kSlotCount; ++slot)
            {
                const i32 source = m_MaterialTextureSources[i][slot];
                if (source >= static_cast<i32>(textures.size()))
                    r.Fail();
                else if (source >= 0)
                    *slots[slot] = textures[static_cast<std::size_t>(source)];
            }
        }
        if (!r.Ok())
            return fail("damaged MATS section");
    }

    if (!file.ReadSection(kSectionGeometry, bytes, &why))
        return fail(why);
    {
        BinaryReader r(bytes);
        m_Geometry.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
        for (Geometry& geometry : m_Geometry)
        {
            geometry.name = r.GetString();
            geometry.boundsMin = r.Get<glm::vec3>();
            geometry.boundsMax = r.Get<glm::vec3>();
            geometry.hasBounds = r.Get<u8>() != 0;
            geometry.primitive.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
            for (Primitive& prim : geometry.primitive)
            {
                GetPrimitiveHeader(r, prim);
                r.GetVector(prim.vertices);
                r.GetVector(prim.indices);
                if (prim.materialIndex >= m_LoadedMaterials.size() && !m_LoadedMaterials.empty())
                    r.Fail();
            }
            if (!r.Ok())
                break;
        }
        if (!r.Ok())
            return fail("damaged GEOM section");
    }

    if (!file.ReadSection(kSectionScene, bytes, &why))
        return fail(why);
    {
        BinaryReader r(bytes);
        m_LoadedMeshes.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
        for (Mesh& mesh : m_LoadedMeshes)
        {
            mesh.name = r.GetString();
            mesh.nodeIndex = static_cast<size_t>(r.Get<u64>());
            mesh.geometryIndex = static_cast<size_t>(r.Get<u64>());
            mesh.LocalTransform = r.Get<glm::mat4>();
            mesh.skinIndex = r.Get<i32>();
            mesh.boundsMin = r.Get<glm::vec3>();
            mesh.boundsMax = r.Get<glm::vec3>();
            mesh.hasBounds = r.Get<u8>() != 0;
            mesh.ownsGeometryBuffers = r.Get<u8>() != 0;
            mesh.primitive.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
            for (Primitive& prim : mesh.primitive)
                GetPrimitiveHeader(r, prim);
            if (mesh.geometryIndex >= m_Geometry.size())
                r.Fail();
            if (!r.Ok())
                break;
        }
        m_Nodes.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
        for (ModelNode& node : m_Nodes)
        {
            node.Name = r.GetString();
            node.Parent = r.Get<i32>();
            r.GetVector(node.Children);
            node.MeshIndex = r.Get<i32>();
            node.Translation = r.Get<glm::vec3>();
            node.Rotation = r.Get<glm::quat>();
            node.Scale = r.Get<glm::vec3>();
            node.LightIndex = r.Get<i32>();
            node.WorldTransform = r.Get<glm::mat4>();
            if (!r.Ok())
                break;
        }
        r.GetVector(m_RootNodes);
        const u64 mapped = r.GetCount(r.Remaining());
        for (u64 i = 0; i < mapped && r.Ok(); ++i)
        {
            const i32 node = r.Get<i32>();
            m_NodeToMesh[node] = r.Get<i32>();
        }
        m_PunctualLights.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
        for (PunctualLight& light : m_PunctualLights)
        {
            light.type = r.Get<PunctualLight::Type>();
            light.Color = r.Get<glm::vec3>();
            light.Intensity = r.Get<f32>();
            light.Range = r.Get<f32>();
            light.InnerConeAngle = r.Get<f32>();
            light.OuterConeAngle = r.Get<f32>();
            light.Name = r.GetString();
        }
        if (!r.Ok())
            return fail("damaged SCNE section");
    }

    if (!file.ReadSection(kSectionAnimation, bytes, &why))
        return fail(why);
    {
        BinaryReader r(bytes);
        m_Skeleton.Bones.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
        for (Bone& bone : m_Skeleton.Bones)
        {
            bone.Name = r.GetString();
            bone.ID = r.Get<i32>();
            bone.ParentID = r.Get<i32>();
            bone.InverseBindMatrix = r.Get<glm::mat4>();
            bone.LocalTransform = r.Get<glm::mat4>();
        }
        r.GetVector(m_Skeleton.JointNodes);
        m_Skeleton.RootBoneID = r.Get<i32>();
        m_Skins.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
        for (Skin& skin : m_Skins)
        {
            skin.Name = r.GetString();
            r.GetVector(skin.JointNodes);
            r.GetVector(skin.InverseBindMatrices);
            r.GetVector(skin.RestPalette);
        }
        m_Animations.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
        for (AnimationClip& clip : m_Animations)
        {
            clip.Name = r.GetString();
            clip.Duration = r.Get<f32>();
            clip.Loop = r.Get<LoopMode>();
            GetTracks(r, clip.PositionTracks);
            GetTracks(r, clip.RotationTracks);
            GetTracks(r, clip.ScaleTracks);
            clip.Events.resize(static_cast<std::size_t>(r.GetCount(r.Remaining())));
            for (AnimationEvent& event : clip.Events)
            {
                event.Time = r.Get<f32>();
                event.Name = r.GetString();
            }
            if (!r.Ok())
                break;
        }
        if (!r.Ok())
            return fail("damaged ANIM section");
    }

    if (options.upload)
    {
        for (std::size_t i = 0; i < m_Geometry.size(); ++i)
            UploadGeometry(m_Geometry[i], i);
    }
    return true;
}

} // namespace UHE::RD3d
