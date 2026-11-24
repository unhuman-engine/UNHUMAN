#pragma once
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHIDevice.h"
#include <vector>
#include <initializer_list>

namespace UHE {

	enum class FramebufferTextureFormat
	{
		None = 0,

		// Color
		RGBA8,
		RED_INTEGER,

		// Depth/stencil
		DEPTH24STENCIL8,

		// Defaults
		Depth = DEPTH24STENCIL8
	};

	struct FramebufferTextureSpecification
	{
		FramebufferTextureSpecification() = default;
		FramebufferTextureSpecification(FramebufferTextureFormat format)
			: TextureFormat(format) {}

		FramebufferTextureFormat TextureFormat = FramebufferTextureFormat::None;
		// TODO: filtering/wrap
	};

	struct FramebufferAttachmentSpecification
	{
		FramebufferAttachmentSpecification() = default;
		FramebufferAttachmentSpecification(std::initializer_list<FramebufferTextureSpecification> attachments)
			: Attachments(attachments) {}

		std::vector<FramebufferTextureSpecification> Attachments;
	};

	struct FramebufferSpecification
	{
		uint32_t Width = 0, Height = 0;
		FramebufferAttachmentSpecification Attachments;
		uint32_t Samples = 1;

		bool SwapChainTarget = false;
	};

	class UHE_API Framebuffer
	{
	public:
		Framebuffer(const FramebufferSpecification& spec);
		~Framebuffer();

		void Invalidate();

		void Bind();
		void Unbind();

		void Resize(uint32_t width, uint32_t height);
		int ReadPixel(uint32_t attachmentIndex, int x, int y);
        /// @brief Picking without a stall: queues a read of (x, y) and returns
        /// the newest finished one (a few frames old), or -1 before the first.
        int ReadPixelAsync(uint32_t attachmentIndex, int x, int y);

        void* GetColorAttachmentRendererID(uint32_t index = 0) const;

        const FramebufferSpecification& GetSpecification() const { return m_Specification; }

        const std::vector<RHI::TextureHandle>& GetColorAttachments() const { return m_ColorAttachments; }
        RHI::TextureHandle GetDepthAttachment() const { return m_DepthAttachment; }

		static Ref<Framebuffer> Create(const FramebufferSpecification& spec);
        
    private:
        FramebufferSpecification m_Specification;

        std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
        FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;

        std::vector<RHI::TextureHandle> m_ColorAttachments;
        RHI::TextureHandle m_DepthAttachment = nullptr;
	};

}
