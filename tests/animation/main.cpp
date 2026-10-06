// No-GPU animation runtime harness (issue #41).
//
// Covers playback controls (speed, loop modes, reverse, pause), STEP/LINEAR/
// CUBICSPLINE sampling, cross-fade blending, events, root motion, multi-skin,
// the animation state machine, the keyframe compressor and the two-bone IK
// solver. The Model is populated directly through the cooked-data API, so no
// glTF parsing or GPU is involved.
#include "UHE/Renderer3D/Animator.h"
#include "UHE/Renderer3D/AnimationStateMachine.h"
#include "UHE/Renderer3D/AnimationCompression.h"
#include "UHE/Renderer3D/IKSolver.h"
#include "UHE/Jobsystem/Jobsystem.h"

#include <cmath>
#include <glm/gtx/quaternion.hpp>
#include <iostream>

using namespace UHE;
using namespace UHE::RD3d;

static int g_failures = 0;

#define CHECK(cond, msg)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (cond)                                                                                  \
            std::cout << "  ok  - " << msg << '\n';                                                \
        else                                                                                       \
        {                                                                                          \
            std::cout << "  FAIL - " << msg << '\n';                                               \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static bool Near(float a, float b, float eps = 1e-3f)
{
    return std::abs(a - b) <= eps;
}

static bool Near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f)
{
    return glm::length(a - b) <= eps;
}

// Three-bone chain: root -> mid -> tip, identity bind pose. Two skins: the
// full chain and a reduced two-joint skin.
static Ref<Model> MakeTestModel()
{
    auto model = CreateRef<Model>();

    Skeleton skeleton;
    for (int i = 0; i < 3; ++i)
    {
        Bone bone;
        bone.Name = "Bone" + std::to_string(i);
        bone.ID = i;
        bone.ParentID = i - 1;
        bone.LocalTransform = glm::mat4(1.0f);
        bone.InverseBindMatrix = glm::mat4(1.0f);
        skeleton.Bones.push_back(bone);
    }
    skeleton.RootBoneID = 0;
    skeleton.JointNodes = {0, 1, 2};
    model->SetSkeleton(skeleton);

    Skin full;
    full.Name = "Full";
    full.JointNodes = {0, 1, 2};
    full.InverseBindMatrices.assign(3, glm::mat4(1.0f));
    model->AddSkin(full);

    Skin tipOnly;
    tipOnly.Name = "Tip";
    tipOnly.JointNodes = {1, 2};
    tipOnly.InverseBindMatrices.assign(2, glm::mat4(1.0f));
    model->AddSkin(tipOnly);

    return model;
}

// "walk": 2 s. Root slides +1 unit/s in X (root motion), tip scales 1 -> 2
// with a STEP key, plus two events. "run": 1 s root track for cross-fading.
static void AddTestClips(Ref<Model> model)
{
    AnimationClip walk;
    walk.Name = "walk";
    walk.Duration = 2.0f;

    VectorTrack rootPos;
    rootPos.TargetBoneID = 0;
    rootPos.Interpolation = InterpolationMode::Linear;
    rootPos.Keyframes = {{0.0f, glm::vec3(0.0f)},
                         {1.0f, glm::vec3(1.0f, 0.0f, 0.0f)},
                         {2.0f, glm::vec3(2.0f, 0.0f, 0.0f)}};
    walk.PositionTracks.push_back(rootPos);

    VectorTrack tipScale;
    tipScale.TargetBoneID = 2;
    tipScale.Interpolation = InterpolationMode::Step;
    tipScale.Keyframes = {{0.0f, glm::vec3(1.0f)}, {1.0f, glm::vec3(2.0f)}};
    walk.ScaleTracks.push_back(tipScale);

    walk.Events = {{0.5f, "half"}, {1.5f, "second"}};
    model->AddAnimation(walk);

    AnimationClip run;
    run.Name = "run";
    run.Duration = 1.0f;
    VectorTrack runPos;
    runPos.TargetBoneID = 0;
    runPos.Interpolation = InterpolationMode::Linear;
    runPos.Keyframes = {{0.0f, glm::vec3(0.0f, 1.0f, 0.0f)}, {1.0f, glm::vec3(1.0f, 1.0f, 0.0f)}};
    run.PositionTracks.push_back(runPos);
    model->AddAnimation(run);
}

static void TestInterpolation()
{
    std::cout << "[interpolation]\n";
    auto model = MakeTestModel();
    AddTestClips(model);

    // Cubic spline track on the mid bone: keys at t=0 (0) and t=2 (1), both
    // Hermite tangents (1,0,0). The curve is v(t) = 0.25t^3 - 1.25t^2 + t.
    AnimationClip clip;
    clip.Name = "spline";
    clip.Duration = 2.0f;
    VectorTrack spline;
    spline.TargetBoneID = 1;
    spline.Interpolation = InterpolationMode::CubicSpline;
    spline.Keyframes = {{0.0f, glm::vec3(0.0f)}, {2.0f, glm::vec3(1.0f, 0.0f, 0.0f)}};
    spline.InTangents = {glm::vec3(9.0f), glm::vec3(1.0f, 0.0f, 0.0f)}; // In[0] unused
    spline.OutTangents = {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(9.0f)}; // Out[1] unused
    clip.PositionTracks.push_back(spline);
    model->AddAnimation(clip);

    Animator animator(model);
    animator.PlayAnimation("spline");

    animator.Seek(1.0f);
    CHECK(Near(animator.GetFinalBoneMatrices()[1][3].x, 0.5f, 1e-2f), "cubic spline mid value");
    animator.Seek(2.0f);
    CHECK(Near(animator.GetFinalBoneMatrices()[1][3].x, 1.0f), "cubic spline key value exact");

    // STEP holds its value across the span (checked via the scaled basis
    // vector of the tip joint, since translation does not show scale).
    animator.PlayAnimation("walk");
    animator.Seek(0.999f);
    CHECK(Near(animator.GetFinalBoneMatrices()[2][0].x, 1.0f), "step holds first key before boundary");
    animator.Seek(1.0f);
    CHECK(Near(animator.GetFinalBoneMatrices()[2][0].x, 2.0f), "step jumps at key boundary");

    // LINEAR blends.
    animator.Seek(1.5f);
    CHECK(Near(animator.GetFinalBoneMatrices()[0][3].x, 1.5f), "linear mid value");
}

static void TestPlayback()
{
    std::cout << "[playback]\n";
    auto model = MakeTestModel();
    AddTestClips(model);
    Animator animator(model);
    animator.PlayAnimation("walk");

    // Loop (default) wraps.
    animator.UpdateAnimation(5.0f);
    CHECK(Near(animator.GetCurrentTime(), 1.0f), "loop wraps after duration");

    // Time scale.
    animator.Seek(0.0f);
    animator.SetTimeScale(2.0f);
    animator.UpdateAnimation(0.5f);
    CHECK(Near(animator.GetCurrentTime(), 1.0f), "time scale doubles progress");
    animator.SetTimeScale(1.0f);

    // Pause freezes time.
    animator.Pause();
    animator.UpdateAnimation(1.0f);
    CHECK(Near(animator.GetCurrentTime(), 1.0f), "paused time does not advance");
    animator.Play();
    animator.UpdateAnimation(0.25f);
    CHECK(Near(animator.GetCurrentTime(), 1.25f), "resume advances from pause point");

    // Reverse plays backwards (and wraps).
    animator.SetReversed(true);
    animator.Seek(0.0f);
    animator.UpdateAnimation(0.5f);
    CHECK(Near(animator.GetCurrentTime(), 1.5f), "reverse wraps backwards from zero");

    // LoopMode::None stops at the end.
    animator.SetReversed(false);
    animator.SetLoopMode(LoopMode::None);
    animator.Seek(1.9f);
    animator.UpdateAnimation(0.5f);
    CHECK(Near(animator.GetCurrentTime(), 2.0f), "None clamps to duration");
    CHECK(!animator.IsPlaying(), "None stops at the end");
    animator.Play();
    animator.UpdateAnimation(0.1f);
    CHECK(Near(animator.GetCurrentTime(), 0.1f), "Play restarts a completed clip");

    // PingPong reflects at both ends.
    animator.SetLoopMode(LoopMode::PingPong);
    animator.Seek(1.8f);
    animator.UpdateAnimation(0.6f); // 1.8 + 0.6 -> reflect off 2.0 down to 1.6
    CHECK(Near(animator.GetCurrentTime(), 1.6f), "pingpong reflects at the end");
    animator.UpdateAnimation(2.0f); // 1.6 - 2.0 -> reflect off 0.0 up to 0.4
    CHECK(Near(animator.GetCurrentTime(), 0.4f), "pingpong reflects at the start");
}

static void TestCrossFade()
{
    std::cout << "[crossfade]\n";
    auto model = MakeTestModel();
    AddTestClips(model);
    Animator animator(model);
    animator.PlayAnimation("walk");

    animator.CrossFade("run", 0.5f);
    CHECK(animator.IsBlending(), "blend active after CrossFade");
    CHECK(animator.GetCurrentAnimationIndex() == 1, "crossfade switched to the target clip");

    animator.UpdateAnimation(0.25f);
    CHECK(Near(animator.GetBlendWeight(), 0.5f, 0.02f), "blend weight half-way");
    animator.UpdateAnimation(0.25f);
    CHECK(Near(animator.GetBlendWeight(), 1.0f), "blend weight reaches one");
    CHECK(!animator.IsBlending(), "blend finished");

    // A hard cut cancels a running fade.
    animator.CrossFade("walk", 1.0f);
    animator.PlayAnimation("run");
    CHECK(!animator.IsBlending(), "hard cut cancels the fade");
}

static void TestEvents()
{
    std::cout << "[events]\n";
    auto model = MakeTestModel();
    AddTestClips(model);
    Animator animator(model);
    animator.PlayAnimation("walk");

    std::vector<std::string> fired;
    animator.SetEventCallback([&](const AnimationEvent& e) { fired.push_back(e.Name); });

    animator.UpdateAnimation(0.6f);
    CHECK(fired.size() == 1 && fired[0] == "half", "event fires crossing 0.5s");

    animator.UpdateAnimation(0.6f);
    CHECK(fired.size() == 1, "no event between 0.6 and 1.2");

    animator.UpdateAnimation(0.6f);
    CHECK(fired.size() == 2 && fired[1] == "second", "event fires crossing 1.5s");

    animator.UpdateAnimation(0.6f); // wraps 1.8 -> 0.4
    CHECK(fired.size() == 2, "wrap fires no events here");
}

static void TestRootMotion()
{
    std::cout << "[rootmotion]\n";
    auto model = MakeTestModel();
    AddTestClips(model);

    // Disabled: no delta reported.
    Animator animator(model);
    animator.PlayAnimation("walk");
    animator.UpdateAnimation(1.0f);
    CHECK(glm::length(animator.GetRootMotionDelta()) == 0.0f, "no delta while disabled");

    // Enabled: the root moves 1 unit/s in X, matrices keep the root at origin.
    animator.SetRootMotionEnabled(true);
    animator.Seek(0.0f);
    animator.UpdateAnimation(1.0f);
    CHECK(Near(animator.GetRootMotionDelta(), glm::vec3(1.0f, 0.0f, 0.0f)), "delta matches track speed");
    CHECK(Near(glm::vec3(animator.GetFinalBoneMatrices()[0][3]), glm::vec3(0.0f)), "root stripped from matrices");

    animator.UpdateAnimation(1.0f);
    CHECK(Near(animator.GetRootMotionDelta(), glm::vec3(1.0f, 0.0f, 0.0f)), "second step keeps moving");

    // Wrap: 0.5 -> 2.5 (wraps to 0.5) still accumulates a full 2-unit stride.
    animator.Seek(0.5f);
    animator.UpdateAnimation(2.0f);
    CHECK(Near(animator.GetRootMotionDelta(), glm::vec3(2.0f, 0.0f, 0.0f), 1e-2f), "wrapped stride accumulates");
}

static void TestMultiSkin()
{
    std::cout << "[multiskin]\n";
    auto model = MakeTestModel();
    AddTestClips(model);
    Animator animator(model);
    animator.PlayAnimation("walk");

    CHECK(animator.GetFinalBoneMatrices().size() == 3, "skin 0 has three joints");
    animator.SetSkin(1);
    CHECK(animator.GetSkinIndex() == 1, "skin switched");
    CHECK(animator.GetFinalBoneMatrices().size() == 2, "skin 1 has two joints");
    animator.SetSkin(99);
    CHECK(animator.GetSkinIndex() == 1, "invalid skin ignored");
}

static void TestStateMachine()
{
    std::cout << "[statemachine]\n";
    auto model = MakeTestModel();
    AddTestClips(model);
    Animator animator(model);

    using Cond = AnimationStateMachine::Condition;
    AnimationStateMachine fsm;
    fsm.AddParameter("IsMoving", false);
    fsm.AddParameter("Blend", 0.0f);
    fsm.AddState({"Idle", "walk", 1.0f, LoopMode::Loop});
    fsm.AddState({"Run", "run", 2.0f, LoopMode::Loop});
    fsm.AddState({"Jump", "run", 1.0f, LoopMode::None});
    fsm.AddTransition({"Idle", "Run", {{"IsMoving", Cond::Op::Equals, true}}, 0.25f});
    fsm.AddTransition({"Run", "Idle", {{"IsMoving", Cond::Op::Equals, false}}, 0.25f});
    fsm.AddTransition({"Idle", "Jump", {{"Jump", Cond::Op::Equals, true}}, 0.1f});

    fsm.Enter("Idle", animator);
    CHECK(fsm.GetCurrentState() == "Idle", "entered idle");

    fsm.Update(animator);
    CHECK(fsm.GetCurrentState() == "Idle", "no transition without parameters");

    fsm.SetBool("IsMoving", true);
    fsm.Update(animator);
    CHECK(fsm.GetCurrentState() == "Run", "bool parameter drives transition");
    CHECK(Near(animator.GetTimeScale(), 2.0f), "state speed applied to animator");

    fsm.SetBool("IsMoving", false);
    fsm.Update(animator);
    CHECK(fsm.GetCurrentState() == "Idle", "transition back");

    // Trigger fires exactly once.
    fsm.SetTrigger("Jump");
    fsm.Update(animator);
    CHECK(fsm.GetCurrentState() == "Jump", "trigger fires transition");
    fsm.Update(animator);
    CHECK(fsm.GetCurrentState() == "Jump", "trigger consumed after one use");

    // Float comparison conditions.
    AnimationStateMachine fsm2;
    fsm2.AddParameter("Blend", 0.0f);
    fsm2.AddState({"A", "walk", 1.0f, LoopMode::Loop});
    fsm2.AddState({"B", "run", 1.0f, LoopMode::Loop});
    fsm2.AddTransition({"A", "B", {{"Blend", Cond::Op::Greater, 0.5f}}, 0.0f});
    fsm2.Enter("A", animator);
    fsm2.SetFloat("Blend", 0.4f);
    fsm2.Update(animator);
    CHECK(fsm2.GetCurrentState() == "A", "float below threshold");
    fsm2.SetFloat("Blend", 0.6f);
    fsm2.Update(animator);
    CHECK(fsm2.GetCurrentState() == "B", "float above threshold");

    // Exit time gates the transition until the normalized threshold.
    AnimationStateMachine fsm3;
    fsm3.AddState({"A", "walk", 1.0f, LoopMode::Loop});
    fsm3.AddState({"B", "run", 1.0f, LoopMode::Loop});
    fsm3.AddTransition({"A", "B", {}, 0.0f, true, 0.5f});
    fsm3.Enter("A", animator);
    animator.Seek(0.5f); // normalized 0.25 of the 2 s walk clip
    fsm3.Update(animator);
    CHECK(fsm3.GetCurrentState() == "A", "exit time blocks early");
    animator.Seek(1.2f); // normalized 0.6
    fsm3.Update(animator);
    CHECK(fsm3.GetCurrentState() == "B", "exit time releases at threshold");
}

static void TestCompression()
{
    std::cout << "[compression]\n";
    // A linear track whose interior keys lie exactly on the straight line
    // reduces to two keys; a wiggling rotation track keeps its keys; a STEP
    // track is untouched.
    AnimationClip clip;
    clip.Name = "compress-me";
    clip.Duration = 10.0f;

    VectorTrack straight;
    straight.TargetBoneID = 0;
    straight.Interpolation = InterpolationMode::Linear;
    for (int i = 0; i <= 10; ++i)
        straight.Keyframes.push_back({static_cast<float>(i), glm::vec3(float(i), 0.0f, 0.0f)});
    clip.PositionTracks.push_back(straight);

    QuaternionTrack wiggling;
    wiggling.TargetBoneID = 1;
    wiggling.Interpolation = InterpolationMode::Linear;
    wiggling.Keyframes = {{0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)},
                          {5.0f, glm::angleAxis(1.5f, glm::vec3(0.0f, 1.0f, 0.0f))},
                          {10.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}};
    clip.RotationTracks.push_back(wiggling);

    VectorTrack stepped;
    stepped.TargetBoneID = 2;
    stepped.Interpolation = InterpolationMode::Step;
    stepped.Keyframes = {{0.0f, glm::vec3(1.0f)}, {5.0f, glm::vec3(2.0f)}, {10.0f, glm::vec3(1.0f)}};
    clip.ScaleTracks.push_back(stepped);

    CompressSettings settings;
    settings.PositionTolerance = 1e-3f;
    settings.RotationTolerance = 0.01f;

    CompressionStats measured = AnimationCompressor::MeasureClip(clip, settings);
    CHECK(measured.KeysBefore == 17, "stats see all keys");
    CHECK(measured.KeysAfter == 8, "straight track reduces, wiggle and step kept");
    CHECK(measured.KeyReduction() > 0.5f, "over half the keys removed");

    CompressionStats stats = AnimationCompressor::CompressClip(clip, settings);
    CHECK(stats.KeysAfter == measured.KeysAfter, "compress matches measure");
    CHECK(clip.PositionTracks[0].Keyframes.size() == 2, "straight track collapsed");
    CHECK(clip.ScaleTracks[0].Keyframes.size() == 3, "step track untouched");
    CHECK(clip.RotationTracks[0].Keyframes.size() == 3, "wiggling track kept");

    // Reduced straight track still samples on the original line.
    const auto& keys = clip.PositionTracks[0].Keyframes;
    float sample3 = glm::mix(keys[0].Value, keys[1].Value, 3.0f / 10.0f).x;
    float sample7 = glm::mix(keys[0].Value, keys[1].Value, 7.0f / 10.0f).x;
    CHECK(Near(sample3, 3.0f, 1e-2f), "reduced track hits t=3");
    CHECK(Near(sample7, 7.0f, 1e-2f), "reduced track hits t=7");

    // Parallel reduction through the jobsystem matches the serial result.
    AnimationClip parallelClip;
    parallelClip.Name = "parallel";
    parallelClip.Duration = 10.0f;
    for (int b = 0; b < 8; ++b)
    {
        VectorTrack track;
        track.TargetBoneID = b;
        track.Interpolation = InterpolationMode::Linear;
        for (int i = 0; i <= 20; ++i)
            track.Keyframes.push_back({static_cast<float>(i), glm::vec3(float(i) * 0.1f * b)});
        parallelClip.PositionTracks.push_back(track);
    }
    Jobsystem::UheJobsystem jobs;
    jobs.Init();
    CompressionStats parallelStats = AnimationCompressor::CompressClip(parallelClip, {}, &jobs);
    jobs.ShutDown();
    CHECK(parallelStats.KeysAfter == 16, "parallel reduction keeps two keys per track");
    CHECK(parallelStats.TracksProcessed == 8, "all tracks processed");
}

static void TestIK()
{
    std::cout << "[ik]\n";
    // Reachable target: the solved chain reaches it with bone lengths kept.
    glm::vec3 root(0.0f), mid(1.0f, 0.0f, 0.0f), end(2.0f, 0.0f, 0.0f);
    glm::vec3 target(1.0f, 1.0f, 0.0f);
    TwoBoneIKResult result = IKSolver::SolveTwoBone(root, mid, end, target, glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(result.Solved, "reachable target solved");

    glm::vec3 solvedMid = root + result.RootRotation * (mid - root);
    glm::vec3 solvedEnd = solvedMid + (result.RootRotation * result.MidRotation) * (end - mid);
    CHECK(Near(solvedEnd, target, 1e-2f), "end reaches the target");
    CHECK(Near(glm::length(solvedMid - root), 1.0f, 1e-2f), "upper bone length preserved");
    CHECK(Near(glm::length(solvedEnd - solvedMid), 1.0f, 1e-2f), "lower bone length preserved");

    // Unreachable target clamps to full extension instead of flipping.
    TwoBoneIKResult stretched =
        IKSolver::SolveTwoBone(root, mid, end, glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(stretched.Solved, "over-extended target still solves");
    glm::vec3 stretchedMid = root + stretched.RootRotation * (mid - root);
    glm::vec3 stretchedEnd = stretchedMid + (stretched.RootRotation * stretched.MidRotation) * (end - mid);
    CHECK(Near(glm::length(stretchedEnd - root), 2.0f, 1e-2f), "over-extension clamps to full reach");

    // Degenerate chain reports failure.
    TwoBoneIKResult degenerate = IKSolver::SolveTwoBone(root, root, end, target, glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(!degenerate.Solved, "zero-length bone reports unsolved");
}

int main()
{
    TestInterpolation();
    TestPlayback();
    TestCrossFade();
    TestEvents();
    TestRootMotion();
    TestMultiSkin();
    TestStateMachine();
    TestCompression();
    TestIK();

    if (g_failures == 0)
    {
        std::cout << "\nALL ANIMATION TESTS PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " ANIMATION TEST(S) FAILED\n";
    return 1;
}
