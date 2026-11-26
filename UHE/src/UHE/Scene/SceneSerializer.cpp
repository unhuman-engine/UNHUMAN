#include "uhepch.h"
#include "SceneSerializer.h"
#include <fstream>
#include "Components.h"
#include "Entity.h"
#include "UHE/Renderer/Texture.h"
#include "UHE/Utils/YamlUtils.h"

namespace UHE
{

SceneSerializer::SceneSerializer(const Ref<Scene>& scene) : m_Scene(scene) {}

static void SerializeEntity(YAML::Emitter& out, Entity entity)
{
    UHE_CORE_ASSERT(entity, "Entity is null!");
    UHE_CORE_ASSERT(entity.GetUUID() != 0, "Entity must have a valid ID!");

    out << YAML::BeginMap;

    out << YAML::Key << "Entity" << YAML::Value << entity.GetUUID();

    if (entity.HasComponent<TagComponent>())
    {
        out << YAML::Key << "TagComponent" << YAML::BeginMap;
        out << YAML::Key << "Tag" << YAML::Value << entity.GetComponent<TagComponent>().Tag;
        out << YAML::EndMap;
    }

    if (entity.HasComponent<TransformComponent>())
    {
        auto& tc = entity.GetComponent<TransformComponent>();

        out << YAML::Key << "TransformComponent" << YAML::BeginMap;
        out << YAML::Key << "Translation" << YAML::Value << tc.Translation;
        out << YAML::Key << "Rotation" << YAML::Value << tc.Rotation;
        out << YAML::Key << "Scale" << YAML::Value << tc.Scale;
        out << YAML::EndMap;
    }

    if (entity.HasComponent<CameraComponent>())
    {
        auto& cc = entity.GetComponent<CameraComponent>();
        auto& cam = cc.Camera;

        out << YAML::Key << "CameraComponent" << YAML::BeginMap;

        out << YAML::Key << "ProjectionType" << YAML::Value << (int)cam.GetProjectionType();
        out << YAML::Key << "PerspectiveFOV" << YAML::Value << cam.GetProjectionFOV();
        out << YAML::Key << "PerspectiveNear" << YAML::Value << cam.GetProjectionNear();
        out << YAML::Key << "PerspectiveFar" << YAML::Value << cam.GetProjectionFar();

        out << YAML::Key << "OrthographicSize" << YAML::Value << cam.GetOrthographicSize();
        out << YAML::Key << "OrthographicNear" << YAML::Value << cam.GetOrthographicNearClip();
        out << YAML::Key << "OrthographicFar" << YAML::Value << cam.GetOrthographicFarClip();

        out << YAML::Key << "Primary" << YAML::Value << cc.Primary;
        out << YAML::Key << "FixedAspectRatio" << YAML::Value << cc.FixedAspectRatio;

        out << YAML::EndMap;
    }

    // Sprite
    if (entity.HasComponent<SpriteRendererComponent>())
    {
        auto& src = entity.GetComponent<SpriteRendererComponent>();

        out << YAML::Key << "SpriteRendererComponent" << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << src.Color;
        out << YAML::Key << "TexturePath" << YAML::Value << src.TexturePath;
        out << YAML::Key << "TilingFactor" << YAML::Value << src.TilingFactor;
        out << YAML::Key << "UseSubTexture" << YAML::Value << src.UseSubTexture;
        out << YAML::Key << "SubTextureCoords" << YAML::Value << src.SubTextureCoords;
        out << YAML::Key << "SubTextureCellSize" << YAML::Value << src.SubTextureCellSize;
        out << YAML::Key << "SubTextureSpriteSize" << YAML::Value << src.SubTextureSpriteSize;
        out << YAML::EndMap;
    }

    // TextComponent
    if (entity.HasComponent<TextComponent>())
    {
        auto& tc = entity.GetComponent<TextComponent>();

        out << YAML::Key << "TextComponent" << YAML::BeginMap;
        out << YAML::Key << "TextString" << YAML::Value << tc.TextString;
        if (tc.FontAsset && tc.FontAsset->IsValid())
            out << YAML::Key << "FontAssetPath" << YAML::Value << tc.FontAsset->GetPath();
        out << YAML::Key << "Color" << YAML::Value << tc.Color;
        out << YAML::Key << "Kerning" << YAML::Value << tc.Kerning;
        out << YAML::Key << "LineSpacing" << YAML::Value << tc.LineSpacing;
        out << YAML::EndMap;
    }


    // Sprite Animation
    if (entity.HasComponent<SpriteAnimationComponent>())
    {
        out << YAML::Key << "SpriteAnimationComponent";
        out << YAML::BeginMap;

        auto& animComp = entity.GetComponent<SpriteAnimationComponent>();
        auto& anim = animComp.Animation;

        out << YAML::Key << "SpriteSheetPath" << YAML::Value << animComp.SpriteSheetPath;
        out << YAML::Key << "FrameSizeX" << YAML::Value << anim.FrameSize.x;
        out << YAML::Key << "FrameSizeY" << YAML::Value << anim.FrameSize.y;
        out << YAML::Key << "FrameCount" << YAML::Value << anim.FrameCount;
        out << YAML::Key << "FrameDuration" << YAML::Value << anim.FrameDuration;
        out << YAML::Key << "Loop" << YAML::Value << anim.Loop;
        out << YAML::Key << "Color" << YAML::Value << animComp.Color;

        out << YAML::EndMap; // SpriteAnimationComponent
    }

    // RigidBody2D
    if (entity.HasComponent<RigidBody2DComponent>())
    {
        auto& rb2d = entity.GetComponent<RigidBody2DComponent>();

        out << YAML::Key << "RigidBody2DComponent" << YAML::BeginMap;
        out << YAML::Key << "BodyType" << YAML::Value << (int)rb2d.Type;
        out << YAML::Key << "FixedRotation" << YAML::Value << rb2d.FixedRotation;
        out << YAML::EndMap;
    }

    // BoxCollider2D
    if (entity.HasComponent<BoxColliderComponent>())
    {
        auto& bc2d = entity.GetComponent<BoxColliderComponent>();

        out << YAML::Key << "BoxColliderComponent" << YAML::BeginMap;
        out << YAML::Key << "Offset" << YAML::Value << bc2d.Offset;
        out << YAML::Key << "Size" << YAML::Value << bc2d.Size;
        out << YAML::Key << "Density" << YAML::Value << bc2d.Density;
        out << YAML::Key << "Friction" << YAML::Value << bc2d.Friction;
        out << YAML::Key << "Restitution" << YAML::Value << bc2d.Restitution;
        out << YAML::Key << "RestitutionThreshold" << YAML::Value << bc2d.RestitutionThreshold;
        out << YAML::EndMap;
    }

    // RigidBody3D
    if (entity.HasComponent<RigidBody3DComponent>())
    {
        auto& rb3d = entity.GetComponent<RigidBody3DComponent>();

        out << YAML::Key << "RigidBody3DComponent" << YAML::BeginMap;
        out << YAML::Key << "BodyType" << YAML::Value << (int)rb3d.Type;
        out << YAML::Key << "Mass" << YAML::Value << rb3d.Mass;
        out << YAML::Key << "LinearDamping" << YAML::Value << rb3d.LinearDamping;
        out << YAML::Key << "AngularDamping" << YAML::Value << rb3d.AngularDamping;
        out << YAML::Key << "IsSensor" << YAML::Value << rb3d.IsSensor;
        out << YAML::Key << "AllowedDOFs" << YAML::BeginSeq;
        for (int i = 0; i < 6; i++) out << rb3d.AllowedDOFs[i];
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }

    // BoxCollider3D
    if (entity.HasComponent<BoxCollider3DComponent>())
    {
        auto& bc3d = entity.GetComponent<BoxCollider3DComponent>();

        out << YAML::Key << "BoxCollider3DComponent" << YAML::BeginMap;
        out << YAML::Key << "Offset" << YAML::Value << bc3d.Offset;
        out << YAML::Key << "HalfExtent" << YAML::Value << bc3d.HalfExtent;
        out << YAML::Key << "Friction" << YAML::Value << bc3d.Friction;
        out << YAML::Key << "Restitution" << YAML::Value << bc3d.Restitution;
        out << YAML::EndMap;
    }

    // SphereCollider3D
    if (entity.HasComponent<SphereCollider3DComponent>())
    {
        auto& sc3d = entity.GetComponent<SphereCollider3DComponent>();

        out << YAML::Key << "SphereCollider3DComponent" << YAML::BeginMap;
        out << YAML::Key << "Offset" << YAML::Value << sc3d.Offset;
        out << YAML::Key << "Radius" << YAML::Value << sc3d.Radius;
        out << YAML::Key << "Friction" << YAML::Value << sc3d.Friction;
        out << YAML::Key << "Restitution" << YAML::Value << sc3d.Restitution;
        out << YAML::EndMap;
    }

    // CapsuleCollider3D
    if (entity.HasComponent<CapsuleCollider3DComponent>())
    {
        auto& cc3d = entity.GetComponent<CapsuleCollider3DComponent>();

        out << YAML::Key << "CapsuleCollider3DComponent" << YAML::BeginMap;
        out << YAML::Key << "Offset" << YAML::Value << cc3d.Offset;
        out << YAML::Key << "Radius" << YAML::Value << cc3d.Radius;
        out << YAML::Key << "HalfHeight" << YAML::Value << cc3d.HalfHeight;
        out << YAML::Key << "Friction" << YAML::Value << cc3d.Friction;
        out << YAML::Key << "Restitution" << YAML::Value << cc3d.Restitution;
        out << YAML::EndMap;
    }

    // Model3D
    if (entity.HasComponent<Model3DComponent>())
    {
        out << YAML::Key << "Model3DComponent" << YAML::BeginMap;
        auto& mc = entity.GetComponent<Model3DComponent>();
        out << YAML::Key << "ModelPath" << YAML::Value << mc.ModelPath;
        out << YAML::EndMap;
    }

    // Script (Lua)
    if (entity.HasComponent<ScriptComponent>())
    {
        auto& sc = entity.GetComponent<ScriptComponent>();
        out << YAML::Key << "ScriptComponent" << YAML::BeginMap;
        out << YAML::Key << "ScriptPath" << YAML::Value << sc.ScriptPath;
        out << YAML::Key << "Enabled" << YAML::Value << sc.Enabled;
        out << YAML::EndMap;
    }

    // Audio source
    if (entity.HasComponent<AudioSourceComponent>())
    {
        auto& audio = entity.GetComponent<AudioSourceComponent>();
        out << YAML::Key << "AudioSourceComponent" << YAML::BeginMap;
        out << YAML::Key << "SoundPath" << YAML::Value << audio.SoundPath;
        out << YAML::Key << "Volume" << YAML::Value << audio.Volume;
        out << YAML::Key << "Pitch" << YAML::Value << audio.Pitch;
        out << YAML::Key << "Pan" << YAML::Value << audio.Pan;
        out << YAML::Key << "Loop" << YAML::Value << audio.Loop;
        out << YAML::Key << "Spatial" << YAML::Value << audio.Spatial;
        out << YAML::Key << "Autoplay" << YAML::Value << audio.Autoplay;
        out << YAML::Key << "Muted" << YAML::Value << audio.Muted;
        out << YAML::Key << "MinDistance" << YAML::Value << audio.MinDistance;
        out << YAML::Key << "MaxDistance" << YAML::Value << audio.MaxDistance;
        out << YAML::Key << "Rolloff" << YAML::Value << audio.Rolloff;
        out << YAML::EndMap;
    }

    // Animator
    if (entity.HasComponent<AnimatorComponent>())
    {
        auto& ac = entity.GetComponent<AnimatorComponent>();
        out << YAML::Key << "AnimatorComponent" << YAML::BeginMap;
        out << YAML::Key << "CurrentAnimationName" << YAML::Value << ac.CurrentAnimationName;
        out << YAML::Key << "PlaybackSpeed" << YAML::Value << ac.PlaybackSpeed;
        out << YAML::Key << "IsPlaying" << YAML::Value << ac.IsPlaying;
        // Playback and cross-fade blending controls
        out << YAML::Key << "LoopMode" << YAML::Value << static_cast<int>(ac.LoopMode);
        out << YAML::Key << "Reverse" << YAML::Value << ac.Reverse;
        out << YAML::Key << "RootMotion" << YAML::Value << ac.RootMotion;
        out << YAML::Key << "CrossFadeDuration" << YAML::Value << ac.CrossFadeDuration;
        out << YAML::Key << "SkinIndex" << YAML::Value << ac.SkinIndex;

        // State-machine graph (editor-authored; runtime state is not persisted).
        out << YAML::Key << "Graph" << YAML::BeginMap;
        out << YAML::Key << "Enabled" << YAML::Value << ac.Graph.Enabled;
        out << YAML::Key << "EntryState" << YAML::Value << ac.Graph.EntryState;

        out << YAML::Key << "States" << YAML::BeginSeq;
        for (const AnimationGraphState& state : ac.Graph.States)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << state.Name;
            out << YAML::Key << "Clip" << YAML::Value << state.Clip;
            out << YAML::Key << "Position" << YAML::Value << state.EditorPosition;
            out << YAML::Key << "Speed" << YAML::Value << state.Speed;
            out << YAML::Key << "Loop" << YAML::Value << state.Loop;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "Transitions" << YAML::BeginSeq;
        for (const AnimationGraphTransition& transition : ac.Graph.Transitions)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "From" << YAML::Value << transition.From;
            out << YAML::Key << "To" << YAML::Value << transition.To;
            out << YAML::Key << "Parameter" << YAML::Value << transition.Parameter;
            out << YAML::Key << "Condition" << YAML::Value << static_cast<int>(transition.Condition);
            out << YAML::Key << "Threshold" << YAML::Value << transition.Threshold;
            out << YAML::Key << "Duration" << YAML::Value << transition.Duration;
            out << YAML::Key << "HasExitTime" << YAML::Value << transition.HasExitTime;
            out << YAML::Key << "ExitTime" << YAML::Value << transition.ExitTime;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "Parameters" << YAML::BeginSeq;
        for (const AnimationGraphParameter& parameter : ac.Graph.Parameters)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << parameter.Name;
            out << YAML::Key << "Value" << YAML::Value << parameter.Value;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap; // Graph

        out << YAML::EndMap; // AnimatorComponent
    }

    // DirectionalLight
    if (entity.HasComponent<DirectionalLightComponent>())
    {
        auto& dlc = entity.GetComponent<DirectionalLightComponent>();
        out << YAML::Key << "DirectionalLightComponent" << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << dlc.Color;
        out << YAML::Key << "Intensity" << YAML::Value << dlc.Intensity;
        out << YAML::Key << "CastShadows" << YAML::Value << dlc.CastShadows;
        out << YAML::EndMap;
    }

    // Foliage (sways in the scene's wind)
    if (entity.HasComponent<FoliageComponent>())
    {
        auto& fc = entity.GetComponent<FoliageComponent>();
        out << YAML::Key << "FoliageComponent" << YAML::BeginMap;
        out << YAML::Key << "Bend" << YAML::Value << fc.Bend;
        out << YAML::Key << "Height" << YAML::Value << fc.Height;
        out << YAML::EndMap;
    }

    // PointLight
    if (entity.HasComponent<PointLightComponent>())
    {
        auto& plc = entity.GetComponent<PointLightComponent>();
        out << YAML::Key << "PointLightComponent" << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << plc.Color;
        out << YAML::Key << "Intensity" << YAML::Value << plc.Intensity;
        out << YAML::Key << "Radius" << YAML::Value << plc.Radius;
        out << YAML::Key << "CastShadows" << YAML::Value << plc.CastShadows;
        out << YAML::EndMap;
    }

    // SpotLight (KHR_lights_punctual parity; cone angles in radians)
    if (entity.HasComponent<SpotLightComponent>())
    {
        auto& slc = entity.GetComponent<SpotLightComponent>();
        out << YAML::Key << "SpotLightComponent" << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << slc.Color;
        out << YAML::Key << "Intensity" << YAML::Value << slc.Intensity;
        out << YAML::Key << "Radius" << YAML::Value << slc.Radius;
        out << YAML::Key << "InnerConeAngle" << YAML::Value << slc.InnerConeAngle;
        out << YAML::Key << "OuterConeAngle" << YAML::Value << slc.OuterConeAngle;
        out << YAML::Key << "CastShadows" << YAML::Value << slc.CastShadows;
        out << YAML::EndMap;
    }

    // AreaLight (a rectangle on the local XY plane, shining along local -Z)
    if (entity.HasComponent<AreaLightComponent>())
    {
        auto& alc = entity.GetComponent<AreaLightComponent>();
        out << YAML::Key << "AreaLightComponent" << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << alc.Color;
        out << YAML::Key << "Intensity" << YAML::Value << alc.Intensity;
        out << YAML::Key << "Width" << YAML::Value << alc.Width;
        out << YAML::Key << "Height" << YAML::Value << alc.Height;
        out << YAML::Key << "Range" << YAML::Value << alc.Range;
        out << YAML::Key << "TwoSided" << YAML::Value << alc.TwoSided;
        out << YAML::Key << "CastShadows" << YAML::Value << alc.CastShadows;
        out << YAML::EndMap;
    }

    // Water (direction in degrees; colours in sRGB)
    if (entity.HasComponent<WaterComponent>())
    {
        auto& wc = entity.GetComponent<WaterComponent>();
        out << YAML::Key << "WaterComponent" << YAML::BeginMap;
        out << YAML::Key << "Size" << YAML::Value << wc.Size;
        out << YAML::Key << "Resolution" << YAML::Value << wc.Resolution;
        out << YAML::Key << "ShallowColor" << YAML::Value << wc.ShallowColor;
        out << YAML::Key << "DeepColor" << YAML::Value << wc.DeepColor;
        out << YAML::Key << "Clarity" << YAML::Value << wc.Clarity;
        out << YAML::Key << "WaveHeight" << YAML::Value << wc.WaveHeight;
        out << YAML::Key << "WaveLength" << YAML::Value << wc.WaveLength;
        out << YAML::Key << "WaveSpeed" << YAML::Value << wc.WaveSpeed;
        out << YAML::Key << "Choppiness" << YAML::Value << wc.Choppiness;
        out << YAML::Key << "Direction" << YAML::Value << wc.Direction;
        out << YAML::Key << "FollowWind" << YAML::Value << wc.FollowWind;
        out << YAML::Key << "Foam" << YAML::Value << wc.Foam;
        out << YAML::EndMap;
    }

    // Game UI (colours sRGB with alpha; layout results and mouse state are not saved)
    if (entity.HasComponent<UIRectComponent>())
    {
        auto& r = entity.GetComponent<UIRectComponent>();
        out << YAML::Key << "UIRectComponent" << YAML::BeginMap;
        out << YAML::Key << "AnchorMin" << YAML::Value << r.AnchorMin;
        out << YAML::Key << "AnchorMax" << YAML::Value << r.AnchorMax;
        out << YAML::Key << "Pivot" << YAML::Value << r.Pivot;
        out << YAML::Key << "Position" << YAML::Value << r.Position;
        out << YAML::Key << "Size" << YAML::Value << r.Size;
        out << YAML::Key << "Order" << YAML::Value << r.Order;
        out << YAML::Key << "Visible" << YAML::Value << r.Visible;
        out << YAML::EndMap;
    }
    if (entity.HasComponent<UIImageComponent>())
    {
        auto& i = entity.GetComponent<UIImageComponent>();
        out << YAML::Key << "UIImageComponent" << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << i.Color;
        out << YAML::Key << "TexturePath" << YAML::Value << i.TexturePath;
        out << YAML::Key << "CornerRadius" << YAML::Value << i.CornerRadius;
        out << YAML::Key << "BorderWidth" << YAML::Value << i.BorderWidth;
        out << YAML::Key << "BorderColor" << YAML::Value << i.BorderColor;
        out << YAML::EndMap;
    }
    if (entity.HasComponent<UITextComponent>())
    {
        auto& t = entity.GetComponent<UITextComponent>();
        out << YAML::Key << "UITextComponent" << YAML::BeginMap;
        out << YAML::Key << "Text" << YAML::Value << t.Text;
        out << YAML::Key << "FontSize" << YAML::Value << t.FontSize;
        out << YAML::Key << "Color" << YAML::Value << t.Color;
        out << YAML::Key << "Align" << YAML::Value << static_cast<int>(t.Align);
        out << YAML::Key << "FontPath" << YAML::Value << t.FontPath;
        out << YAML::EndMap;
    }
    if (entity.HasComponent<UIButtonComponent>())
    {
        auto& b = entity.GetComponent<UIButtonComponent>();
        out << YAML::Key << "UIButtonComponent" << YAML::BeginMap;
        out << YAML::Key << "NormalColor" << YAML::Value << b.NormalColor;
        out << YAML::Key << "HoverColor" << YAML::Value << b.HoverColor;
        out << YAML::Key << "PressedColor" << YAML::Value << b.PressedColor;
        out << YAML::Key << "Interactable" << YAML::Value << b.Interactable;
        out << YAML::EndMap;
    }
    if (entity.HasComponent<UIProgressBarComponent>())
    {
        auto& p = entity.GetComponent<UIProgressBarComponent>();
        out << YAML::Key << "UIProgressBarComponent" << YAML::BeginMap;
        out << YAML::Key << "Value" << YAML::Value << p.Value;
        out << YAML::Key << "FillColor" << YAML::Value << p.FillColor;
        out << YAML::Key << "BackgroundColor" << YAML::Value << p.BackgroundColor;
        out << YAML::Key << "CornerRadius" << YAML::Value << p.CornerRadius;
        out << YAML::Key << "Interactable" << YAML::Value << p.Interactable;
        out << YAML::EndMap;
    }

    // Terrain (colours in sRGB; the mesh and heights are rebuilt on load)
    if (entity.HasComponent<TerrainComponent>())
    {
        auto& tc = entity.GetComponent<TerrainComponent>();
        out << YAML::Key << "TerrainComponent" << YAML::BeginMap;
        out << YAML::Key << "HeightmapPath" << YAML::Value << tc.HeightmapPath;
        out << YAML::Key << "Size" << YAML::Value << tc.Size;
        out << YAML::Key << "Height" << YAML::Value << tc.Height;
        out << YAML::Key << "Resolution" << YAML::Value << tc.Resolution;
        out << YAML::Key << "Seed" << YAML::Value << tc.Seed;
        out << YAML::Key << "FeatureSize" << YAML::Value << tc.FeatureSize;
        out << YAML::Key << "Octaves" << YAML::Value << tc.Octaves;
        out << YAML::Key << "Roughness" << YAML::Value << tc.Roughness;
        out << YAML::Key << "SandColor" << YAML::Value << tc.SandColor;
        out << YAML::Key << "GrassColor" << YAML::Value << tc.GrassColor;
        out << YAML::Key << "RockColor" << YAML::Value << tc.RockColor;
        out << YAML::Key << "SnowColor" << YAML::Value << tc.SnowColor;
        out << YAML::Key << "SandLevel" << YAML::Value << tc.SandLevel;
        out << YAML::Key << "SnowLevel" << YAML::Value << tc.SnowLevel;
        out << YAML::Key << "RockSlope" << YAML::Value << tc.RockSlope;
        out << YAML::Key << "Collider" << YAML::Value << tc.Collider;
        out << YAML::EndMap;
    }

    // Wind zone (its direction is the entity's local -Z, so only the strengths are stored)
    if (entity.HasComponent<WindComponent>())
    {
        auto& wind = entity.GetComponent<WindComponent>();
        out << YAML::Key << "WindComponent" << YAML::BeginMap;
        out << YAML::Key << "Strength" << YAML::Value << wind.Strength;
        out << YAML::Key << "GustStrength" << YAML::Value << wind.GustStrength;
        out << YAML::Key << "GustFrequency" << YAML::Value << wind.GustFrequency;
        out << YAML::EndMap;
    }

    // Issue #17: entity hierarchy (parent/children by UUID)
    if (entity.HasComponent<RelationshipComponent>())
    {
        auto& rel = entity.GetComponent<RelationshipComponent>();
        out << YAML::Key << "RelationshipComponent" << YAML::BeginMap;
        out << YAML::Key << "Parent" << YAML::Value << rel.Parent;
        out << YAML::Key << "Children" << YAML::BeginSeq;
        for (u64 childID : rel.Children)
            out << childID;
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }

    // Issue #17: glTF node entity marker
    if (entity.HasComponent<ModelNodeComponent>())
    {
        auto& mnc = entity.GetComponent<ModelNodeComponent>();
        out << YAML::Key << "ModelNodeComponent" << YAML::BeginMap;
        out << YAML::Key << "ModelEntity" << YAML::Value << mnc.ModelEntity;
        out << YAML::Key << "NodeIndex" << YAML::Value << mnc.NodeIndex;
        out << YAML::Key << "NodeName" << YAML::Value << mnc.NodeName;
        out << YAML::Key << "HasChildrenNodes" << YAML::Value << mnc.HasChildrenNodes;
        out << YAML::EndMap;
    }

    out << YAML::EndMap;
}

/// @brief Writes the scene's RenderSettings as the top-level "Environment" map, one sub-map per group.
static void SerializeRenderSettings(YAML::Emitter& out, const RenderSettings& settings)
{
    out << YAML::Key << "Environment" << YAML::BeginMap;

    const EnvironmentSettings& env = settings.Environment;
    out << YAML::Key << "Environment" << YAML::BeginMap;
    out << YAML::Key << "SkyColor" << YAML::Value << env.SkyColor;
    out << YAML::Key << "GroundColor" << YAML::Value << env.GroundColor;
    out << YAML::Key << "AmbientIntensity" << YAML::Value << env.AmbientIntensity;
    out << YAML::Key << "Exposure" << YAML::Value << env.Exposure;
    out << YAML::EndMap;

    const SkySettings& sky = settings.Sky;
    out << YAML::Key << "Sky" << YAML::BeginMap;
    const char* modeNames[] = {"Color", "Procedural", "Image"};
    out << YAML::Key << "Mode" << YAML::Value << modeNames[std::clamp(static_cast<int>(sky.Mode), 0, 2)];
    out << YAML::Key << "Intensity" << YAML::Value << sky.Intensity;
    out << YAML::Key << "ZenithColor" << YAML::Value << sky.ZenithColor;
    out << YAML::Key << "HorizonColor" << YAML::Value << sky.HorizonColor;
    out << YAML::Key << "GroundColor" << YAML::Value << sky.GroundColor;
    out << YAML::Key << "SunSize" << YAML::Value << sky.SunSize;
    out << YAML::Key << "SunBrightness" << YAML::Value << sky.SunBrightness;
    out << YAML::Key << "ImagePath" << YAML::Value << sky.ImagePath;
    out << YAML::Key << "ImageRotation" << YAML::Value << sky.ImageRotation;
    out << YAML::Key << "AmbientFromSky" << YAML::Value << sky.AmbientFromSky;
    out << YAML::EndMap;

    out << YAML::Key << "AntiAliasing" << YAML::BeginMap;
    out << YAML::Key << "FXAA" << YAML::Value << settings.AntiAliasing.FXAA;
    out << YAML::EndMap;

    out << YAML::Key << "UI" << YAML::BeginMap;
    out << YAML::Key << "ScaleWithScreen" << YAML::Value << settings.UI.ScaleWithScreen;
    out << YAML::Key << "ReferenceHeight" << YAML::Value << settings.UI.ReferenceHeight;
    out << YAML::EndMap;

    const ShadowSettings& shadows = settings.Shadows;
    out << YAML::Key << "Shadows" << YAML::BeginMap;
    out << YAML::Key << "Enabled" << YAML::Value << shadows.Enabled;
    out << YAML::Key << "CascadeCount" << YAML::Value << shadows.CascadeCount;
    out << YAML::Key << "Distance" << YAML::Value << shadows.Distance;
    out << YAML::Key << "SplitLambda" << YAML::Value << shadows.SplitLambda;
    out << YAML::Key << "Resolution" << YAML::Value << shadows.Resolution;
    out << YAML::Key << "DepthBias" << YAML::Value << shadows.DepthBias;
    out << YAML::Key << "NormalBias" << YAML::Value << shadows.NormalBias;
    out << YAML::Key << "Softness" << YAML::Value << shadows.Softness;
    out << YAML::Key << "ShowCascades" << YAML::Value << shadows.ShowCascades;
    out << YAML::EndMap;

    const LocalShadowSettings& localShadows = settings.LocalShadows;
    out << YAML::Key << "LocalShadows" << YAML::BeginMap;
    out << YAML::Key << "Enabled" << YAML::Value << localShadows.Enabled;
    out << YAML::Key << "TileResolution" << YAML::Value << localShadows.TileResolution;
    out << YAML::Key << "MaxTiles" << YAML::Value << localShadows.MaxTiles;
    out << YAML::Key << "DepthBias" << YAML::Value << localShadows.DepthBias;
    out << YAML::Key << "NormalBias" << YAML::Value << localShadows.NormalBias;
    out << YAML::EndMap;

    const SSAOSettings& ssao = settings.SSAO;
    out << YAML::Key << "SSAO" << YAML::BeginMap;
    out << YAML::Key << "Enabled" << YAML::Value << ssao.Enabled;
    out << YAML::Key << "Radius" << YAML::Value << ssao.Radius;
    out << YAML::Key << "Intensity" << YAML::Value << ssao.Intensity;
    out << YAML::Key << "Bias" << YAML::Value << ssao.Bias;
    out << YAML::EndMap;

    const BloomSettings& bloom = settings.Bloom;
    out << YAML::Key << "Bloom" << YAML::BeginMap;
    out << YAML::Key << "Enabled" << YAML::Value << bloom.Enabled;
    out << YAML::Key << "Intensity" << YAML::Value << bloom.Intensity;
    out << YAML::Key << "FilterRadius" << YAML::Value << bloom.FilterRadius;
    out << YAML::EndMap;

    const WindSettings& wind = settings.Wind;
    out << YAML::Key << "Wind" << YAML::BeginMap;
    out << YAML::Key << "Direction" << YAML::Value << wind.Direction;
    out << YAML::Key << "Strength" << YAML::Value << wind.Strength;
    out << YAML::Key << "GustStrength" << YAML::Value << wind.GustStrength;
    out << YAML::Key << "GustFrequency" << YAML::Value << wind.GustFrequency;
    out << YAML::EndMap;

    out << YAML::EndMap; // Environment
}

/**
 * @brief Reads `node[key]` into `value`, keeping the current value when the key is missing or mistyped.
 *
 * The Environment block is newer than most scenes, and a scene that predates a
 * field must load with that field's default rather than fail to load at all.
 */
template <typename T> static void ReadOptional(const YAML::Node& node, const char* key, T& value)
{
    if (!node)
        return;
    if (const YAML::Node field = node[key])
        value = field.as<T>(value);
}

/// @brief Reads the "Environment" map into `settings`; any missing group or field keeps its default.
static void DeserializeRenderSettings(const YAML::Node& node, RenderSettings& settings)
{
    if (!node || !node.IsMap())
        return;

    const YAML::Node envNode = node["Environment"];
    EnvironmentSettings& env = settings.Environment;
    ReadOptional(envNode, "SkyColor", env.SkyColor);
    ReadOptional(envNode, "GroundColor", env.GroundColor);
    ReadOptional(envNode, "AmbientIntensity", env.AmbientIntensity);
    ReadOptional(envNode, "Exposure", env.Exposure);

    const YAML::Node skyNode = node["Sky"];
    SkySettings& sky = settings.Sky;
    std::string skyMode;
    ReadOptional(skyNode, "Mode", skyMode);
    if (skyMode == "Color")
        sky.Mode = SkySettings::SkyMode::Color;
    else if (skyMode == "Image")
        sky.Mode = SkySettings::SkyMode::Image;
    else if (skyMode == "Procedural")
        sky.Mode = SkySettings::SkyMode::Procedural;
    ReadOptional(skyNode, "Intensity", sky.Intensity);
    ReadOptional(skyNode, "ZenithColor", sky.ZenithColor);
    ReadOptional(skyNode, "HorizonColor", sky.HorizonColor);
    ReadOptional(skyNode, "GroundColor", sky.GroundColor);
    ReadOptional(skyNode, "SunSize", sky.SunSize);
    ReadOptional(skyNode, "SunBrightness", sky.SunBrightness);
    ReadOptional(skyNode, "ImagePath", sky.ImagePath);
    ReadOptional(skyNode, "ImageRotation", sky.ImageRotation);
    ReadOptional(skyNode, "AmbientFromSky", sky.AmbientFromSky);

    ReadOptional(node["AntiAliasing"], "FXAA", settings.AntiAliasing.FXAA);

    const YAML::Node uiNode = node["UI"];
    ReadOptional(uiNode, "ScaleWithScreen", settings.UI.ScaleWithScreen);
    ReadOptional(uiNode, "ReferenceHeight", settings.UI.ReferenceHeight);
    settings.UI.ReferenceHeight = std::clamp(settings.UI.ReferenceHeight, 100.0f, 8640.0f);

    const YAML::Node shadowNode = node["Shadows"];
    ShadowSettings& shadows = settings.Shadows;
    ReadOptional(shadowNode, "Enabled", shadows.Enabled);
    ReadOptional(shadowNode, "CascadeCount", shadows.CascadeCount);
    ReadOptional(shadowNode, "Distance", shadows.Distance);
    ReadOptional(shadowNode, "SplitLambda", shadows.SplitLambda);
    ReadOptional(shadowNode, "Resolution", shadows.Resolution);
    ReadOptional(shadowNode, "DepthBias", shadows.DepthBias);
    ReadOptional(shadowNode, "NormalBias", shadows.NormalBias);
    ReadOptional(shadowNode, "Softness", shadows.Softness);
    ReadOptional(shadowNode, "ShowCascades", shadows.ShowCascades);
    // These two size GPU resources, so a hand-edited file must not take them out of range.
    shadows.CascadeCount = std::clamp(shadows.CascadeCount, 1, 4);
    shadows.Resolution = std::clamp(shadows.Resolution, 512, 4096);

    const YAML::Node localShadowNode = node["LocalShadows"];
    LocalShadowSettings& localShadows = settings.LocalShadows;
    ReadOptional(localShadowNode, "Enabled", localShadows.Enabled);
    ReadOptional(localShadowNode, "TileResolution", localShadows.TileResolution);
    ReadOptional(localShadowNode, "MaxTiles", localShadows.MaxTiles);
    ReadOptional(localShadowNode, "DepthBias", localShadows.DepthBias);
    ReadOptional(localShadowNode, "NormalBias", localShadows.NormalBias);
    // Both size the shadow atlas, so they are held in range like the cascade settings above.
    localShadows.TileResolution = std::clamp(localShadows.TileResolution, 256, 2048);
    localShadows.MaxTiles = std::clamp(localShadows.MaxTiles, 1, 64);

    const YAML::Node ssaoNode = node["SSAO"];
    SSAOSettings& ssao = settings.SSAO;
    ReadOptional(ssaoNode, "Enabled", ssao.Enabled);
    ReadOptional(ssaoNode, "Radius", ssao.Radius);
    ReadOptional(ssaoNode, "Intensity", ssao.Intensity);
    ReadOptional(ssaoNode, "Bias", ssao.Bias);

    const YAML::Node bloomNode = node["Bloom"];
    BloomSettings& bloom = settings.Bloom;
    ReadOptional(bloomNode, "Enabled", bloom.Enabled);
    ReadOptional(bloomNode, "Intensity", bloom.Intensity);
    ReadOptional(bloomNode, "FilterRadius", bloom.FilterRadius);

    const YAML::Node windNode = node["Wind"];
    WindSettings& wind = settings.Wind;
    ReadOptional(windNode, "Direction", wind.Direction);
    ReadOptional(windNode, "Strength", wind.Strength);
    ReadOptional(windNode, "GustStrength", wind.GustStrength);
    ReadOptional(windNode, "GustFrequency", wind.GustFrequency);
}

static void SerializeEntityRecursive(YAML::Emitter& out, Entity entity, std::unordered_set<u64>& written)
{
    // Exactly-once guard: a UUID reachable through two paths (e.g. listed in
    // two Children lists) must serialize a single time.
    if (!written.insert(entity.GetUUID()).second)
        return;

    SerializeEntity(out, entity);

    // Issue #17 hardening: walk children explicitly instead of serializing
    // every registry entity at top level. A child entry is followed only when
    // the child's own Parent points back at us, so stale child-list entries
    // (double-listed or re-parented elsewhere) cannot serialize twice.
    if (entity.HasComponent<RelationshipComponent>())
    {
        auto childrenCopy = entity.GetComponent<RelationshipComponent>().Children;
        for (u64 childID : childrenCopy)
        {
            Entity child = entity.GetScene()->GetEntityWithUUID(childID);
            if (child && child.HasComponent<RelationshipComponent>() &&
                child.GetComponent<RelationshipComponent>().Parent == entity.GetUUID())
                SerializeEntityRecursive(out, child, written);
        }
    }
}

void SceneSerializer::Serialize(const std::string& filepath)
{
    YAML::Emitter out;

    out << YAML::BeginMap;
    out << YAML::Key << "Scene" << YAML::Value << "Untitled";
    // 2: cameras store their own perspective far plane (format 1 wrote the
    // orthographic one there; those files are repaired on load).
    out << YAML::Key << "SceneFormat" << YAML::Value << 2;
    SerializeRenderSettings(out, m_Scene->GetRenderSettings());
    out << YAML::Key << "Entities" << YAML::BeginSeq;

    // Pass 1: hierarchy order — roots first, then each root's subtree.
    std::unordered_set<u64> written;
    for (Entity root : m_Scene->GetRootEntities())
        SerializeEntityRecursive(out, root, written);

    // Pass 2: fallback sweep — any live entity not reached through a parent's
    // Children list (missing/stale parent entry) would silently vanish from
    // the save. Emit it at top level so nothing is ever lost.
    auto view = m_Scene->m_registry.view<entt::entity>();
    for (auto entityID : view)
    {
        Entity entity{entityID, m_Scene.get()};
        if (entity && !written.count(entity.GetUUID()))
            SerializeEntityRecursive(out, entity, written);
    }

    out << YAML::EndSeq;
    out << YAML::EndMap;

    std::ofstream fout(filepath);
    fout << out.c_str();
}

bool SceneSerializer::Deserialize(const std::string& filepath)
{
    // yaml-cpp reports malformed input and every missing or mistyped field
    // (`node["X"].as<T>()`) by throwing. Contain all of it here.
    try
    {
        return DeserializeUnchecked(filepath);
    }
    catch (const YAML::Exception& e)
    {
        UHE_CORE_ERROR("Could not load scene '{0}': {1}", filepath, e.what());
    }
    catch (const std::exception& e)
    {
        UHE_CORE_ERROR("Could not load scene '{0}': {1}", filepath, e.what());
    }
    return false;
}

bool SceneSerializer::DeserializeUnchecked(const std::string& filepath)
{
    std::ifstream stream(filepath);
    if (!stream.is_open())
        return false;

    YAML::Node data = YAML::Load(stream);
    const int sceneFormat = data["SceneFormat"] ? data["SceneFormat"].as<int>(1) : 1;
    if (!data["Scene"])
        return false;

    // Before the entity list: a scene with no entities still has its environment.
    DeserializeRenderSettings(data["Environment"], m_Scene->GetRenderSettings());

    auto entities = data["Entities"];
    if (!entities)
        return true;

    // Issue #17 hardening: pass 1 creates every entity (restoring serialized
    // UUIDs) and queues hierarchy links; pass 2 fills components. This way a
    // child can resolve its parent even when it appears earlier in the file.
    std::vector<YAML::Node> ordered;
    for (auto entityNode : entities)
        ordered.push_back(entityNode);

    std::unordered_set<u64> seenUUIDs;
    for (auto entityNode : ordered)
    {
        if (entityNode["Entity"])
        {
            u64 serializedID = entityNode["Entity"].as<u64>();
            if (serializedID != 0)
            {
                if (!seenUUIDs.insert(serializedID).second)
                {
                    UHE_CORE_ERROR("Duplicate UUID {0} found in serialized scene", serializedID);
                    return false;
                }
            }
        }
    }

    std::vector<Entity> created;
    created.reserve(ordered.size());
    for (auto entityNode : ordered)
    {
        std::string name = "Entity";
        auto tagNode = entityNode["TagComponent"];
        if (tagNode)
            name = tagNode["Tag"].as<std::string>();

        Entity entity = m_Scene->CreateEntity(name);
        if (entityNode["Entity"])
        {
            u64 serializedID = entityNode["Entity"].as<u64>();
            if (serializedID != 0) // 0 = legacy/pre-UUID file: keep fresh IDs
            {
                u64 generatedID = entity.GetUUID();
                entity.GetComponent<IDComponent>().ID = serializedID;
                m_Scene->UnindexEntity(generatedID); // drop the auto-generated key
                m_Scene->IndexEntity(entity, entity.GetUUID()); // keep the O(1) UUID index in sync
            }
        }

        if (auto relNode = entityNode["RelationshipComponent"])
        {
            auto& rel = entity.GetComponent<RelationshipComponent>();
            if (relNode["Parent"])
                rel.Parent = relNode["Parent"].as<u64>();
            if (relNode["Children"] && relNode["Children"].IsSequence())
            {
                for (auto childID : relNode["Children"])
                    rel.Children.push_back(childID.as<u64>());
            }
        }
        created.push_back(entity);
    }

    // Pass 2: component data.
    for (size_t i = 0; i < ordered.size(); i++)
    {
        auto& entityNode = ordered[i];
        Entity entity = created[i];

        // Transform
        auto transformNode = entityNode["TransformComponent"];
        if (transformNode)
        {
            auto& tc = entity.GetComponent<TransformComponent>();
            tc.Translation = transformNode["Translation"].as<glm::vec3>();
            tc.Rotation = transformNode["Rotation"].as<glm::vec3>();
            tc.Scale = transformNode["Scale"].as<glm::vec3>();
        }

        auto cameraNode = entityNode["CameraComponent"];
        if (cameraNode)
        {
            auto& cc = entity.AddComponent<CameraComponent>();
            auto& cam = cc.Camera;

            cam.SetProjectionType((SceneCamera::ProjectionType)cameraNode["ProjectionType"].as<int>());

            cam.SetPerspectiveFOV(cameraNode["PerspectiveFOV"].as<float>());
            cam.SetPerpecstiveNear(cameraNode["PerspectiveNear"].as<float>());
            // Scenes saved before the GetProjectionFar fix stored the
            // orthographic far plane here; they load with the default instead.
            // Only files from before format 2 can carry the old value; a newer
            // file whose two far planes match was authored that way.
            bool repaired = false;
            const float savedFar = cameraNode["PerspectiveFar"].as<float>();
            cam.SetPerspecstiveFar(
                sceneFormat < 2 ? SceneCamera::LoadedPerspectiveFar(cameraNode["PerspectiveNear"].as<float>(), savedFar,
                                                                    cameraNode["OrthographicFar"].as<float>(), repaired)
                                : savedFar);
            if (repaired)
                UHE_CORE_WARN("Scene '{0}': camera '{1}' had a perspective far plane of {2} (saved by an older "
                              "editor); using {3}. Save the scene to keep it.",
                              filepath, entity.GetComponent<TagComponent>().Tag,
                              cameraNode["PerspectiveFar"].as<float>(), SceneCamera::kDefaultPerspectiveFar);

            cam.SetOrthographicSize(cameraNode["OrthographicSize"].as<float>());
            cam.SetOrthoGraphicNearClip(cameraNode["OrthographicNear"].as<float>());
            cam.SetOrthoGraphicFarClip(cameraNode["OrthographicFar"].as<float>());

            cc.Primary = cameraNode["Primary"].as<bool>();
            cc.FixedAspectRatio = cameraNode["FixedAspectRatio"].as<bool>();
        }

        auto spriteNode = entityNode["SpriteRendererComponent"];
        if (spriteNode)
        {
            auto& src = entity.AddComponent<SpriteRendererComponent>();
            src.Color = spriteNode["Color"].as<glm::vec4>();

            if (spriteNode["TexturePath"])
            {
                src.TexturePath = spriteNode["TexturePath"].as<std::string>();
                if (!src.TexturePath.empty())
                {
                    src.Texture = Texture2D::Create(src.TexturePath);
                }
            }

            if (spriteNode["TilingFactor"])
                src.TilingFactor = spriteNode["TilingFactor"].as<float>();

            if (spriteNode["UseSubTexture"])
            {
                src.UseSubTexture = spriteNode["UseSubTexture"].as<bool>();
                src.SubTextureCoords = spriteNode["SubTextureCoords"].as<glm::vec2>();
                src.SubTextureCellSize = spriteNode["SubTextureCellSize"].as<glm::vec2>();
                src.SubTextureSpriteSize = spriteNode["SubTextureSpriteSize"].as<glm::vec2>();
            }
        }

        auto textNode = entityNode["TextComponent"];
        if (textNode)
        {
            auto& tc = entity.AddComponent<TextComponent>();
            tc.TextString = textNode["TextString"].as<std::string>();
            tc.Color = textNode["Color"].as<glm::vec4>();
            tc.Kerning = textNode["Kerning"].as<float>();
            tc.LineSpacing = textNode["LineSpacing"].as<float>();

            if (textNode["FontAssetPath"])
            {
                std::string fontPath = textNode["FontAssetPath"].as<std::string>();
                if (!fontPath.empty())
                {
                    tc.FontAsset = Font2D::Get(fontPath);
                }
            }
        }

        auto animNode = entityNode["SpriteAnimationComponent"];
        if (animNode)
        {
            auto& animComp = entity.AddComponent<SpriteAnimationComponent>();
            animComp.SpriteSheetPath = animNode["SpriteSheetPath"].as<std::string>();
            auto& anim = animComp.Animation;
            if (!animComp.SpriteSheetPath.empty())
            {
                anim.SpriteSheet = Texture2D::Create(animComp.SpriteSheetPath);
            }

            anim.FrameSize.x = animNode["FrameSizeX"].as<float>();
            anim.FrameSize.y = animNode["FrameSizeY"].as<float>();
            anim.FrameCount = animNode["FrameCount"].as<int>();
            anim.FrameDuration = animNode["FrameDuration"].as<float>();
            anim.Loop = animNode["Loop"].as<bool>();
            if (animNode["Color"])
                animComp.Color = animNode["Color"].as<glm::vec4>();
        }

        // RigidBody2D
        auto rb2dNode = entityNode["RigidBody2DComponent"];
        if (rb2dNode)
        {
            auto& rb2d = entity.AddComponent<RigidBody2DComponent>();
            rb2d.Type = (RigidBody2DComponent::BodyType)rb2dNode["BodyType"].as<int>();
            if (rb2dNode["FixedRotation"])
                rb2d.FixedRotation = rb2dNode["FixedRotation"].as<bool>();
        }

        // BoxCollider2D
        auto bc2dNode = entityNode["BoxColliderComponent"];
        if (bc2dNode)
        {
            auto& bc2d = entity.AddComponent<BoxColliderComponent>();
            bc2d.Offset = bc2dNode["Offset"].as<glm::vec2>();
            bc2d.Size = bc2dNode["Size"].as<glm::vec2>();
            bc2d.Density = bc2dNode["Density"].as<float>();
            bc2d.Friction = bc2dNode["Friction"].as<float>();
            bc2d.Restitution = bc2dNode["Restitution"].as<float>();
            if (bc2dNode["RestitutionThreshold"])
                bc2d.RestitutionThreshold = bc2dNode["RestitutionThreshold"].as<float>();
        }

        // RigidBody3D
        auto rb3dNode = entityNode["RigidBody3DComponent"];
        if (rb3dNode)
        {
            auto& rb3d = entity.AddComponent<RigidBody3DComponent>();
            rb3d.Type = (RigidBody3DComponent::BodyType)rb3dNode["BodyType"].as<int>();
            if (rb3dNode["Mass"]) rb3d.Mass = rb3dNode["Mass"].as<float>();
            if (rb3dNode["LinearDamping"]) rb3d.LinearDamping = rb3dNode["LinearDamping"].as<float>();
            if (rb3dNode["AngularDamping"]) rb3d.AngularDamping = rb3dNode["AngularDamping"].as<float>();
            if (rb3dNode["IsSensor"]) rb3d.IsSensor = rb3dNode["IsSensor"].as<bool>();
            if (rb3dNode["AllowedDOFs"] && rb3dNode["AllowedDOFs"].IsSequence())
            {
                auto seq = rb3dNode["AllowedDOFs"];
                for (int i = 0; i < 6 && i < seq.size(); i++)
                    rb3d.AllowedDOFs[i] = seq[i].as<bool>();
            }
        }

        // BoxCollider3D
        auto bc3dNode = entityNode["BoxCollider3DComponent"];
        if (bc3dNode)
        {
            auto& bc3d = entity.AddComponent<BoxCollider3DComponent>();
            bc3d.Offset = bc3dNode["Offset"].as<glm::vec3>();
            bc3d.HalfExtent = bc3dNode["HalfExtent"].as<glm::vec3>();
            bc3d.Friction = bc3dNode["Friction"].as<float>();
            bc3d.Restitution = bc3dNode["Restitution"].as<float>();
        }

        // SphereCollider3D
        auto sc3dNode = entityNode["SphereCollider3DComponent"];
        if (sc3dNode)
        {
            auto& sc3d = entity.AddComponent<SphereCollider3DComponent>();
            sc3d.Offset = sc3dNode["Offset"].as<glm::vec3>();
            sc3d.Radius = sc3dNode["Radius"].as<float>();
            sc3d.Friction = sc3dNode["Friction"].as<float>();
            sc3d.Restitution = sc3dNode["Restitution"].as<float>();
        }

        // CapsuleCollider3D
        auto cc3dNode = entityNode["CapsuleCollider3DComponent"];
        if (cc3dNode)
        {
            auto& cc3d = entity.AddComponent<CapsuleCollider3DComponent>();
            cc3d.Offset = cc3dNode["Offset"].as<glm::vec3>();
            cc3d.Radius = cc3dNode["Radius"].as<float>();
            cc3d.HalfHeight = cc3dNode["HalfHeight"].as<float>();
            cc3d.Friction = cc3dNode["Friction"].as<float>();
            cc3d.Restitution = cc3dNode["Restitution"].as<float>();
        }

        // Model3D
        auto modelNode = entityNode["Model3DComponent"];
        if (modelNode)
        {
            auto& mc = entity.AddComponent<Model3DComponent>();
            mc.ModelPath = modelNode["ModelPath"].as<std::string>();
            if (!mc.ModelPath.empty())
            {
                mc.IsLoaded = mc.ModelData->loadModel(mc.ModelPath);
            }
        }

        // Script (Lua)
        auto scriptNode = entityNode["ScriptComponent"];
        if (scriptNode)
        {
            auto& sc = entity.AddComponent<ScriptComponent>();
            sc.ScriptPath = scriptNode["ScriptPath"] ? scriptNode["ScriptPath"].as<std::string>() : std::string();
            sc.Enabled = scriptNode["Enabled"] ? scriptNode["Enabled"].as<bool>() : true;
        }

        // Audio source
        auto audioNode = entityNode["AudioSourceComponent"];
        if (audioNode)
        {
            auto& audio = entity.AddComponent<AudioSourceComponent>();
            audio.SoundPath = audioNode["SoundPath"] ? audioNode["SoundPath"].as<std::string>() : std::string();
            audio.Volume = audioNode["Volume"] ? audioNode["Volume"].as<float>() : 1.0f;
            audio.Pitch = audioNode["Pitch"] ? audioNode["Pitch"].as<float>() : 1.0f;
            audio.Pan = audioNode["Pan"] ? audioNode["Pan"].as<float>() : 0.0f;
            audio.Loop = audioNode["Loop"] ? audioNode["Loop"].as<bool>() : false;
            audio.Spatial = audioNode["Spatial"] ? audioNode["Spatial"].as<bool>() : false;
            audio.Autoplay = audioNode["Autoplay"] ? audioNode["Autoplay"].as<bool>() : false;
            audio.Muted = audioNode["Muted"] ? audioNode["Muted"].as<bool>() : false;
            audio.MinDistance = audioNode["MinDistance"] ? audioNode["MinDistance"].as<float>() : 1.0f;
            audio.MaxDistance = audioNode["MaxDistance"] ? audioNode["MaxDistance"].as<float>() : 100.0f;
            audio.Rolloff = audioNode["Rolloff"] ? audioNode["Rolloff"].as<float>() : 1.0f;
        }

        // Animator
        auto acNode = entityNode["AnimatorComponent"];
        if (acNode)
        {
            auto& ac = entity.AddComponent<AnimatorComponent>();
            ac.CurrentAnimationName = acNode["CurrentAnimationName"].as<std::string>();
            ac.PlaybackSpeed = acNode["PlaybackSpeed"].as<float>();
            ac.IsPlaying = acNode["IsPlaying"].as<bool>();
            // Backward-compatible defaults for legacy serialized scenes
            ac.LoopMode = acNode["LoopMode"] ? static_cast<RD3d::LoopMode>(acNode["LoopMode"].as<int>())
                                             : RD3d::LoopMode::Loop;
            ac.Reverse = acNode["Reverse"] ? acNode["Reverse"].as<bool>() : false;
            ac.RootMotion = acNode["RootMotion"] ? acNode["RootMotion"].as<bool>() : false;
            ac.CrossFadeDuration =
                acNode["CrossFadeDuration"] ? acNode["CrossFadeDuration"].as<float>() : 0.25f;
            ac.SkinIndex = acNode["SkinIndex"] ? acNode["SkinIndex"].as<int>() : 0;

            if (YAML::Node graphNode = acNode["Graph"])
            {
                AnimationGraph& graph = ac.Graph;
                graph.Enabled = graphNode["Enabled"] ? graphNode["Enabled"].as<bool>() : false;
                graph.EntryState = graphNode["EntryState"] ? graphNode["EntryState"].as<int>() : 0;

                if (YAML::Node states = graphNode["States"])
                {
                    for (const YAML::Node& stateNode : states)
                    {
                        AnimationGraphState state;
                        state.Name = stateNode["Name"] ? stateNode["Name"].as<std::string>() : "State";
                        state.Clip = stateNode["Clip"] ? stateNode["Clip"].as<std::string>() : std::string();
                        state.EditorPosition =
                            stateNode["Position"] ? stateNode["Position"].as<glm::vec2>() : glm::vec2(0.0f);
                        state.Speed = stateNode["Speed"] ? stateNode["Speed"].as<f32>() : 1.0f;
                        state.Loop = stateNode["Loop"] ? stateNode["Loop"].as<bool>() : true;
                        graph.States.push_back(std::move(state));
                    }
                }

                if (YAML::Node transitions = graphNode["Transitions"])
                {
                    for (const YAML::Node& transitionNode : transitions)
                    {
                        AnimationGraphTransition transition;
                        transition.From = transitionNode["From"] ? transitionNode["From"].as<int>() : -1;
                        transition.To = transitionNode["To"] ? transitionNode["To"].as<int>() : -1;
                        transition.Parameter =
                            transitionNode["Parameter"] ? transitionNode["Parameter"].as<std::string>() : std::string();
                        transition.Condition =
                            transitionNode["Condition"]
                                ? static_cast<AnimGraphCondition>(transitionNode["Condition"].as<int>())
                                : AnimGraphCondition::Greater;
                        transition.Threshold =
                            transitionNode["Threshold"] ? transitionNode["Threshold"].as<f32>() : 0.5f;
                        transition.Duration = transitionNode["Duration"] ? transitionNode["Duration"].as<f32>() : 0.2f;
                        transition.HasExitTime =
                            transitionNode["HasExitTime"] ? transitionNode["HasExitTime"].as<bool>() : false;
                        transition.ExitTime = transitionNode["ExitTime"] ? transitionNode["ExitTime"].as<f32>() : 1.0f;
                        graph.Transitions.push_back(std::move(transition));
                    }
                }

                if (YAML::Node parameters = graphNode["Parameters"])
                {
                    for (const YAML::Node& parameterNode : parameters)
                    {
                        AnimationGraphParameter parameter;
                        parameter.Name =
                            parameterNode["Name"] ? parameterNode["Name"].as<std::string>() : std::string();
                        parameter.Value = parameterNode["Value"] ? parameterNode["Value"].as<f32>() : 0.0f;
                        graph.Parameters.push_back(std::move(parameter));
                    }
                }
            }

            if (entity.HasComponent<Model3DComponent>())
            {
                auto& mc = entity.GetComponent<Model3DComponent>();
                if (mc.IsLoaded && mc.ModelData)
                {
                    ac.Animator = CreateRef<RD3d::Animator>(mc.ModelData);
                    if (!ac.CurrentAnimationName.empty())
                        ac.Animator->PlayAnimation(ac.CurrentAnimationName);
                }
            }
        }

        // DirectionalLight
        auto dlcNode = entityNode["DirectionalLightComponent"];
        if (dlcNode)
        {
            auto& dlc = entity.AddComponent<DirectionalLightComponent>();
            dlc.Color = dlcNode["Color"].as<glm::vec3>();
            dlc.Intensity = dlcNode["Intensity"].as<float>();
            dlc.CastShadows = dlcNode["CastShadows"] ? dlcNode["CastShadows"].as<bool>() : true;
        }

        // Foliage
        auto foliageNode = entityNode["FoliageComponent"];
        if (foliageNode)
        {
            auto& fc = entity.AddComponent<FoliageComponent>();
            fc.Bend = foliageNode["Bend"] ? foliageNode["Bend"].as<float>() : 1.0f;
            fc.Height = foliageNode["Height"] ? foliageNode["Height"].as<float>() : 2.0f;
        }

        // PointLight
        auto plcNode = entityNode["PointLightComponent"];
        if (plcNode)
        {
            auto& plc = entity.AddComponent<PointLightComponent>();
            plc.Color = plcNode["Color"].as<glm::vec3>();
            plc.Intensity = plcNode["Intensity"].as<float>();
            plc.Radius = plcNode["Radius"].as<float>();
            ReadOptional(plcNode, "CastShadows", plc.CastShadows);
        }

        // SpotLight
        auto slcNode = entityNode["SpotLightComponent"];
        if (slcNode)
        {
            auto& slc = entity.AddComponent<SpotLightComponent>();
            slc.Color = slcNode["Color"].as<glm::vec3>();
            slc.Intensity = slcNode["Intensity"].as<float>();
            slc.Radius = slcNode["Radius"].as<float>();
            slc.InnerConeAngle = slcNode["InnerConeAngle"].as<float>();
            slc.OuterConeAngle = slcNode["OuterConeAngle"].as<float>();
            ReadOptional(slcNode, "CastShadows", slc.CastShadows);
        }

        // AreaLight: every field optional, so a scene saved before a field existed keeps its default
        if (auto alcNode = entityNode["AreaLightComponent"])
        {
            auto& alc = entity.AddComponent<AreaLightComponent>();
            ReadOptional(alcNode, "Color", alc.Color);
            ReadOptional(alcNode, "Intensity", alc.Intensity);
            ReadOptional(alcNode, "Width", alc.Width);
            ReadOptional(alcNode, "Height", alc.Height);
            ReadOptional(alcNode, "Range", alc.Range);
            ReadOptional(alcNode, "TwoSided", alc.TwoSided);
            ReadOptional(alcNode, "CastShadows", alc.CastShadows);
        }

        // Water: every field optional, as for the area light
        if (auto waterNode = entityNode["WaterComponent"])
        {
            auto& wc = entity.AddComponent<WaterComponent>();
            ReadOptional(waterNode, "Size", wc.Size);
            ReadOptional(waterNode, "Resolution", wc.Resolution);
            ReadOptional(waterNode, "ShallowColor", wc.ShallowColor);
            ReadOptional(waterNode, "DeepColor", wc.DeepColor);
            ReadOptional(waterNode, "Clarity", wc.Clarity);
            ReadOptional(waterNode, "WaveHeight", wc.WaveHeight);
            ReadOptional(waterNode, "WaveLength", wc.WaveLength);
            ReadOptional(waterNode, "WaveSpeed", wc.WaveSpeed);
            ReadOptional(waterNode, "Choppiness", wc.Choppiness);
            ReadOptional(waterNode, "Direction", wc.Direction);
            ReadOptional(waterNode, "FollowWind", wc.FollowWind);
            ReadOptional(waterNode, "Foam", wc.Foam);
            // The grid is Resolution^2 cells: a hand-edited 0 or 100000 must not reach the mesh builder.
            wc.Resolution = std::clamp(wc.Resolution, 1, 512);
        }

        // Game UI: every field optional
        if (auto node = entityNode["UIRectComponent"])
        {
            auto& r = entity.AddComponent<UIRectComponent>();
            ReadOptional(node, "AnchorMin", r.AnchorMin);
            ReadOptional(node, "AnchorMax", r.AnchorMax);
            ReadOptional(node, "Pivot", r.Pivot);
            ReadOptional(node, "Position", r.Position);
            ReadOptional(node, "Size", r.Size);
            ReadOptional(node, "Order", r.Order);
            ReadOptional(node, "Visible", r.Visible);
        }
        if (auto node = entityNode["UIImageComponent"])
        {
            auto& i = entity.AddComponent<UIImageComponent>();
            ReadOptional(node, "Color", i.Color);
            ReadOptional(node, "TexturePath", i.TexturePath);
            ReadOptional(node, "CornerRadius", i.CornerRadius);
            ReadOptional(node, "BorderWidth", i.BorderWidth);
            ReadOptional(node, "BorderColor", i.BorderColor);
        }
        if (auto node = entityNode["UITextComponent"])
        {
            auto& t = entity.AddComponent<UITextComponent>();
            ReadOptional(node, "Text", t.Text);
            ReadOptional(node, "FontSize", t.FontSize);
            ReadOptional(node, "Color", t.Color);
            int align = static_cast<int>(t.Align);
            ReadOptional(node, "Align", align);
            t.Align = static_cast<UITextComponent::Alignment>(std::clamp(align, 0, 2));
            ReadOptional(node, "FontPath", t.FontPath);
        }
        if (auto node = entityNode["UIButtonComponent"])
        {
            auto& b = entity.AddComponent<UIButtonComponent>();
            ReadOptional(node, "NormalColor", b.NormalColor);
            ReadOptional(node, "HoverColor", b.HoverColor);
            ReadOptional(node, "PressedColor", b.PressedColor);
            ReadOptional(node, "Interactable", b.Interactable);
        }
        if (auto node = entityNode["UIProgressBarComponent"])
        {
            auto& p = entity.AddComponent<UIProgressBarComponent>();
            ReadOptional(node, "Value", p.Value);
            ReadOptional(node, "FillColor", p.FillColor);
            ReadOptional(node, "BackgroundColor", p.BackgroundColor);
            ReadOptional(node, "CornerRadius", p.CornerRadius);
            ReadOptional(node, "Interactable", p.Interactable);
        }

        // Terrain: every field optional
        if (auto terrainNode = entityNode["TerrainComponent"])
        {
            auto& tc = entity.AddComponent<TerrainComponent>();
            ReadOptional(terrainNode, "HeightmapPath", tc.HeightmapPath);
            ReadOptional(terrainNode, "Size", tc.Size);
            ReadOptional(terrainNode, "Height", tc.Height);
            ReadOptional(terrainNode, "Resolution", tc.Resolution);
            ReadOptional(terrainNode, "Seed", tc.Seed);
            ReadOptional(terrainNode, "FeatureSize", tc.FeatureSize);
            ReadOptional(terrainNode, "Octaves", tc.Octaves);
            ReadOptional(terrainNode, "Roughness", tc.Roughness);
            ReadOptional(terrainNode, "SandColor", tc.SandColor);
            ReadOptional(terrainNode, "GrassColor", tc.GrassColor);
            ReadOptional(terrainNode, "RockColor", tc.RockColor);
            ReadOptional(terrainNode, "SnowColor", tc.SnowColor);
            ReadOptional(terrainNode, "SandLevel", tc.SandLevel);
            ReadOptional(terrainNode, "SnowLevel", tc.SnowLevel);
            ReadOptional(terrainNode, "RockSlope", tc.RockSlope);
            ReadOptional(terrainNode, "Collider", tc.Collider);
            // Resolution^2 quads are built: a hand-edited value must not reach the builder unclamped.
            tc.Resolution = std::clamp(tc.Resolution, 8, 1024);
        }

        // Wind zone: every field optional, as for the area light
        if (auto windNode = entityNode["WindComponent"])
        {
            auto& wind = entity.AddComponent<WindComponent>();
            ReadOptional(windNode, "Strength", wind.Strength);
            ReadOptional(windNode, "GustStrength", wind.GustStrength);
            ReadOptional(windNode, "GustFrequency", wind.GustFrequency);
        }

        // Issue #17: glTF node entity marker
        auto mncNode = entityNode["ModelNodeComponent"];
        if (mncNode)
        {
            auto& mnc = entity.AddComponent<ModelNodeComponent>();
            if (mncNode["ModelEntity"])
                mnc.ModelEntity = mncNode["ModelEntity"].as<u64>();
            if (mncNode["NodeIndex"])
                mnc.NodeIndex = mncNode["NodeIndex"].as<int>();
            if (mncNode["NodeName"])
                mnc.NodeName = mncNode["NodeName"].as<std::string>();
            if (mncNode["HasChildrenNodes"])
                mnc.HasChildrenNodes = mncNode["HasChildrenNodes"].as<bool>();
        }
    }

    // Issue #17 hardening: sanitize relationships after both passes. Stale or
    // duplicated child entries (e.g. from an older save format or a corrupted
    // file) previously caused ghost entities in the editor UI.
    auto relView = m_Scene->m_registry.view<RelationshipComponent>();
    for (auto handle : relView)
    {
        auto& rel = relView.get<RelationshipComponent>(handle);
        if (rel.Parent != 0 && !m_Scene->GetEntityWithUUID(rel.Parent))
            rel.Parent = 0;

        std::unordered_set<u64> seen;
        std::vector<u64> clean;
        for (u64 childID : rel.Children)
        {
            if (childID == 0 || !seen.insert(childID).second)
                continue; // drop nulls and duplicates
            Entity child = m_Scene->GetEntityWithUUID(childID);
            if (child)
                clean.push_back(childID);
        }
        rel.Children = std::move(clean);
    }

    // Issue #17 hardening: detect and break cycles in parent links.
    for (auto handle : relView)
    {
        u64 cursorID = m_Scene->m_registry.get<IDComponent>(handle).ID;
        std::unordered_set<u64> path;
        
        while (cursorID != 0)
        {
            if (!path.insert(cursorID).second)
            {
                Entity entityToBreak = m_Scene->GetEntityWithUUID(cursorID);
                if (entityToBreak && entityToBreak.HasComponent<RelationshipComponent>())
                {
                    auto& relToBreak = entityToBreak.GetComponent<RelationshipComponent>();
                    u64 formerParentID = relToBreak.Parent;
                    relToBreak.Parent = 0;
                    
                    Entity formerParent = m_Scene->GetEntityWithUUID(formerParentID);
                    if (formerParent && formerParent.HasComponent<RelationshipComponent>())
                    {
                        auto& formerParentRel = formerParent.GetComponent<RelationshipComponent>();
                        formerParentRel.Children.erase(
                            std::remove(formerParentRel.Children.begin(), formerParentRel.Children.end(), cursorID),
                            formerParentRel.Children.end());
                    }
                }
                break;
            }
            
            Entity cursor = m_Scene->GetEntityWithUUID(cursorID);
            if (!cursor || !cursor.HasComponent<RelationshipComponent>())
                break;
                
            cursorID = cursor.GetComponent<RelationshipComponent>().Parent;
        }
    }

    return true;
}

} // namespace UHE
