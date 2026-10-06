#pragma once
#include <cstddef>
#include "UHE/Core/Core.h"
#include "UHE/Renderer3D/Animation.h"

namespace UHE::Jobsystem
{
class UheJobsystem;
}

namespace UHE::RD3d
{

// Issue #41: lossy keyframe reduction ("animation compression" without
// changing the clip format). A key is dropped when resampling the original
// track through linear interpolation between its surviving neighbours stays
// within tolerance; rotations are compared as spherical angles, vectors as
// euclidean distance. STEP tracks are left untouched (reduction changes
// their semantics) and CUBICSPLINE tracks are only reduced when the tangents
// of the dropped key are also within tolerance of the spline-free blend.
struct UHE_API CompressSettings
{
    float PositionTolerance = 1e-3f; // linear units
    float ScaleTolerance = 1e-4f;    // scale units
    float RotationTolerance = 0.02f; // radians (~1.15 degrees)
};

struct UHE_API CompressionStats
{
    size_t KeysBefore = 0;
    size_t KeysAfter = 0;
    size_t TracksProcessed = 0;

    // Estimated payload: 16 B per vector key, 20 B per quaternion key.
    size_t BytesBefore = 0;
    size_t BytesAfter = 0;

    float KeyReduction() const
    {
        return KeysBefore > 0 ? 1.0f - static_cast<float>(KeysAfter) / static_cast<float>(KeysBefore) : 0.0f;
    }
    float ByteReduction() const
    {
        return BytesBefore > 0 ? 1.0f - static_cast<float>(BytesAfter) / static_cast<float>(BytesBefore) : 0.0f;
    }
};

class UHE_API AnimationCompressor
{
public:
    // Reduces 'clip' in place and returns what was removed. When 'jobs' is
    // provided the tracks are reduced in parallel on the jobsystem.
    static CompressionStats CompressClip(AnimationClip& clip, const CompressSettings& settings = {},
                                         Jobsystem::UheJobsystem* jobs = nullptr);

    // Non-mutating estimate of the same reduction, for tooling/inspector UI.
    static CompressionStats MeasureClip(const AnimationClip& clip, const CompressSettings& settings = {});
};

} // namespace UHE::RD3d
