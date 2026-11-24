// Stubs for the scripting harness: the layers a script never touches in a
// headless test. Input is stubbed in tests/scene/stubs.cpp, which this harness
// links too.
#include <utility>
#include "UHE/Renderer3D/Animator.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace UHE
{
// SceneSerializer loads a Model when a scene declares one. The harness's scenes
// do not, so this exists to link, not to run -- the same reason tests/scene
// stubs the renderer it never calls. The Animator stubs live in
// tests/scene/stubs.cpp, which both harnesses link.
bool RD3d::Model::loadModel(const std::filesystem::path&, const RD3d::ModelLoadOptions&)
{
    return false;
}
} // namespace UHE
