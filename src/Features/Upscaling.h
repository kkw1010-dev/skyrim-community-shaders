#pragma once

#include "Feature.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/RCAS/RCAS.h"
#include "Upscaling/NeuralRendering.h"
#include "Upscaling/Streamline.h"
#include <chrono>
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

/**
 * @brief Provides upscaling functionality including DLSS, FSR and TAA.
 *
 * This feature handles various upscaling methods and frame generation technologies
 * to improve performance while maintaining visual quality.
 */
struct Upscaling : Feature
{
private:
	static constexpr std::string_view MOD_ID = "156952";

public:
	// Feature interface
	virtual inline std::string GetName() override { return "Upscaling"; }
	virtual std::string GetDisplayName() override { return T("feature.upscaling.name", "Upscaling"); }
	virtual inline std::string GetShortName() override { return "Upscaling"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline bool SupportsVR() override { return true; }
	virtual inline bool IsCore() const override { return false; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.upscaling.description", "Advanced upscaling and frame generation technologies for improved performance"),
			{ T("feature.upscaling.key_feature_1", "DLSS (Deep Learning Super Sampling) support"),
				T("feature.upscaling.key_feature_2", "FSR (FidelityFX Super Resolution) support"),
				T("feature.upscaling.key_feature_3", "TAA (Temporal Anti-Aliasing) support"),
				T("feature.upscaling.key_feature_4", "Frame generation for supported systems") } };
	};

	float2 jitter = { 0, 0 };

	enum class FrameGenMethod
	{
		kNone,
		kFSR,
		kDLSSG
	};

	enum class UpscaleMethod
	{
		kNONE,
		kTAA,
		kFSR,
		kDLSS
	};

	struct Settings
	{
		uint upscaleMethod = (uint)UpscaleMethod::kDLSS;
		uint upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
		uint qualityMode = 1;  // Default to Quality (1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance, 0=Native AA)
		uint frameLimitMode = 1;
		uint frameGenerationMode = 1;
		uint frameGenerationForceEnable = 0;
		bool frameGenerationAllowInMenus = false;
		// DLSS-G is opt-in; off keeps AMD FSR frame generation.
		bool enableDLSSFrameGen = false;
		// Generated frames per real frame (1=2x). Clamped at apply time to the
		// hardware-reported DLSSGState::numFramesToGenerateMax.
		uint dlssgFramesToGenerate = 1;
		uint streamlineLogLevel = 0;  // 0=Off, 1=Default, 2=Verbose
		float sharpnessFSR = 0.0f;
		float sharpnessDLSS = 0.0f;
		uint presetDLSS = 0;  // 0=Default, 1=J, 2=K, 3=L, 4=M
		bool reflexLowLatencyMode = false;
		bool reflexLowLatencyBoost = false;
		bool reflexUseMarkersToOptimize = false;
		bool reflexUseFPSLimit = false;
		float reflexFPSLimit = 60.0f;
		// DLSS Neural Rendering (NGX Feature 18), ported from Open Shaders; off until the user enables it.
		bool neuralRenderingEnabled = false;
		// Where NR runs: 0 before upscaling (Open Shaders' placement), 1 on the final image (NeuralRendering::Placement).
		// Default: the final image, the user's choice after run set 7.
		uint32_t neuralRenderingPlacement = 1;
		// Share of NR's edit shown, 0 to 2: above 1 extrapolates it.
		float neuralRenderingMix = 1.0f;
		NR::Context::Profiles neuralRenderingContexts;
		NR::Tuning neuralRenderingTuning;
		// Test aid: frames of the once-per-session before/after NR capture (0 = off, at most 8).
		uint32_t neuralRenderingTestCaptureFrames = 0;
		// Test aid: true keeps stage 1's unit white point for the NR proxy instead of the scene exposure.
		bool neuralRenderingUnitExposure = false;
		// Test aid: with the test capture on, NR switches itself on and off in turns of this many seconds (0 = off, at least 10).
		uint32_t neuralRenderingTestCycleSeconds = 0;
		// Test aid: the test cycle's NR turns alternate before upscaling and the final image.
		bool neuralRenderingTestCyclePlacements = false;
		// Test aid: the test cycle's NR turns step through run set 6's variants (skin structure, tone transfer, no depth).
		bool neuralRenderingTestCycleVariants = false;
		// Test aid: the test cycle's NR turns run on the final image at each of run set 7's working scales.
		bool neuralRenderingTestCycleScales = false;
		// Test aid: the test cycle's NR turns step through run set 8's mix x local tone / structure pairs on the final image.
		bool neuralRenderingTestCycleMixes = false;
		// Test aid: the test cycle steps through CS feature conditions (PP and lighting A/B), NR on then off in each.
		bool neuralRenderingTestCycleConditions = false;
	};

	/** @brief The saved NR placement, clamped to a known one. */
	NeuralRendering::Placement GetNeuralRenderingPlacement() const
	{
		return static_cast<NeuralRendering::Placement>(std::min(settings.neuralRenderingPlacement, NeuralRendering::kMaxPlacement));
	}

	Settings settings;

	struct JitterCB
	{
		float2 jitter;
		float useWideKernel;
		float pad0;
	};

	struct UpscalingDataCB
	{
		float2 trueSamplingDim;  // per-eye render dim in VR, full render dim otherwise
		uint eyeOffsetX;         // X offset into stereo source buffers; 0 for non-VR / left eye
		uint pad0;
	};

	ConstantBuffer* jitterCB = nullptr;
	ConstantBuffer* upscalingDataCB = nullptr;

	// Runtime state
	bool isWindowed = false;
	bool lowRefreshRate = false;
	bool fidelityFXMissing = false;
	bool d3d12SwapChainActive = false;
	bool frameGenerationPrepared = false;

	// Timing and scaling
	double refreshRate = 0.0f;
	float2 resolutionScale = { 1.0f, 1.0f };
	LARGE_INTEGER qpf;

	// FG FPS Measurement for Overlay
	bool IsFrameGenerationDx12PathActive() const;
	bool IsFrameGenerationActive() const;
	/** @brief Returns whether settings and menu state permit frame generation, before any hold. */
	bool FrameGenerationPermitted() const;
	/** @brief Returns whether settings and menu state permit preparing frame-generation inputs and no hold keeps it off. */
	bool ShouldPrepareFrameGeneration() const;
	/**
	 * @brief Keeps frame generation off for at least a_duration and kFrameGenerationHoldFrames post-processed frames
	 *        from now, so DLSS-G never comes back in the same frame as the Reflex markers it needs (F001).
	 * @param a_reason Logged when the hold starts; nullptr for the per-frame menu hold, which would flood the log.
	 */
	void HoldFrameGeneration(std::chrono::milliseconds a_duration, const char* a_reason);
	/** @brief True while a hold keeps frame generation off. */
	bool FrameGenerationHeld() const;
	/** @brief Once per frame, before frame generation is prepared: holds it after a loading screen and while a menu blocks it. */
	void UpdateFrameGenerationHold();
	/** @brief Shortest hold after a loading screen or a menu, in time and in frames: both must pass. */
	static constexpr std::chrono::milliseconds kFrameGenerationHold{ 1000 };
	static constexpr uint32_t kFrameGenerationHoldFrames = 30;
	/**
	 * @brief More presented frames than this between two post-processed ones are a loading screen (or another screen
	 *        without the world); a slow gameplay frame is still one frame, so a hitch does not start the hold.
	 */
	static constexpr uint32_t kFrameGenerationLoadingGapFrames = 5;
	/** @brief The hold lasts until this time and until frameGenerationHoldFrames more frames have been post-processed. */
	std::chrono::steady_clock::time_point frameGenerationHoldUntil{};
	uint32_t frameGenerationHoldFrames = 0;
	/** @brief Engine frame UpdateFrameGenerationHold last ran in; a gap in the count is a loading screen. */
	uint32_t frameGenerationHoldUpdatedFrame = UINT32_MAX;
	/** @brief Returns the prepared frame's generation decision until its buffers are cleared after Present. */
	bool ShouldUseFrameGenerationThisFrame() const;
	bool IsUpscalingActive() const;

	// Feature interface overrides
	virtual void DrawSettings() override;
	virtual void SaveSettings(json& o_json) override;
	virtual void LoadSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	/**
	 * @brief Installs Direct3D-related hooks for device and factory creation.
	 *
	 * Loads FidelityFX support and patches the import address table (IAT) to redirect D3D11 device and DXGI factory creation functions to custom hook implementations.
	**/
	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;
	/** @brief Propagates frame inactivity to NR temporal history. */
	virtual void Reset() override { neuralRendering.Reset(settings.neuralRenderingEnabled, settings.neuralRenderingTuning.regionOfInterest, settings.neuralRenderingTuning.regionFit, settings.neuralRenderingTuning.regionGroup); }
	/**
	 * @brief The Neural Rendering hotkey: flips the same setting as the panel's checkbox, effective on the
	 *        next frame. Switching on is refused, with a log line, while the runtime cannot load.
	 */
	void ToggleNeuralRendering();

	UpscaleMethod GetUpscaleMethod() const;
	FrameGenMethod GetFrameGenMethod() const;
	bool UsesDLSSGFrameGen() const;

	/** @brief Real:generated frame ratio for the active FG backend -- the single source
	 * of truth for pacing (FrameLimiter) and reporting (PerformanceOverlay). */
	uint GetFrameGenerationMultiplier() const;

	void CheckResources(UpscaleMethod a_upscalemethod);
	void CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod);
	void DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod);

	winrt::com_ptr<ID3D11ComputeShader> encodeTexturesCS[5];          // One for each UpscaleMethod
	winrt::com_ptr<ID3D11ComputeShader> encodeTexturesCSDepthOutput;  // FSR + VR: converts R24G8_TYPELESS depth to R32_FLOAT
	ID3D11ComputeShader* GetEncodeTexturesCS();

	/** @brief Encoder outputs a caller binds; selects the permutation. */
	enum class EncodeOutput : uint8_t
	{
		kMasksOnly,
		kTypedDepth,  // converts R24G8_TYPELESS depth to R32_FLOAT
		kCount
	};

	/** @brief Encoder permutations for callers outside the upscale pass (Neural Rendering's guides). */
	Util::LazyShader<ID3D11ComputeShader> encodeTexturesCSVariants[5][static_cast<size_t>(EncodeOutput::kCount)];
	ID3D11ComputeShader* GetEncodeTexturesCS(UpscaleMethod a_method, EncodeOutput a_output);

	/** @brief The encoder's four inputs in slot order: TAA mask, normals, motion vectors, depth. */
	using EncodeInputViews = std::array<ID3D11ShaderResourceView*, 4>;
	/** @brief Fills the encoder inputs; on failure names the missing one in a_missing. */
	bool GetEncodeInputs(EncodeInputViews& a_views, const char*& a_missing) const;

	winrt::com_ptr<ID3D11PixelShader> depthRefractionUpscalePS;
	ID3D11PixelShader* GetDepthRefractionUpscalePS();

	winrt::com_ptr<ID3D11PixelShader> underwaterMaskUpscalePS;
	ID3D11PixelShader* GetUnderwaterMaskUpscalePS();

	winrt::com_ptr<ID3D11VertexShader> upscaleVS;
	ID3D11VertexShader* GetUpscaleVS();

	winrt::com_ptr<ID3D11DepthStencilState> upscaleDepthStencilState;
	winrt::com_ptr<ID3D11BlendState> upscaleBlendState;
	winrt::com_ptr<ID3D11RasterizerState> upscaleRasterizerState;

	// Shared VR HMD Mask Clearing
	winrt::com_ptr<ID3D11ComputeShader> vrClearHMDMaskCS;
	winrt::com_ptr<ID3D11Buffer> vrClearHMDMaskCB;
	// Helper to dispatch mask clearing for a single eye region
	void ClearHMDMask(ID3D11UnorderedAccessView* colorUAV, ID3D11ShaderResourceView* depthSRV,
		uint32_t eyeWidth, uint32_t eyeHeight, uint32_t depthOffsetX, uint32_t colorOffsetX);

	// Shared VR Per-Eye Intermediate Buffers
	// Owned here so both Streamline (DLSS) and FidelityFX (FSR) can use them.
	eastl::unique_ptr<Texture2D> vrIntermediateColorIn[2];           // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateColorOut[2];          // per-eye output resolution
	eastl::unique_ptr<Texture2D> vrIntermediateDepth;                // right-eye render resolution (R24G8_TYPELESS, DLSS only)
	eastl::unique_ptr<Texture2D> vrIntermediateLinearDepth[2];       // per-eye render resolution (R32_FLOAT, for FSR)
	eastl::unique_ptr<Texture2D> vrIntermediateMotionVectors[2];     // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateReactiveMask[2];      // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateTransparencyMask[2];  // per-eye render resolution

	// Helper to create/resize per-eye buffers matching source formats
	void CreateVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);

	// Helper: Create a Texture2D matching source format at a given size
	static eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags = false, bool createSRV = false, bool createUAV = false, const char* name = nullptr);

	// Shared Pipeline Steps

	/// Ensures VR per-eye intermediate textures exist at the correct resolution.
	/// Must be called before any per-eye EncodeTexturesCS dispatch or PreparePerEyeInputs.
	void EnsureVRIntermediateTextures();

	/// Splits the combined stereo color buffer into per-eye intermediates, copies raw
	/// motion vectors, and clears the HMD hidden area. FSR-only.
	/// Reactive/transparency masks are written by EncodeTexturesCS.
	void PreparePerEyeInputs(ID3D11Resource* colorSrc);
	void FinalizePerEyeOutputs(ID3D11Resource* colorDst);

	void ConfigureTAA();
	void ConfigureUpscaling(RE::BSGraphics::State* a_state);
	void Upscale();

	// D3D11 textures
	Texture2D* reactiveMaskTexture = nullptr;
	Texture2D* transparencyCompositionMaskTexture = nullptr;
	Texture2D* motionVectorCopyTexture = nullptr;
	Texture2D* sharpenerTexture = nullptr;

	virtual void ClearShaderCache() override;

	// Static instances instead of singletons
	static inline Streamline streamline;      ///< DX11 instance: DLSS + Reflex + PCL
	static inline Streamline streamlineDX12;  ///< DX12 instance: DLSS-G frame generation
	static inline FidelityFX fidelityFX;      ///< AMD FSR frame generation
	static inline DX12SwapChain dx12SwapChain;
	static inline RCAS rcas;  ///< Standalone RCAS sharpening for DLSS

	NeuralRendering neuralRendering;  ///< DLSS Neural Rendering, before upscaling or on the final image (opt-in)

	winrt::com_ptr<ID3D11PixelShader> copyDepthToSharedBufferPS;

	float projectionPosScaleX = 0.0f;
	float projectionPosScaleY = 0.0f;

	float dynamicResolutionWidthRatio = 1.0f;
	float dynamicResolutionHeightRatio = 1.0f;

	bool previousUpscalingWasActive = false;
	bool depthUpscaleUseWideKernel = false;

	/// Set by MenuOpenCloseEventHandler when LoadingMenu closes (cell/worldspace transitions,
	/// initial load). Consumed at the start of Upscale() to force a one-frame DLSS feature
	/// rebuild — works around a VR-only persistent ~2-3ms GPU regression after worldspace
	/// loads that otherwise only clears when the user manually toggles DLSS/preset. VR+DLSS
	/// only; flat has no repro and per-eye extent asymmetry doesn't apply.
	std::atomic<bool> pendingDLSSReset{ false };

	/** @brief Copies depth and motion inputs, returning false if the required shaders are unavailable. */
	bool CopySharedD3D12Resources();
	void PostDisplay();
	void PerformUpscaling();
	void UpscaleDepth();

	/**
	 * @brief Applies RCAS sharpening to the main render target after DLSS upscaling.
	 *
	 * Runs in HDR space before tonemapping. Only called when DLSS is active and sharpness > 0.
	 */
	void ApplySharpening();

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter();

	static double GetRefreshRate(HWND a_window);

	// Unified interface methods - external code should use these instead of direct access
	void LoadUpscalingSDKs();  // Loads all SDKs at once
	HANDLE GetFrameLatencyWaitableObject() const;
	float GetFrameTime() const;

	// Backend interface methods
	bool IsBackendInitialized() const;
	void CheckBackendFeatures(IDXGIAdapter* adapter);
	void UpgradeBackendInterface(void** ppInterface);
	void SetBackendD3DDevice(ID3D11Device* device);
	void PostBackendDevice();

	// Module availability methods
	bool HasFrameGenModule() const;

	// Proxy interface methods
	void SetProxyD3D11Device(ID3D11Device* device);
	void SetProxyD3D11DeviceContext(ID3D11DeviceContext* context);
	void CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateProxySwapChainDirect(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateProxyInterop();
	IDXGISwapChain* GetProxySwapChain();

	using BlurResources = DX12SwapChain::BlurResources;

	// Get all D3D11 resources needed for background blur when D3D12 swap chain is active
	BlurResources GetBlurResources() const;

private:
	struct Main_UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetScissorRect
	{
		static void thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_RenderPrecipitation
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSFaceGenManager_UpdatePendingCustomizationTextures
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static bool Register();
	};
};
