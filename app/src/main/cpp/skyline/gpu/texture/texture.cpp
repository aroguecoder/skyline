// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include <common/trace.h>
#include <kernel/types/KProcess.h>
#include "texture.h"
#include "copy.h"

namespace skyline::gpu {
    std::shared_ptr<memory::StagingBuffer> Texture::SynchronizeHostImpl(const std::shared_ptr<FenceCycle> &pCycle) {
        if (!guest)
            throw exception("Synchronization of host textures requires a valid guest texture to synchronize from");
        else if (guest->dimensions != dimensions)
            throw exception("Guest and host dimensions being different is not supported currently");
        else if (guest->mappings.size() > 1)
            throw exception("Synchronizing textures across {} mappings is not supported", guest->mappings.size());

        auto pointer{guest->mappings[0].data()};
        auto size{format->GetSize(dimensions)};

        WaitOnBacking();

        u8 *bufferData;
        auto stagingBuffer{[&]() -> std::shared_ptr<memory::StagingBuffer> {
            if (tiling == vk::ImageTiling::eOptimal || !std::holds_alternative<memory::Image>(backing)) {
                // We need a staging buffer for all optimal copies (since we aren't aware of the host optimal layout) and linear textures which we cannot map on the CPU since we do not have access to their backing VkDeviceMemory
                auto stagingBuffer{gpu.memory.AllocateStagingBuffer(size)};
                bufferData = stagingBuffer->data();
                return stagingBuffer;
            } else if (tiling == vk::ImageTiling::eLinear) {
                // We can optimize linear texture sync on a UMA by mapping the texture onto the CPU and copying directly into it rather than a staging buffer
                bufferData = std::get<memory::Image>(backing).data();
                if (cycle.lock() != pCycle)
                    WaitOnFence();
                return nullptr;
            } else {
                throw exception("Guest -> Host synchronization of images tiled as '{}' isn't implemented", vk::to_string(tiling));
            }
        }()};

        if (guest->tileConfig.mode == texture::TileMode::Block)
            CopyBlockLinearToLinear(*guest, pointer, bufferData);
        else if (guest->tileConfig.mode == texture::TileMode::Pitch)
            CopyPitchLinearToLinear(*guest, pointer, bufferData);
        else if (guest->tileConfig.mode == texture::TileMode::Linear)
            std::memcpy(bufferData, pointer, size);

        if (stagingBuffer && cycle.lock() != pCycle)
            WaitOnFence();

        return stagingBuffer;
    }

    void Texture::CopyFromStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer) {
        auto image{GetBacking()};
        if (layout != vk::ImageLayout::eTransferDstOptimal) {
            commandBuffer.pipelineBarrier(layout != vk::ImageLayout::eUndefined ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                .image = image,
                .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = layout,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .subresourceRange = {
                    .aspectMask = format->vkAspect,
                    .levelCount = mipLevels,
                    .layerCount = layerCount,
                },
            });

            if (layout == vk::ImageLayout::eUndefined)
                layout = vk::ImageLayout::eTransferDstOptimal;
        }

        commandBuffer.copyBufferToImage(stagingBuffer->vkBuffer, image, vk::ImageLayout::eTransferDstOptimal, vk::BufferImageCopy{
            .imageExtent = dimensions,
            .imageSubresource = {
                .aspectMask = format->vkAspect,
                .layerCount = layerCount,
            },
        });

        if (layout != vk::ImageLayout::eTransferDstOptimal)
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                .image = image,
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .subresourceRange = {
                    .aspectMask = format->vkAspect,
                    .levelCount = mipLevels,
                    .layerCount = layerCount,
                },
            });
    }

    void Texture::CopyIntoStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer) {
        auto image{GetBacking()};
        if (layout != vk::ImageLayout::eTransferSrcOptimal) {
            commandBuffer.pipelineBarrier(layout != vk::ImageLayout::eUndefined ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                .image = image,
                .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = layout,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .subresourceRange = {
                    .aspectMask = format->vkAspect,
                    .levelCount = mipLevels,
                    .layerCount = layerCount,
                },
            });

            if (layout == vk::ImageLayout::eUndefined)
                layout = vk::ImageLayout::eTransferSrcOptimal;
        }

        commandBuffer.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer->vkBuffer, vk::BufferImageCopy{
            .imageExtent = dimensions,
            .imageSubresource = {
                .aspectMask = format->vkAspect,
                .layerCount = layerCount,
            },
        });

        if (layout != vk::ImageLayout::eTransferSrcOptimal)
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                .image = image,
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .subresourceRange = {
                    .aspectMask = format->vkAspect,
                    .levelCount = mipLevels,
                    .layerCount = layerCount,
                },
            });
    }

    void Texture::CopyToGuest(u8 *hostBuffer) {
        auto guestOutput{guest->mappings[0].data()};
        auto size{format->GetSize(dimensions)};

        if (guest->tileConfig.mode == texture::TileMode::Block)
            CopyLinearToBlockLinear(*guest, hostBuffer, guestOutput);
        else if (guest->tileConfig.mode == texture::TileMode::Pitch)
            CopyLinearToPitchLinear(*guest, hostBuffer, guestOutput);
        else if (guest->tileConfig.mode == texture::TileMode::Linear)
            std::memcpy(hostBuffer, guestOutput, format->GetSize(dimensions));
    }

    Texture::TextureBufferCopy::TextureBufferCopy(std::shared_ptr<Texture> texture, std::shared_ptr<memory::StagingBuffer> stagingBuffer) : texture(std::move(texture)), stagingBuffer(std::move(stagingBuffer)) {}

    Texture::TextureBufferCopy::~TextureBufferCopy() {
        texture->CopyToGuest(stagingBuffer ? stagingBuffer->data() : std::get<memory::Image>(texture->backing).data());
    }

    Texture::Texture(GPU &gpu, BackingType &&backing, GuestTexture guest, texture::Dimensions dimensions, texture::Format format, vk::ImageLayout layout, vk::ImageTiling tiling, u32 mipLevels, u32 layerCount, vk::SampleCountFlagBits sampleCount)
        : gpu(gpu),
          backing(std::move(backing)),
          layout(layout),
          guest(std::move(guest)),
          dimensions(dimensions),
          format(format),
          tiling(tiling),
          mipLevels(mipLevels),
          layerCount(layerCount),
          sampleCount(sampleCount) {
        if (GetBacking())
            SynchronizeHost();
    }

    Texture::Texture(GPU &gpu, BackingType &&backing, texture::Dimensions dimensions, texture::Format format, vk::ImageLayout layout, vk::ImageTiling tiling, u32 mipLevels, u32 layerCount, vk::SampleCountFlagBits sampleCount)
        : gpu(gpu),
          backing(std::move(backing)),
          dimensions(dimensions),
          format(format),
          layout(layout),
          tiling(tiling),
          mipLevels(mipLevels),
          layerCount(layerCount),
          sampleCount(sampleCount) {}

    Texture::Texture(GPU &pGpu, GuestTexture pGuest)
        : gpu(pGpu),
          guest(std::move(pGuest)),
          dimensions(guest->dimensions),
          format(guest->format),
          layout(vk::ImageLayout::eUndefined),
          tiling((guest->tileConfig.mode == texture::TileMode::Block) ? vk::ImageTiling::eOptimal : vk::ImageTiling::eLinear),
          mipLevels(1),
          layerCount(guest->layerCount),
          sampleCount(vk::SampleCountFlagBits::e1) {
        vk::ImageCreateInfo imageCreateInfo{
            .imageType = guest->dimensions.GetType(),
            .format = *guest->format,
            .extent = guest->dimensions,
            .mipLevels = 1,
            .arrayLayers = guest->layerCount,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = tiling,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &gpu.vkQueueFamilyIndex,
            .initialLayout = layout,
        };
        backing = tiling != vk::ImageTiling::eLinear ? gpu.memory.AllocateImage(imageCreateInfo) : gpu.memory.AllocateMappedImage(imageCreateInfo);
        TransitionLayout(vk::ImageLayout::eGeneral);
    }

    Texture::Texture(GPU &gpu, texture::Dimensions dimensions, texture::Format format, vk::ImageLayout initialLayout, vk::ImageUsageFlags usage, vk::ImageTiling tiling, u32 mipLevels, u32 layerCount, vk::SampleCountFlagBits sampleCount)
        : gpu(gpu),
          dimensions(dimensions),
          format(format),
          layout(initialLayout == vk::ImageLayout::ePreinitialized ? vk::ImageLayout::ePreinitialized : vk::ImageLayout::eUndefined),
          tiling(tiling),
          mipLevels(mipLevels),
          layerCount(layerCount),
          sampleCount(sampleCount) {
        vk::ImageCreateInfo imageCreateInfo{
            .imageType = dimensions.GetType(),
            .format = *format,
            .extent = dimensions,
            .mipLevels = mipLevels,
            .arrayLayers = layerCount,
            .samples = sampleCount,
            .tiling = tiling,
            .usage = usage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &gpu.vkQueueFamilyIndex,
            .initialLayout = layout,
        };
        backing = tiling != vk::ImageTiling::eLinear ? gpu.memory.AllocateImage(imageCreateInfo) : gpu.memory.AllocateMappedImage(imageCreateInfo);
        if (initialLayout != layout)
            TransitionLayout(initialLayout);
    }

    bool Texture::WaitOnBacking() {
        TRACE_EVENT("gpu", "Texture::WaitOnBacking");

        if (GetBacking()) [[likely]] {
            return false;
        } else {
            std::unique_lock lock(mutex, std::adopt_lock);
            backingCondition.wait(lock, [&]() -> bool { return GetBacking(); });
            lock.release();
            return true;
        }
    }

    void Texture::WaitOnFence() {
        TRACE_EVENT("gpu", "Texture::WaitOnFence");

        auto lCycle{cycle.lock()};
        if (lCycle) {
            lCycle->Wait();
            cycle.reset();
        }
    }

    void Texture::SwapBacking(BackingType &&pBacking, vk::ImageLayout pLayout) {
        WaitOnFence();

        backing = std::move(pBacking);
        layout = pLayout;
        if (GetBacking())
            backingCondition.notify_all();
    }

    void Texture::TransitionLayout(vk::ImageLayout pLayout) {
        WaitOnBacking();
        WaitOnFence();

        TRACE_EVENT("gpu", "Texture::TransitionLayout");

        if (layout != pLayout) {
            cycle = gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                commandBuffer.pipelineBarrier(layout != vk::ImageLayout::eUndefined ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = GetBacking(),
                    .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
                    .oldLayout = layout,
                    .newLayout = pLayout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = {
                        .aspectMask = format->vkAspect,
                        .levelCount = mipLevels,
                        .layerCount = layerCount,
                    },
                });
            });
            layout = pLayout;
        }
    }

    void Texture::SynchronizeHost() {
        TRACE_EVENT("gpu", "Texture::SynchronizeHost");

        auto stagingBuffer{SynchronizeHostImpl(nullptr)};
        if (stagingBuffer) {
            auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                CopyFromStagingBuffer(commandBuffer, stagingBuffer);
            })};
            lCycle->AttachObjects(stagingBuffer, shared_from_this());
            cycle = lCycle;
        }
    }

    void Texture::SynchronizeHostWithBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &pCycle) {
        TRACE_EVENT("gpu", "Texture::SynchronizeHostWithBuffer");

        auto stagingBuffer{SynchronizeHostImpl(pCycle)};
        if (stagingBuffer) {
            CopyFromStagingBuffer(commandBuffer, stagingBuffer);
            pCycle->AttachObjects(stagingBuffer, shared_from_this());
            cycle = pCycle;
        }
    }

    void Texture::SynchronizeGuest() {
        if (!guest)
            throw exception("Synchronization of guest textures requires a valid guest texture to synchronize to");
        else if (layout == vk::ImageLayout::eUndefined)
            return; // If the state of the host texture is undefined then so can the guest
        else if (guest->mappings.size() > 1)
            throw exception("Synchronizing textures across {} mappings is not supported", guest->mappings.size());

        TRACE_EVENT("gpu", "Texture::SynchronizeGuest");

        WaitOnBacking();
        WaitOnFence();

        if (tiling == vk::ImageTiling::eOptimal || !std::holds_alternative<memory::Image>(backing)) {
            auto size{format->GetSize(dimensions)};
            auto stagingBuffer{gpu.memory.AllocateStagingBuffer(size)};

            auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                CopyIntoStagingBuffer(commandBuffer, stagingBuffer);
            })};
            lCycle->AttachObject(std::make_shared<TextureBufferCopy>(shared_from_this(), stagingBuffer));
            cycle = lCycle;
        } else if (tiling == vk::ImageTiling::eLinear) {
            // We can optimize linear texture sync on a UMA by mapping the texture onto the CPU and copying directly from it rather than using a staging buffer
            CopyToGuest(std::get<memory::Image>(backing).data());
        } else {
            throw exception("Host -> Guest synchronization of images tiled as '{}' isn't implemented", vk::to_string(tiling));
        }
    }

    void Texture::SynchronizeGuestWithBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &pCycle) {
        if (!guest)
            throw exception("Synchronization of guest textures requires a valid guest texture to synchronize to");
        else if (layout == vk::ImageLayout::eUndefined)
            return; // If the state of the host texture is undefined then so can the guest
        else if (guest->mappings.size() > 1)
            throw exception("Synchronizing textures across {} mappings is not supported", guest->mappings.size());

        TRACE_EVENT("gpu", "Texture::SynchronizeGuestWithBuffer");

        WaitOnBacking();
        if (cycle.lock() != pCycle)
            WaitOnFence();

        if (tiling == vk::ImageTiling::eOptimal || !std::holds_alternative<memory::Image>(backing)) {
            auto size{format->GetSize(dimensions)};
            auto stagingBuffer{gpu.memory.AllocateStagingBuffer(size)};

            CopyIntoStagingBuffer(commandBuffer, stagingBuffer);
            pCycle->AttachObject(std::make_shared<TextureBufferCopy>(shared_from_this(), stagingBuffer));
            cycle = pCycle;
        } else if (tiling == vk::ImageTiling::eLinear) {
            CopyToGuest(std::get<memory::Image>(backing).data());
            pCycle->AttachObject(std::make_shared<TextureBufferCopy>(shared_from_this()));
            cycle = pCycle;
        } else {
            throw exception("Host -> Guest synchronization of images tiled as '{}' isn't implemented", vk::to_string(tiling));
        }
    }

    void Texture::CopyFrom(std::shared_ptr<Texture> source, const vk::ImageSubresourceRange &subresource) {
        WaitOnBacking();
        WaitOnFence();

        source->WaitOnBacking();
        source->WaitOnFence();

        if (source->layout == vk::ImageLayout::eUndefined)
            throw exception("Cannot copy from image with undefined layout");
        else if (source->dimensions != dimensions)
            throw exception("Cannot copy from image with different dimensions");
        else if (source->format != format)
            throw exception("Cannot copy from image with different format");

        TRACE_EVENT("gpu", "Texture::CopyFrom");

        auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
            auto sourceBacking{source->GetBacking()};
            if (source->layout != vk::ImageLayout::eTransferSrcOptimal) {
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = sourceBacking,
                    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                    .oldLayout = source->layout,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = subresource,
                });
            }

            auto destinationBacking{GetBacking()};
            if (layout != vk::ImageLayout::eTransferDstOptimal) {
                commandBuffer.pipelineBarrier(layout != vk::ImageLayout::eUndefined ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = destinationBacking,
                    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .oldLayout = layout,
                    .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = subresource,
                });

                if (layout == vk::ImageLayout::eUndefined)
                    layout = vk::ImageLayout::eTransferDstOptimal;
            }

            vk::ImageSubresourceLayers subresourceLayers{
                .aspectMask = subresource.aspectMask,
                .mipLevel = subresource.baseMipLevel,
                .baseArrayLayer = subresource.baseArrayLayer,
                .layerCount = subresource.layerCount == VK_REMAINING_ARRAY_LAYERS ? layerCount - subresource.baseArrayLayer : subresource.layerCount,
            };
            for (; subresourceLayers.mipLevel < (subresource.levelCount == VK_REMAINING_MIP_LEVELS ? mipLevels - subresource.baseMipLevel : subresource.levelCount); subresourceLayers.mipLevel++)
                commandBuffer.copyImage(sourceBacking, vk::ImageLayout::eTransferSrcOptimal, destinationBacking, vk::ImageLayout::eTransferDstOptimal, vk::ImageCopy{
                    .srcSubresource = subresourceLayers,
                    .dstSubresource = subresourceLayers,
                    .extent = dimensions,
                });

            if (layout != vk::ImageLayout::eTransferDstOptimal)
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = destinationBacking,
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = subresource,
                });

            if (layout != vk::ImageLayout::eTransferSrcOptimal)
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = sourceBacking,
                    .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                    .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .newLayout = source->layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = subresource,
                });
        })};
        lCycle->AttachObjects(std::move(source), shared_from_this());
        cycle = lCycle;
    }

    Texture::~Texture() {
        WaitOnFence();
    }

    TextureView::TextureView(std::shared_ptr<Texture> backing, vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format format, vk::ComponentMapping mapping) : backing(std::move(backing)), type(type), format(format), mapping(mapping), range(range) {}

    vk::ImageView TextureView::GetView() {
        if (view)
            return **view;

        auto viewType{[&]() {
            switch (backing->dimensions.GetType()) {
                case vk::ImageType::e1D:
                    return range.layerCount > 1 ? vk::ImageViewType::e1DArray : vk::ImageViewType::e1D;
                case vk::ImageType::e2D:
                    return range.layerCount > 1 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
                case vk::ImageType::e3D:
                    return vk::ImageViewType::e3D;
            }
        }()};

        vk::ImageViewCreateInfo createInfo{
            .image = backing->GetBacking(),
            .viewType = viewType,
            .format = format ? *format : *backing->format,
            .components = mapping,
            .subresourceRange = range,
        };

        auto &views{backing->views};
        auto iterator{std::find_if(views.begin(), views.end(), [&](const std::pair<vk::ImageViewCreateInfo, vk::raii::ImageView> &item) {
            return item.first == createInfo;
        })};
        if (iterator != views.end())
            return *iterator->second;

        return *views.emplace_back(createInfo, vk::raii::ImageView(backing->gpu.vkDevice, createInfo)).second;
    }
}
