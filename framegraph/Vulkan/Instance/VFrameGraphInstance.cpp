// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'

#include "VFrameGraphInstance.h"
#include "VFrameGraphThread.h"

namespace FG
{

/*
=================================================
	constructor
=================================================
*/
	VFrameGraphInstance::VFrameGraphInstance (const VulkanDeviceInfo &vdi) :
		_device{ vdi },
		_submissionGraph{ _device },
		_resourceMngr{ _device },
		_defaultCompilationFlags{ Default },
		_defaultDebugFlags{ Default }
	{
		SCOPELOCK( _rcCheck );
		_threads.reserve( 32 );
	}
	
/*
=================================================
	destructor
=================================================
*/
	VFrameGraphInstance::~VFrameGraphInstance ()
	{
		SCOPELOCK( _rcCheck );
		ASSERT( not _IsInitialized() );
		CHECK( _GetState() == EState::Destroyed );
		CHECK( _threads.empty() );
	}
	
/*
=================================================
	CreateThread
=================================================
*/
	FGThreadPtr  VFrameGraphInstance::CreateThread (const ThreadDesc &desc)
	{
		SCOPELOCK( _rcCheck );
		CHECK_ERR( _IsInitialized() );

		EThreadUsage	usage = desc.usage;

		// validate usage
		if ( EnumEq( usage, EThreadUsage::AsyncStreaming ) )
			usage |= (EThreadUsage::Transfer | EThreadUsage::MemAllocation);

		if ( EnumEq( usage, EThreadUsage::Transfer ) )
			usage |= EThreadUsage::MemAllocation;

		// create thread
		SharedPtr<VFrameGraphThread>	thread{ new VFrameGraphThread{ *this, usage, desc.name }};

		for (auto& comp : _pplnCompilers) {
			thread->AddPipelineCompiler( comp );
		}

		thread->SetCompilationFlags( _defaultCompilationFlags, _defaultDebugFlags );

		SCOPELOCK( _threadLock );
		_threads.push_back( thread );

		return thread;
	}
	
/*
=================================================
	_IsInitialized
=================================================
*/
	inline bool  VFrameGraphInstance::_IsInitialized () const
	{
		return _ringBufferSize > 0;
	}

/*
=================================================
	Initialize
=================================================
*/
	bool  VFrameGraphInstance::Initialize (uint ringBufferSize)
	{
		SCOPELOCK( _rcCheck );
		CHECK_ERR( not _IsInitialized() );

		_ringBufferSize	= ringBufferSize;
		_frameId		= 0;

		CHECK_ERR( _submissionGraph.Initialize( ringBufferSize ));
		CHECK_ERR( _resourceMngr.Initialize( ringBufferSize ));
		
		CHECK_ERR( _SetState( EState::Initial, EState::Idle ));
		return true;
	}
	
/*
=================================================
	Deinitialize
=================================================
*/
	void  VFrameGraphInstance::Deinitialize ()
	{
		SCOPELOCK( _rcCheck );
		CHECK( _IsInitialized() );
		
		// checks if all threads was destroyed
		{
			SCOPELOCK( _threadLock );
			for (auto iter = _threads.begin(); iter != _threads.end(); ++iter)
			{
				auto	thread = iter->lock();
				CHECK( not thread );
			}
		}

		CHECK( _WaitIdle() );
		CHECK( _SetState( EState::Idle, EState::Destroyed ));

		_threads.clear();
		_ringBufferSize = 0;

		_submissionGraph.Deinitialize();
		_resourceMngr.Deinitialize();
	}

/*
=================================================
	AddPipelineCompiler
=================================================
*/
	bool  VFrameGraphInstance::AddPipelineCompiler (const IPipelineCompilerPtr &comp)
	{
		SCOPELOCK( _rcCheck );
		CHECK_ERR( _GetState() == EState::Idle );

		if ( not _pplnCompilers.insert( comp ).second )
			return true;
		
		SCOPELOCK( _threadLock );
		for (auto iter = _threads.begin(); iter != _threads.end(); ++iter)
		{
			auto	thread = iter->lock();
			if ( thread ) {
				thread->AddPipelineCompiler( comp );
			}
		}

		return true;
	}
	
/*
=================================================
	SetCompilationFlags
=================================================
*/
	void  VFrameGraphInstance::SetCompilationFlags (ECompilationFlags flags, ECompilationDebugFlags debugFlags)
	{
		SCOPELOCK( _rcCheck );

		_defaultCompilationFlags	= flags;
		_defaultDebugFlags			= debugFlags;
	}
	
/*
=================================================
	BeginFrame
=================================================
*/
	bool  VFrameGraphInstance::BeginFrame (const SubmissionGraph &graph)
	{
		SCOPELOCK( _rcCheck );
		CHECK_ERR( _SetState( EState::Idle, EState::Begin ));

		_frameId	= (_frameId + 1) % _ringBufferSize;
		_statistics	= Default;

		CHECK_ERR( _submissionGraph.WaitFences( _frameId ));
		CHECK_ERR( _submissionGraph.Recreate( _frameId, graph ));

		// begin thread execution
		{
			SCOPELOCK( _threadLock );
			for (auto iter = _threads.begin(); iter != _threads.end();)
			{
				auto	thread = iter->lock();

				if ( not thread or thread->IsDestroyed() ) {
					iter = _threads.erase( iter );
					continue;
				}

				CHECK( thread->SyncOnBegin( &_submissionGraph ));
				++iter;
			}
		}


		_debugger.OnBeginFrame( graph );
		_resourceMngr.OnBeginFrame( _frameId );

		CHECK_ERR( _SetState( EState::Begin, EState::RunThreads ));
		return true;
	}
	
/*
=================================================
	SkipBatch
----
	thread safe and lock-free
=================================================
*/
	bool  VFrameGraphInstance::SkipBatch (const CommandBatchID &batchId, uint indexInBatch)
	{
		CHECK_ERR( _GetState() == EState::RunThreads );

		CHECK_ERR( _submissionGraph.SkipSubBatch( batchId, indexInBatch ));
		return true;
	}
	
/*
=================================================
	SubmitBatch
----
	thread safe and wait-free
=================================================
*/
	bool  VFrameGraphInstance::SubmitBatch (const CommandBatchID &batchId, uint indexInBatch, const ExternalCmdBatch_t &data)
	{
		CHECK_ERR( _GetState() == EState::RunThreads );

		auto*	batch_data = UnionGetIf<VulkanCommandBatch>( &data );
		CHECK_ERR( batch_data );

		for (auto& sem : batch_data->signalSemaphores) {
			CHECK( _submissionGraph.SignalSemaphore( batchId, BitCast<VkSemaphore>(sem) ));
		}

		for (auto& wait_sem : batch_data->waitSemaphores) {
			CHECK( _submissionGraph.WaitSemaphore( batchId, BitCast<VkSemaphore>(wait_sem.first), BitCast<VkPipelineStageFlags>(wait_sem.second) ));
		}

		VDeviceQueueInfoPtr		queue_ptr = null;

		for (auto& queue : _device.GetVkQueues())
		{
			if ( queue.handle == BitCast<VkQueue>(batch_data->queue) )
				queue_ptr = &queue;
		}
		CHECK_ERR( queue_ptr );

		CHECK_ERR( _submissionGraph.Submit( queue_ptr, batchId, indexInBatch,
											ArrayView{ Cast<VkCommandBuffer>(batch_data->commands.data()), batch_data->commands.size() }
					));
		return true;
	}

/*
=================================================
	EndFrame
=================================================
*/
	bool  VFrameGraphInstance::EndFrame ()
	{
		SCOPELOCK( _rcCheck );
		CHECK_ERR( _SetState( EState::RunThreads, EState::End ));

		CHECK_ERR( _submissionGraph.IsAllBatchesSubmitted() );

		// complete thread execution
		{
			SCOPELOCK( _threadLock );
			for (auto iter = _threads.begin(); iter != _threads.end();)
			{
				auto	thread = iter->lock();

				if ( not thread or thread->IsDestroyed() ) {
					iter = _threads.erase( iter );
					continue;
				}

				CHECK( thread->SyncOnExecute( INOUT _statistics ));
				++iter;
			}
		}

		_resourceMngr.OnEndFrame();
		_debugger.OnEndFrame();
		
		CHECK_ERR( _SetState( EState::End, EState::Idle ));
		return true;
	}

/*
=================================================
	WaitIdle
=================================================
*/
	bool  VFrameGraphInstance::WaitIdle ()
	{
		SCOPELOCK( _rcCheck );
		CHECK_ERR( _GetState() == EState::Idle );

		return _WaitIdle();
	}

	bool  VFrameGraphInstance::_WaitIdle ()
	{
		SCOPELOCK( _threadLock );

		for (uint i = 0; i < _ringBufferSize; ++i)
		{
			const uint	frame_id = (_frameId + i) % _ringBufferSize;
			
			CHECK_ERR( _submissionGraph.WaitFences( frame_id ));

			for (auto iter = _threads.begin(); iter != _threads.end();)
			{
				auto	thread = iter->lock();
				
				if ( not thread or thread->IsDestroyed() ) {
					iter = _threads.erase( iter );
					continue;
				}

				CHECK( thread->OnWaitIdle() );
				++iter;
			}
		}
		return true;
	}

/*
=================================================
	GetStatistics
=================================================
*/
	bool  VFrameGraphInstance::GetStatistics (OUT Statistics &result) const
	{
		SHAREDLOCK( _rcCheck );
		CHECK_ERR( _GetState() == EState::Idle );
		
		result = _statistics;
		return true;
	}
	
/*
=================================================
	DumpToString
=================================================
*/
	bool  VFrameGraphInstance::DumpToString (OUT String &result) const
	{
		SHAREDLOCK( _rcCheck );
		CHECK_ERR( _GetState() == EState::Idle );

		_debugger.GetFrameDump( OUT result );
		return true;
	}
	
/*
=================================================
	DumpToGraphViz
=================================================
*/
	bool  VFrameGraphInstance::DumpToGraphViz (OUT String &result) const
	{
		SHAREDLOCK( _rcCheck );
		CHECK_ERR( _GetState() == EState::Idle );

		_debugger.GetGraphDump( OUT result );
		return true;
	}
	
/*
=================================================
	_GetState / _SetState
=================================================
*/
	inline VFrameGraphInstance::EState  VFrameGraphInstance::_GetState () const
	{
		return _state.load( memory_order_acquire );
	}

	inline void  VFrameGraphInstance::_SetState (EState newState)
	{
		_state.store( newState, memory_order_release );
	}

	inline bool  VFrameGraphInstance::_SetState (EState expected, EState newState)
	{
		return _state.compare_exchange_strong( INOUT expected, newState, memory_order_release, memory_order_relaxed );
	}


}	// FG