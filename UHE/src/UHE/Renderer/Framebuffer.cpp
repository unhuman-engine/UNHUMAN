#include "Framebuffer.h"
#include "UHE/RHI/RHITexture.h"
#include "UHE/Renderer/Renderer.h"

namespace UHE
{

static const uint32_t s_MaxFramebufferSize = 8192;

namespace Utils
{

static RHI::TextureFormat UHEFBTextureFormatToRHI(FramebufferTextureFormat format)
{
    switch (format)
    {
        case FramebufferTextureFormat::RGBA8:
            return RHI::TextureFormat::RGBA8_SRGB;
        case FramebufferTextureFormat::RED_INTEGER:
            return RHI::TextureFormat::R32_SINT;
        case FramebufferTextureFormat::DEPTH24STENCIL8:
            return RHI::TextureFormat::D24_UNORM_S8;
    }
    return RHI::TextureFormat::Undefined;
}

} // namespace Utils

Framebuffer::Framebuffer(const FramebufferSpecification& spec) : m_Specification(spec)
{
    for (auto spec : m_Specification.Attachments.Attachments)
    {
        if (spec.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8)
            m_DepthAttachmentSpecification = spec;
        else
            m_ColorAttachmentSpecifications.emplace_back(spec);
    }

    Invalidate();
}

Framebuffer::~Framebuffer()
{
    auto& device = Renderer::GetDevice();
    device.WaitIdle();
    for (auto handle : m_ColorAttachments)
        device.DestroyTexture(handle);
    if (m_DepthAttachment)
        device.DestroyTexture(m_DepthAttachment);
}

void Framebuffer::Invalidate()
{
    auto& device = Renderer::GetDevice();

    if (m_ColorAttachments.size())
    {
        device.WaitIdle();
        for (auto handle : m_ColorAttachments)
            device.DestroyTexture(handle);
        if (m_DepthAttachment)
            device.DestroyTexture(m_DepthAttachment);
        m_ColorAttachments.clear();
        m_DepthAttachment = nullptr;
    }

    bool multisample = m_Specification.Samples > 1;

    if (m_ColorAttachmentSpecifications.size())
    {
        m_ColorAttachments.resize(m_ColorAttachmentSpecifications.size());

        for (size_t i = 0; i < m_ColorAttachments.size(); i++)
        {
            RHI::TextureDesc desc;
            desc.width = m_Specification.Width;
            desc.height = m_Specification.Height;
            desc.format = Utils::UHEFBTextureFormatToRHI(m_ColorAttachmentSpecifications[i].TextureFormat);
            desc.usage = RHI::TextureUsage::ColorAttach | RHI::TextureUsage::Sampled;
            if (m_ColorAttachmentSpecifications[i].TextureFormat == FramebufferTextureFormat::RED_INTEGER)
            {
                desc.usage = desc.usage | RHI::TextureUsage::TransferSrc; // For ReadPixel
            }

            m_ColorAttachments[i] = device.CreateTexture(desc);
        }
    }

    if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
    {
        RHI::TextureDesc desc;
        desc.width = m_Specification.Width;
        desc.height = m_Specification.Height;
        desc.format = Utils::UHEFBTextureFormatToRHI(m_DepthAttachmentSpecification.TextureFormat);
        desc.usage = RHI::TextureUsage::DepthAttach | RHI::TextureUsage::Sampled;

        m_DepthAttachment = device.CreateTexture(desc);
    }
}

void Framebuffer::Bind()
{
    // In RHI, binding is done via BeginRenderPass.
    // We leave this empty or use it to setup the render pass later.
}

void Framebuffer::Unbind() {}

void Framebuffer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || width > s_MaxFramebufferSize || height > s_MaxFramebufferSize)
    {
        return;
    }

    m_Specification.Width = width;
    m_Specification.Height = height;

    Invalidate();
}

int Framebuffer::ReadPixel(uint32_t attachmentIndex, int x, int y)
{
    if (attachmentIndex >= m_ColorAttachments.size())
        return -1;

    if (x < 0 || y < 0 || x >= (int)m_Specification.Width || y >= (int)m_Specification.Height)
        return -1;

    int pixelData = -1;
    auto& device = Renderer::GetDevice();
    device.ReadPixel(m_ColorAttachments[attachmentIndex], x, y, &pixelData);
    return pixelData;
}

void* Framebuffer::GetColorAttachmentRendererID(uint32_t index) const
{
    if (index < m_ColorAttachments.size())
    {
        auto texHandle = m_ColorAttachments[index];
        if (texHandle)
        {
            return reinterpret_cast<RHI::RHITexture*>(texHandle)->GetImGuiTextureID();
        }
    }
    return nullptr;
}

Ref<Framebuffer> Framebuffer::Create(const FramebufferSpecification& spec)
{
    return CreateRef<Framebuffer>(spec);
}

} // namespace UHE
