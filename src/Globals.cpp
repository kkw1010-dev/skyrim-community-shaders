#include "Globals.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/GrassCollision.h"
#include "Features/GrassLighting.h"
#include "Features/HDRDisplay.h"
#include "Features/HairSpecular.h"
#include "Features/IBL.h"
#include "Features/InteriorSun.h"
#include "Features/InverseSquareLighting.h"
#include "Features/LODBlending.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PerformanceOverlay.h"
#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"
#include "Features/PseudoSunBounce.h"
#include "Features/RenderDoc.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpacePointLightShadows.h"
#include "Features/ScreenSpaceRayTracing.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Skin.h"
#include "Features/SkySync.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainHelper.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "Features/VR.h"
#include "Features/VanillaFresnel.h"
#include "Features/VolumetricLighting.h"
#include "Features/VolumetricShadows.h"
#include "Features/WaterEffects.h"
#include "Features/WeatherEditor.h"
#include "Features/WetnessEffects.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/Game.h"

namespace globals
{
	namespace d3d
	{
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		IDXGISwapChain* swapChain = nullptr;
	}

	namespace features
	{
		CloudShadows cloudShadows{};
		DynamicCubemaps dynamicCubemaps{};
		VolumetricShadows volumetricShadows{};
		ExtendedMaterials extendedMaterials{};
		GrassCollision grassCollision{};
		GrassLighting grassLighting{};
		IBL ibl{};
		LightLimitFix lightLimitFix{};
		LinearLighting linearLighting{};
		LODBlending lodBlending{};
		HairSpecular hairSpecular{};
		InteriorSun interiorSun{};
		InverseSquareLighting inverseSquareLighting{};
		PhysicalSky physicalSky{};
		ScreenSpaceGI screenSpaceGI{};
		ScreenSpacePointLightShadows screenSpacePointLightShadows{};
		ScreenSpaceRayTracing screenSpaceRayTracing{};
		ScreenSpaceShadows screenSpaceShadows{};
		Skylighting skylighting{};
		TerrainVariation terrainVariation{};
		SkySync skySync{};
		SubsurfaceScattering subsurfaceScattering{};
		TerrainBlending terrainBlending{};
		TerrainHelper terrainHelper{};
		TerrainShadows terrainShadows{};
		UnifiedWater unifiedWater{};
		VanillaFresnel vanillaFresnel{};
		VolumetricLighting volumetricLighting{};
		VR vr{};
		WaterEffects waterEffects{};
		PerformanceOverlay performanceOverlay{};
		WetnessEffects wetnessEffects{};
		ExtendedTranslucency extendedTranslucency{};
		Upscaling upscaling{};
		HDRDisplay hdrDisplay{};
		RenderDoc renderDoc{};
		ScreenshotFeature screenshotFeature{};
		WeatherEditor weatherEditor{};
		ExponentialHeightFog exponentialHeightFog{};
		TruePBR truePBR{};
		PostProcessing postProcessing{};
		Skin skin{};
		PseudoSunBounce pseudoSunBounce{};

		namespace llf
		{
		}
	}

	namespace game
	{
		RE::BSGraphics::RendererShadowState* shadowState = nullptr;
		RE::BSGraphics::State* graphicsState = nullptr;
		RE::BSGraphics::Renderer* renderer = nullptr;
		RE::BSShaderManager::State* smState = nullptr;
		RE::TES* tes = nullptr;
		bool isVR = false;
		RE::MemoryManager* memoryManager = nullptr;
		RE::INISettingCollection* iniSettingCollection = nullptr;
		RE::INIPrefSettingCollection* iniPrefSettingCollection = nullptr;
		RE::GameSettingCollection* gameSettingCollection = nullptr;
		float* cameraNear = nullptr;
		float* cameraFar = nullptr;
		float* deltaTime = nullptr;
		RE::BSUtilityShader* utilityShader = nullptr;
		RE::PlayerCharacter* player = nullptr;
		RE::Sky* sky = nullptr;
		RE::UI* ui = nullptr;
		RE::Calendar* calendar = nullptr;
		std::atomic<bool> quitGame{ false };

		RE::BSGraphics::PixelShader** currentPixelShader = nullptr;
		RE::BSGraphics::VertexShader** currentVertexShader = nullptr;
		REX::EnumSet<RE::BSGraphics::ShaderFlags, uint32_t>* stateUpdateFlags = nullptr;

		RE::Setting* bEnableLandFade = nullptr;
		RE::Setting* bShadowsOnGrass = nullptr;
		RE::Setting* shadowMaskQuarter = nullptr;

		REL::Relocation<ID3D11Buffer**> perFrame;
		REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> currentAccumulator;

		D3D11_MAPPED_SUBRESOURCE* mappedFrameBuffer = nullptr;
		FrameBufferCache frameBufferCached{};
	}

	static void RefreshTES()
	{
		if (auto tes = RE::TES::GetSingleton())
			game::tes = tes;
	}

	namespace rtti
	{
		REL::Relocation<const RE::NiRTTI*> NiIntegerExtraDataRTTI;
		REL::Relocation<const RE::NiRTTI*> BSLightingShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> BSEffectShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> BSWaterShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> NiParticleSystemRTTI;
		REL::Relocation<const RE::NiRTTI*> NiBillboardNodeRTTI;
		REL::Relocation<const RE::NiRTTI*> NiAlphaPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> NiSourceTextureRTTI;
	}

	State* state = nullptr;
	Deferred* deferred = nullptr;
	Menu* menu = nullptr;
	SIE::ShaderCache* shaderCache = nullptr;

	void OnInit()
	{
		shaderCache = &SIE::ShaderCache::Instance();
		state = State::GetSingleton();
		menu = Menu::GetSingleton();
		deferred = Deferred::GetSingleton();
	}

	void ReInit()
	{
		{
			using namespace game;

			shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			graphicsState = RE::BSGraphics::State::GetSingleton();
			renderer = RE::BSGraphics::Renderer::GetSingleton();
			smState = &RE::BSShaderManager::State::GetSingleton();
			isVR = REL::Module::IsVR();
			iniSettingCollection = RE::INISettingCollection::GetSingleton();
			iniPrefSettingCollection = RE::INIPrefSettingCollection::GetSingleton();
			gameSettingCollection = RE::GameSettingCollection::GetSingleton();
			RefreshTES();
			cameraNear = (float*)(REL::RelocationID(517032, 403540).address() + 0x40);
			cameraFar = (float*)(REL::RelocationID(517032, 403540).address() + 0x44);
			deltaTime = (float*)REL::RelocationID(523660, 410199).address();

			currentPixelShader = GET_INSTANCE_MEMBER_PTR(currentPixelShader, shadowState);
			currentVertexShader = GET_INSTANCE_MEMBER_PTR(currentVertexShader, shadowState);
			stateUpdateFlags = GET_INSTANCE_MEMBER_PTR(stateUpdateFlags, shadowState);

			ui = RE::UI::GetSingleton();
			calendar = RE::Calendar::GetSingleton();
			perFrame = { REL::RelocationID(524768, 411384) };

			currentAccumulator = { REL::RelocationID(527650, 414600) };
		}

		{
			using namespace rtti;
			NiIntegerExtraDataRTTI = { RE::NiIntegerExtraData::Ni_RTTI };
			BSLightingShaderPropertyRTTI = { RE::BSLightingShaderProperty::Ni_RTTI };
			BSEffectShaderPropertyRTTI = { RE::BSEffectShaderProperty::Ni_RTTI };
			BSWaterShaderPropertyRTTI = { RE::BSWaterShaderProperty::Ni_RTTI };
			NiParticleSystemRTTI = { RE::NiParticleSystem::Ni_RTTI };
			NiBillboardNodeRTTI = { RE::NiBillboardNode::Ni_RTTI };
			NiAlphaPropertyRTTI = { RE::NiAlphaProperty::Ni_RTTI };
			NiSourceTextureRTTI = { RE::NiSourceTexture::Ni_RTTI };
		}

		d3d::device = reinterpret_cast<ID3D11Device*>(game::renderer->GetRuntimeData().forwarder);
		d3d::context = reinterpret_cast<ID3D11DeviceContext*>(game::renderer->GetRuntimeData().context);
		d3d::swapChain = reinterpret_cast<IDXGISwapChain*>(game::renderer->GetRuntimeData().renderWindows->swapChain);
	}

	void OnDataLoaded()
	{
		using namespace game;
		RefreshTES();
		player = RE::PlayerCharacter::GetSingleton();
		tes = RE::TES::GetSingleton();
		sky = RE::Sky::GetSingleton();
		utilityShader = RE::BSUtilityShader::GetSingleton();

		bEnableLandFade = iniSettingCollection->GetSetting("bEnableLandFade:Display");

		bShadowsOnGrass = RE::GetINISetting("bShadowsOnGrass:Display");
		shadowMaskQuarter = RE::GetINISetting("iShadowMaskQuarter:Display");
	}

	void OnGameWindowClose()
	{
		game::quitGame = true;
		if (shaderCache)
			shaderCache->StopCompilation();
	}

	/**
 * @brief Caches the current frame buffer data and clears the mapped pointer.
 *
 * Copies the contents of the mapped frame buffer into an internal cache and resets the mapped frame buffer pointer.
 */
	void CacheFramebuffer()
	{
		using namespace game;
		if (REL::Module::IsVR()) {
			auto frameBufferVR = (FrameBufferVR*)mappedFrameBuffer->pData;
			frameBufferCached.vr = *frameBufferVR;
		} else {
			auto frameBuffer = (FrameBuffer*)mappedFrameBuffer->pData;
			frameBufferCached.nonVR = *frameBuffer;
		}
		mappedFrameBuffer = nullptr;
	}

	/**
 * @brief Hooks the ID3D11DeviceContext::Map method to track mapping of the per-frame resource.
 *
 * Calls the original Map function and, if the mapped resource matches the current per-frame buffer, stores the mapped subresource pointer for later use.
 *
 * @return HRESULT Result of the original Map call.
 */
	struct ID3D11DeviceContext_Map
	{
		static HRESULT thunk(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource)
		{
			HRESULT hr = func(This, pResource, Subresource, MapType, MapFlags, pMappedResource);
			if (hr == S_OK) {
				if (*globals::game::perFrame.get() == pResource)
					globals::game::mappedFrameBuffer = pMappedResource;
			}
			return hr;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
 * @brief Hooked implementation of ID3D11DeviceContext::Unmap that caches the frame buffer if applicable.
 *
 * If the resource being unmapped matches the current per-frame buffer and a mapped frame buffer is present, caches the frame buffer data before calling the original Unmap function.
 */
	struct ID3D11DeviceContext_Unmap
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Subresource)
		{
			if (*globals::game::perFrame.get() == pResource && globals::game::mappedFrameBuffer) {
				CacheFramebuffer();
			}
			func(This, pResource, Subresource);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief Hooked OMSetRenderTargets — injects POM offset UAV at slot 7 when in the deferred pass.
	 *
	 * vtable index 33 for ID3D11DeviceContext::OMSetRenderTargets.
	 * After Skyrim binds the deferred MRT (clearing all UAVs), this hook re-adds the POM offset
	 * UAV at slot u7 so the Lighting PS (VR_STEREO_OPT permutation) can write per-pixel parallax
	 * depth offsets without overloading Reflectance.w.
	 */
	struct ID3D11DeviceContext_OMSetRenderTargets
	{
		static void STDMETHODCALLTYPE thunk(ID3D11DeviceContext* This, UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView)
		{
			func(This, NumViews, ppRenderTargetViews, pDepthStencilView);

			// D3D11 handles any SRV/UAV conflict automatically (silently unbinds the UAV when
			// the same resource is later bound as an SRV), so no NumViews guard is needed.
			if (globals::deferred->deferredPass) {
				auto& stereoOpt = globals::features::vr.stereoOpt;
				if (stereoOpt.loaded) {
					if (auto* uav = stereoOpt.GetPomOffsetUAV()) {
						This->OMSetRenderTargetsAndUnorderedAccessViews(
							D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
							7, 1, &uav, nullptr);
					}
				}
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief Hooked OMSetDepthStencilState — replaces DSS with stencil-enforcing version when VR stereo opt is active.
	 *
	 * vtable index 36 for ID3D11DeviceContext::OMSetDepthStencilState.
	 * When VRStereoOptimizations has written stencil marks, this hook transparently swaps
	 * the game's DSS for a modified version that adds a stencil NOT_EQUAL test, causing
	 * marked Eye 1 pixels to be skipped during normal rendering.
	 */
	struct ID3D11DeviceContext_OMSetDepthStencilState
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef)
		{
			if (globals::game::isVR) {
				auto& stereoOpt = globals::features::vr.stereoOpt;
				if (stereoOpt.loaded && stereoOpt.IsStencilActive()) {
					pDepthStencilState = stereoOpt.GetOrCreateModifiedDSS(pDepthStencilState);
					stereoOpt.NoteStencilSwap();
					StencilRef = 1;  // Must match the ref written by our stencil pass
				}
			}
			func(This, pDepthStencilState, StencilRef);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief Hooked ClearDepthStencilView — blocks stencil clears when VR stereo opt stencil is active.
	 *
	 * vtable index 53 for ID3D11DeviceContext::ClearDepthStencilView.
	 * Prevents the game from clearing our stencil marks between the stencil write and
	 * the stereo overwrite blend pass by stripping the D3D11_CLEAR_STENCIL flag.
	 */
	struct ID3D11DeviceContext_ClearDepthStencilView
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
		{
			if (globals::game::isVR) {
				auto& stereoOpt = globals::features::vr.stereoOpt;
				if (stereoOpt.loaded && stereoOpt.IsStencilActive()) {
					// Only protect the main scene DSV — allow other DSVs to clear normally
					auto renderer = globals::game::renderer;
					auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
					if (mainDepth.views[0]) {
						// Compare the DSV being cleared against the main scene DSV
						ID3D11Resource* clearRes = nullptr;
						ID3D11Resource* mainRes = nullptr;
						pDepthStencilView->GetResource(&clearRes);
						mainDepth.views[0]->GetResource(&mainRes);
						bool isMainDSV = (clearRes == mainRes);
						if (clearRes)
							clearRes->Release();
						if (mainRes)
							mainRes->Release();
						if (isMainDSV) {
							ClearFlags &= ~D3D11_CLEAR_STENCIL;
							if (ClearFlags == 0)
								return;
						}
					}
				}
			}
			func(This, pDepthStencilView, ClearFlags, Depth, Stencil);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief Watches GetData without changing it (vtable index 29), so a GPU that stops answering is logged (F002).
	 *
	 * The engine waits for each frame's event query with GetData in a 1 ms sleep loop that never gives up and
	 * treats an error like "not ready" (SkyrimSE.exe 77271). After a GPU fault it spins there forever with no
	 * crash, no TDR and no log line. This logs, once, the first device-loss result with the device's removed
	 * reason, and the first kMaxStallReports polls of one query that is still unfinished after kStallMs, with
	 * the perf events CS began last. The result is always returned as the runtime gave it.
	 */
	struct ID3D11DeviceContext_GetData
	{
		static constexpr ULONGLONG kStallMs = 2000;
		static constexpr uint32_t kMaxStallReports = 3;

		static HRESULT STDMETHODCALLTYPE thunk(ID3D11DeviceContext* This, ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT GetDataFlags)
		{
			const HRESULT hr = func(This, pAsync, pData, DataSize, GetDataFlags);
			// Per thread: the query this thread last saw unfinished, since when, and whether that wait was reported.
			thread_local ID3D11Asynchronous* pending = nullptr;
			thread_local ULONGLONG pendingSince = 0;
			thread_local bool pendingReported = false;
			if (hr == S_FALSE) {
				const ULONGLONG now = GetTickCount64();
				if (pAsync != pending) {
					pending = pAsync;
					pendingSince = now;
					pendingReported = false;
				} else if (!pendingReported && now - pendingSince >= kStallMs) {
					pendingReported = true;
					ReportStall(pAsync, GetDataFlags, now - pendingSince);
				}
			} else {
				pending = nullptr;
				if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
					ReportDeviceLoss(hr);
			}
			return hr;
		}
		static inline REL::Relocation<decltype(thunk)> func;

	private:
		static inline std::atomic<uint32_t> stallReports{ 0 };
		static inline std::atomic_bool deviceLossReported{ false };

		static HRESULT RemovedReason()
		{
			return globals::d3d::device ? globals::d3d::device->GetDeviceRemovedReason() : E_POINTER;
		}

		static void ReportStall(ID3D11Asynchronous* a_async, UINT a_flags, ULONGLONG a_waitedMs)
		{
			const auto report = stallReports.fetch_add(1);
			if (report >= kMaxStallReports)
				return;
			std::string kind = "not a query";
			winrt::com_ptr<ID3D11Query> query;
			if (a_async && SUCCEEDED(a_async->QueryInterface(IID_PPV_ARGS(query.put())))) {
				D3D11_QUERY_DESC desc{};
				query->GetDesc(&desc);
				kind = std::format("{} ({})", magic_enum::enum_name(desc.Query), static_cast<int>(desc.Query));
			}
			logger::error("[GPU watch] GetData on thread {} has waited {} ms for one {} (flags {:#x}); device removed reason {:#010x}, frame {}{}. "
						  "CS perf events begun last (the GPU may be a few frames behind them):{}",
				GetCurrentThreadId(), a_waitedMs, kind, a_flags, static_cast<uint32_t>(RemovedReason()),
				globals::state ? globals::state->frameCount : 0u, report + 1 == kMaxStallReports ? "; later stalls are not logged" : "",
				globals::state ? globals::state->DescribeRecentPerfEvents() : std::string(" none"));
		}

		static void ReportDeviceLoss(HRESULT a_result)
		{
			if (deviceLossReported.exchange(true))
				return;
			logger::error("[GPU watch] GetData returned {:#010x} on thread {}; device removed reason {:#010x}, frame {}. CS perf events begun last:{}",
				static_cast<uint32_t>(a_result), GetCurrentThreadId(), static_cast<uint32_t>(RemovedReason()),
				globals::state ? globals::state->frameCount : 0u,
				globals::state ? globals::state->DescribeRecentPerfEvents() : std::string(" none"));
		}
	};

	/**
 * @brief Installs hooks on the Map and Unmap methods of the provided D3D11 device context.
 *
 * This enables interception of resource mapping and unmapping operations for frame buffer caching.
 */
	void InstallD3DHooks(ID3D11DeviceContext* a_context)
	{
		stl::detour_vfunc<14, ID3D11DeviceContext_Map>(a_context);
		stl::detour_vfunc<15, ID3D11DeviceContext_Unmap>(a_context);
		stl::detour_vfunc<29, ID3D11DeviceContext_GetData>(a_context);

		// VR stereo optimization hooks: installed only when stereo reprojection is enabled at startup.
		// Changing stereoMode at runtime requires a restart; the UI communicates this to the user.
		if (globals::game::isVR && globals::features::vr.stereoOpt.settings.stereoMode != VRStereoOptimizations::StereoMode::Off) {
			stl::detour_vfunc<33, ID3D11DeviceContext_OMSetRenderTargets>(a_context);
			stl::detour_vfunc<36, ID3D11DeviceContext_OMSetDepthStencilState>(a_context);
			stl::detour_vfunc<53, ID3D11DeviceContext_ClearDepthStencilView>(a_context);
		}
	}
}
