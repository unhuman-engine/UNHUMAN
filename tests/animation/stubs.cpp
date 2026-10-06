// GPU stubs for the animation test harness (issue #41).
//
// The harness compiles the real Animator/AnimationStateMachine/
// AnimationCompression/IKSolver translation units. Those never touch the RHI,
// but Animator.cpp uses Ref<Model> whose destructor lives in LoadModel.cpp
// (a GPU-heavy TU we deliberately do not compile), so the destructor and
// Destroy() are stubbed here.
#include "UHE/Renderer3D/LoadModel.h"

namespace UHE::RD3d {

Model::~Model() = default;
void Model::Destroy() {}

} // namespace UHE::RD3d
