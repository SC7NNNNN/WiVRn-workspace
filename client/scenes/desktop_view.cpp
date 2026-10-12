/*
 * WiVRn VR streaming
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "desktop_view.h"

#ifdef __ANDROID__

#include "application.h"
#include "decoder/android/android_decoder.h"
#include "render/imgui_impl.h"
#include "vk/shader.h"

#include <android/hardware_buffer.h>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <unordered_map>
#include <vulkan/vulkan_android.h>

DEUGLIFY(AMediaFormat)

namespace
{
// Frames waiting to be decoded before the queue is considered late
constexpr size_t max_pending_frames = 30;

void check(media_status_t status, const char * msg)
{
	if (status != AMEDIA_OK)
		throw std::runtime_error(std::string(msg) + ": MediaCodec error " + std::to_string(int(status)));
}

// A decoded image imported in Vulkan
struct mapped_buffer
{
	vk::raii::DeviceMemory memory = nullptr;
	vk::raii::Image image = nullptr;
	vk::raii::ImageView view = nullptr;
	vk::ImageLayout layout = vk::ImageLayout::eUndefined;
};

// Keeps a decoded image alive while the GPU reads it
struct held_image
{
	std::shared_ptr<AImageReader> reader;
	AImage_ptr image;

	void reset()
	{
		// The image must be released before its reader
		image.reset();
		reader.reset();
	}
};
} // namespace

struct desktop_view::impl
{
	vk::raii::Device & device;
	vk::raii::PhysicalDevice & physical_device;
	thread_safe<vk::raii::Queue> & queue;
	imgui_textures & textures;

	// Decoder, owned by the decoder thread except for the reader shared with the render thread
	std::mutex mutex;
	std::condition_variable_any cv;
	std::deque<wivrn::to_headset::desktop_frame> frames;
	std::optional<std::pair<uint16_t, uint16_t>> pending_reset;
	bool pending_stop = false;
	std::shared_ptr<AImageReader> reader; // locked by mutex
	std::jthread thread;

	// Render thread
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID vkGetAndroidHardwareBufferPropertiesANDROID;
	std::shared_ptr<AImageReader> mapped_reader;
	std::unordered_map<AHardwareBuffer *, std::shared_ptr<mapped_buffer>> mapped;
	uint64_t external_format = 0;
	vk::raii::SamplerYcbcrConversion conversion = nullptr;
	vk::raii::Sampler sampler = nullptr;
	vk::raii::DescriptorSetLayout ds_layout = nullptr;
	vk::raii::DescriptorPool ds_pool = nullptr;
	vk::raii::DescriptorSet ds = nullptr;
	vk::raii::PipelineLayout pipeline_layout = nullptr;
	vk::raii::RenderPass renderpass = nullptr;
	vk::raii::Pipeline pipeline = nullptr;

	vk::raii::CommandPool command_pool = nullptr;
	vk::raii::CommandBuffer command_buffer = nullptr;
	vk::raii::Fence fence = nullptr;
	bool in_flight = false;
	held_image in_flight_image;

	// RGBA copy drawn by the GUI
	vk::Extent2D target_extent{};
	vk::raii::DeviceMemory target_memory = nullptr;
	vk::raii::Image target = nullptr;
	vk::raii::ImageView target_view_unorm = nullptr; // rendered to
	vk::raii::ImageView target_view_srgb = nullptr;  // sampled by the GUI
	vk::raii::Framebuffer framebuffer = nullptr;
	ImTextureID texture = 0;
	bool has_frame = false;

	impl(vk::raii::Device & device, vk::raii::PhysicalDevice & physical_device, thread_safe<vk::raii::Queue> & queue, uint32_t queue_family_index, imgui_textures & textures) :
	        device(device),
	        physical_device(physical_device),
	        queue(queue),
	        textures(textures)
	{
		vkGetAndroidHardwareBufferPropertiesANDROID = application::get_vulkan_proc<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>("vkGetAndroidHardwareBufferPropertiesANDROID");

		command_pool = vk::raii::CommandPool(device, vk::CommandPoolCreateInfo{
		                                                     .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		                                                     .queueFamilyIndex = queue_family_index,
		                                             });
		command_buffer = std::move(device.allocateCommandBuffers({
		        .commandPool = *command_pool,
		        .level = vk::CommandBufferLevel::ePrimary,
		        .commandBufferCount = 1,
		})[0]);
		fence = device.createFence({});

		renderpass = create_renderpass();

		thread = std::jthread([this](std::stop_token stop) { run(stop); });
		pthread_setname_np(thread.native_handle(), "desktop_decoder");
	}

	~impl()
	{
		thread.request_stop();
		cv.notify_all();
		if (thread.joinable())
			thread.join();

		if (in_flight)
			(void)device.waitForFences(*fence, true, UINT64_MAX);
		in_flight_image.reset();

		if (texture)
			textures.free_texture(texture);
	}

	vk::raii::RenderPass create_renderpass()
	{
		vk::AttachmentDescription attachment{
		        .format = vk::Format::eR8G8B8A8Unorm,
		        .samples = vk::SampleCountFlagBits::e1,
		        .loadOp = vk::AttachmentLoadOp::eDontCare,
		        .storeOp = vk::AttachmentStoreOp::eStore,
		        .initialLayout = vk::ImageLayout::eUndefined,
		        .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::AttachmentReference color_ref{.attachment = 0, .layout = vk::ImageLayout::eColorAttachmentOptimal};
		vk::SubpassDescription subpass{
		        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
		        .colorAttachmentCount = 1,
		        .pColorAttachments = &color_ref,
		};
		std::array dependencies{
		        // The GUI of the previous frame may still sample the image
		        vk::SubpassDependency{
		                .srcSubpass = vk::SubpassExternal,
		                .dstSubpass = 0,
		                .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader,
		                .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                .srcAccessMask = {},
		                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
		        },
		        // The GUI samples the image after the conversion
		        vk::SubpassDependency{
		                .srcSubpass = 0,
		                .dstSubpass = vk::SubpassExternal,
		                .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,
		                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
		                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
		        },
		};
		return vk::raii::RenderPass(device, vk::RenderPassCreateInfo{
		                                            .attachmentCount = 1,
		                                            .pAttachments = &attachment,
		                                            .subpassCount = 1,
		                                            .pSubpasses = &subpass,
		                                            .dependencyCount = dependencies.size(),
		                                            .pDependencies = dependencies.data(),
		                                    });
	}

	// Decoder thread
	void run(std::stop_token stop)
	{
		AMediaCodec_ptr codec;
		bool waiting_idr = true;

		while (not stop.stop_requested())
		{
			std::deque<wivrn::to_headset::desktop_frame> todo;
			{
				std::unique_lock lock(mutex);
				cv.wait_for(lock, stop, std::chrono::milliseconds(5), [&] { return not frames.empty() or pending_reset or pending_stop; });
				if (stop.stop_requested())
					break;

				if (pending_stop or pending_reset)
				{
					// The old reader stays alive while the render thread holds its images
					codec.reset();
					reader.reset();
					pending_stop = false;
				}

				if (pending_reset)
				{
					auto [width, height] = *pending_reset;
					pending_reset.reset();
					frames.clear();
					waiting_idr = true;
					try
					{
						codec = create_codec(width, height);
					}
					catch (std::exception & e)
					{
						spdlog::error("Cannot create the desktop decoder: {}", e.what());
					}
				}

				todo.swap(frames);
			}

			if (not codec)
				continue;

			if (todo.size() > max_pending_frames)
				spdlog::warn("Desktop decoder is late by {} frames", todo.size());

			for (auto & frame: todo)
			{
				// Decoding can only start on a full frame
				if (waiting_idr and not frame.idr)
					continue;
				waiting_idr = false;
				queue_input(codec.get(), frame);
				drain_output(codec.get());
			}
			drain_output(codec.get());
		}
	}

	AMediaCodec_ptr create_codec(uint16_t width, uint16_t height)
	{
		AImageReader * ir;
		check(AImageReader_newWithUsage(
		              width,
		              height,
		              AIMAGE_FORMAT_PRIVATE,
		              AHARDWAREBUFFER_USAGE_CPU_READ_NEVER | AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
		              4,
		              &ir),
		      "AImageReader_newWithUsage");
		std::shared_ptr<AImageReader> new_reader(ir, AImageReader_deleter{});

		AMediaFormat_ptr format(AMediaFormat_new());
		AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, "video/avc");
		AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, width);
		AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, height);
		AMediaFormat_setInt32(format.get(), "low-latency", 1);

		AMediaCodec_ptr codec(AMediaCodec_createDecoderByType("video/avc"));
		if (not codec)
			throw std::runtime_error("No H.264 decoder");

		ANativeWindow * window;
		check(AImageReader_getWindow(new_reader.get(), &window), "AImageReader_getWindow");
		check(AMediaCodec_configure(codec.get(), format.get(), window, nullptr, 0), "AMediaCodec_configure");
		check(AMediaCodec_start(codec.get()), "AMediaCodec_start");

		spdlog::info("Desktop decoder created for {}x{}", width, height);
		reader = std::move(new_reader);
		return codec;
	}

	void queue_input(AMediaCodec * codec, const wivrn::to_headset::desktop_frame & frame)
	{
		ssize_t idx = -1;
		// Wait for an input buffer, the decoder frees them as it outputs images
		for (int attempt = 0; attempt < 50 and idx < 0; ++attempt)
		{
			idx = AMediaCodec_dequeueInputBuffer(codec, 10'000);
			if (idx < 0)
				drain_output(codec);
		}
		if (idx < 0)
		{
			spdlog::warn("Desktop decoder: no input buffer, frame {} dropped", frame.frame_idx);
			return;
		}

		size_t capacity;
		uint8_t * buffer = AMediaCodec_getInputBuffer(codec, idx, &capacity);
		size_t size = std::min(capacity, frame.data.size());
		if (size < frame.data.size())
			spdlog::warn("Desktop frame larger than the decoder buffer");
		std::memcpy(buffer, frame.data.data(), size);
		AMediaCodec_queueInputBuffer(codec, idx, 0, size, frame.frame_idx * 1000, 0);
	}

	void drain_output(AMediaCodec * codec)
	{
		AMediaCodecBufferInfo info;
		ssize_t idx;
		while ((idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 0)) != AMEDIACODEC_INFO_TRY_AGAIN_LATER)
		{
			if (idx >= 0)
				AMediaCodec_releaseOutputBuffer(codec, idx, true); // to the image reader
		}
	}

	// Render thread
	std::shared_ptr<mapped_buffer> map(AHardwareBuffer * buffer)
	{
		if (auto it = mapped.find(buffer); it != mapped.end())
			return it->second;

		vk::AndroidHardwareBufferFormatPropertiesANDROID format_properties;
		vk::AndroidHardwareBufferPropertiesANDROID properties{.pNext = &format_properties};
		if (vk::Result(vkGetAndroidHardwareBufferPropertiesANDROID(*device, buffer, reinterpret_cast<VkAndroidHardwareBufferPropertiesANDROID *>(&properties))) != vk::Result::eSuccess)
			throw std::runtime_error("vkGetAndroidHardwareBufferPropertiesANDROID failed");

		if (format_properties.externalFormat != external_format or not *pipeline)
		{
			try
			{
				create_pipeline(format_properties);
			}
			catch (...)
			{
				// Retry from scratch on the next frame
				pipeline = nullptr;
				external_format = 0;
				throw;
			}
		}

		AHardwareBuffer_Desc desc{};
		AHardwareBuffer_describe(buffer, &desc);

		vk::StructureChain image_info{
		        vk::ImageCreateInfo{
		                .imageType = vk::ImageType::e2D,
		                .format = vk::Format::eUndefined,
		                .extent = {desc.width, desc.height, 1},
		                .mipLevels = 1,
		                .arrayLayers = 1,
		                .samples = vk::SampleCountFlagBits::e1,
		                .tiling = vk::ImageTiling::eOptimal,
		                .usage = vk::ImageUsageFlagBits::eSampled,
		                .sharingMode = vk::SharingMode::eExclusive,
		                .initialLayout = vk::ImageLayout::eUndefined,
		        },
		        vk::ExternalMemoryImageCreateInfo{
		                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID,
		        },
		        vk::ExternalFormatANDROID{
		                .externalFormat = format_properties.externalFormat,
		        },
		};

		auto result = std::make_shared<mapped_buffer>();
		result->image = vk::raii::Image(device, image_info.get());

		vk::StructureChain memory_info{
		        vk::MemoryAllocateInfo{
		                .allocationSize = properties.allocationSize,
		                .memoryTypeIndex = uint32_t(ffs(properties.memoryTypeBits) - 1),
		        },
		        vk::MemoryDedicatedAllocateInfo{
		                .image = *result->image,
		        },
		        vk::ImportAndroidHardwareBufferInfoANDROID{
		                .buffer = buffer,
		        },
		};
		result->memory = vk::raii::DeviceMemory(device, memory_info.get());
		result->image.bindMemory(*result->memory, 0);

		vk::StructureChain view_info{
		        vk::ImageViewCreateInfo{
		                .image = *result->image,
		                .viewType = vk::ImageViewType::e2D,
		                .format = vk::Format::eUndefined,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 1,
		                },
		        },
		        vk::SamplerYcbcrConversionInfo{
		                .conversion = *conversion,
		        },
		};
		result->view = vk::raii::ImageView(device, view_info.get());

		mapped[buffer] = result;
		return result;
	}

	void create_pipeline(const vk::AndroidHardwareBufferFormatPropertiesANDROID & format)
	{
		if (in_flight)
		{
			(void)device.waitForFences(*fence, true, UINT64_MAX);
			in_flight = false;
			in_flight_image.reset();
		}
		mapped.clear();
		pipeline = nullptr;
		pipeline_layout = nullptr;
		ds = nullptr;
		ds_pool = nullptr;
		ds_layout = nullptr;
		sampler = nullptr;
		conversion = nullptr;
		external_format = format.externalFormat;

		vk::Filter filter = (format.formatFeatures & vk::FormatFeatureFlagBits::eSampledImageYcbcrConversionLinearFilter) ? vk::Filter::eLinear : vk::Filter::eNearest;

		// The server encodes limited range BT.709
		vk::StructureChain conversion_info{
		        vk::SamplerYcbcrConversionCreateInfo{
		                .format = vk::Format::eUndefined,
		                .ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr709,
		                .ycbcrRange = vk::SamplerYcbcrRange::eItuNarrow,
		                .components = format.samplerYcbcrConversionComponents,
		                .xChromaOffset = format.suggestedXChromaOffset,
		                .yChromaOffset = format.suggestedYChromaOffset,
		                .chromaFilter = filter,
		        },
		        vk::ExternalFormatANDROID{
		                .externalFormat = format.externalFormat,
		        },
		};
		conversion = vk::raii::SamplerYcbcrConversion(device, conversion_info.get<vk::SamplerYcbcrConversionCreateInfo>());

		vk::StructureChain sampler_info{
		        vk::SamplerCreateInfo{
		                .magFilter = filter,
		                .minFilter = filter,
		                .mipmapMode = vk::SamplerMipmapMode::eNearest,
		                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
		                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
		                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
		                .maxAnisotropy = 1,
		                .borderColor = vk::BorderColor::eFloatOpaqueBlack,
		        },
		        vk::SamplerYcbcrConversionInfo{
		                .conversion = *conversion,
		        },
		};
		sampler = vk::raii::Sampler(device, sampler_info.get());

		vk::DescriptorSetLayoutBinding binding{
		        .binding = 0,
		        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		        .descriptorCount = 1,
		        .stageFlags = vk::ShaderStageFlagBits::eFragment,
		        .pImmutableSamplers = &*sampler,
		};
		ds_layout = vk::raii::DescriptorSetLayout(device, vk::DescriptorSetLayoutCreateInfo{.bindingCount = 1, .pBindings = &binding});

		// YCbCr formats can use several descriptors per sampler
		vk::DescriptorPoolSize pool_size{.type = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 4};
		ds_pool = vk::raii::DescriptorPool(device, vk::DescriptorPoolCreateInfo{
		                                                   .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
		                                                   .maxSets = 1,
		                                                   .poolSizeCount = 1,
		                                                   .pPoolSizes = &pool_size,
		                                           });
		ds = std::move(device.allocateDescriptorSets({
		        .descriptorPool = *ds_pool,
		        .descriptorSetCount = 1,
		        .pSetLayouts = &*ds_layout,
		})[0]);

		vk::PushConstantRange push_constant{
		        .stageFlags = vk::ShaderStageFlagBits::eVertex,
		        .offset = 0,
		        .size = sizeof(float) * 2,
		};
		pipeline_layout = vk::raii::PipelineLayout(device, vk::PipelineLayoutCreateInfo{
		                                                           .setLayoutCount = 1,
		                                                           .pSetLayouts = &*ds_layout,
		                                                           .pushConstantRangeCount = 1,
		                                                           .pPushConstantRanges = &push_constant,
		                                                   });

		auto vertex_shader = load_shader(device, "desktop_yuv.vert");
		auto fragment_shader = load_shader(device, "desktop_yuv.frag");
		std::array stages{
		        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex, .module = **vertex_shader, .pName = "main"},
		        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment, .module = **fragment_shader, .pName = "main"},
		};
		vk::PipelineVertexInputStateCreateInfo vertex_input{};
		vk::PipelineInputAssemblyStateCreateInfo input_assembly{.topology = vk::PrimitiveTopology::eTriangleList};
		vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
		vk::PipelineRasterizationStateCreateInfo rasterization{.polygonMode = vk::PolygonMode::eFill, .cullMode = vk::CullModeFlagBits::eNone, .lineWidth = 1};
		vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples = vk::SampleCountFlagBits::e1};
		vk::PipelineColorBlendAttachmentState blend_attachment{
		        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
		};
		vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1, .pAttachments = &blend_attachment};
		std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount = dynamic_states.size(), .pDynamicStates = dynamic_states.data()};

		pipeline = vk::raii::Pipeline(device, nullptr, vk::GraphicsPipelineCreateInfo{
		                                                       .stageCount = stages.size(),
		                                                       .pStages = stages.data(),
		                                                       .pVertexInputState = &vertex_input,
		                                                       .pInputAssemblyState = &input_assembly,
		                                                       .pViewportState = &viewport,
		                                                       .pRasterizationState = &rasterization,
		                                                       .pMultisampleState = &multisample,
		                                                       .pColorBlendState = &blend,
		                                                       .pDynamicState = &dynamic,
		                                                       .layout = *pipeline_layout,
		                                                       .renderPass = *renderpass,
		                                                       .subpass = 0,
		                                               });
	}

	void ensure_target(vk::Extent2D extent)
	{
		if (extent == target_extent and *target)
			return;

		if (texture)
		{
			// The GUI may still use the old image
			queue.lock()->waitIdle();
			textures.free_texture(texture);
			texture = 0;
		}
		framebuffer = nullptr;
		target_view_srgb = nullptr;
		target_view_unorm = nullptr;
		target = nullptr;
		target_memory = nullptr;
		target_extent = extent;
		has_frame = false;

		// Mutable format: rendered as UNORM, sampled as sRGB
		target = vk::raii::Image(device, vk::ImageCreateInfo{
		                                         .flags = vk::ImageCreateFlagBits::eMutableFormat,
		                                         .imageType = vk::ImageType::e2D,
		                                         .format = vk::Format::eR8G8B8A8Unorm,
		                                         .extent = {extent.width, extent.height, 1},
		                                         .mipLevels = 1,
		                                         .arrayLayers = 1,
		                                         .samples = vk::SampleCountFlagBits::e1,
		                                         .tiling = vk::ImageTiling::eOptimal,
		                                         .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		                                         .sharingMode = vk::SharingMode::eExclusive,
		                                         .initialLayout = vk::ImageLayout::eUndefined,
		                                 });

		auto requirements = target.getMemoryRequirements();
		auto memory_properties = physical_device.getMemoryProperties();
		uint32_t type = 0;
		for (; type < memory_properties.memoryTypeCount; ++type)
		{
			if ((requirements.memoryTypeBits & (1 << type)) and (memory_properties.memoryTypes[type].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal))
				break;
		}
		target_memory = vk::raii::DeviceMemory(device, vk::MemoryAllocateInfo{.allocationSize = requirements.size, .memoryTypeIndex = type});
		target.bindMemory(*target_memory, 0);

		vk::ImageViewCreateInfo view_info{
		        .image = *target,
		        .viewType = vk::ImageViewType::e2D,
		        .format = vk::Format::eR8G8B8A8Unorm,
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .layerCount = 1,
		        },
		};
		target_view_unorm = vk::raii::ImageView(device, view_info);
		// The shader writes gamma encoded values, sampling as sRGB decodes them for the GUI
		view_info.format = vk::Format::eR8G8B8A8Srgb;
		target_view_srgb = vk::raii::ImageView(device, view_info);

		framebuffer = vk::raii::Framebuffer(device, vk::FramebufferCreateInfo{
		                                                    .renderPass = *renderpass,
		                                                    .attachmentCount = 1,
		                                                    .pAttachments = &*target_view_unorm,
		                                                    .width = extent.width,
		                                                    .height = extent.height,
		                                                    .layers = 1,
		                                            });

		texture = textures.add_texture(*target_view_srgb);
	}

	ImTextureID update()
	{
		std::shared_ptr<AImageReader> current_reader;
		{
			std::lock_guard lock(mutex);
			current_reader = reader;
		}

		if (current_reader != mapped_reader)
		{
			// New decoder: forget the buffers of the old one
			if (in_flight)
			{
				(void)device.waitForFences(*fence, true, UINT64_MAX);
				in_flight = false;
			}
			in_flight_image.reset();
			mapped.clear();
			mapped_reader = current_reader;
		}

		if (not current_reader)
			return has_frame ? texture : 0;

		if (in_flight)
		{
			if (fence.getStatus() != vk::Result::eSuccess)
				return has_frame ? texture : 0;
			in_flight = false;
			in_flight_image.reset();
		}

		AImage * image_ptr = nullptr;
		if (AImageReader_acquireLatestImage(current_reader.get(), &image_ptr) != AMEDIA_OK or not image_ptr)
			return has_frame ? texture : 0;

		held_image image{current_reader, AImage_ptr(image_ptr)};

		try
		{
			AHardwareBuffer * buffer;
			check(AImage_getHardwareBuffer(image.image.get(), &buffer), "AImage_getHardwareBuffer");
			AImageCropRect crop{};
			check(AImage_getCropRect(image.image.get(), &crop), "AImage_getCropRect");
			AHardwareBuffer_Desc desc{};
			AHardwareBuffer_describe(buffer, &desc);

			auto source = map(buffer);
			if (not *pipeline)
				throw std::runtime_error("no conversion pipeline");
			vk::Extent2D extent{uint32_t(crop.right - crop.left), uint32_t(crop.bottom - crop.top)};
			if (extent.width == 0 or extent.height == 0)
				extent = {desc.width, desc.height};
			ensure_target(extent);

			vk::DescriptorImageInfo image_info{
			        .imageView = *source->view,
			        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			};
			device.updateDescriptorSets(vk::WriteDescriptorSet{
			                                    .dstSet = *ds,
			                                    .dstBinding = 0,
			                                    .descriptorCount = 1,
			                                    .descriptorType = vk::DescriptorType::eCombinedImageSampler,
			                                    .pImageInfo = &image_info,
			                            },
			                            nullptr);

			command_buffer.reset();
			command_buffer.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

			if (source->layout != vk::ImageLayout::eShaderReadOnlyOptimal)
			{
				command_buffer.pipelineBarrier(
				        vk::PipelineStageFlagBits::eTopOfPipe,
				        vk::PipelineStageFlagBits::eFragmentShader,
				        {},
				        nullptr,
				        nullptr,
				        vk::ImageMemoryBarrier{
				                .srcAccessMask = {},
				                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
				                .oldLayout = source->layout,
				                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
				                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
				                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
				                .image = *source->image,
				                .subresourceRange = {
				                        .aspectMask = vk::ImageAspectFlagBits::eColor,
				                        .levelCount = 1,
				                        .layerCount = 1,
				                },
				        });
				source->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
			}

			command_buffer.beginRenderPass(vk::RenderPassBeginInfo{
			                                       .renderPass = *renderpass,
			                                       .framebuffer = *framebuffer,
			                                       .renderArea = {.extent = extent},
			                               },
			                               vk::SubpassContents::eInline);
			command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
			command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, *ds, nullptr);
			command_buffer.setViewport(0, vk::Viewport{.width = float(extent.width), .height = float(extent.height), .minDepth = 0, .maxDepth = 1});
			command_buffer.setScissor(0, vk::Rect2D{.extent = extent});
			std::array<float, 2> uv_scale{float(extent.width) / desc.width, float(extent.height) / desc.height};
			command_buffer.pushConstants<std::array<float, 2>>(*pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, uv_scale);
			command_buffer.draw(3, 1, 0, 0);
			command_buffer.endRenderPass();
			command_buffer.end();

			device.resetFences(*fence);
			queue.lock()->submit(vk::SubmitInfo{
			                             .commandBufferCount = 1,
			                             .pCommandBuffers = &*command_buffer,
			                     },
			                     *fence);
			in_flight = true;
			in_flight_image = std::move(image);
			has_frame = true;
		}
		catch (std::exception & e)
		{
			spdlog::error("Desktop view: {}", e.what());
		}

		return has_frame ? texture : 0;
	}
};

desktop_view::desktop_view(vk::raii::Device & device, vk::raii::PhysicalDevice & physical_device, thread_safe<vk::raii::Queue> & queue, uint32_t queue_family_index, imgui_textures & textures) :
        d(std::make_unique<impl>(device, physical_device, queue, queue_family_index, textures))
{
}

bool desktop_view::supported()
{
	return true;
}

void desktop_view::reset(uint16_t width, uint16_t height)
{
	std::lock_guard lock(d->mutex);
	d->pending_reset = {width, height};
	d->cv.notify_all();
}

void desktop_view::push_frame(wivrn::to_headset::desktop_frame && frame)
{
	std::lock_guard lock(d->mutex);
	d->frames.push_back(std::move(frame));
	d->cv.notify_all();
}

void desktop_view::stop()
{
	std::lock_guard lock(d->mutex);
	d->frames.clear();
	d->pending_reset.reset();
	d->pending_stop = true;
	d->cv.notify_all();
}

ImTextureID desktop_view::update()
{
	return d->update();
}

ImVec2 desktop_view::size() const
{
	return ImVec2(d->target_extent.width, d->target_extent.height);
}

#else

// No hardware decoding outside Android: the workspace falls back to the trackpad
struct desktop_view::impl
{};

desktop_view::desktop_view(vk::raii::Device &, vk::raii::PhysicalDevice &, thread_safe<vk::raii::Queue> &, uint32_t, imgui_textures &)
{
}

bool desktop_view::supported()
{
	return false;
}

void desktop_view::reset(uint16_t, uint16_t) {}
void desktop_view::push_frame(wivrn::to_headset::desktop_frame &&) {}
void desktop_view::stop() {}

ImTextureID desktop_view::update()
{
	return 0;
}

ImVec2 desktop_view::size() const
{
	return {};
}

#endif

desktop_view::~desktop_view() = default;
