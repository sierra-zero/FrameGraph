// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'

#pragma once

#include "framegraph/Public/FrameGraph.h"
#include "VResourceManager.h"
#include "VDevice.h"
#include "VCmdBatch.h"
#include "VDebugger.h"

namespace FG
{

	//
	// Vulkan Frame Graph instance
	//

	class VFrameGraph final : public IFrameGraph
	{
	// types
	private:
		enum class EState : uint
		{
			Initial,
			Initialization,
			Idle,
			Destroyed,
		};
		
		using EBatchState		= VCmdBatch::EState;
		using PerQueueSem_t		= StaticArray< VkSemaphore, uint(EQueueType::_Count) >;

		struct QueueData
		{
			VDeviceQueueInfoPtr			ptr;

			ExeOrderIndex				lastSubmitted	= ExeOrderIndex::First;
			ExeOrderIndex				lastCompleted	= ExeOrderIndex::First;

			PerQueueSem_t				semaphores		{};
			Array<VCmdBatchPtr>			pending;		// TODO: circular queue
			Array<VSubmittedPtr>		submitted;

			SpinLock					cmdPoolGuard;
			VkCommandPool				cmdPool			= VK_NULL_HANDLE;
			Array<VkImageMemoryBarrier>	imageBarriers;
		};

		using CmdBuffers_t		= FixedArray< UniquePtr<VCommandBuffer>, 32 >;
		using QueueMap_t		= StaticArray< QueueData, uint(EQueueType::_Count) >;
		using Fences_t			= Array< VkFence >;
		using Semaphores_t		= Array< VkSemaphore >;


	// variables
	private:
		std::atomic<EState>		_state;

		VDevice					_device;
		QueueMap_t				_queueMap;
		EQueueUsage				_queueUsage;

		std::recursive_mutex	_cmdBuffersGuard;
		CmdBuffers_t			_cmdBuffers;

		VResourceManager		_resourceMngr;
		VDebugger				_debugger;

		ShaderDebugCallback_t	_shaderDebugCallback;

		Fences_t				_fenceCache;
		Semaphores_t			_semaphoreCache;


	// methods
	public:
		explicit VFrameGraph (const VulkanDeviceInfo &);
		~VFrameGraph ();

		// initialization //
		bool			Initialize ();
		void			Deinitialize () override;
		bool			AddPipelineCompiler (const PipelineCompiler &comp) override;
		//void			SetCompilationFlags (ECompilationFlags flags, ECompilationDebugFlags debugFlags) override;
		bool			SetShaderDebugCallback (ShaderDebugCallback_t &&) override;
		DeviceInfo_t	GetDeviceInfo () const override;


		// resource manager //
		MPipelineID		CreatePipeline (INOUT MeshPipelineDesc &desc, StringView dbgName) override;
		RTPipelineID	CreatePipeline (INOUT RayTracingPipelineDesc &desc) override;
		GPipelineID		CreatePipeline (INOUT GraphicsPipelineDesc &desc, StringView dbgName) override;
		CPipelineID		CreatePipeline (INOUT ComputePipelineDesc &desc, StringView dbgName) override;
		ImageID			CreateImage (const ImageDesc &desc, const MemoryDesc &mem, StringView dbgName) override;
		BufferID		CreateBuffer (const BufferDesc &desc, const MemoryDesc &mem, StringView dbgName) override;
		ImageID			CreateImage (const ExternalImageDesc_t &desc, OnExternalImageReleased_t &&, StringView dbgName) override;
		BufferID		CreateBuffer (const ExternalBufferDesc_t &desc, OnExternalBufferReleased_t &&, StringView dbgName) override;
		SamplerID		CreateSampler (const SamplerDesc &desc, StringView dbgName) override;
		SwapchainID		CreateSwapchain (const SwapchainCreateInfo_t &, RawSwapchainID oldSwapchain, StringView dbgName) override;
		RTGeometryID	CreateRayTracingGeometry (const RayTracingGeometryDesc &desc, const MemoryDesc &mem, StringView dbgName) override;
		RTSceneID		CreateRayTracingScene (const RayTracingSceneDesc &desc, const MemoryDesc &mem, StringView dbgName) override;
		RTShaderTableID	CreateRayTracingShaderTable (StringView dbgName) override;
		bool			InitPipelineResources (RawGPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const override;
		bool			InitPipelineResources (RawCPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const override;
		bool			InitPipelineResources (RawMPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const override;
		bool			InitPipelineResources (RawRTPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const override;
		bool			CachePipelineResources (INOUT PipelineResources &resources) override;
		void			ReleaseResource (INOUT PipelineResources &resources) override;
		void			ReleaseResource (INOUT GPipelineID &id) override;
		void			ReleaseResource (INOUT CPipelineID &id) override;
		void			ReleaseResource (INOUT MPipelineID &id) override;
		void			ReleaseResource (INOUT RTPipelineID &id) override;
		void			ReleaseResource (INOUT ImageID &id) override;
		void			ReleaseResource (INOUT BufferID &id) override;
		void			ReleaseResource (INOUT SamplerID &id) override;
		void			ReleaseResource (INOUT SwapchainID &id) override;
		void			ReleaseResource (INOUT RTGeometryID &id) override;
		void			ReleaseResource (INOUT RTSceneID &id) override;
		void			ReleaseResource (INOUT RTShaderTableID &id) override;

		BufferDesc const&	GetDescription (RawBufferID id) const override;
		ImageDesc const&	GetDescription (RawImageID id) const override;
		
		bool			UpdateHostBuffer (RawBufferID id, BytesU offset, BytesU size, const void *data) override;
		bool			MapBufferRange (RawBufferID id, BytesU offset, INOUT BytesU &size, OUT void* &data) override;


		// frame execution //
		CommandBuffer	Begin (const CommandBufferDesc &, ArrayView<CommandBuffer> dependsOn) override;
		bool			Execute (INOUT CommandBuffer &) override;
		bool			Wait (ArrayView<CommandBuffer> commands, Nanoseconds timeout) override;
		bool			Flush () override;
		bool			WaitIdle () override;


		// debugging //
		bool			GetStatistics (OUT Statistics &result) const override;
		bool			DumpToString (OUT String &result) const override;
		bool			DumpToGraphViz (OUT String &result) const override;

		
		ND_ VDeviceQueueInfoPtr	FindQueue (EQueueType type) const;
		ND_ VDevice const&		GetDevice ()				const	{ return _device; }
		ND_ VResourceManager &	GetResourceManager ()				{ return _resourceMngr; }


	private:
		// resource manager //
		template <typename Desc, typename ID>
		ND_ Desc const&  _GetDescription (const ID &id) const;
		
		template <typename PplnID>
		bool _InitPipelineResources (const PplnID &pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const;

		template <typename ID>
		void _ReleaseResource (INOUT ID &id);
		
		void _TransitImageLayoutToDefault (RawImageID imageId, VkImageLayout initialLayout, uint queueFamily);

		ND_ VkFence		_CreateFence ();
		ND_ VkSemaphore	_CreateSemaphore ();


		// queues //
		ND_ EQueueFamilyMask	_GetQueuesMask (EQueueUsage types) const;
			bool				_IsUnique (VDeviceQueueInfoPtr ptr) const;
			bool				_AddGraphicsQueue ();
			bool				_AddAsyncComputeQueue ();
			bool				_AddAsyncTransferQueue ();
			bool				_CreateQueue (EQueueType, VDeviceQueueInfoPtr);

			bool				_TryFlush (const VCmdBatchPtr &batch);
			bool				_FlushAll (uint maxIter);
			bool				_FlushQueue (EQueueType queue, uint maxIter);
			bool				_WaitQueue (EQueueType queue, Nanoseconds timeout);


		// states //
		ND_ bool	_IsInitialized () const;
		ND_ EState	_GetState () const;
		ND_ bool	_SetState (EState expected, EState newState);
	};


}	// FG
