#pragma once
#include "uhepch.h"
#include <box2d/types.h>
#include "SceneCamera.h"
#include "UHE/Animation/Animation2D/SpriteAnimation.h"
#include "UHE/Animation/AnimationGraph.h"
#include "UHE/Core/Core.h"
#include "UHE/Core/Timestep.h"
#include "UHE/Core/UUID.h"
#include "UHE/Renderer/Font.h"
#include "UHE/Renderer/Texture.h"
#include "UHE/Renderer3D/Animator.h"
#include "UHE/Renderer3D/LoadModel.h"
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace UHE
{
struct UHE_API TransformComponent
{
    glm::vec3 Translation{0.0f};
    glm::vec3 Rotation{0.0f}; // assumed in degrees
    glm::vec3 Scale{1.0f};

    TransformComponent() = default;
    TransformComponent(const TransformComponent&) = default;

    TransformComponent(const glm::vec3& translation) : Translation(translation) {}

    glm::mat4 GetTransform() const
    {
        glm::mat4 rotation = glm::mat4(glm::quat(Rotation));

        return glm::translate(glm::mat4(1.0f), Translation) * rotation * glm::scale(glm::mat4(1.0f), Scale);
    }
};

struct UHE_API IDComponent
{
    u64 ID = 0;
    // Fresh entities get a real random UUID here: leaving it 0 made EVERY
    // entity share identity 0, so GetEntityWithUUID returned the first
    // registry entity and all UUID-based logic (deferred child creation,
    // unparenting, serialization, dedup) silently operated on the wrong
    // entities. Deserialization overwrites this with the stored value.
    IDComponent() : ID(UUID()) {}
    explicit IDComponent(u64 id) : ID(id) {}
    IDComponent(const IDComponent&) = default;
};

struct UHE_API SpriteRendererComponent
{
    glm::vec4 Color{1.0f};
    std::string TexturePath;
    Ref<Texture2D> Texture;
    f32 TilingFactor = 1.0f;

    bool UseSubTexture = false;
    glm::vec2 SubTextureCoords = {0.0f, 0.0f};
    glm::vec2 SubTextureCellSize = {16.0f, 16.0f};
    glm::vec2 SubTextureSpriteSize = {1.0f, 1.0f};

    SpriteRendererComponent() = default;
    SpriteRendererComponent(const SpriteRendererComponent&) = default;
    SpriteRendererComponent(const glm::vec4& color) : Color(color) {}
};

struct UHE_API TextComponent
{
    std::string TextString = "Text Node";
    Ref<Font2D> FontAsset = Font2D::GetDefault();
    glm::vec4 Color{1.0f};
    f32 Kerning = 0.0f;
    f32 LineSpacing = 0.0f;

    TextComponent() = default;
    TextComponent(const TextComponent&) = default;
};

struct UHE_API TagComponent
{
    std::string Tag;

    TagComponent() = default;
    TagComponent(const TagComponent&) = default;
    TagComponent(const std::string& tag) : Tag(tag) {}
};

struct UHE_API CameraComponent
{
    SceneCamera Camera;
    bool Primary = true;
    bool FixedAspectRatio = false;

    CameraComponent() = default;
    CameraComponent(const CameraComponent&) = default;
};

struct UHE_API SpriteAnimationComponent
{
    std::string SpriteSheetPath;
    SpriteAnimation Animation;
    glm::vec4 Color = {1.0f, 1.0f, 1.0f, 1.0f};

    SpriteAnimationComponent() = default;
    SpriteAnimationComponent(const SpriteAnimationComponent&) = default;
};

class ScriptableEntity;

struct UHE_API NativeScriptComponent
{
    ScriptableEntity* Instance = nullptr;

    ScriptableEntity* (*InstantiateScript)();
    void (*DestroyScript)(NativeScriptComponent*);

    template <typename T> void Bind()
    {
        InstantiateScript = []() { return static_cast<ScriptableEntity*>(new T()); };
        DestroyScript = [](NativeScriptComponent* nsc)
        {
            delete nsc->Instance;
            nsc->Instance = nullptr;
        };
    }
};

/**
 * A Lua script attached to an entity (docs/architecture/scripting.md).
 *
 * The path resolves through the asset registry like any other asset, so a script
 * lives with the project's content ("Scripts/drone.lua") and can be moved with
 * it. The engine runs `OnCreate(self)`, `OnUpdate(self, dt)` and
 * `OnDestroy(self)` from the file; see `UHE/Scripting/ScriptEngine.h` for the API
 * a script is given.
 *
 * `RuntimeIndex` is the script engine's slot for this entity's instance. It is
 * runtime state: it is never serialized, and a component that is copied gets a
 * fresh instance the first time the copy is updated.
 */
struct UHE_API ScriptComponent
{
    std::string ScriptPath;
    bool Enabled = true;
    u32 RuntimeIndex = 0xFFFFFFFFu;

    ScriptComponent() = default;
    ScriptComponent(const ScriptComponent&) = default;
    explicit ScriptComponent(std::string path) : ScriptPath(std::move(path)) {}
};

struct UHE_API AudioSourceComponent
{
    std::string SoundPath; // project-relative asset path
    float Volume = 1.0f;
    float Pitch = 1.0f;
    float Pan = 0.0f; // -1 left .. +1 right; ignored when Spatial
    bool Loop = false;
    bool Spatial = false;
    bool Autoplay = false;
    bool Muted = false;
    float MinDistance = 1.0f;
    float MaxDistance = 100.0f;
    float Rolloff = 1.0f;

    // Runtime-only playback state (never serialized).
    u32 RuntimeHandle = 0;
    bool RuntimePlaying = false;
    bool RuntimeAutoStarted = false;
    bool PlayRequested = false;
    bool StopRequested = false;

    AudioSourceComponent() = default;
    AudioSourceComponent(const AudioSourceComponent& other)
        : SoundPath(other.SoundPath), Volume(other.Volume), Pitch(other.Pitch), Pan(other.Pan), Loop(other.Loop),
          Spatial(other.Spatial), Autoplay(other.Autoplay), Muted(other.Muted), MinDistance(other.MinDistance),
          MaxDistance(other.MaxDistance), Rolloff(other.Rolloff)
    {
    }
    AudioSourceComponent& operator=(const AudioSourceComponent& other)
    {
        SoundPath = other.SoundPath;
        Volume = other.Volume;
        Pitch = other.Pitch;
        Pan = other.Pan;
        Loop = other.Loop;
        Spatial = other.Spatial;
        Autoplay = other.Autoplay;
        Muted = other.Muted;
        MinDistance = other.MinDistance;
        MaxDistance = other.MaxDistance;
        Rolloff = other.Rolloff;
        return *this;
    }
};

struct UHE_API RigidBody2DComponent
{
    enum class BodyType
    {
        Static = 0,
        Kinematic,
        Dynamic
    };
    BodyType Type = BodyType::Static;
    bool FixedRotation = false;

    b2BodyId RuntimeBody = b2_nullBodyId;

    RigidBody2DComponent() = default;
    RigidBody2DComponent(const RigidBody2DComponent&) = default;
};

struct UHE_API Model3DComponent
{
    std::string ModelPath;
    Ref<RD3d::Model> ModelData = CreateRef<RD3d::Model>();
    bool IsLoaded = false;

    Model3DComponent() = default;
    Model3DComponent(const Model3DComponent&) = default;
};

struct UHE_API AnimatorComponent
{
    Ref<RD3d::Animator> Animator;
    bool IsPlaying = false;
    f32 PlaybackSpeed = 1.0f;
    std::string CurrentAnimationName = "";

    // Playback state and blend configuration mirrored to the active runtime Animator
    RD3d::LoopMode LoopMode = RD3d::LoopMode::Loop;
    bool Reverse = false;
    bool RootMotion = false;
    f32 CrossFadeDuration = 0.25f; ///< Cross-fade blend duration when changing clips
    i32 SkinIndex = 0;               ///< Active skin joint set index

    // State-machine graph. When enabled it decides which clip plays and can
    // cross-fade between states; CurrentAnimationName becomes an output.
    AnimationGraph Graph;
    std::shared_ptr<RD3d::AnimationStateMachine> GraphRuntime; ///< compiled, rebuilt on edit
    u64 GraphRuntimeHash = 0;                                  ///< hash of the graph the runtime was built from

    AnimatorComponent() = default;
    AnimatorComponent(const AnimatorComponent&) = default;
};

struct UHE_API DirectionalLightComponent
{
    glm::vec3 Color{1.0f, 1.0f, 1.0f};
    float Intensity = 1.0f;
    /// The first directional light with this set casts the scene's cascaded shadows.
    bool CastShadows = true;

    DirectionalLightComponent() = default;
    DirectionalLightComponent(const DirectionalLightComponent&) = default;
};

struct UHE_API PointLightComponent
{
    glm::vec3 Color{1.0f, 1.0f, 1.0f};
    float Intensity = 1.0f;
    float Radius = 10.0f;
    /// Casts shadows: six tiles of the local shadow atlas (one per cube face).
    bool CastShadows = false;

    PointLightComponent() = default;
    PointLightComponent(const PointLightComponent&) = default;
};

// Cone light. Angles are RADIANS here, matching the KHR_lights_punctual values
// the glTF loader hands over; the editor UI converts to degrees. The light
// points down the entity's local -Z, the same convention DirectionalLight-
// Component and glTF lights use. Intensity is full inside InnerConeAngle and
// fades to zero at OuterConeAngle.
struct UHE_API SpotLightComponent
{
    glm::vec3 Color{1.0f, 1.0f, 1.0f};
    float Intensity = 1.0f;
    float Radius = 10.0f;
    float InnerConeAngle = 0.2617994f; // ~15 degrees
    float OuterConeAngle = 0.5235988f; // ~30 degrees
    /// Casts shadows: one tile of the local shadow atlas.
    bool CastShadows = false;

    SpotLightComponent() = default;
    SpotLightComponent(const SpotLightComponent&) = default;
};

/**
 * @brief A rectangular light: a lit panel, window or screen, on the entity's local XY plane.
 *
 * It shines along the entity's local -Z, like the spot and directional
 * lights. Diffuse light is the panel's exact form factor, so its size
 * shows in how soft the light falls off; the highlight on glossy surfaces
 * takes the panel's shape.
 */
struct UHE_API AreaLightComponent
{
    glm::vec3 Color{1.0f, 1.0f, 1.0f};
    float Intensity = 4.0f;
    float Width = 2.0f;    ///< along local X
    float Height = 1.0f;   ///< along local Y
    float Range = 15.0f;   ///< distance at which the light fades out
    bool TwoSided = false; ///< also lights the side behind the panel
    /// Casts shadows, from the panel's centre (one tile of the local shadow atlas).
    bool CastShadows = false;

    AreaLightComponent() = default;
    AreaLightComponent(const AreaLightComponent&) = default;
};

/**
 * @brief A wind zone: the scene's wind blows along this entity's forward axis (local -Z).
 *
 * While one exists it replaces the Environment's wind (RenderSettings::Wind),
 * so the direction can be set by rotating it in the viewport. Bends foliage
 * and drives water that follows the wind.
 */
struct UHE_API WindComponent
{
    float Strength = 0.5f;       ///< steady wind, 0..2
    float GustStrength = 0.4f;   ///< extra wind that comes and goes, 0..2
    float GustFrequency = 0.25f; ///< gusts per second

    WindComponent() = default;
    WindComponent(const WindComponent&) = default;
};

/**
 * @brief Makes a model sway in the scene's wind (RenderSettings::Wind): grass, bushes, trees.
 *
 * The bend grows with height above the model's origin, so the base stays put.
 */
struct UHE_API FoliageComponent
{
    float Bend = 1.0f;   ///< 0 = rigid, 1 = full response to the wind
    float Height = 2.0f; ///< model-space height where the bend reaches full strength

    FoliageComponent() = default;
    FoliageComponent(const FoliageComponent&) = default;
};

/**
 * @brief A water surface: a square of animated waves centred on the entity, on its local XZ plane.
 *
 * Waves are Gerstner waves in world space, so neighbouring water entities
 * line up. The surface refracts and tints what lies below it by depth (the
 * deeper, the more of DeepColor), reflects the sky, and foams where it meets
 * geometry. Drawn after the opaque scene in the lit view.
 */
struct UHE_API WaterComponent
{
    glm::vec2 Size{20.0f, 20.0f};                ///< extent on local X and Z
    int Resolution = 128;                        ///< grid cells per side (vertex detail of the waves)
    glm::vec3 ShallowColor{0.10f, 0.55f, 0.60f}; ///< tint of shallow water (sRGB)
    glm::vec3 DeepColor{0.01f, 0.10f, 0.18f};    ///< colour of deep water (sRGB)
    float Clarity = 3.0f;                        ///< metres of water before the bottom fades into DeepColor
    float WaveHeight = 0.25f;                    ///< amplitude of the largest wave
    float WaveLength = 8.0f;                     ///< wavelength of the largest wave, in metres
    float WaveSpeed = 1.0f;                      ///< multiplies the physical (deep water) wave speed
    float Choppiness = 0.6f;                     ///< 0 = rolling swell, 1 = sharp crests
    float Direction = 0.0f;                      ///< degrees on the ground plane, used when not following the wind
    bool FollowWind = true;                      ///< waves travel with the scene's wind (RenderSettings::Wind)
    float Foam = 0.6f;                           ///< shoreline and crest foam, 0..1

    WaterComponent() = default;
    WaterComponent(const WaterComponent&) = default;
};

/**
 * @brief Ground made from a heightfield: a grayscale heightmap image, or
 *        generated noise when there is none.
 *
 * A grid of Resolution x Resolution quads, Size metres on a side, centred on
 * the entity; white (or the highest noise) is Height metres up. It draws
 * through the ordinary 3D path (shadows, occlusion, picking), coloured in
 * layers by height and slope, and in Play it is a static Jolt heightfield
 * that bodies rest and drive on. Editing a field rebuilds it on the next frame.
 */
struct UHE_API TerrainComponent
{
    std::string HeightmapPath; ///< grayscale image (8 or 16 bit); empty: generated noise
    float Size = 64.0f;        ///< width and depth, metres
    float Height = 8.0f;       ///< metres between the lowest and the highest point
    int Resolution = 128;      ///< quads per side, 8..1024

    // Generated terrain.
    int Seed = 1;
    float FeatureSize = 24.0f; ///< metres across the largest hills
    int Octaves = 5;           ///< layers of finer detail, 1..8
    float Roughness = 0.5f;    ///< how much each finer layer adds, 0..1

    // Colour layers, sRGB as a picker shows them.
    glm::vec3 SandColor{0.76f, 0.70f, 0.50f};
    glm::vec3 GrassColor{0.27f, 0.45f, 0.17f};
    glm::vec3 RockColor{0.42f, 0.40f, 0.37f};
    glm::vec3 SnowColor{0.94f, 0.95f, 0.97f};
    float SandLevel = 0.06f; ///< below this fraction of Height: sand
    float SnowLevel = 0.85f; ///< above this fraction of Height: snow
    float RockSlope = 35.0f; ///< steeper than this many degrees: rock

    bool Collider = true; ///< a static heightfield body in Play

    // Built from the fields above; not saved.
    Ref<RD3d::Model> Mesh;
    std::shared_ptr<const std::vector<float>> Heights; ///< (Resolution+1)^2 metres, row-major along +Z
    u64 BuiltKey = 0;

    TerrainComponent() = default;
    TerrainComponent(const TerrainComponent&) = default;
};

// ── Game UI ────────────────────────────────────────────────────────────────
// Screen-space UI, drawn over the finished frame in the game and in the
// editor's viewport. Every UI element has a UIRectComponent; the others add
// what it shows and does. Pixels, origin at the top left, y down. A UI
// entity's rect is laid out inside its parent's when the parent is UI too,
// otherwise inside the screen. UI::Update (UI/UISystem.h) lays them out,
// handles the mouse and builds the draw list.

/**
 * @brief Where a UI element sits, relative to its parent (Unity's RectTransform).
 *
 * The anchors are fractions of the parent: equal anchors pin a point (a
 * corner, the centre), different ones stretch the element with the parent on
 * that axis. On a stretched axis Size is added to the stretched size (so -20
 * leaves 10 px either side); otherwise it is the size. Position moves the
 * pivot away from the anchor point.
 */
struct UHE_API UIRectComponent
{
    glm::vec2 AnchorMin{0.5f, 0.5f};
    glm::vec2 AnchorMax{0.5f, 0.5f};
    glm::vec2 Pivot{0.5f, 0.5f};    ///< the point of the element that sits at Position, fractions of its size
    glm::vec2 Position{0.0f, 0.0f}; ///< pixels from the anchor point to the pivot
    glm::vec2 Size{160.0f, 40.0f};  ///< pixels (added to the stretched size on a stretched axis)
    int Order = 0;                  ///< among siblings, higher draws on top and gets the mouse first
    bool Visible = true;            ///< hidden elements and their children neither draw nor take the mouse

    // Laid out by UI::Update; not saved.
    glm::vec2 ComputedMin{0.0f};
    glm::vec2 ComputedMax{0.0f};
};

/// @brief A filled box: colour, an optional image, rounded corners and a border.
struct UHE_API UIImageComponent
{
    glm::vec4 Color{0.10f, 0.12f, 0.16f, 0.85f}; ///< sRGB with alpha; multiplies the image
    std::string TexturePath;                     ///< empty: a plain box
    float CornerRadius = 6.0f;                   ///< pixels
    float BorderWidth = 0.0f;                    ///< pixels
    glm::vec4 BorderColor{1.0f, 1.0f, 1.0f, 0.25f};

    Ref<Texture2D> Texture; ///< loaded from TexturePath; not saved
    std::string LoadedPath; ///< the path Texture came from
};

/// @brief Text inside the element's rect.
struct UHE_API UITextComponent
{
    enum class Alignment : int
    {
        Left = 0,
        Center = 1,
        Right = 2,
    };

    std::string Text = "Text";
    float FontSize = 22.0f; ///< pixels per line
    glm::vec4 Color{1.0f};  ///< sRGB with alpha
    Alignment Align = Alignment::Center;
    std::string FontPath; ///< a .ttf in the assets; empty: the engine's default font
};

/**
 * @brief Makes the element clickable: its UIImage takes the normal, hover
 *        and pressed colours, and a click calls `OnClick(self)` on the
 *        element's script.
 */
struct UHE_API UIButtonComponent
{
    glm::vec4 NormalColor{0.20f, 0.24f, 0.32f, 0.95f};
    glm::vec4 HoverColor{0.28f, 0.34f, 0.46f, 1.0f};
    glm::vec4 PressedColor{0.14f, 0.17f, 0.24f, 1.0f};
    bool Interactable = true;

    // Mouse state, kept by UI::Update; not saved.
    bool Hovered = false;
    bool Pressed = false;
    u32 Clicks = 0; ///< clicks since the scene started
};

/**
 * @brief A bar filled to Value: a readout (speed, battery, progress), or a
 *        slider the mouse drags when Interactable.
 */
struct UHE_API UIProgressBarComponent
{
    float Value = 0.5f; ///< 0..1
    glm::vec4 FillColor{0.30f, 0.65f, 1.0f, 1.0f};
    glm::vec4 BackgroundColor{0.08f, 0.09f, 0.12f, 0.85f};
    float CornerRadius = 4.0f;
    bool Interactable = false; ///< a slider: dragging sets Value

    bool Dragging = false; ///< kept by UI::Update; not saved
};

struct UHE_API BoxColliderComponent
{
    glm::vec2 Offset{0.0f, 0.0f};
    glm::vec2 Size{0.5f, 0.5f};

    float Density = 1.0f;
    float Friction = 0.5f;
    float Restitution = 0.0f;
    float RestitutionThreshold = 0.5f;

    b2ShapeId RuntimeShape = b2_nullShapeId;

    BoxColliderComponent() = default;
    BoxColliderComponent(const BoxColliderComponent&) = default;
};

// --- 3D Physics Components ---

struct UHE_API RigidBody3DComponent
{
    enum class BodyType
    {
        Static = 0,
        Kinematic,
        Dynamic
    };
    BodyType Type = BodyType::Static;
    
    float Mass = 1.0f;
    float LinearDamping = 0.05f;
    float AngularDamping = 0.05f;
    bool AllowedDOFs[6] = {true, true, true, true, true, true}; // Translation X,Y,Z Rotation X,Y,Z
    bool IsSensor = false;

    // We store the JPH::BodyID as a uint32_t to avoid including Jolt headers everywhere
    uint32_t RuntimeBodyID = 0xFFFFFFFF; // JPH::BodyID::cInvalidBodyID

    RigidBody3DComponent() = default;
    RigidBody3DComponent(const RigidBody3DComponent&) = default;
};

struct UHE_API BoxCollider3DComponent
{
    glm::vec3 Offset{0.0f, 0.0f, 0.0f};
    glm::vec3 HalfExtent{0.5f, 0.5f, 0.5f};

    float Friction = 0.2f;
    float Restitution = 0.0f;

    BoxCollider3DComponent() = default;
    BoxCollider3DComponent(const BoxCollider3DComponent&) = default;
};

struct UHE_API SphereCollider3DComponent
{
    glm::vec3 Offset{0.0f, 0.0f, 0.0f};
    float Radius = 0.5f;

    float Friction = 0.2f;
    float Restitution = 0.0f;

    SphereCollider3DComponent() = default;
    SphereCollider3DComponent(const SphereCollider3DComponent&) = default;
};

struct UHE_API CapsuleCollider3DComponent
{
    glm::vec3 Offset{0.0f, 0.0f, 0.0f};
    float Radius = 0.5f;
    float HalfHeight = 1.0f; // Total height is 2 * (Radius + HalfHeight)

    float Friction = 0.2f;
    float Restitution = 0.0f;

    CapsuleCollider3DComponent() = default;
    CapsuleCollider3DComponent(const CapsuleCollider3DComponent&) = default;
};

// Issue #17: entity hierarchy support. Attached to every entity created
// through Scene::CreateEntity. An entity without a parent (Parent == 0) is a
// root node; children are stored as UUIDs so the hierarchy survives
// serialization and scene copies.
struct UHE_API RelationshipComponent
{
    u64 Parent = 0;
    std::vector<u64> Children;

    RelationshipComponent() = default;
    RelationshipComponent(const RelationshipComponent&) = default;
};

// Marks an entity as coming from a glTF node of a loaded 3D model. Entities
// created by Scene::CollapseModelNodes carry this so the editor can show the
// model's node tree and rendering can submit individual sub-meshes.
struct UHE_API ModelNodeComponent
{
    u64 ModelEntity = 0; // UUID of the entity that owns the loaded model
    int NodeIndex = -1;  // glTF node index inside RD3d::Model::GetNodes()
    std::string NodeName;
    // True when the model's node tree was expanded into child entities; the
    // entity itself then acts as a pure group node and renders nothing.
    bool HasChildrenNodes = false;

    ModelNodeComponent() = default;
    ModelNodeComponent(const ModelNodeComponent&) = default;
};

} // namespace UHE
