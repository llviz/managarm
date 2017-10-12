
#ifndef DRIVERS_GFX_VIRTIO_VIRTIO_HPP
#define DRIVERS_GFX_VIRTIO_VIRTIO_HPP

#include <queue>
#include <map>
#include <unordered_map>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/jump.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <core/virtio/core.hpp>
#include <helix/ipc.hpp>

#include "spec.hpp"

struct GfxDevice : drm_core::Device, std::enable_shared_from_this<GfxDevice> {
	struct FrameBuffer;

	struct Configuration : drm_core::Configuration {
		Configuration(GfxDevice *device)
		: _device(device), _width(0), _height(0), _fb(nullptr) { };
		
		bool capture(std::vector<drm_core::Assignment> assignment) override;
		void dispose() override;
		void commit() override;
		
	private:
		cofiber::no_future _dispatch();
		
		GfxDevice *_device;
		int _width;
		int _height;
		GfxDevice::FrameBuffer *_fb;
		std::shared_ptr<drm_core::Blob> _mode;
	};

	struct Plane : drm_core::Plane {
		Plane(GfxDevice *device);
	};
	
	struct BufferObject : drm_core::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(GfxDevice *device, uint32_t id, size_t size, helix::UniqueDescriptor memory,
			uint32_t width, uint32_t height)
		: _device{device}, _size{size}, _hardwareId{id}, _memory{std::move(memory)},
			_width{width}, _height{height} { };
		
		std::shared_ptr<drm_core::BufferObject> sharedBufferObject() override;
		size_t getSize() override;
		std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() override;
		uint32_t getWidth();
		uint32_t getHeight();
		cofiber::no_future _initHw();
		async::result<void> wait();
		uint32_t hardwareId();
	
	private:
		GfxDevice *_device;
		uint32_t _hardwareId;
		size_t _size;
		helix::UniqueDescriptor _memory;
		uint32_t _width;
		uint32_t _height;
		async::jump _jump;
	};

	struct Connector : drm_core::Connector {
		Connector(GfxDevice *device);
	};

	struct Encoder : drm_core::Encoder {
		Encoder(GfxDevice *device);
	};
	
	struct Crtc : drm_core::Crtc {
		Crtc(GfxDevice *device, std::shared_ptr<Plane> plane);
		
		drm_core::Plane *primaryPlane() override;
		// cursorPlane
	
	private:	
		GfxDevice *_device;
		std::shared_ptr<Plane> _primaryPlane;
	};

	struct FrameBuffer : drm_core::FrameBuffer {
		FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo);

		GfxDevice::BufferObject *getBufferObject();

	private:
		std::shared_ptr<GfxDevice::BufferObject> _bo;
	};

	GfxDevice(std::unique_ptr<virtio_core::Transport> transport);
	
	cofiber::no_future initialize();
	std::unique_ptr<drm_core::Configuration> createConfiguration() override;
	std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> createDumb(uint32_t width,
			uint32_t height, uint32_t bpp) override;
	std::shared_ptr<drm_core::FrameBuffer> 
			createFrameBuffer(std::shared_ptr<drm_core::BufferObject> bo,
			uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) override;

private:
	std::shared_ptr<Crtc> _theCrtcs[16];
	std::shared_ptr<Encoder> _theEncoders[16];
	std::shared_ptr<Connector> _activeConnectors[16];

	std::unique_ptr<virtio_core::Transport> _transport;
	virtio_core::Queue *_controlQ;
	virtio_core::Queue *_cursorQ;
	uint32_t _numScanouts;
	id_allocator<uint32_t> _hwAllocator;
};

#endif // DRIVERS_GFX_VIRTIO_VIRTIO_HPP

