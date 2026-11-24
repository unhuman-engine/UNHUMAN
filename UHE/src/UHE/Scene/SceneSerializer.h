#pragma once
#include "Scene.h"

namespace UHE {

	class UHE_API SceneSerializer
	{
	public:
		SceneSerializer(const Ref<Scene>& scene);

		void Serialize(const std::string& filepath);
        /// \returns false, with the reason logged, when the file is missing,
        /// is not YAML, or is missing a field a component requires. It never
        /// throws: a hand-edited or older scene must not take the editor down.
        /// On failure the scene may hold the entities read before the error,
        /// so load into a fresh scene and keep it only on success.
        bool Deserialize(const std::string& filepath);

    private:
        bool DeserializeUnchecked(const std::string& filepath);

        Ref<Scene> m_Scene;
    };
}