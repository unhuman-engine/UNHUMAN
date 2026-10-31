// Stubs for the scripting harness: the layers a script never touches in a
// headless test. Input is stubbed in tests/scene/stubs.cpp, which this harness
// links too.
#include "UHE/Renderer3D/LoadModel.h"
#include "UHE/Renderer3D/Animator.h"

#include <utility>

namespace UHE
{
// SceneSerializer constructs a Model and an Animator when a scene declares those
// components. The harness's scenes do not, so these exist to link, not to run --
// the same reason tests/scene stubs the renderer it never calls.
bool RD3d::Model::loadModel(const std::filesystem::path&, const RD3d::ModelLoadOptions&) { return false; }
RD3d::Animator::Animator(Ref<RD3d::Model> model) : m_Model(std::move(model)) {}
void RD3d::Animator::PlayAnimation(const std::string&) {}
} // namespace UHE
