#pragma once

#include "../../Buffer.h"
#include "../../State.h"

#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>

#define NV_WINDOWS

#pragma warning(push)
#pragma warning(disable: 4471)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)

class Streamline
{
public:
	Streamline() = default;

	inline std::string GetShortName() { return "Streamline"; }

	bool enabledAtBoot = false;
	static constexpr UINT kNvidiaVendorId = 0x10DE;

	// Configure before calling LoadInterposer(). DX12 instance uses a separate plugin
	// directory and interposer DLL so the two SDK states are fully independent per-process.
	sl::RenderAPI renderAPI = sl::RenderAPI::eD3D11;
	std::wstring pluginDir = L"Data\\Shaders\\Upscaling\\Streamline";
	std::wstring interposerDllName = L"sl.interposer.dll";
	std::string instanceTag = "DX11";

	bool initialized = false;
	bool triedInitialization = false;

	bool featureDLSS = false;
	bool featureDLSSG = false;
	bool dlssgResourcesRetained = false;
	// Upper bound for DLSSGOptions::numFramesToGenerate (DX12 instance only). 1 = 2x-only.
	// A query before the swap chain exists can under-report it, so it is re-read later.
	uint32_t dlssgMaxFramesToGenerate = 1;
	bool dlssgMaxQueriedAfterPresent = false;
	bool dlssgLoggedEnabled = false;
	uint32_t dlssgLoggedFramesToGenerate = 0;
	uint32_t dlssgLoggedMaxFramesToGenerate = 0;
	bool dlssgLoggedUIRecomposition = false;
	// Last slDLSSGGetState results, cached for GetDiagnostics (querying there would
	// steal the since-last-query frame counter from the present path).
	sl::DLSSGStatus lastDLSSGStatus = sl::DLSSGStatus::eOk;
	uint32_t lastDLSSGFramesPresented = 0;
	bool featureReflex = false;
	bool featurePCL = false;
	bool reflexSupportedOnCurrentAdapter = false;

	sl::ViewportHandle viewport{ 0 };
	sl::ViewportHandle viewportRight{ 1 };
	static constexpr uint32_t MAX_RESOLUTION = 8192;
	HMODULE interposer = NULL;

	// SL Interposer Functions
	PFun_slInit* slInit{};
	PFun_slShutdown* slShutdown{};
	PFun_slIsFeatureSupported* slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};
	PFun_slSetFeatureLoaded* slSetFeatureLoaded{};
	PFun_slEvaluateFeature* slEvaluateFeature{};
	PFun_slAllocateResources* slAllocateResources{};
	PFun_slFreeResources* slFreeResources{};
	PFun_slSetTag* slSetTag{};
	PFun_slSetTagForFrame* slSetTagForFrame{};
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};
	PFun_slGetFeatureVersion* slGetFeatureVersion{};
	PFun_slUpgradeInterface* slUpgradeInterface{};
	PFun_slSetConstants* slSetConstants{};
	PFun_slGetNativeInterface* slGetNativeInterface{};
	PFun_slGetFeatureFunction* slGetFeatureFunction{};
	PFun_slGetNewFrameToken* slGetNewFrameToken{};
	PFun_slSetD3DDevice* slSetD3DDevice{};

	// DLSS specific functions (DX11 instance)
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};
	PFun_slDLSSGetState* slDLSSGetState{};
	PFun_slDLSSSetOptions* slDLSSSetOptions{};

	// DLSS-G specific functions (DX12 instance)
	PFun_slDLSSGGetState* slDLSSGGetState{};
	PFun_slDLSSGSetOptions* slDLSSGSetOptions{};

	// Reflex specific functions (DX11 instance)
	PFun_slReflexGetState* slReflexGetState{};
	PFun_slReflexSleep* slReflexSleep{};
	PFun_slReflexSetOptions* slReflexSetOptions{};
	PFun_slPCLSetMarker* slPCLSetMarker{};

	Util::FrameChecker frameChecker;
	sl::FrameToken* frameToken = nullptr;

	struct ReflexOptionsCache
	{
		bool valid = false;
		sl::ReflexMode mode = sl::ReflexMode::eOff;
		uint32_t frameLimitUs = 0;
		bool useMarkersToOptimize = false;
	};
	ReflexOptionsCache reflexOptionsCache{};
	uint32_t lastReflexSleepFrame = UINT32_MAX;

	// Helper: Execute DLSS for a single viewport with given resources
	void EvaluateDLSS(sl::ViewportHandle vp, uint32_t eyeIndex,
		ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
		ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
		const sl::Extent& extentIn, const sl::Extent& extentOut, uint32_t outputWidth);

	// Cached DLL version info for Streamline plugin directory
	static std::vector<std::pair<std::string, std::string>> dllVersions;

	void LoadInterposer();

	void CheckFeatures(IDXGIAdapter* a_adapter);

	// Bind a DX12 device to this Streamline instance (DX12 instance only).
	void SetD3DDevice12(ID3D12Device* a_device);

	/**
	 * @brief Resets the driver-profile DRS key that silently disables DLSS-G for this
	 * executable (status stays eOk with zero interpolated frames when it is set). Must
	 * run at plugin load: the driver latches the profile early in process life, so a
	 * reset after device creation only takes effect on the next launch.
	 */
	static void EnsureDriverProfileAllowsDLSSG();

	/** @brief Reads the driver's "Smooth Motion" DRS setting (see NvApiDrs::kKeySmoothMotionEnable) for this executable; not a Streamline feature. */
	static bool IsSmoothMotionEnabledForProfile();

	/** @brief Binds DLSS and Reflex feature functions after the D3D device is created. */
	void PostDevice();

	/** @brief Resolves one feature function pointer, logging on failure. */
	bool BindFeatureFunction(sl::Feature a_feature, const char* a_functionName, void*& a_function);
	/** @brief Requests a feature be marked loaded, logging on failure. */
	void RequestFeatureLoad(sl::Feature a_feature, const char* a_featureName);
	/** @brief Binds Reflex/PCL functions and updates their availability flags. */
	void BindReflexAndPCL();

	// DLSS-G frame generation methods (DX12 instance only)
	/**
	 * @brief Configures DLSS-G and retains its resources across temporary pauses.
	 * @param uiRecomposition Hudless and UI color/alpha were both tagged this frame.
	 */
	void ConfigureDLSSG(bool enabled, bool uiRecomposition);
	/** @brief Re-reads DLSSGState::numFramesToGenerateMax, logging when it changes. */
	void RefreshDLSSGMaxFrames(const char* a_when);
	/**
	 * @brief Emits a PCL latency marker for the current frame token. The marker's frame
	 * index is how DLSS-G's pacer matches presents to constants -- structural for FG.
	 */
	void EmitPCLMarker(sl::PCLMarker a_marker);

	/** @brief Reflex's latency over the frames it last reported, up to 64. */
	struct ReflexLatency
	{
		uint32_t frames = 0;
		double pcLatencyMs = 0.0;  ///< Mean from simulation start to the end of the GPU's render: PC latency without the display.
		double gpuFrameMs = 0.0;   ///< Mean time between GPU render ends: the rendered frame interval as the GPU saw it.
	};
	/**
	 * @brief Reads Reflex's frame reports, which the PCL markers feed. False while it has none (Reflex
	 *        unavailable, or no reports yet). Not thread safe, as slReflexGetState is not.
	 */
	bool SampleReflexLatency(ReflexLatency& a_latency);
	void TagDX12Resources(ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* depth, ID3D12Resource* mvec, ID3D12Resource* hudLessColor,
		ID3D12Resource* uiColorAndAlpha, uint32_t width, uint32_t height);

	/** @brief Acquires a new frame token from Streamline for the current frame. */
	bool EnsureFrameToken();
	bool CheckFrameConstants(sl::ViewportHandle p_viewport, uint32_t eyeIndex = 0);

	void SetDLSSOptions(sl::ViewportHandle p_viewport, uint32_t width);

	void Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors);
	void UpdateReflex();

	void DestroyDLSSResources();
};
