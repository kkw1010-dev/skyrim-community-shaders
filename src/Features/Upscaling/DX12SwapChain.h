#pragma once

#include <Windows.Foundation.h>
#include <stdio.h>
#include <winrt/base.h>
#include <wrl\client.h>
#include <wrl\wrappers\corewrappers.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <string>

#include <directx/d3dx12.h>

#include "Utils/LazyShader.h"

class WrappedResource
{
public:
	// a_name follows Util::SetResourceName's "Feature::Name" convention; views get " SRV"/
	// " UAV"/" RTV" suffixes. Pass empty to leave resources unnamed.
	WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device, const std::string& a_name = {});
	~WrappedResource();

	ID3D11Texture2D* resource11 = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11UnorderedAccessView* uav = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	winrt::com_ptr<ID3D12Resource> resource;
};

/** @brief D3D12 fence shared into D3D11; value is the last value handed out by Next(). */
struct SharedFence
{
	static constexpr DWORD kRemovalPollMs = 100;
	/** @brief Longest a CPU wait may take before its caller treats the GPU work as stuck. */
	static constexpr DWORD kFenceTimeoutMs = 5000;
	winrt::com_ptr<ID3D12Fence> fence12;
	winrt::com_ptr<ID3D11Fence> fence11;
	uint64_t value = 0;

	/** @brief How a CPU wait ended. */
	enum class WaitOutcome : uint8_t
	{
		kComplete,  ///< The fence reached the value, or there was nothing to wait for.
		kTimeout,   ///< The value did not complete within the timeout.
		kFailed     ///< The wait could not be set up, or the device was removed.
	};

	/** @brief Returns the next value to signal, advancing the monotonic counter. */
	uint64_t Next() { return ++value; }
	/** @brief Creates and names the fence; throws on failure without leaking the NT handle. */
	void Create(ID3D12Device* a_device12, ID3D11Device5* a_device11, const char* a_name);
	/** @brief Releases both views of the fence and restarts the counter, for an owner tearing its device down. */
	void Reset()
	{
		fence11 = nullptr;
		fence12 = nullptr;
		value = 0;
	}
	/** @brief Waits on the CPU up to a_timeoutMs, polling device removal via fence12's own device.
	 *  Trivially true when the fence is unset or a_value is 0 (nothing to wait for). */
	bool CpuWait(uint64_t a_value, DWORD a_timeoutMs) const;
	/**
	 * @brief CpuWait that reports why it ended. a_error, when given, receives the Win32 error or the
	 *        device-removed HRESULT behind a kFailed, and 0 otherwise.
	 */
	WaitOutcome CpuWaitOutcome(uint64_t a_value, DWORD a_timeoutMs, DWORD* a_error = nullptr) const;
};

struct DXGISwapChainProxy : IDXGISwapChain
{
public:
	DXGISwapChainProxy(IDXGISwapChain4* a_swapChain);

	IDXGISwapChain4* swapChain;

	/****IUnknown****/
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
	virtual ULONG STDMETHODCALLTYPE AddRef() override;
	virtual ULONG STDMETHODCALLTYPE Release() override;

	/****IDXGIObject****/
	virtual HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown) override;
	virtual HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent) override;

	/****IDXGIDeviceSubObject****/
	virtual HRESULT STDMETHODCALLTYPE GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice) override;

	/****IDXGISwapChain****/
	virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags);
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface);
	virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget);
	virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget);
	virtual HRESULT STDMETHODCALLTYPE GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc);
	virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
	virtual HRESULT STDMETHODCALLTYPE ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters);
	virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput);
	virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats);
	virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(_Out_ UINT* pLastPresentCount);
};

class DX12SwapChain
{
public:
	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;

	// 4x DLSS-G generates 3 interpolated frames per real frame; the SL pacer needs those
	// plus 2 slots of its own slack to avoid oversubscribing the flip-model chain.
	static constexpr UINT kMaxBackBuffers = 5;

	winrt::com_ptr<ID3D12CommandAllocator> commandAllocators[kMaxBackBuffers];
	winrt::com_ptr<ID3D12GraphicsCommandList4> commandLists[kMaxBackBuffers];

	IDXGISwapChain4* swapChain;

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;

	WrappedResource* swapChainBufferWrapped;
	WrappedResource* uiBufferWrapped;
	// DLSS-G only: the scene before UI composition, tagged as HUDLessColor.
	WrappedResource* hudlessBufferWrapped = nullptr;
	Util::LazyShader<ID3D11ComputeShader> composeUICS;

	// D3D12 interop resources for frame generation
	WrappedResource* depthBufferShared12 = nullptr;
	WrappedResource* motionVectorBufferShared12 = nullptr;

	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;

	SharedFence interopFence;

	winrt::com_ptr<ID3D12Resource> swapChainBuffers[kMaxBackBuffers];

	UINT frameIndex = 0;

	UINT64 frameFenceValues[kMaxBackBuffers] = {};

	LARGE_INTEGER qpf;

	double refreshRate = 0;

	DXGISwapChainProxy* swapChainProxy = nullptr;

	bool useDLSSG = false;

	// Actual buffer count the live swap chain was created/resized with (<= kMaxBackBuffers).
	UINT backBufferCount = 2;

	// Returns the current frame time (in seconds) for accurate FPS calculation when frame generation is active
	float GetFrameTime() const;

	void CreateD3D12Device(IDXGIAdapter* a_adapter);
	void CreateSwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateSwapChainDirect(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);

	void CreateInterop();
	void RecreateWrappedResources(const DXGI_SWAP_CHAIN_DESC1& desc);

	DXGISwapChainProxy* GetSwapChainProxy();
	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);

	HRESULT GetBuffer(UINT buffer, REFIID riid, void** ppSurface);
	/** @brief IDXGISwapChain::ResizeBuffers equivalent; rejects any bufferCount differing from the chain's own backBufferCount. */
	HRESULT ResizeBuffers(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags);
	HRESULT Present(UINT SyncInterval, UINT Flags);
	/**
	 * @brief DLSS-G: keeps the UI-less scene in hudlessBufferWrapped and composites the UI
	 * buffer over the back buffer (premultiplied alpha). Returns false if it could not run.
	 */
	bool ComposeDLSSGFrame();
	/** @brief True when ComposeDLSSGFrame can run (buffers exist and its shader compiled). */
	bool CanComposeDLSSGFrame();
	/** @brief Writes the HUD-less, UI and final DLSS-G inputs as PNGs next to the plugin log. */
	void DumpDLSSGInputs();
	uint32_t dlssgComposedFrames = 0;
	/** @brief Writes one DLSS-G input as a PNG next to the plugin log; false, logged, on failure. */
	bool WriteInputPng(WrappedResource* a_resource, const std::wstring& a_fileName, bool a_keepAlpha);
	/** @brief Test aid: the next composed frame's HUD-less image is written as CommunityShaders-NRTest-<tag>-HUDless.png. */
	void RequestTestDump(std::wstring a_tag) { testDumpTag = std::move(a_tag); }
	std::wstring testDumpTag;
	HRESULT GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice);
	HANDLE GetFrameLatencyWaitableObject();

	void SetColorSpace(bool enableHDR);

	// Resources needed by BackgroundBlur when D3D12 swap chain is active
	struct BlurResources
	{
		ID3D11Texture2D* backbufferTex = nullptr;
		ID3D11RenderTargetView* backbufferRTV = nullptr;
		ID3D11ShaderResourceView* backbufferSRV = nullptr;
		ID3D11ShaderResourceView* uiBufferSRV = nullptr;
		ID3D11RenderTargetView* uiBufferRTV = nullptr;
	};

	// Get all resources needed for background blur in one call
	BlurResources GetBlurResources() const;

	// D3D12 interop resource management
	void CreateSharedResources();
};
