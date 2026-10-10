#include "NeuralRendering.h"

#include "Features/HDRDisplay.h"
#include "Features/PostProcessing.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "NeuralRendering/ActorRegion.h"
#include "NeuralRendering/D3D12Interop.h"
#include "NeuralRendering/FoveaClip.h"
#include "NeuralRendering/Lifecycle.h"
#include "NeuralRendering/Runtime.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/LazyShader.h"
#include "Utils/RegionOverlay.h"
#include "Utils/Subrect.h"
#include "Utils/UI.h"

#include <chrono>
#include <cstring>
#include <span>

#define I18N_KEY_PREFIX "feature.upscaling.neural_rendering."

namespace
{
	/** @brief Active-status line; malformed braces in a translation fall back to the English text. */
	std::string FormatActiveStatus(const std::string& runtime, bool finalImage)
	{
		const char* fallback = finalImage ? "Active on the final image (runtime {})" : "Active before upscaling (runtime {})";
		try {
			return std::vformat(finalImage ? T(TKEY("status_active_final"), fallback) : T(TKEY("status_active"), fallback), std::make_format_args(runtime));
		} catch (const std::format_error&) {
			return std::vformat(fallback, std::make_format_args(runtime));
		}
	}

	/** @brief The driver's user-mode version (e.g. 32.0.16.1664) and NVIDIA's reading of it (616.64), for testers' logs. */
	std::string DriverVersion()
	{
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		winrt::com_ptr<IDXGIAdapter> adapter;
		LARGE_INTEGER umd{};
		if (!globals::d3d::device || FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))) ||
			FAILED(dxgiDevice->GetAdapter(adapter.put())) || FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
			return "unknown";
		const auto high = static_cast<DWORD>(umd.HighPart), low = static_cast<DWORD>(umd.LowPart);
		const uint32_t nvidia = (HIWORD(low) % 10) * 10000 + LOWORD(low);
		return std::format("{}.{}.{}.{} (NVIDIA {}.{:02})", HIWORD(high), LOWORD(high), HIWORD(low), LOWORD(low), nvidia / 100, nvidia % 100);
	}

	/**
	 * @brief One line with everything a tester's log needs to tell their setup apart: build, GPU, driver, runtime,
	 *        placement and sizes, and every tuning value. Logged each time NR becomes active.
	 */
	void LogTesterSummary(const char* a_placement, const std::string& a_runtime, const NR::Tuning& a_tuning, float a_mix, uint32_t a_modelWidth,
		uint32_t a_modelHeight, uint32_t a_frameWidth, uint32_t a_frameHeight)
	{
		auto& upscaling = globals::features::upscaling;
		const std::string skin = a_tuning.skinStructureStrength < 0.0f ? std::string("Auto") : std::format("{:.2f}", a_tuning.skinStructureStrength);
		const std::string frameGeneration = upscaling.UsesDLSSGFrameGen() ? std::format("DLSS-G {}x", upscaling.GetFrameGenerationMultiplier()) : std::string("off");
		const bool hdr = globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR;
		logger::info("[NeuralRendering] tester summary: CS {} | GPU {} | driver {} | NR runtime {} | placement {} | model {}x{} for a {}x{} frame | "
					 "mix {:.2f}, style {}, intensity {:.2f}, local tone {:.2f}, local structure {:.2f}, skin {}, auto mask {} | frame generation {} | HDR {}",
			Plugin::BUILD_DESCRIBE, globals::state->adapterDescription, DriverVersion(), a_runtime, a_placement, a_modelWidth, a_modelHeight, a_frameWidth,
			a_frameHeight, a_mix, a_tuning.style, a_tuning.intensity, a_tuning.localToneStrength, a_tuning.localStructureStrength, skin,
			a_tuning.useAutoMask ? "on" : "off", frameGeneration, hdr ? "on" : "off");
	}

	/** @brief Absolute plugin directory the runtime loads from; the load and the panel's verdict must resolve it alike. */
	std::filesystem::path RuntimeDirectory()
	{
		return Util::PathHelpers::SafeAbsolute(Upscaling::streamline.pluginDir);
	}

	/**
	 * @brief How to get a runtime the pass will load, for a verdict that found none it can use.
	 *        A missing file needs supplying; a rejected build needs replacing, never trusting it.
	 */
	const char* RuntimeFixHint(NR::RuntimeAvailability::State state)
	{
		if (state == NR::RuntimeAvailability::State::kMissing)
			return T(TKEY("runtime_missing_fix"), "Install nvngx_dlssnr.dll under Data\\Shaders\\Upscaling\\Streamline\\.");
		return T(TKEY("runtime_build_fix"), "Replace it with a validated 310.8 build of that file.");
	}

	// Stage 1 of the 05-29 port leaves out the tracked-actor crop: its bound and line-of-sight
	// queries need CommonLib APIs the 05-29 pin does not have. NR always evaluates the full frame.

	/** @brief Pass-to-pass timing ratio above which a calibration result is flagged as unsteady. */
	constexpr float kCalibrationUnstableRatio = 1.5f;

	/** @brief Widest the settings-panel region preview is drawn, in ImGui pixels. */
	constexpr float kRegionPreviewMaxWidth = 400.0f;

	/** @brief Rectangles the preview can draw: a crop and an actor box per eye. */
	constexpr size_t kMaxPreviewRegions = 4;

	/** @brief Width the debug region overlay draws the crop outline at, in NR render-resolution pixels. */
	constexpr float kRegionOutlineThicknessPixels = 3.0f;

	/** @brief Colour the preview draws the tracked actor's projected box in; the shader outline uses the same yellow. */
	constexpr ImU32 kActorBoxPreviewColor = IM_COL32(255, 255, 0, 255);

	/**
	 * @brief NR frames from NR starting to its test capture, so the temporal history has settled: about
	 *        5 s at 60 fps. At load this also stays clear of the DLSS-G input dump at composed frame 600.
	 */
	constexpr uint32_t kTestCaptureAfterFrames = 300;

	/** @brief Most consecutive frames one test capture holds; each frame keeps two render-size copies. */
	constexpr uint32_t kMaxTestCaptureFrames = 8;

	/** @brief Shortest test-cycle turn: the capture needs kTestCaptureAfterFrames (about 5 s) after each start. */
	constexpr uint32_t kMinTestCycleSeconds = 10;

	/** @brief Seconds into a test-cycle turn before its frame rates and GPU time are counted: a switch and NR's warm-up stay out. */
	constexpr double kTestCycleSettleSeconds = 3.0;

	/** @brief One run of the variant test cycle (run set 6): what it changes against the saved tuning. */
	struct TestCycleVariant
	{
		const char* name;
		float skinStructure;
		uint32_t toneTransfer;
		bool withoutDepth;
	};
	// Skin -1 is the shipped Auto, which runtime 310.8 treats as off; only 0.00-0.99 reach the image. The last
	// run repeats aces-skin050 without depth, to see whether 310.8 reads the depth guide.
	constexpr TestCycleVariant kTestCycleVariants[] = {
		{ "gain-skinauto", -1.0f, NR::Tuning::kToneTransferGain, false },
		{ "gain-skin000", 0.0f, NR::Tuning::kToneTransferGain, false },
		{ "gain-skin050", 0.5f, NR::Tuning::kToneTransferGain, false },
		{ "gain-skin090", 0.9f, NR::Tuning::kToneTransferGain, false },
		{ "aces-skinauto", -1.0f, NR::Tuning::kToneTransferAces, false },
		{ "aces-skin000", 0.0f, NR::Tuning::kToneTransferAces, false },
		{ "aces-skin050", 0.5f, NR::Tuning::kToneTransferAces, false },
		{ "aces-skin090", 0.9f, NR::Tuning::kToneTransferAces, false },
		{ "aces-skin050-nodepth", 0.5f, NR::Tuning::kToneTransferAces, true },
	};

	/** @brief One run of the working-scale test cycle (run set 7): NR on the final image at this share of the output. */
	struct TestCycleScale
	{
		const char* name;
		float scale;
		float skinStructure;
	};
	// Skin 0.5 is the user's play setting; the last run repeats full size with Auto (-1), to see whether the
	// skin value reaches the final image at all.
	constexpr TestCycleScale kTestCycleScales[] = {
		{ "final-s100", 1.0f, 0.5f },
		{ "final-s085", 0.85f, 0.5f },
		{ "final-s075", 0.75f, 0.5f },
		{ "final-s060", 0.6f, 0.5f },
		{ "final-s100-skinauto", 1.0f, NR::Tuning::kAutomaticSkinStructure },
	};

	/**
	 * @brief One condition of the feature test cycle (run set 9): settings patches on other CS features, applied
	 *        through their own SaveSettings / LoadSettings, plus the DLSS sharpening. NR runs on, then off, in each.
	 */
	struct TestCondition
	{
		const char* name;
		const char* patches;  // JSON object: feature name -> merge patch of its settings
		float sharpness;      // DLSS sharpening for the condition; negative keeps the user's value
	};
	constexpr TestCondition kTestConditions[] = {
		{ "base", "{}", -1.0f },
		{ "pp-overlap-off",
			R"({"Post Processing":{"Local Exposure":{"enabled":false},"Physical Glare":{"enabled":false},"COD Bloom":{"enabled":false},"Lens Flare":{"enabled":false}}})",
			0.0f },
		{ "ssgi-ao-off", R"({"Screen Space GI":{"AOPower":0.0}})", -1.0f },
		{ "skin-hair-off", R"({"Subsurface Scattering":{"BaseProfile":{"Strength":[0.0,0.0,0.0]}},"Hair Specular":{"Enabled":0}})", -1.0f },
		{ "shadow-fill-off", R"({"Screen Space Shadows":{"Enable":0},"Pseudo Sun Bounce":{"intensity":0.0}})", -1.0f },
	};

	/** @brief One run of the mix test cycle (run set 8): NR on the final image with this mix and local strengths. */
	struct TestCycleMix
	{
		const char* name;
		float mix, localTone, localStructure;
	};
	// Tone / structure 1 / 1 is the default; 0.3 / 0.7 is dxvk-remix's.
	constexpr TestCycleMix kTestCycleMixes[] = {
		{ "mix050-t10s10", 0.5f, 1.0f, 1.0f },
		{ "mix100-t10s10", 1.0f, 1.0f, 1.0f },
		{ "mix150-t10s10", 1.5f, 1.0f, 1.0f },
		{ "mix200-t10s10", 2.0f, 1.0f, 1.0f },
		{ "mix050-t03s07", 0.5f, 0.3f, 0.7f },
		{ "mix100-t03s07", 1.0f, 0.3f, 0.7f },
		{ "mix150-t03s07", 1.5f, 0.3f, 0.7f },
		{ "mix200-t03s07", 2.0f, 0.3f, 0.7f },
	};

	/**
	 * @brief Runtime feature slot the final-image placement evaluates in: the second eye's, which flat
	 *        rendering never uses. Its own NGX feature keeps the output-resolution history apart.
	 */
	constexpr uint32_t kFinalImageSlot = 1;

	/** @brief Side of the square pixel tile one SceneKeyCS Reduce group covers. */
	constexpr uint32_t kSceneKeyTilePixels = 64;
	/** @brief Most tiles the scene-key reduction holds: 16.7 megapixels per eye. */
	constexpr uint32_t kMaxSceneKeyTiles = 4096;
	/** @brief How fast the scene key follows the frame, as Post Processing's default AdaptSpeed. */
	constexpr float kSceneKeyAdaptSpeed = 1.5f;
	/** @brief Range the scene key is clamped to before 0.18 / key exposes the proxy: 2^-24 to 2^8. */
	constexpr float kSceneKeyMinimum = 5.9604645e-8f, kSceneKeyMaximum = 256.0f;
	/** @brief NR frames between two scene-key log lines. */
	constexpr uint32_t kSceneKeyReportFrames = 600;

	/** @brief Post Processing's histogram auto exposure while it runs, else nullptr. */
	HistogramAutoExposure* PostProcessingAutoExposure()
	{
		auto& postProcessing = globals::features::postProcessing;
		if (!postProcessing.loaded || postProcessing.bypass)
			return nullptr;
		auto* autoExposure = postProcessing.GetPipelineFeature<HistogramAutoExposure>(PostProcessing::FeaturePipelineIndex::AutoExposure);
		return autoExposure && autoExposure->enabled && autoExposure->GetAdaptationSRV() ? autoExposure : nullptr;
	}

	/**
	 * @brief GPU time of the NR pass from D3D11 timestamps: the encoder, the D3D12 evaluate the
	 *        immediate context waits on, and the composite. Results are read a few frames late so
	 *        the readback never stalls, and the average is logged every kReportFrames samples.
	 */
	class PassTimer
	{
	public:
		/** @brief Sum, extremes and count of the samples gathered over a span. */
		struct Totals
		{
			uint32_t samples = 0;
			double sumMs = 0.0, minMs = 0.0, maxMs = 0.0;
			void Add(double a_ms)
			{
				minMs = samples ? std::min(minMs, a_ms) : a_ms;
				maxMs = samples ? std::max(maxMs, a_ms) : a_ms;
				sumMs += a_ms;
				++samples;
			}
		};

		/** @param a_label The placement the timer measures, named in its log lines. */
		explicit PassTimer(const char* a_label) :
			label(a_label) {}

		/** @brief Samples gathered since the last call, for the test cycle's per-turn line; restarts the count. */
		Totals TakeTurn() { return std::exchange(turn, Totals{}); }

		/** @brief Times the enclosing block; the destructor ends the measurement on every exit path. */
		class Scope
		{
		public:
			explicit Scope(PassTimer& a_timer) :
				timer(a_timer) { timer.Begin(); }
			~Scope() { timer.End(); }
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

		private:
			PassTimer& timer;
		};

	private:
		static constexpr uint32_t kSlots = 4;
		static constexpr uint32_t kReportFrames = 600;
		struct Slot
		{
			winrt::com_ptr<ID3D11Query> disjoint, begin, end;
			bool pending = false;
		};

		void Begin()
		{
			auto& slot = slots[next];
			Collect(slot);
			if (!slot.disjoint && !Create(slot))
				return;
			globals::d3d::context->Begin(slot.disjoint.get());
			globals::d3d::context->End(slot.begin.get());
			open = true;
		}

		void End()
		{
			if (!open)
				return;
			auto& slot = slots[next];
			globals::d3d::context->End(slot.end.get());
			globals::d3d::context->End(slot.disjoint.get());
			slot.pending = true;
			open = false;
			next = (next + 1) % kSlots;
		}

		static bool Create(Slot& a_slot)
		{
			const D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			const D3D11_QUERY_DESC stampDesc{ D3D11_QUERY_TIMESTAMP, 0 };
			auto* device = globals::d3d::device;
			if (SUCCEEDED(device->CreateQuery(&disjointDesc, a_slot.disjoint.put())) && SUCCEEDED(device->CreateQuery(&stampDesc, a_slot.begin.put())) &&
				SUCCEEDED(device->CreateQuery(&stampDesc, a_slot.end.put())))
				return true;
			a_slot = {};
			return false;
		}

		/** @brief Adds the slot's sample when its queries are ready; a slot that is still busy is dropped. */
		void Collect(Slot& a_slot)
		{
			if (!a_slot.pending)
				return;
			a_slot.pending = false;
			auto* context = globals::d3d::context;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			UINT64 begin = 0, end = 0;
			constexpr UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
			if (context->GetData(a_slot.disjoint.get(), &disjoint, sizeof(disjoint), kNoFlush) != S_OK || disjoint.Disjoint || !disjoint.Frequency ||
				context->GetData(a_slot.begin.get(), &begin, sizeof(begin), kNoFlush) != S_OK ||
				context->GetData(a_slot.end.get(), &end, sizeof(end), kNoFlush) != S_OK || end < begin)
				return;
			const double ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
			turn.Add(ms);
			report.Add(ms);
			if (report.samples < kReportFrames)
				return;
			logger::info("[NeuralRendering] GPU time over {} frames ({}): avg {:.3f} ms, min {:.3f} ms, max {:.3f} ms", report.samples, label,
				report.sumMs / report.samples, report.minMs, report.maxMs);
			report = {};
		}

		const char* label;
		std::array<Slot, kSlots> slots;
		uint32_t next = 0;
		bool open = false;
		Totals report, turn;
	};
}

struct NeuralRendering::Impl
{
	NR::D3D12Interop interop;
	NR::Runtime runtime;
	struct Eye
	{
		std::unique_ptr<WrappedResource> color, depth, motion, output;
		/** @brief DLSSNR.UIAlpha built from the Masks2 category lane; created only while the material strength is on. */
		std::unique_ptr<WrappedResource> materialAlpha;
		NR::FrameParameters frame;
		std::unique_ptr<Texture2D> resolved, toneData;
		DirectX::SimpleMath::Vector3 position{}, forward{};
	};
	std::array<Eye, 2> eyes;
	/**
	 * @brief The final-image placement's pass: the HUD-less frame's copy, the output-size proxy, guides
	 *        and output the runtime shares, and the temporal state of its own NGX feature (kFinalImageSlot).
	 */
	struct FinalImage
	{
		std::unique_ptr<WrappedResource> color, depth, motion, output;
		/** @brief The HUD-less frame as it was before NR: the composite's original and the test capture's "before". */
		std::unique_ptr<Texture2D> original;
		NR::FrameParameters frame;
		DirectX::SimpleMath::Vector3 position{}, forward{};
		/** @brief The model's working size (color, guides, output) and the HUD-less frame's size (original). */
		uint32_t width = 0, height = 0, outputWidth = 0, outputHeight = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		/** @brief Engine frame the pass last ran on; a gap resets the history. */
		uint32_t lastFrame = UINT32_MAX;
	} finalImage;
	/** @brief Bilinear clamp sampler: the final image's downscale into the working size and the edit's upscale. */
	winrt::com_ptr<ID3D11SamplerState> finalSampler;
	/** @brief FinalImageCS.hlsl cbuffer: the output, rendered and working sizes and the share of NR shown. */
	struct alignas(16) FinalImageData
	{
		uint32_t outputWidth, outputHeight, renderWidth, renderHeight;
		float mix;
		uint32_t workWidth, workHeight;
		float pad = 0.0f;
	};
	static_assert(sizeof(FinalImageData) == 32);
	std::unique_ptr<ConstantBuffer> finalImageBuffer;
	Util::LazyShader<ID3D11ComputeShader> finalPrepare, finalComposite;
	winrt::com_ptr<ID3D11DeviceContext1> context;
	winrt::com_ptr<ID3DDeviceContextState> isolated;
	/** @brief Runs the enclosing block in NR's own context state and gives the game's state back on every exit path. */
	struct IsolatedContext
	{
		ID3D11DeviceContext1* context;
		winrt::com_ptr<ID3DDeviceContextState> previous;
		IsolatedContext(ID3D11DeviceContext1* a_context, ID3DDeviceContextState* a_isolated) :
			context(a_context)
		{
			context->SwapDeviceContextState(a_isolated, previous.put());
			context->ClearState();
		}
		~IsolatedContext()
		{
			context->ClearState();
			context->SwapDeviceContextState(previous.get(), nullptr);
		}
		IsolatedContext(const IsolatedContext&) = delete;
		IsolatedContext& operator=(const IsolatedContext&) = delete;
	};
	Util::LazyShader<ID3D11ComputeShader> prepareColor, prepareToneData, compositeColor, materialAlphaShader;
	/** @brief Cbuffer CategoryAlphaCS.hlsl reads: the eye extent, the stereo offset, the softness radius and the six strengths. */
	struct alignas(16) CategoryAlphaData
	{
		uint32_t width, height, eyeOffsetX, edgeSoftness;
		float4 strengthsA;
		float2 strengthsB;
		float2 pad{};
	};
	static_assert(offsetof(CategoryAlphaData, strengthsA) == 16);
	static_assert(offsetof(CategoryAlphaData, strengthsB) == 32);
	static_assert(sizeof(CategoryAlphaData) == 48);
	struct alignas(16) ColorTransferData
	{
		uint32_t width, height, eyeOffsetX, hasExposure = 0;
		uint32_t conversionMode = 0, exposureMode = 0, compositeMode = 0, maskMode = 0;
		uint32_t visualMode = 0;
		float exposureCompensation = 1.0f, exposureMin = 1.0f, exposureMax = 1.0f, manualExposure = 1.0f;
		float differenceStrength = 1.0f, splitPosition = 0.5f;
		float dynamicRangePadding = 0.0f;
		float4 dynamicRangeProtect{};
		float toneLowStrength = 1.0f, toneRadius = 1.0f, toneHighStrength = 1.0f;
		uint32_t hasToneData = 0;
		uint32_t regionBaseX = 0, regionBaseY = 0, regionWidth = 0, regionHeight = 0;
		uint32_t regionOverlayEnabled = 0;
		float regionOutlineThickness = kRegionOutlineThicknessPixels;
		uint32_t regionActorBaseX = 0, regionActorBaseY = 0, regionActorWidth = 0, regionActorHeight = 0;
		float mix = 1.0f;  ///< Share of NR's edit applied (the mix slider); fills what was padding.
		uint32_t toneTransfer = NR::Tuning::kToneTransferGain;  ///< ColorTransferCS ToneTransfer; was padding.
		// Per-category tone multipliers, Skin..Landscape in .x; the 16-byte rows mirror
		// ColorTransferCS.hlsl's float4 CategoryStrength[5].
		float4 categoryStrength[5]{};
		uint32_t materialMapEnabled = 0, materialMapMode = 0, materialMapFilter = 0, materialMapStrengthBound = 0;
		float4 materialStrengthsA{};
		float4 materialStrengthsB{};
	};
	static_assert(offsetof(ColorTransferData, dynamicRangeProtect) == 64);
	static_assert(offsetof(ColorTransferData, toneLowStrength) == 80);
	static_assert(offsetof(ColorTransferData, toneRadius) == 84);
	static_assert(offsetof(ColorTransferData, toneHighStrength) == 88);
	static_assert(offsetof(ColorTransferData, hasToneData) == 92);
	static_assert(offsetof(ColorTransferData, regionBaseX) == 96);
	static_assert(offsetof(ColorTransferData, regionOverlayEnabled) == 112);
	static_assert(offsetof(ColorTransferData, regionOutlineThickness) == 116);
	static_assert(offsetof(ColorTransferData, regionActorBaseX) == 120);
	static_assert(offsetof(ColorTransferData, regionActorHeight) == 132);
	static_assert(offsetof(ColorTransferData, mix) == 136);
	static_assert(offsetof(ColorTransferData, categoryStrength) == 144);
	static_assert(offsetof(ColorTransferData, materialMapEnabled) == 224);
	static_assert(offsetof(ColorTransferData, materialMapStrengthBound) == 236);
	static_assert(offsetof(ColorTransferData, materialStrengthsA) == 240);
	static_assert(offsetof(ColorTransferData, materialStrengthsB) == 256);
	static_assert(sizeof(ColorTransferData) == 272);
	// ModeValues.hlsli carries these numbers for ColorTransferCS.hlsl's tint.
	static_assert(static_cast<uint32_t>(NR::MaterialMap::Mode::kCategory) == 0);
	static_assert(static_cast<uint32_t>(NR::MaterialMap::Mode::kStrength) == 1);
	std::unique_ptr<ConstantBuffer> colorBuffer;
	std::unique_ptr<ConstantBuffer> categoryAlphaBuffer;
	/** @brief Material-strength switch the persistent eye features were last created with; drives recreation. */
	bool materialStrengthOn = false;
	/** @brief The graded protection is bound for the evaluate now in flight, so a fault belongs to it. */
	bool materialStrengthInFlight = false;
	/** @brief Last evaluate bound the graded protection; reported by the status readout. */
	bool materialStrengthBound = false;
	/** @brief Latched setup failure: the material lane or the alpha texture is unusable for this session. */
	bool materialStrengthDegraded = false;
	std::unique_ptr<Texture2D> original;
	std::unique_ptr<ConstantBuffer> encodeBuffer;
	std::array<std::unique_ptr<Texture2D>, 2> encodeMasks;
	uint32_t width = 0, height = 0, guideWidth = 0, guideHeight = 0, eyeCount = 0, lastFrame = UINT32_MAX;
	/** @brief Crop of the last frame NR evaluated; inactive means it covered the whole frame. */
	Util::Region::StereoRegion lastRegion;
	/** @brief Crop this frame's evaluation uses, published by the main thread and read by TransferColor. */
	Util::Region::StereoRegion region;
	/** @brief The tracked actor's unpadded projected box, published alongside the crop and drawn by the overlay. */
	Util::Region::StereoRegion actorBox;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	bool ready = false, failed = false;
	uint32_t lastDiagnosticOptions = 0;
	uint32_t debugOptions = 0;
	NR::Diagnostics::ColorConversion conversionMode = NR::Diagnostics::ColorConversion::Production;
	NR::Diagnostics::ExposureMode exposureMode = NR::Diagnostics::ExposureMode::Production;
	NR::Diagnostics::CompositeMode compositeMode = NR::Diagnostics::CompositeMode::Production;
	NR::Diagnostics::VisualMode visualMode = NR::Diagnostics::VisualMode::None;
	float manualExposure = 1.0f, differenceStrength = 1.0f, splitPosition = 0.5f;
	float shadowProtect = 0.0f, highlightProtect = 0.0f;
	float toneLowStrength = 1.0f, toneRadius = 1.0f, toneHighStrength = 1.0f;
	/** @brief Per-category tone multipliers from the tuning, consumed by TransferColor's cbuffer. */
	std::array<float, 5> categoryToneStrength{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	bool useResolutionMotionScale = true;
	/** @brief Draws the evaluated crop outline into the composite; read from the tuning each NR frame. */
	bool regionOverlay = false;
	/** @brief Material-map tint of the last frame, read from the tuning: the switch, the mode and the category filter. */
	bool materialMapEnabled = false;
	uint32_t materialMapMode = 0, materialMapFilter = NR::MaterialMap::kAllCategories;
	/** @brief Per-category by-material strengths of the last frame, in NeuralRenderingCategory id order. */
	std::array<float, NR::MaterialStrength::kCount> materialMapStrength{};
	NR::Diagnostics* captureDiagnostics = nullptr;
	uint32_t captureFrame = UINT32_MAX;
	/** @brief Share of NR's edit shown, 0 to 2 (above 1 extrapolates): the user's mix, set before each frame. */
	float mix = 1.0f;
	/** @brief The before-upscaling pass's NR::Tuning::toneTransfer this frame. */
	uint32_t toneTransfer = NR::Tuning::kToneTransferGain;
	/** @brief Test aid (variant cycle): evaluate without DLSSNR.Depth. */
	bool withoutDepth = false;
	PassTimer passTimer{ "before upscaling" }, finalPassTimer{ "final image" };
	/** @brief File-name stage of the running test capture: "test" before upscaling, "final" on the final image. */
	const char* testStage = "test";
	/** @brief One frame of a test capture: render-size copies of the scene before and after NR. */
	struct TestFrame
	{
		winrt::com_ptr<ID3D11Texture2D> before, after;
		uint32_t frame = 0;
	};
	std::vector<TestFrame> testFrames;
	/** @brief Frames of the running test capture already copied; the files are written once all are in. */
	uint32_t testFramesCopied = 0;

	/** @brief Where this frame's proxy exposure comes from; logged when it changes. */
	enum class ExposureSource : uint8_t
	{
		kUnit,            ///< None: Open Shaders' unit white point (stage 1, or the test setting).
		kPostProcessing,  ///< Post Processing's histogram auto exposure, as its composite applies it.
		kSceneKey         ///< NR's own adapted scene key (SceneKeyCS), when Post Processing has none.
	};
	/** @brief The adapted luminance and constants the Prepare pass exposes the proxy with; no SRV keeps unit exposure. */
	struct ProxyExposure
	{
		ID3D11ShaderResourceView* adaptation = nullptr;
		float compensation = 1.0f, minimum = 1.0f, maximum = 1.0f;
	} proxyExposure;
	/** @brief Set from the settings before each frame: the stage-1 unit white point, for comparisons. */
	bool unitExposure = false;
	ExposureSource exposureSource = ExposureSource::kUnit;
	bool exposureSourceLogged = false;
	/** @brief SceneKeyCS cbuffer: the eye extent, the tile grid and this frame's adaptation step. */
	struct alignas(16) SceneKeyData
	{
		uint32_t width, height, eyeOffsetX, tileCount;
		uint32_t tilesX;
		float adaptLerp;
		float2 pad{};
	};
	static_assert(sizeof(SceneKeyData) == 32);
	Util::LazyShader<ID3D11ComputeShader> sceneKeyReduce, sceneKeyAdapt;
	std::unique_ptr<ConstantBuffer> sceneKeyBuffer;
	std::unique_ptr<StructuredBuffer> sceneKeyTiles, sceneKeyAdaptation;
	/** @brief Reads the adapted key back for the log without a stall: copied at one report, mapped at the next. */
	winrt::com_ptr<ID3D11Buffer> sceneKeyStaging;
	bool sceneKeyStagingPending = false;
	uint32_t sceneKeyFrames = 0;

	~Impl()
	{
		// Static destruction, where the interop wait can block on a GPU that is already gone.
		if (NR::processTerminating.load(std::memory_order_relaxed))
			return;
		try {
			interop.Drain();
		} catch (...) {
			logger::warn("[NeuralRendering] Device unavailable during resource retirement");
		}
	}

	void Initialize()
	{
		interop.Initialize();
		winrt::com_ptr<ID3D11Device1> device;
		winrt::check_hresult(globals::d3d::device->QueryInterface(device.put()));
		winrt::check_hresult(globals::d3d::context->QueryInterface(context.put()));
		const auto level = globals::d3d::device->GetFeatureLevel();
		winrt::check_hresult(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
			__uuidof(ID3D11Device), nullptr, isolated.put()));
		Util::SetResourceName(isolated.get(), "NeuralRendering::ContextState");
		encodeBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Upscaling::UpscalingDataCB>(), "NeuralRendering::Encode CB");
		colorBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ColorTransferData>(), "NeuralRendering::ColorTransfer CB");
		categoryAlphaBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CategoryAlphaData>(), "NeuralRendering::CategoryAlpha CB");
		finalImageBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<FinalImageData>(), "NeuralRendering::FinalImage CB");
		runtime.Initialize(interop.Device(), RuntimeDirectory(), globals::state->IsDeveloperMode());
		ready = true;
	}

	/**
	 * @brief Releases the per-frame pass resources, keeping the D3D12 device and NGX instance.
	 *        Rebuilding those costs a driver-level re-init; the textures do not.
	 */
	void ReleasePassResources()
	{
		interop.Drain();
		runtime.ResetFeatures();
		eyes = {};
		original.reset();
		encodeMasks = {};
		finalImage = {};
		// A running test capture copies from the textures released here.
		testFrames.clear();
		testFramesCopied = 0;
		materialStrengthInFlight = materialStrengthBound = false;
		width = height = guideWidth = guideHeight = eyeCount = 0;
		format = DXGI_FORMAT_UNKNOWN;
		lastFrame = UINT32_MAX;
		lastRegion = {};
	}

	/** @brief True while pass resources for either placement exist and can be released. */
	bool HasPassResources() const { return eyeCount != 0 || finalImage.width != 0; }

	/** @brief True while the before-upscaling placement's resources exist. */
	bool HasBeforeUpscalingResources() const { return eyeCount != 0; }

	/** @brief The model's working size for an output size and a share of it per axis (NR::Tuning::finalScale). */
	static std::pair<uint32_t, uint32_t> FinalWorkSize(uint32_t a_width, uint32_t a_height, float a_scale)
	{
		const auto scaled = [a_scale](uint32_t a_size) {
			return std::min(std::max(static_cast<uint32_t>(std::lround(a_size * static_cast<double>(a_scale))), 16u), a_size);
		};
		return { scaled(a_width), scaled(a_height) };
	}

	/**
	 * @brief Creates the final-image pass for an output size, format and working scale. A change releases every
	 *        pass resource first, the runtime's features included, so the new size starts a fresh history.
	 */
	void EnsureFinalResources(uint32_t w, uint32_t h, DXGI_FORMAT frameFormat, float scale, bool force)
	{
		auto& pass = finalImage;
		const auto [workWidth, workHeight] = FinalWorkSize(w, h, scale);
		if (!force && pass.outputWidth == w && pass.outputHeight == h && pass.width == workWidth && pass.height == workHeight && pass.format == frameFormat)
			return;
		ReleasePassResources();
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = w;
		desc.Height = h;
		desc.Format = frameFormat;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		pass.original = std::make_unique<Texture2D>(desc, "NeuralRendering::FinalImage Original");
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = frameFormat;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		pass.original->CreateSRV(srv);
		pass.color = interop.CreateTexture(workWidth, workHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, "NeuralRendering::FinalImage Input");
		pass.depth = interop.CreateTexture(workWidth, workHeight, DXGI_FORMAT_R32_FLOAT, "NeuralRendering::FinalImage Depth");
		pass.motion = interop.CreateTexture(workWidth, workHeight, DXGI_FORMAT_R16G16_FLOAT, "NeuralRendering::FinalImage Motion");
		pass.output = interop.CreateTexture(workWidth, workHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, "NeuralRendering::FinalImage Output");
		pass.width = workWidth;
		pass.height = workHeight;
		pass.outputWidth = w;
		pass.outputHeight = h;
		pass.format = frameFormat;
		if (workWidth == w && workHeight == h)
			logger::info("[NeuralRendering] final-image NR {}x{}, frame format {}", w, h, static_cast<uint32_t>(frameFormat));
		else
			logger::info("[NeuralRendering] final-image NR {}x{} working size for a {}x{} frame (scale {:.2f}), frame format {}", workWidth, workHeight, w, h,
				scale, static_cast<uint32_t>(frameFormat));
	}

	/** @brief Publishes this frame's crop for one eye; every kernel that reads a neural sample needs it. */
	void SetRegion(ColorTransferData& a_data, uint32_t a_eye) const
	{
		if (region.active) {
			const auto& crop = region.eye[a_eye];
			a_data.regionBaseX = crop.x;
			a_data.regionBaseY = crop.y;
			a_data.regionWidth = crop.w;
			a_data.regionHeight = crop.h;
		}
		if (actorBox.active) {
			const auto& box = actorBox.eye[a_eye];
			a_data.regionActorBaseX = box.x;
			a_data.regionActorBaseY = box.y;
			a_data.regionActorWidth = box.w;
			a_data.regionActorHeight = box.h;
		}
	}

	void EnsureResources(uint32_t w, uint32_t h, uint32_t gw, uint32_t gh, uint32_t count, DXGI_FORMAT colorFormat, bool force)
	{
		if (!force && width == w && height == h && guideWidth == gw && guideHeight == gh && eyeCount == count && format == colorFormat)
			return;
		ReleasePassResources();
		D3D11_TEXTURE2D_DESC maskDesc{};
		maskDesc.Width = gw;
		maskDesc.Height = gh;
		maskDesc.Format = DXGI_FORMAT_R8_UNORM;
		maskDesc.MipLevels = maskDesc.ArraySize = maskDesc.SampleDesc.Count = 1;
		maskDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC maskUAV{};
		maskUAV.Format = maskDesc.Format;
		maskUAV.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		for (uint32_t i = 0; i < encodeMasks.size(); ++i) {
			encodeMasks[i] = std::make_unique<Texture2D>(maskDesc, std::format("NeuralRendering::EncodeMask{}", i).c_str());
			encodeMasks[i]->CreateUAV(maskUAV);
		}
		D3D11_TEXTURE2D_DESC colorDesc = maskDesc;
		colorDesc.Width = w * count;
		colorDesc.Height = h;
		colorDesc.Format = colorFormat;
		colorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = colorFormat;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		original = std::make_unique<Texture2D>(colorDesc, "NeuralRendering::OriginalHDR");
		original->CreateSRV(srv);
		colorDesc.Width = w;
		colorDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC colorUAV = maskUAV;
		colorUAV.Format = colorFormat;
		for (uint32_t i = 0; i < count; ++i) {
			auto& eye = eyes[i];
			const auto name = std::format("NeuralRendering::Eye{}", i);
			eye.resolved = std::make_unique<Texture2D>(colorDesc, (name + " ResolvedHDR").c_str());
			eye.resolved->CreateUAV(colorUAV);
			eye.color = interop.CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " HDRInput");
			eye.depth = interop.CreateTexture(gw, gh, DXGI_FORMAT_R32_FLOAT, name + " Depth");
			eye.motion = interop.CreateTexture(gw, gh, DXGI_FORMAT_R16G16_FLOAT, name + " Motion");
			eye.output = interop.CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " HDROutput");
		}
		width = w;
		height = h;
		guideWidth = gw;
		guideHeight = gh;
		logger::debug("[NeuralRendering] Render-resolution NR {}x{}, guides {}x{}, eyes {}", w, h, gw, gh, count);
		eyeCount = count;
		format = colorFormat;
	}

	uint32_t UpdateFrame(uint32_t i, uint32_t reset, NR::Diagnostics::Frame& diagnostic)
	{
		auto& eye = eyes[i];
		return UpdateCamera(i, eye.position, eye.forward, eye.frame, reset, diagnostic.options, diagnostic.camera[i], true);
	}

	/**
	 * @brief Detects camera cuts against what one NGX feature last saw and fills its frame parameters.
	 * @param a_camera The engine camera (eye) the frame was rendered from.
	 * @param a_jittered False for a frame without the upscaler's jitter, such as the final image.
	 * @return The reset reasons, a_reset plus the requested and detected ones that apply.
	 */
	uint32_t UpdateCamera(uint32_t a_camera, DirectX::SimpleMath::Vector3& a_position, DirectX::SimpleMath::Vector3& a_forward, NR::FrameParameters& a_frame,
		uint32_t a_reset, uint32_t a_options, NR::Diagnostics::CameraSample& sample, bool a_jittered)
	{
		auto& cached = globals::game::frameBufferCached;
		const auto inverseView = cached.GetCameraViewInverse(a_camera).Transpose();
		const auto projection = cached.GetCameraProjUnjittered(a_camera).Transpose();
		const auto adjusted = cached.GetCameraPosAdjust(a_camera);
		const DirectX::SimpleMath::Vector3 position{ adjusted.x, adjusted.y, adjusted.z };
		const DirectX::SimpleMath::Vector3 forward{ inverseView._31, inverseView._32, inverseView._33 };
		const auto enginePrevious = cached.GetCameraPreviousPosAdjust(a_camera);
		sample.position = { position.x, position.y, position.z };
		sample.previous = { a_position.x, a_position.y, a_position.z };
		sample.enginePrevious = { enginePrevious.x, enginePrevious.y, enginePrevious.z };
		sample.viewTranslation = { inverseView._41, inverseView._42, inverseView._43 };
		sample.distance = (position - a_position).Length();
		sample.directionDot = forward.Dot(a_forward);
		sample.projectionDelta = std::max(std::abs(projection._11 - a_frame.viewToClip._11), std::abs(projection._22 - a_frame.viewToClip._22));
		uint32_t cameraReset = 0;
		if ((position - a_position).LengthSquared() > NR::kCameraCutDistance * NR::kCameraCutDistance)
			cameraReset |= NR::Diagnostics::CameraPosition;
		if (forward.Dot(a_forward) < NR::kCameraCutDirectionDot)
			cameraReset |= NR::Diagnostics::CameraDirection;
		if (std::abs(projection._11 - a_frame.viewToClip._11) > NR::kProjectionCutThreshold ||
			std::abs(projection._22 - a_frame.viewToClip._22) > NR::kProjectionCutThreshold)
			cameraReset |= NR::Diagnostics::Projection;
		sample.detected = cameraReset;
		if (a_options & NR::Diagnostics::ApplyCameraCuts) {
			if (a_options & NR::Diagnostics::IgnorePosition)
				cameraReset &= ~NR::Diagnostics::CameraPosition;
			a_reset |= cameraReset;
		}
		if (a_options & NR::Diagnostics::ForceReset)
			a_reset |= NR::Diagnostics::Requested;
		a_frame.reset = a_reset != 0;
		a_position = position;
		a_forward = forward;
		a_frame.worldToView = inverseView.Invert();
		a_frame.viewToClip = projection;
		const auto jitter = a_jittered ? globals::features::upscaling.jitter : float2{};
		a_frame.jitterX = NR::SanitizeJitter(-jitter.x);
		a_frame.jitterY = NR::SanitizeJitter(-jitter.y);
		a_frame.frameTimeMs = NR::SanitizeFrameTimeMs(*globals::game::deltaTime * 1000.0f);
		a_frame.feedCameraData = (a_options & NR::Diagnostics::FeedCameraData) != 0;
		if (a_options & NR::Diagnostics::ZeroJitter)
			a_frame.jitterX = a_frame.jitterY = 0;
		sample.jitterX = a_frame.jitterX;
		sample.jitterY = a_frame.jitterY;
		sample.frameTimeMs = a_frame.frameTimeMs;
		return a_reset;
	}

	void Transition(ID3D12GraphicsCommandList* commands, Eye& eye, bool enter)
	{
		Transition(commands, eye.color->resource.get(), eye.depth->resource.get(), eye.motion->resource.get(),
			eye.output->resource.get(), eye.materialAlpha ? eye.materialAlpha->resource.get() : nullptr, enter);
	}

	/** @brief Moves the shared NR resources between COMMON and the states the evaluate reads and writes them in. */
	static void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
		ID3D12Resource* output, ID3D12Resource* alpha, bool enter)
	{
		ID3D12Resource* resources[5]{ color, depth, motion, output, alpha };
		const D3D12_RESOURCE_STATES states[5]{
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
		};
		D3D12_RESOURCE_BARRIER barriers[5]{};
		uint32_t count = 0;
		for (uint32_t i = 0; i < 5; ++i) {
			if (!resources[i])
				continue;
			barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[count].Transition = { resources[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				enter ? D3D12_RESOURCE_STATE_COMMON : states[i], enter ? states[i] : D3D12_RESOURCE_STATE_COMMON };
			++count;
		}
		commands->ResourceBarrier(count, barriers);
	}

	bool NeedsToneData() const
	{
		const bool showBands = visualMode == NR::Diagnostics::VisualMode::ToneLow || visualMode == NR::Diagnostics::VisualMode::ToneHigh;
		const bool useTone = (compositeMode == NR::Diagnostics::CompositeMode::Production && toneTransfer == NR::Tuning::kToneTransferGain) ||
		                     visualMode == NR::Diagnostics::VisualMode::ToneLowGain;
		return toneRadius > 0.01f && (showBands || (useTone && toneLowStrength != toneHighStrength));
	}

	void PrepareToneData(uint32_t i)
	{
		if (!NeedsToneData())
			return;
		CS_GPU_PASS("Upscaling::NRPrepareTone");
		auto* shader = prepareToneData.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
			{}, "cs_5_0", "PrepareToneData", "NeuralRendering::PrepareToneData CS");
		if (!shader)
			throw std::runtime_error("NR tone-data shader unavailable");
		auto& eye = eyes[i];
		if (!eye.toneData) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R32G32_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			auto texture = std::make_unique<Texture2D>(desc, std::format("NeuralRendering::Eye{} ToneData", i).c_str());
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			texture->CreateSRV(srv);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = desc.Format;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			texture->CreateUAV(uav);
			eye.toneData = std::move(texture);
		}
		context->ClearState();
		ColorTransferData data{ width, height, i * width };
		SetRegion(data, i);
		colorBuffer->Update(data);
		auto buffer = colorBuffer->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
		ID3D11ShaderResourceView* inputs[]{ nullptr, eye.color->srv, eye.output->srv };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		auto* output = eye.toneData->uav.get();
		context->CSSetUnorderedAccessViews(2, 1, &output, nullptr);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	void TransferColor(uint32_t i, bool prepare)
	{
		CS_GPU_PASS_SELECT(prepare, "Upscaling::NRPrepare", "Upscaling::NRComposite");
		context->ClearState();
		auto& eye = eyes[i];
		ColorTransferData data{ width, height, i * width };
		data.conversionMode = static_cast<uint32_t>((debugOptions & NR::Diagnostics::DisableColorTransform) ? NR::Diagnostics::ColorConversion::Raw : conversionMode);
		data.exposureMode = static_cast<uint32_t>((debugOptions & NR::Diagnostics::DisableExposure) ? NR::Diagnostics::ExposureMode::Ignore : exposureMode);
		data.compositeMode = static_cast<uint32_t>(compositeMode);
		data.visualMode = static_cast<uint32_t>(visualMode);
		if (debugOptions & (NR::Diagnostics::VisualizeMask | NR::Diagnostics::VisualizeSkinMask | NR::Diagnostics::VisualizeAutoMask))
			data.visualMode = static_cast<uint32_t>(NR::Diagnostics::VisualMode::Mask);
		data.manualExposure = manualExposure;
		data.mix = mix;
		data.toneTransfer = toneTransfer;
		data.differenceStrength = differenceStrength;
		data.splitPosition = splitPosition;
		data.dynamicRangeProtect = float4{ shadowProtect, highlightProtect, 0.0f, 0.0f };
		data.toneLowStrength = toneLowStrength;
		data.toneRadius = toneRadius;
		data.toneHighStrength = toneHighStrength;
		for (size_t category = 0; category < categoryToneStrength.size(); ++category)
			data.categoryStrength[category].x = categoryToneStrength[category];
		data.hasToneData = !prepare && NeedsToneData();
		// Never on the Prepare dispatch: that writes NGX's input proxy, which the overlay would corrupt.
		data.regionOverlayEnabled = (!prepare && regionOverlay) ? 1u : 0u;
		data.regionOutlineThickness = kRegionOutlineThicknessPixels;
		data.materialMapEnabled = (!prepare && materialMapEnabled) ? 1u : 0u;
		data.materialMapMode = materialMapMode;
		data.materialMapFilter = materialMapFilter;
		data.materialMapStrengthBound = materialStrengthBound ? 1u : 0u;
		data.materialStrengthsA = float4{ materialMapStrength[NR::MaterialStrength::kNone], materialMapStrength[NR::MaterialStrength::kSkin],
			materialMapStrength[NR::MaterialStrength::kHair], materialMapStrength[NR::MaterialStrength::kEyes] };
		data.materialStrengthsB = float4{ materialMapStrength[NR::MaterialStrength::kFoliage], materialMapStrength[NR::MaterialStrength::kLandscape], 0.0f, 0.0f };
		SetRegion(data, i);
		if (debugOptions & NR::Diagnostics::ForceMaskZero)
			data.maskMode = static_cast<uint32_t>(NR::Diagnostics::MaskMode::ForceZero);
		else if (debugOptions & NR::Diagnostics::ForceMaskOne)
			data.maskMode = static_cast<uint32_t>(NR::Diagnostics::MaskMode::ForceOne);
		else if (debugOptions & NR::Diagnostics::BypassMask)
			data.maskMode = static_cast<uint32_t>(NR::Diagnostics::MaskMode::ForceOne);
		// The proxy is exposed by this frame's source (SelectExposure): Post Processing's auto exposure, the
		// scene key, or none, which keeps Open Shaders' unit white point (hasExposure 0). The ACES round trip
		// divides the same exposure back out in the composite, so it binds it there too.
		ID3D11ShaderResourceView* exposure = nullptr;
		if ((prepare || toneTransfer == NR::Tuning::kToneTransferAces) && proxyExposure.adaptation) {
			exposure = proxyExposure.adaptation;
			data.hasExposure = 1;
			data.exposureCompensation = proxyExposure.compensation;
			data.exposureMin = proxyExposure.minimum;
			data.exposureMax = proxyExposure.maximum;
		}
		colorBuffer->Update(data);
		auto buffer = colorBuffer->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
		globals::state->BindSharedDataCS(context.get(), true);
		// 05-29 writes no material category lane, so that target holds engine data; bind it only when it
		// is the R16G16 lane. Unbound, every pixel decodes as uncategorised and the category edits stay neutral.
		auto* masks2 = MaterialLane();
		ID3D11ShaderResourceView* inputs[]{ original->srv.get(), prepare ? nullptr : eye.color->srv,
			prepare ? nullptr : eye.output->srv, exposure,
			data.hasToneData ? eye.toneData->srv.get() : nullptr, masks2 };
		ID3D11UnorderedAccessView* outputs[]{ prepare ? eye.color->uav : eye.resolved->uav.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->CSSetShader(prepare ? prepareColor.get() : compositeColor.get(), nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	/**
	 * @brief Returns the Masks2 SRV only when it carries the R16G16 material category lane this
	 *        alpha decodes. Any other target would read as None everywhere and protect everything.
	 */
	ID3D11ShaderResourceView* MaterialLane()
	{
		const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS_DOWNSCALED];
		if (!target.SRV)
			return nullptr;
		auto* srv = Util::AsReal(target.SRV);
		D3D11_TEXTURE2D_DESC desc{};
		return Util::GetTexture2DDesc(srv, desc) && desc.Format == DXGI_FORMAT_R16G16_UNORM ? srv : nullptr;
	}

	/**
	 * @brief Builds each eye's DLSSNR.UIAlpha from the Masks2 category lane for this frame.
	 * @param masks2 The deferred Masks2 SRV the categories are decoded from.
	 * @param strengths The per-category protection and edge softness CategoryAlphaCS.hlsl applies.
	 * @return False on a shader or texture failure; the caller then binds no protection.
	 */
	bool BuildMaterialAlpha(ID3D11ShaderResourceView* masks2, const NR::MaterialStrength::Values& strengths)
	{
		CS_GPU_PASS("Upscaling::NRMaterialAlpha");
		auto* shader = materialAlphaShader.Get(L"Data/Shaders/Upscaling/NeuralRendering/CategoryAlphaCS.hlsl",
			{}, "cs_5_0", "main", "NeuralRendering::CategoryAlpha CS");
		if (!shader)
			return false;
		try {
			for (uint32_t i = 0; i < eyeCount; ++i) {
				auto& eye = eyes[i];
				if (!eye.materialAlpha) {
					const auto name = eyeCount > 1 ? std::format("NeuralRendering::MaterialAlpha{}", i) : std::string("NeuralRendering::MaterialAlpha");
					eye.materialAlpha = interop.CreateTexture(width, height, DXGI_FORMAT_R8_UNORM, name);
				}
				context->ClearState();
				categoryAlphaBuffer->Update(CategoryAlphaData{ width, height, i * width, strengths.edgeSoftness,
					{ strengths.strength[NR::MaterialStrength::kNone], strengths.strength[NR::MaterialStrength::kSkin],
						strengths.strength[NR::MaterialStrength::kHair], strengths.strength[NR::MaterialStrength::kEyes] },
					{ strengths.strength[NR::MaterialStrength::kFoliage], strengths.strength[NR::MaterialStrength::kLandscape] } });
				auto buffer = categoryAlphaBuffer->CB();
				context->CSSetConstantBuffers(0, 1, &buffer);
				context->CSSetShaderResources(0, 1, &masks2);
				auto* output = eye.materialAlpha->uav;
				context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
				context->CSSetShader(shader, nullptr, 0);
				context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
				context->ClearState();
			}
		} catch (...) {
			if (interop.DeviceRemoved())
				throw;
			return false;
		}
		return true;
	}

	/**
	 * @brief Allocates the copies for a test capture of the next a_frames NR frames. Copying on the
	 *        GPU and writing only after the last frame keeps the captured frames free of readback stalls.
	 * @param a_before The texture the "before" copies are made from (its size and format).
	 * @param a_after The texture the "after" copies are made from.
	 * @param a_stage File-name stage prefix: "test" before upscaling, "final" on the final image.
	 * @return False when a copy texture cannot be made.
	 */
	bool StartTestCapture(uint32_t a_frames, ID3D11Resource* a_before, ID3D11Resource* a_after, const char* a_stage)
	{
		testFrames.clear();
		testFramesCopied = 0;
		testStage = a_stage;
		winrt::com_ptr<ID3D11Texture2D> beforeTexture, afterTexture;
		if (!a_before || !a_after || FAILED(a_before->QueryInterface(beforeTexture.put())) || FAILED(a_after->QueryInterface(afterTexture.put())))
			return false;
		const auto copyDesc = [](ID3D11Texture2D* a_source) {
			D3D11_TEXTURE2D_DESC desc{};
			a_source->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;
			return desc;
		};
		const auto beforeDesc = copyDesc(beforeTexture.get());
		const auto afterDesc = copyDesc(afterTexture.get());
		testFrames.resize(a_frames);
		for (auto& frame : testFrames) {
			if (FAILED(globals::d3d::device->CreateTexture2D(&beforeDesc, nullptr, frame.before.put())) ||
				FAILED(globals::d3d::device->CreateTexture2D(&afterDesc, nullptr, frame.after.put()))) {
				testFrames.clear();
				return false;
			}
			Util::SetResourceName(frame.before.get(), "NeuralRendering::TestBefore");
			Util::SetResourceName(frame.after.get(), "NeuralRendering::TestAfter");
		}
		return true;
	}

	/** @brief True once every frame of the running test capture has been copied. */
	bool TestCaptureComplete() const { return !testFrames.empty() && testFramesCopied == testFrames.size(); }

	/** @brief Copies this frame's before and after into the running test capture, if it still wants frames. */
	void CopyTestFrame(ID3D11Resource* a_before, ID3D11Resource* a_after, uint32_t a_frame)
	{
		if (testFramesCopied >= testFrames.size())
			return;
		auto& copy = testFrames[testFramesCopied++];
		context->CopyResource(copy.before.get(), a_before);
		context->CopyResource(copy.after.get(), a_after);
		copy.frame = a_frame;
	}

	/**
	 * @brief Measures the first eye's scene key from the original copy and eases the adapted key toward it,
	 *        the way Post Processing's histogram auto exposure adapts. Runs once per frame, before Prepare.
	 * @return The adaptation buffer Prepare reads, or nullptr when the pass is unavailable.
	 */
	ID3D11ShaderResourceView* UpdateSceneKey()
	{
		const uint32_t tilesX = (width + kSceneKeyTilePixels - 1) / kSceneKeyTilePixels;
		const uint32_t tilesY = (height + kSceneKeyTilePixels - 1) / kSceneKeyTilePixels;
		if (!original || !tilesX || !tilesY || tilesX * tilesY > kMaxSceneKeyTiles)
			return nullptr;
		if (!sceneKeyAdaptation) {
			sceneKeyBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SceneKeyData>(), "NeuralRendering::SceneKey CB");
			sceneKeyTiles = std::make_unique<StructuredBuffer>(StructuredBufferDesc<float2>(kMaxSceneKeyTiles, false), kMaxSceneKeyTiles, "NeuralRendering::SceneKey Tiles");
			sceneKeyTiles->CreateUAV();
			sceneKeyAdaptation = std::make_unique<StructuredBuffer>(StructuredBufferDesc<float>(1u, false), 1, "NeuralRendering::SceneKey Adaptation");
			sceneKeyAdaptation->CreateSRV();
			sceneKeyAdaptation->CreateUAV();
		}
		auto* reduce = sceneKeyReduce.Get(L"Data/Shaders/Upscaling/NeuralRendering/SceneKeyCS.hlsl", {}, "cs_5_0", "Reduce", "NeuralRendering::SceneKeyReduce CS");
		auto* adapt = sceneKeyAdapt.Get(L"Data/Shaders/Upscaling/NeuralRendering/SceneKeyCS.hlsl", {}, "cs_5_0", "Adapt", "NeuralRendering::SceneKeyAdapt CS");
		if (!reduce || !adapt)
			return nullptr;
		CS_GPU_PASS("Upscaling::NRSceneKey");
		const float realDelta = RE::BSTimer::GetSingleton()->realTimeDelta;
		const SceneKeyData data{ width, height, 0, tilesX * tilesY, tilesX, std::clamp(1.0f - std::exp(-realDelta * kSceneKeyAdaptSpeed), 0.0f, 1.0f) };
		sceneKeyBuffer->Update(data);
		context->ClearState();
		auto* buffer = sceneKeyBuffer->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
		auto* source = original->srv.get();
		context->CSSetShaderResources(0, 1, &source);
		ID3D11UnorderedAccessView* outputs[]{ sceneKeyTiles->UAV(0), sceneKeyAdaptation->UAV(0) };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->CSSetShader(reduce, nullptr, 0);
		context->Dispatch(tilesX, tilesY, 1);
		context->CSSetShader(adapt, nullptr, 0);
		context->Dispatch(1, 1, 1);
		context->ClearState();
		ReportSceneKey();
		return sceneKeyAdaptation->SRV(0);
	}

	/** @brief Logs the adapted key and the exposure it gives every kSceneKeyReportFrames, without waiting on the GPU. */
	void ReportSceneKey()
	{
		if (++sceneKeyFrames < kSceneKeyReportFrames)
			return;
		sceneKeyFrames = 0;
		if (!sceneKeyStaging) {
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = sizeof(float);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(float);
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, sceneKeyStaging.put())))
				return;
			Util::SetResourceName(sceneKeyStaging.get(), "NeuralRendering::SceneKey Readback");
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (sceneKeyStagingPending && SUCCEEDED(context->Map(sceneKeyStaging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
			const float key = *static_cast<const float*>(mapped.pData);
			context->Unmap(sceneKeyStaging.get(), 0);
			logger::info("[NeuralRendering] scene key {:.6f}: proxy exposure x{:.1f}", key, 0.18f / std::clamp(key, kSceneKeyMinimum, kSceneKeyMaximum));
		}
		winrt::com_ptr<ID3D11Resource> adaptation;
		sceneKeyAdaptation->SRV(0)->GetResource(adaptation.put());
		context->CopyResource(sceneKeyStaging.get(), adaptation.get());
		sceneKeyStagingPending = true;
	}

	/**
	 * @brief Picks this frame's proxy exposure: Post Processing's auto exposure while it runs, else the
	 *        scene key, else none. Runs after the original copy, which the scene key reads.
	 */
	void SelectExposure()
	{
		proxyExposure = {};
		auto source = ExposureSource::kUnit;
		if (!unitExposure) {
			if (auto* autoExposure = PostProcessingAutoExposure()) {
				// The same constants Post Processing's composite applies: 0.18 * 2^EV / clamp(adapted, 2^range).
				proxyExposure = { autoExposure->GetAdaptationSRV(), std::exp2(autoExposure->settings.ExposureCompensation),
					std::exp2(autoExposure->settings.AdaptationRange.x), std::exp2(autoExposure->settings.AdaptationRange.y) };
				source = ExposureSource::kPostProcessing;
			} else if (auto* key = UpdateSceneKey()) {
				proxyExposure = { key, 1.0f, kSceneKeyMinimum, kSceneKeyMaximum };
				source = ExposureSource::kSceneKey;
			}
		}
		if (!exposureSourceLogged || source != exposureSource) {
			static constexpr std::array<const char*, 3> kSourceNames{ "unit white point", "Post Processing auto exposure", "scene key" };
			logger::info("[NeuralRendering] proxy exposure: {}", kSourceNames[static_cast<size_t>(source)]);
			exposureSource = source;
			exposureSourceLogged = true;
		}
	}

	/** @brief Writes the finished test capture as lossless DDS through the diagnostics writer, then frees the copies. */
	void WriteTestCapture(NR::Diagnostics& diagnostics)
	{
		const auto before = std::format("{}_before", testStage);
		const auto after = std::format("{}_after", testStage);
		for (const auto& frame : testFrames) {
			diagnostics.DumpTexture(before.c_str(), frame.before.get(), frame.frame);
			diagnostics.DumpTexture(after.c_str(), frame.after.get(), frame.frame);
		}
		logger::info("[NeuralRendering] test capture written: {} {} frames", testFrames.size(), testStage);
		testFrames.clear();
		testFramesCopied = 0;
	}

	bool Draw(ID3D11Texture2D* color, ID3D11ShaderResourceView* const* inputs, ID3D11ComputeShader* shader, uint32_t reset, bool materialStrengthWanted, const NR::MaterialStrength::Values& materialStrengths, const NR::Tuning& tuning, NR::Diagnostics::Frame& diagnostic, NR::Diagnostics& diagnostics)
	{
		CS_GPU_PASS("Upscaling::NeuralRendering");
		captureDiagnostics = &diagnostics;
		captureFrame = diagnostic.number;
		const IsolatedContext scope(context.get(), isolated.get());
		const D3D11_BOX originalBox{ 0, 0, 0, width * eyeCount, height, 1 };
		context->CopySubresourceRegion(original->resource.get(), 0, 0, 0, 0, color, 0, &originalBox);
		SelectExposure();
		const bool capture = diagnostics.BeginCapture(diagnostic.number);
		if (capture)
			diagnostics.DumpTexture("00_original_scene", original->resource.get(), diagnostic.number);
		if (capture) {
			diagnostics.CaptureView("NR_depth", inputs[3], diagnostic.number);
			diagnostics.CaptureView("NR_motion", inputs[2], diagnostic.number);
		}
		for (uint32_t i = 0; i < eyeCount; ++i)
			TransferColor(i, true);
		if (capture)
			diagnostics.DumpTexture("01_input", eyes[0].color->resource11, diagnostic.number);
		if (debugOptions & NR::Diagnostics::BypassEvaluation) {
			diagnostic.outcome = NR::Diagnostics::Outcome::Bypassed;
			diagnostics.FinishCapture(diagnostic.number);
			return true;
		}
		materialStrengthInFlight = materialStrengthBound = false;
		if (materialStrengthWanted && !materialStrengthDegraded) {
			auto* lane = MaterialLane();
			if (!lane) {
				materialStrengthDegraded = true;
				logger::warn("[NeuralRendering] material strength unavailable: the deferred material lane is not present, so the whole frame is processed");
			} else if (BuildMaterialAlpha(lane, materialStrengths)) {
				materialStrengthInFlight = materialStrengthBound = true;
			} else {
				materialStrengthDegraded = true;
				logger::warn("[NeuralRendering] material strength unavailable: its shader or texture could not be created, so the whole frame is processed");
			}
		}
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetShaderResources(0, 4, inputs);
		globals::state->BindSharedDataCS(context.get(), true);
		{
			CS_GPU_PASS("Upscaling::NREncodeGuides");
			for (uint32_t i = 0; i < eyeCount; ++i) {
				auto& eye = eyes[i];
				diagnostic.reset[i] = UpdateFrame(i, reset, diagnostic);
				Upscaling::UpscalingDataCB data{ { float(guideWidth), float(guideHeight) }, i * guideWidth, 0 };
				encodeBuffer->Update(data);
				auto buffer = encodeBuffer->CB();
				context->CSSetConstantBuffers(0, 1, &buffer);
				// The shared encoder writes both masks even though NR only consumes motion and depth.
				ID3D11UnorderedAccessView* outputs[]{ encodeMasks[0]->uav.get(), encodeMasks[1]->uav.get(),
					eye.motion->uav, eye.depth->uav };
				context->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
				context->Dispatch((guideWidth + 7) / 8, (guideHeight + 7) / 8, 1);
				if (eye.frame.reset || (diagnostic.options & NR::Diagnostics::ZeroMotion)) {
					constexpr float zero[4]{};
					context->ClearUnorderedAccessViewFloat(eye.motion->uav, zero);
				}
			}
		}
		context->ClearState();
		// Resetting persistent NR history must not overlap its prior GPU evaluation.
		for (uint32_t i = 0; i < eyeCount; ++i) {
			if (eyes[i].frame.reset) {
				const auto started = std::chrono::steady_clock::now();
				interop.Drain();
				diagnostic.resetDrainMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
				break;
			}
		}
		bool success = true;
		{
			CS_GPU_PASS("Upscaling::NREvaluate");
			auto* commands = interop.Begin();
			for (uint32_t i = 0; i < eyeCount && success; ++i) {
				auto& eye = eyes[i];
				Transition(commands, eye, true);
				if (debugOptions & (NR::Diagnostics::InteropRoundTrip | NR::Diagnostics::CopyInputToOutput)) {
					D3D12_RESOURCE_BARRIER copyBarriers[2]{};
					copyBarriers[0].Type = copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					copyBarriers[0].Transition = { eye.color->resource.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
						D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE };
					copyBarriers[1].Transition = { eye.output->resource.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST };
					commands->ResourceBarrier(2, copyBarriers);
					commands->CopyResource(eye.output->resource.get(), eye.color->resource.get());
					copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
					copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
					copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
					copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
					commands->ResourceBarrier(2, copyBarriers);
				} else {
					// The encoder extracts render-resolution guides into zero-origin per-eye textures.
					const auto crop = region.active ? Util::Region::ClampToFrame(region.eye[i], width, height) : Util::Region::kEmptyRegion;
					const auto eyeRegion = (crop.w && crop.h) ? NR::ToGuideRegion(crop) : NR::GuideRegion{ 0, 0, width, height };
					NR::GuideParameters guides;
					guides.depth = eyeRegion;
					guides.motion = eyeRegion;
					guides.colorOutput = eyeRegion;
					// MotionBlur produces normalized eye-UV displacement; NR consumes input-pixel displacement.
					guides.motionScaleX = useResolutionMotionScale ? static_cast<float>(width) : 1.0f;
					guides.motionScaleY = useResolutionMotionScale ? static_cast<float>(height) : 1.0f;
					guides.depthInverted = false;
					guides.withoutDepth = withoutDepth;
					const NR::ProtectionResources protection{
						materialStrengthInFlight && eye.materialAlpha ? eye.materialAlpha->resource.get() : nullptr,
						// A protected pixel is restored from the NR input itself, so the alpha and the
						// backbuffer share the proxy colour domain the model works in.
						materialStrengthInFlight ? eye.color->resource.get() : nullptr
					};
					success = runtime.Evaluate(commands, i, eye.color->resource.get(), eye.depth->resource.get(),
						eye.motion->resource.get(), eye.output->resource.get(), protection,
						width, height, guides, eye.frame, tuning);
				}
				diagnostic.result[i] = eye.frame.result;
				if (success)
					diagnostic.evaluated |= 1u << i;
				if (eye.frame.created) {
					diagnostic.created |= 1u << i;
					diagnostic.reset[i] |= NR::Diagnostics::FeatureCreated;
				}
				Transition(commands, eye, false);
			}
			interop.End();
		}
		if (success)
			materialStrengthInFlight = false;
		if (diagnostic.options & NR::Diagnostics::SerializeGPU)
			interop.Drain();
		diagnostic.submittedFence = interop.SubmittedFence();
		diagnostic.completedFence = interop.CompletedFence();
		if (!success) {
			diagnostics.FinishCapture(diagnostic.number);
			return false;
		}
		if (capture) {
			interop.Drain();
			diagnostics.DumpTexture("02_output", eyes[0].output->resource11, diagnostic.number);
		}
		if (diagnostic.options & NR::Diagnostics::BypassWriteback) {
			diagnostics.FinishCapture(diagnostic.number);
			return true;
		}
		for (uint32_t i = 0; i < eyeCount; ++i) {
			PrepareToneData(i);
			TransferColor(i, false);
		}
		if (capture) {
			diagnostics.DumpTexture("03_pre_composite", original->resource.get(), diagnostic.number);
		}
		const D3D11_BOX box{ 0, 0, 0, width, height, 1 };
		for (uint32_t i = 0; i < eyeCount; ++i) {
			context->CopySubresourceRegion(color, 0, i * width, 0, 0, eyes[i].resolved->resource.get(), 0, &box);
			diagnostic.copied |= 1u << i;
		}
		if (capture)
			diagnostics.DumpTexture("04_post_composite", color, diagnostic.number);
		CopyTestFrame(original->resource.get(), eyes[0].resolved->resource.get(), diagnostic.number);
		// Capture stays open for Main_PostProcessing's pre-SR/post-SR stages; CaptureAfterUpscaling finishes it.
		captureDiagnostics = nullptr;
		return true;
	}

	/**
	 * @brief One frame of the final-image placement: copies the HUD-less frame, builds the output-size
	 *        input and guides from it and from DLSS-G's depth and motion, evaluates the runtime in
	 *        kFinalImageSlot, and writes the model's output back into the HUD-less frame, mixed with the
	 *        original by the user's share. EnsureFinalResources must have run for this frame's size.
	 * @param a_reset History reset reasons from the caller; the camera checks add theirs as the other placement does.
	 * @return False when the runtime failed to evaluate; the HUD-less frame is then left as it was.
	 */
	bool DrawFinal(ID3D11Texture2D* a_hudless, ID3D11UnorderedAccessView* a_hudlessUAV, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_motion,
		uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_reset, uint32_t a_options, const NR::Tuning& a_tuning, uint32_t a_frame)
	{
		CS_GPU_PASS("Upscaling::NeuralRenderingFinal");
		auto* prepare = finalPrepare.Get(L"Data/Shaders/Upscaling/NeuralRendering/FinalImageCS.hlsl", {}, "cs_5_0", "Prepare", "NeuralRendering::FinalImagePrepare CS");
		auto* composite = finalComposite.Get(L"Data/Shaders/Upscaling/NeuralRendering/FinalImageCS.hlsl", {}, "cs_5_0", "Composite", "NeuralRendering::FinalImageComposite CS");
		if (!prepare || !composite)
			throw std::runtime_error("NR final-image shader unavailable");
		auto& pass = finalImage;
		if (!finalSampler) {
			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			winrt::check_hresult(globals::d3d::device->CreateSamplerState(&samplerDesc, finalSampler.put()));
			Util::SetResourceName(finalSampler.get(), "NeuralRendering::FinalImage Sampler");
		}
		auto* sampler = finalSampler.get();
		const IsolatedContext scope(context.get(), isolated.get());
		context->CopyResource(pass.original->resource.get(), a_hudless);
		NR::Diagnostics::CameraSample camera;
		UpdateCamera(0, pass.position, pass.forward, pass.frame, a_reset, a_options, camera, false);
		FinalImageData data{ pass.outputWidth, pass.outputHeight, a_renderWidth, a_renderHeight, std::clamp(mix, 0.0f, NR::Tuning::kMaxMix), pass.width, pass.height };
		finalImageBuffer->Update(data);
		auto* buffer = finalImageBuffer->CB();
		{
			CS_GPU_PASS("Upscaling::NRFinalPrepare");
			context->CSSetConstantBuffers(0, 1, &buffer);
			context->CSSetSamplers(0, 1, &sampler);
			ID3D11ShaderResourceView* inputs[]{ pass.original->srv.get(), a_depth, a_motion };
			ID3D11UnorderedAccessView* outputs[]{ pass.color->uav, pass.depth->uav, pass.motion->uav };
			context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
			context->CSSetShader(prepare, nullptr, 0);
			context->Dispatch((pass.width + 7) / 8, (pass.height + 7) / 8, 1);
			if (pass.frame.reset || (a_options & NR::Diagnostics::ZeroMotion)) {
				constexpr float zero[4]{};
				context->ClearUnorderedAccessViewFloat(pass.motion->uav, zero);
			}
			context->ClearState();
		}
		// Resetting persistent NR history must not overlap its prior GPU evaluation.
		if (pass.frame.reset)
			interop.Drain();
		bool success = false;
		{
			CS_GPU_PASS("Upscaling::NRFinalEvaluate");
			auto* commands = interop.Begin();
			Transition(commands, pass.color->resource.get(), pass.depth->resource.get(), pass.motion->resource.get(), pass.output->resource.get(), nullptr, true);
			const NR::GuideRegion whole{ 0, 0, pass.width, pass.height };
			NR::GuideParameters guides;
			guides.depth = whole;
			guides.motion = whole;
			guides.colorOutput = whole;
			// The engine's motion is a normalized UV displacement, the same at any resolution; NR consumes input pixels.
			guides.motionScaleX = static_cast<float>(pass.width);
			guides.motionScaleY = static_cast<float>(pass.height);
			guides.depthInverted = false;
			success = runtime.Evaluate(commands, kFinalImageSlot, pass.color->resource.get(), pass.depth->resource.get(), pass.motion->resource.get(),
				pass.output->resource.get(), {}, pass.width, pass.height, guides, pass.frame, a_tuning);
			Transition(commands, pass.color->resource.get(), pass.depth->resource.get(), pass.motion->resource.get(), pass.output->resource.get(), nullptr, false);
			interop.End();
		}
		if (!success)
			return false;
		{
			CS_GPU_PASS("Upscaling::NRFinalComposite");
			context->CSSetConstantBuffers(0, 1, &buffer);
			context->CSSetSamplers(0, 1, &sampler);
			ID3D11ShaderResourceView* inputs[]{ pass.original->srv.get(), pass.output->srv, pass.color->srv };
			context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
			context->CSSetUnorderedAccessViews(0, 1, &a_hudlessUAV, nullptr);
			context->CSSetShader(composite, nullptr, 0);
			context->Dispatch((pass.outputWidth + 7) / 8, (pass.outputHeight + 7) / 8, 1);
			context->ClearState();
		}
		CopyTestFrame(pass.original->resource.get(), a_hudless, a_frame);
		pass.lastFrame = a_frame;
		return true;
	}
};

NeuralRendering::NeuralRendering() :
	impl(std::make_unique<Impl>()) {}
NeuralRendering::~NeuralRendering() = default;

void NeuralRendering::InstallHooks()
{
	// Stage 1: no actor-tracking hook; the crop is not part of the 05-29 port yet.
}

void NeuralRendering::UpdateCalibration()
{
	auto& upscaling = globals::features::upscaling;
	const auto resources = GetStatus();
	if (upscaling.IsFrameGenerationActive()) {
		calibration.Fail(NR::CropCalibration::Failure::kFrameGeneration);
	} else if (publishedState.load(std::memory_order_relaxed) != Status::State::kActive || !resources.eyes || !resources.width || !resources.height) {
		calibration.Fail(NR::CropCalibration::Failure::kNotActive);
	} else {
		constexpr float gpuMs = 0.0f;  // no GPU profiler in the 05-29 tree
		calibration.AddFrame(gpuMs);
	}
	Util::Region::StereoRegion forced;
	if (calibration.Running() && calibration.CurrentFraction() < 1.0f) {
		const auto bounds = NR::CenteredBounds(calibration.CurrentFraction());
		for (uint32_t eye = 0; eye < resources.eyes; ++eye)
			forced.eye[eye] = Util::Region::PixelRegionFromBounds(bounds, resources.width, resources.height, NR::ActorRegion::kTightPadding);
		forced.active = true;
	}
	if (calibration.GetResult().state == NR::CropCalibration::State::kDone)
		calibratedKneeFraction = calibration.GetResult().kneeFraction;
	std::scoped_lock lock(regionMutex);
	regionStabilizer.Reset();
	region = forced;
	actorBox = {};
	calibrationResult = calibration.GetResult();
}

NR::CropCalibration::Result NeuralRendering::GetCalibration() const
{
	std::scoped_lock lock(regionMutex);
	return calibrationResult;
}

void NeuralRendering::UpdateRegionOfInterest()
{
	if (calibrationRequested.exchange(false, std::memory_order_relaxed)) {
		calibration.Start();
		std::scoped_lock lock(regionMutex);
		calibrationResult = calibration.GetResult();
	}
	if (calibration.Running()) {
		UpdateCalibration();
		return;
	}
	trackedActor = {};
	std::scoped_lock lock(regionMutex);
	regionStabilizer.Reset();
	region = {};
	actorBox = {};
}

Util::Region::StereoRegion NeuralRendering::GetRegionOfInterest() const
{
	std::scoped_lock lock(regionMutex);
	return region;
}

Util::Region::StereoRegion NeuralRendering::GetActorBox() const
{
	std::scoped_lock lock(regionMutex);
	return actorBox;
}

const char* NeuralRendering::RegionSourceName(RegionSource a_source)
{
	switch (a_source) {
	case RegionSource::kActor:
		return "actor";
	case RegionSource::kFovea:
		return "fovea";
	case RegionSource::kBoth:
		return "both";
	default:
		return "none";
	}
}

bool NeuralRendering::DialogueOpen()
{
	return globals::game::ui && globals::game::ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
}

void NeuralRendering::SetupResources() { retryRequested = recreate = resetHistory = true; }
void NeuralRendering::SetTestCapture(uint32_t a_frames) { testCaptureFrames.store(std::min(a_frames, kMaxTestCaptureFrames), std::memory_order_relaxed); }
void NeuralRendering::SetUnitExposure(bool a_unit) { unitExposure.store(a_unit, std::memory_order_relaxed); }
void NeuralRendering::SetTestCycle(uint32_t a_seconds, bool a_placements, bool a_variants, bool a_scales, bool a_mixes, bool a_conditions)
{
	testCycleConditions.store(a_conditions, std::memory_order_relaxed);
	testCycleMixes.store(a_mixes, std::memory_order_relaxed);
	testCycleSeconds.store(a_seconds ? std::max(a_seconds, kMinTestCycleSeconds) : 0, std::memory_order_relaxed);
	testCyclePlacements.store(a_placements, std::memory_order_relaxed);
	testCycleVariants.store(a_variants, std::memory_order_relaxed);
	testCycleScales.store(a_scales, std::memory_order_relaxed);
}

const char* NeuralRendering::PlacementName(Placement a_placement)
{
	return a_placement == Placement::kFinalImage ? "final" : "before";
}

const char* NeuralRendering::FinalImageUnavailableReason()
{
	auto& upscaling = globals::features::upscaling;
	if (globals::game::isVR)
		return "VR has no final-image pass";
	// The pass rides on DLSS-G's UI composition, the only point with the frame before its UI.
	if (!upscaling.UsesDLSSGFrameGen() || !upscaling.settings.frameGenerationMode)
		return "it needs DLSS frame generation";
	if (globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR)
		return "HDR output is not supported";
	return nullptr;
}

void NeuralRendering::ReportTestCycleTurn(std::chrono::steady_clock::time_point a_now)
{
	// Taken every turn so a turn's GPU time never carries into the next.
	const auto beforeTimes = impl->passTimer.TakeTurn();
	const auto finalTimes = impl->finalPassTimer.TakeTurn();
	if (!testCycleWindowOpen)
		return;
	testCycleWindowOpen = false;
	const double seconds = std::chrono::duration<double>(a_now - testCycleWindowStart).count();
	if (seconds <= 0.0 || !testCycleWindowFrames)
		return;
	const double realFps = testCycleWindowFrames / seconds;
	// Without DLSS-G nothing is generated, so every rendered frame is the one presented.
	const double outputFps = testCycleWindowPresented ? testCycleWindowPresented / seconds : realFps;
	const auto& timer = finalTimes.samples ? finalTimes : beforeTimes;
	const std::string gpu = timer.samples ? std::format("NR GPU avg {:.3f} ms (min {:.3f}, max {:.3f}) over {} frames", timer.sumMs / timer.samples, timer.minMs, timer.maxMs, timer.samples) :
	                                        std::string("no NR GPU time");
	// Reflex's GPU frame interval is the cross-check: it should match 1000 / real fps.
	const std::string latency = testCycleLatencyReports ?
	                                std::format("Reflex PC latency {:.1f} ms, GPU frame {:.2f} ms over {} reports", testCycleLatencyMs / testCycleLatencyReports,
										testCycleGpuFrameMs / testCycleLatencyReports, testCycleLatencyReports) :
	                                std::string("no Reflex latency");
	logger::info("[NeuralRendering] test cycle: turn {} ({}) over {:.1f} s: real {:.1f} fps, output {:.1f} fps ({:.2f} presented per frame), {}, {}",
		testCycleTurn, testCycleTurnName, seconds, realFps, outputFps, testCycleWindowPresented ? static_cast<double>(testCycleWindowPresented) / testCycleWindowFrames : 1.0, gpu,
		latency);
}

bool NeuralRendering::ApplyTestCycle(bool a_enabled, Placement& a_placement)
{
	const auto seconds = testCycleSeconds.load(std::memory_order_relaxed);
	if (!a_enabled || !seconds || !testCaptureFrames.load(std::memory_order_relaxed) || !globals::state->worldRenderedThisFrame)
		return a_enabled;
	const bool conditions = testCycleConditions.load(std::memory_order_relaxed);
	const bool mixes = !conditions && testCycleMixes.load(std::memory_order_relaxed);
	const bool scales = !mixes && testCycleScales.load(std::memory_order_relaxed);
	const bool variants = !mixes && !scales && testCycleVariants.load(std::memory_order_relaxed);
	const bool placements = !mixes && !scales && !variants && testCyclePlacements.load(std::memory_order_relaxed);
	const auto now = std::chrono::steady_clock::now();
	if (testCycleTurn == UINT32_MAX) {
		testCycleStart = now;
		if (conditions)
			logger::info("[NeuralRendering] test cycle: {} s turns through {} CS feature conditions, NR on then off in each", seconds, std::size(kTestConditions));
		else if (mixes)
			logger::info("[NeuralRendering] test cycle: {} s turns of NR on the final image through {} mix and tone / structure pairs, each followed by a turn without NR", seconds, std::size(kTestCycleMixes));
		else if (scales)
			logger::info("[NeuralRendering] test cycle: {} s turns of NR on the final image at {} working scales, each followed by a turn without NR", seconds, std::size(kTestCycleScales));
		else if (variants)
			logger::info("[NeuralRendering] test cycle: {} s turns of NR before upscaling through {} variants, each followed by a turn without NR", seconds, std::size(kTestCycleVariants));
		else if (placements)
			logger::info("[NeuralRendering] test cycle: {} s turns of NR before upscaling, none, NR on the final image, none", seconds);
		else
			logger::info("[NeuralRendering] test cycle: {} s with NR, {} s without, in turns", seconds, seconds);
	}
	const double elapsed = std::chrono::duration<double>(now - testCycleStart).count();
	const auto turn = static_cast<uint32_t>(elapsed / seconds);
	const bool run = turn % 2 == 0;
	if (run && placements)
		a_placement = turn % 4 == 0 ? Placement::kBeforeUpscaling : Placement::kFinalImage;
	testCycleVariant = -1;
	testCycleScale = -1;
	testCycleMix = -1;
	// Conditions: turns 2k and 2k+1 share condition k (NR on, then off), so each condition has its own NR-off frame.
	// The turn's name outlives this call (testCycleTurnName keeps the pointer), so it lives in a static string.
	static std::string conditionName;
	if (conditions) {
		const auto condition = static_cast<int32_t>((turn / 2) % std::size(kTestConditions));
		if (condition != testCycleCondition)
			ApplyTestCondition(condition);
		auto name = std::format("{}-{}", kTestConditions[condition].name, run ? "on" : "off");
		if (name != conditionName)
			conditionName = std::move(name);
	}
	if (run && mixes) {
		a_placement = Placement::kFinalImage;
		testCycleMix = static_cast<int32_t>((turn / 2) % std::size(kTestCycleMixes));
	}
	if (run && variants) {
		a_placement = Placement::kBeforeUpscaling;
		testCycleVariant = static_cast<int32_t>((turn / 2) % std::size(kTestCycleVariants));
	}
	if (run && scales) {
		a_placement = Placement::kFinalImage;
		testCycleScale = static_cast<int32_t>((turn / 2) % std::size(kTestCycleScales));
	}
	const char* name = conditions            ? conditionName.c_str() :
	                   !run                  ? "off" :
	                   testCycleMix >= 0     ? kTestCycleMixes[testCycleMix].name :
	                   testCycleScale >= 0   ? kTestCycleScales[testCycleScale].name :
	                   testCycleVariant >= 0 ? kTestCycleVariants[testCycleVariant].name :
	                   placements            ? PlacementName(a_placement) :
	                                           "on";
	if (turn != testCycleTurn) {
		if (testCycleTurn != UINT32_MAX)
			ReportTestCycleTurn(now);
		testCycleTurn = turn;
		testCycleTurnName = name;
		testCycleDumpRequested = false;
		testCyclePairRequested = false;
		testCycleWindowFrames = 0;
		testCycleWindowPresented = 0;
		testCycleLatencyMs = testCycleGpuFrameMs = 0.0;
		testCycleLatencyReports = 0;
		logger::info("[NeuralRendering] test cycle: turn {}, NR {}", turn, name);
	}
	// Counted from kTestCycleSettleSeconds in, so a placement switch and NR's warm-up stay out of the numbers.
	const double intoTurn = elapsed - static_cast<double>(turn) * seconds;
	const auto frame = globals::state->frameCount;
	if (intoTurn >= kTestCycleSettleSeconds && frame != testCycleCountedFrame) {
		testCycleCountedFrame = frame;
		if (!testCycleWindowOpen) {
			testCycleWindowOpen = true;
			testCycleWindowStart = now;
			// The GPU time of the settling seconds goes with them.
			impl->passTimer.TakeTurn();
			impl->finalPassTimer.TakeTurn();
		} else {
			// The window's first frame only starts the clock: frames are counted as intervals.
			++testCycleWindowFrames;
			auto& upscaling = globals::features::upscaling;
			if (upscaling.UsesDLSSGFrameGen() && upscaling.IsFrameGenerationActive())
				testCycleWindowPresented += upscaling.streamlineDX12.lastDLSSGFramesPresented;
			// Reflex keeps the last 64 frames' reports, so a sample every 60 frames covers the window without gaps.
			Streamline::ReflexLatency latency;
			if (testCycleWindowFrames % 60 == 0 && upscaling.UsesDLSSGFrameGen() && upscaling.streamlineDX12.SampleReflexLatency(latency)) {
				testCycleLatencyMs += latency.pcLatencyMs * latency.frames;
				testCycleGpuFrameMs += latency.gpuFrameMs * latency.frames;
				testCycleLatencyReports += latency.frames;
			}
		}
	}
	// The turn's last 1.5 s: NR's history, or its absence, has settled by then.
	// Variant cycle: one more frame 1.5 s earlier, so each turn has a still-camera pair to measure flicker on.
	if ((variants || scales || mixes || conditions) && !testCyclePairRequested && intoTurn >= seconds - 3.0) {
		testCyclePairRequested = true;
		globals::features::upscaling.dx12SwapChain.RequestTestDump(std::format(L"t{:02}-{}-a", turn, std::wstring(name, name + std::strlen(name))));
	}
	if (!testCycleDumpRequested && intoTurn >= seconds - 1.5) {
		testCycleDumpRequested = true;
		globals::features::upscaling.dx12SwapChain.RequestTestDump(std::format(L"t{:02}-{}", turn, std::wstring(name, name + std::strlen(name))));
		logger::info("[NeuralRendering] test cycle: turn {} final frame requested", turn);
	}
	return run;
}
void NeuralRendering::ApplyTestCondition(int32_t a_index)
{
	auto& upscaling = globals::features::upscaling;
	if (testCycleCondition < 0) {
		// The settings before the first condition: every condition is applied on top of these.
		for (const auto& condition : kTestConditions)
			for (const auto& [featureName, patch] : json::parse(condition.patches).items())
				for (auto* feature : Feature::GetFeatureList())
					if (feature->GetName() == featureName && !testCycleOriginals.contains(featureName)) {
						json current;
						feature->SaveSettings(current);
						testCycleOriginals[featureName] = current;
					}
		testCycleOriginalSharpness = upscaling.settings.sharpnessDLSS;
	}
	testCycleCondition = a_index;
	const auto& condition = kTestConditions[a_index];
	const auto patches = json::parse(condition.patches);
	for (auto* feature : Feature::GetFeatureList()) {
		const auto original = testCycleOriginals.find(feature->GetName());
		if (original == testCycleOriginals.end())
			continue;
		json settings = original->second;
		if (patches.contains(feature->GetName()))
			settings.merge_patch(patches[feature->GetName()]);
		try {
			feature->LoadSettings(settings);
		} catch (const std::exception& error) {
			logger::warn("[NeuralRendering] test condition {}: {} rejected its settings: {}", condition.name, feature->GetName(), error.what());
		}
	}
	upscaling.settings.sharpnessDLSS = condition.sharpness >= 0.0f ? condition.sharpness : testCycleOriginalSharpness;
	logger::info("[NeuralRendering] test condition {}: {} (DLSS sharpening {:.2f})", condition.name, condition.patches, upscaling.settings.sharpnessDLSS);
}

void NeuralRendering::ReportStatus()
{
	static constexpr double kStatusSeconds = 60.0;
	if (testCycleSeconds.load(std::memory_order_relaxed) || !globals::state)
		return;
	const auto frame = globals::state->frameCount;
	if (frame == statusLastFrame)
		return;
	statusLastFrame = frame;
	const auto now = std::chrono::steady_clock::now();
	if (!statusFrames && !statusPresented)
		statusWindowStart = now;
	++statusFrames;
	auto& upscaling = globals::features::upscaling;
	if (upscaling.UsesDLSSGFrameGen() && upscaling.IsFrameGenerationActive())
		statusPresented += upscaling.streamlineDX12.lastDLSSGFramesPresented;
	const double seconds = std::chrono::duration<double>(now - statusWindowStart).count();
	if (seconds < kStatusSeconds)
		return;
	const auto before = impl->passTimer.TakeTurn();
	const auto finalImage = impl->finalPassTimer.TakeTurn();
	const auto& gpu = finalImage.samples ? finalImage : before;
	const std::string gpuText = gpu.samples ? std::format("NR GPU avg {:.2f} ms (max {:.2f}) on the {}", gpu.sumMs / gpu.samples, gpu.maxMs,
	                                              finalImage.samples ? "final image" : "image before upscaling") :
	                                          std::string("NR not run");
	logger::info("[NeuralRendering] status over {:.0f} s: NR {}, real {:.1f} fps, output {:.1f} fps, {}, last NGX result 0x{:08X}", seconds,
		magic_enum::enum_name(publishedState.load(std::memory_order_relaxed)), statusFrames / seconds,
		(statusPresented ? statusPresented : statusFrames) / seconds, gpuText, lastNgxResult[0].load(std::memory_order_relaxed));
	statusFrames = statusPresented = 0;
}
void NeuralRendering::ResetHistory() { resetHistory = true; }
void NeuralRendering::ClearShaderCache() { retryRequested = clearShaders = resetHistory = true; }

void NeuralRendering::Reset(bool enabled, bool regionOfInterest, uint32_t cropFit, bool cropGroup)
{
	regionEnabled.store(enabled && regionOfInterest, std::memory_order_relaxed);
	regionFit.store(cropFit, std::memory_order_relaxed);
	regionGroup.store(cropGroup, std::memory_order_relaxed);
	diagnostics.SetDeveloperMode(globals::state->IsDeveloperMode());
	if (enabled)
		diagnostics.EndFrame(globals::state->frameCount, globals::state->worldRenderedThisFrame, globals::state->IsPausedOrMenuOpen(globals::game::ui));
	if (!enabled)
		retryRequested = true;
	if (!enabled || !globals::state->worldRenderedThisFrame)
		resetHistory = true;
}

void NeuralRendering::PublishStatus(Status::State state, std::string text)
{
	Status next;
	next.state = state;
	next.text = std::move(text);
	next.failed = state == Status::State::kFailed;
	next.runtimeVersion = impl->runtime.Version();
	std::scoped_lock lock(statusMutex);
	status = std::move(next);
	publishedState.store(state, std::memory_order_relaxed);
	PublishResourcesLocked();
}

void NeuralRendering::PublishFailure(const std::string& detail)
{
	PublishStatus(Status::State::kFailed, std::format("{} {}", T(TKEY("status_failed"), "Neural Rendering stopped:"), detail));
}

void NeuralRendering::PublishResources()
{
	std::scoped_lock lock(statusMutex);
	PublishResourcesLocked();
}

void NeuralRendering::PublishResourcesLocked()
{
	// The final-image pass reports its output size as one eye; the other placement its render size.
	const auto& pass = impl->finalImage;
	status.width = pass.width ? pass.width : impl->width;
	status.height = pass.width ? pass.height : impl->height;
	status.eyes = pass.width ? 1 : impl->eyeCount;
}

NeuralRendering::Status NeuralRendering::GetStatus() const
{
	std::scoped_lock lock(statusMutex);
	auto snapshot = status;
	if (snapshot.text.empty())
		snapshot.text = T(TKEY("status_off"), "Off");
	snapshot.lastAppliedFrame = appliedFrame.load(std::memory_order_relaxed);
	snapshot.appliedFrames = appliedFrames.load(std::memory_order_relaxed);
	snapshot.ngxResult = { lastNgxResult[0].load(std::memory_order_relaxed), lastNgxResult[1].load(std::memory_order_relaxed) };
	snapshot.materialStrengthActive = materialStrengthActive.load(std::memory_order_relaxed);
	snapshot.materialStrengthAvailable = materialStrengthAvailable.load(std::memory_order_relaxed);
	for (size_t i = 0; i < snapshot.materialStrength.size(); ++i)
		snapshot.materialStrength[i] = materialStrengthValues[i].load(std::memory_order_relaxed);
	snapshot.materialEdgeSoftness = materialEdgeSoftness.load(std::memory_order_relaxed);
	return snapshot;
}

NR::RuntimeAvailability NeuralRendering::GetRuntimeAvailability() const
{
	return NR::InspectRuntime(RuntimeDirectory());
}

void NeuralRendering::LogRuntimeCheck() const
{
	const auto availability = GetRuntimeAvailability();
	if (availability.state == NR::RuntimeAvailability::State::kReady)
		logger::info("[NeuralRendering] runtime check: nvngx_dlssnr.dll {} accepted (validated SHA-256); Neural Rendering can be switched on", availability.version);
	else
		logger::warn("[NeuralRendering] runtime check: Neural Rendering unavailable: {}. It stays off; DLSS, frame generation and the rest of Community Shaders run normally.",
			availability.reason);
}

void NeuralRendering::DrawRuntimeDiagnostics() const
{
	// The Streamline table lists this file's version like any other DLL in that folder; the
	// verdict is what says whether that version is one the pass will actually load.
	const auto availability = GetRuntimeAvailability();
	if (availability.Ready()) {
		Util::Text::Success(T(TKEY("runtime_validated"), "Neural Rendering runtime: %s %s is a validated build."), NR::kRuntimeFileName, availability.version.c_str());
		return;
	}
	Util::Text::WrappedWarning(T(TKEY("runtime_unavailable"), "Neural Rendering runtime: %s"), availability.reason.c_str());
	if (availability.AllowsLoad(globals::state && globals::state->IsDeveloperMode()))
		Util::Text::Disabled("%s", T(TKEY("runtime_developer_load"), "Loading this build because developer mode is on; its output is unverified."));
	else
		Util::Text::Disabled("%s", RuntimeFixHint(availability.state));
}

namespace
{
	constexpr uint32_t kMaterialChecklistColumns = 3;
	constexpr float kStrengthLegendWidthFraction = 0.6f;
	using MaterialSelection = std::array<uint8_t, NR::MaterialMap::kBits>;

	/** @brief NeuralRenderingCategory ids in the order the material checklists list them. */
	constexpr std::array<uint32_t, NR::MaterialMap::kBits> kMaterialListOrder{
		NR::MaterialStrength::kSkin, NR::MaterialStrength::kHair, NR::MaterialStrength::kEyes,
		NR::MaterialStrength::kFoliage, NR::MaterialStrength::kLandscape, NR::MaterialStrength::kNone
	};

	ImU32 ToColor(const NR::MaterialMap::Color& color)
	{
		return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, 1.0f));
	}

	/** @brief Draws one checkbox per material, three to a row, optionally led by its map colour; returns true when one changed. */
	bool DrawMaterialChecklist(MaterialSelection& selected, bool colourSwatches)
	{
		const std::array<const char*, NR::MaterialMap::kBits> labels{
			T(TKEY("category_skin"), "Skin"), T(TKEY("category_hair"), "Hair"), T(TKEY("category_eyes"), "Eyes"),
			T(TKEY("category_foliage"), "Foliage"), T(TKEY("category_landscape"), "Landscape"), T(TKEY("material_other"), "Everything Else")
		};
		bool changed = false;
		for (uint32_t i = 0; i < selected.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			if (i % kMaterialChecklistColumns != 0)
				ImGui::SameLine();
			if (colourSwatches) {
				const auto& color = NR::MaterialMap::kColors[kMaterialListOrder[i]];
				ImGui::ColorButton("##swatch", ImVec4(color.r, color.g, color.b, 1.0f),
					ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
					ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
				ImGui::SameLine();
			}
			bool value = selected[i] != 0;
			if (ImGui::Checkbox(labels[i], &value)) {
				selected[i] = value ? 1 : 0;
				changed = true;
			}
			ImGui::PopID();
		}
		return changed;
	}

	/** @brief Select All and Select None for a material checklist; returns true when a button changed it. */
	bool DrawMaterialSelectionButtons(MaterialSelection& selected)
	{
		const auto before = selected;
		Util::DrawSelectionButtons(selected,
			T("feature.scene_manager.action.select_all", "Select All"),
			T("feature.scene_manager.action.select_none", "Select None"));
		return selected != before;
	}

	/** @brief Draws the colour bar the Strength map mode reads against, 0 at the left to 1 at the right. */
	void DrawStrengthLegend()
	{
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x * kStrengthLegendWidthFraction;
		const float height = ImGui::GetTextLineHeight();
		const auto& ramp = NR::MaterialMap::kStrengthRamp;
		const float segment = width / static_cast<float>(std::size(ramp) - 1);
		auto* drawList = ImGui::GetWindowDrawList();
		for (size_t i = 0; i + 1 < std::size(ramp); ++i) {
			const float left = origin.x + segment * static_cast<float>(i);
			drawList->AddRectFilledMultiColor(ImVec2(left, origin.y), ImVec2(left + segment, origin.y + height),
				ToColor(ramp[i]), ToColor(ramp[i + 1]), ToColor(ramp[i + 1]), ToColor(ramp[i]));
		}
		ImGui::Dummy(ImVec2(width, height));
		ImGui::TextUnformatted(T(TKEY("material_map_strength_legend"), "Strength: 0 at the left, 1 at the right"));
	}

	/** @brief Draws the by-material selection and the material map controls; returns true when any value changed. */
	bool DrawMaterialControls(NR::Tuning& tuning, bool materialStrengthAvailable)
	{
		bool changed = false;
		ImGui::TextUnformatted(T(TKEY("material_apply_to"), "Apply Neural Rendering To"));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("material_apply_to_tooltip"),
				"Choose the materials Neural Rendering is applied to, using the labels the deferred pass writes. All selected is the normal behaviour. It changes where the effect shows, not how much GPU time it costs, and needs the deferred pass. Fine-Tune Strengths sets partial amounts."));
		ImGui::PushID("materialSelection");
		MaterialSelection selected{};
		for (uint32_t i = 0; i < selected.size(); ++i)
			selected[i] = tuning.MaterialSelected(kMaterialListOrder[i]) ? 1 : 0;
		const auto before = selected;
		DrawMaterialSelectionButtons(selected);
		ImGui::SameLine();
		if (ImGui::SmallButton(T(TKEY("material_characters_only"), "Characters Only")))
			for (uint32_t i = 0; i < selected.size(); ++i)
				selected[i] = NR::MaterialStrength::kCharactersOnly[kMaterialListOrder[i]] > NR::MaterialStrength::kMinStrength ? 1 : 0;
		DrawMaterialChecklist(selected, false);
		ImGui::PopID();
		for (uint32_t i = 0; i < selected.size(); ++i) {
			if (selected[i] != before[i]) {
				tuning.SetMaterialSelected(kMaterialListOrder[i], selected[i] != 0);
				changed = true;
			}
		}
		if (ImGui::TreeNodeEx(T(TKEY("material_strengths"), "Fine-Tune Strengths"), ImGuiTreeNodeFlags_None)) {
			ImGui::PushID("materialStrength");
			bool strengthChanged = false;
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_skin"), "Skin"), &tuning.strengthSkin, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_hair"), "Hair"), &tuning.strengthHair, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_eyes"), "Eyes"), &tuning.strengthEyes, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_foliage"), "Foliage"), &tuning.strengthFoliage, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_landscape"), "Landscape"), &tuning.strengthLandscape, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("material_other"), "Everything Else"), &tuning.strengthOther, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (strengthChanged) {
				tuning.SyncMaterialSwitch();
				changed = true;
			}
			int edgeSoftness = static_cast<int>(tuning.strengthEdgeSoftness);
			if (ImGui::SliderInt(T(TKEY("edge_softness"), "Edge Softness"), &edgeSoftness, 0, static_cast<int>(NR::MaterialStrength::kMaxEdgeSoftness))) {
				tuning.strengthEdgeSoftness = static_cast<uint32_t>(edgeSoftness);
				changed = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("edge_softness_tooltip"),
					"Blends each material's strength into its neighbours over this many pixels; 0 keeps a hard boundary between materials."));
			ImGui::PopID();
			ImGui::TreePop();
		}
		if (tuning.materialStrength && !materialStrengthAvailable)
			Util::Text::WrappedWarning("%s", T(TKEY("material_unavailable"), "Neural Rendering by material is unavailable this session, so the whole frame is processed. Check the log; restarting the game retries."));
		if (ImGui::Checkbox(T(TKEY("material_map"), "Show Material Map"), &tuning.showMaterialMap))
			changed = true;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("material_map_tooltip"),
				"Tints each pixel by the material the deferred pass labelled it, so a character the classification mislabels is visible. The filter picks which materials show; the mode picks the category's colour or a colour ramp of the strength the by-material selection gives that pixel, read at the pixel without Edge Softness. Needs the deferred pass; unlabelled pixels follow Everything Else."));
		ImGui::BeginDisabled(!tuning.showMaterialMap);
		ImGui::PushID("materialMap");
		int materialMapMode = static_cast<int>(std::min(tuning.materialMapMode, NR::MaterialMap::kMaxMode));
		const std::array<const char*, NR::MaterialMap::kMaxMode + 1> materialMapModeLabels{
			T(TKEY("material_map_category"), "Category colours"),
			T(TKEY("material_map_strength"), "Strength"),
		};
		if (ImGui::Combo(T(TKEY("material_map_mode"), "Map Mode"), &materialMapMode, materialMapModeLabels.data(), static_cast<int>(materialMapModeLabels.size()))) {
			tuning.materialMapMode = static_cast<uint32_t>(materialMapMode);
			changed = true;
		}
		if (tuning.materialMapMode == static_cast<uint32_t>(NR::MaterialMap::Mode::kStrength))
			DrawStrengthLegend();
		ImGui::TextUnformatted(T(TKEY("material_map_filter"), "Show"));
		MaterialSelection shown{};
		for (uint32_t i = 0; i < shown.size(); ++i)
			shown[i] = NR::MaterialMap::Contains(tuning.materialMapFilter, kMaterialListOrder[i]) ? 1 : 0;
		bool filterChanged = DrawMaterialSelectionButtons(shown);
		filterChanged |= DrawMaterialChecklist(shown, true);
		if (filterChanged) {
			for (uint32_t i = 0; i < shown.size(); ++i)
				tuning.materialMapFilter = NR::MaterialMap::Set(tuning.materialMapFilter, kMaterialListOrder[i], shown[i] != 0);
			changed = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("material_map_filter_tooltip"),
				"Each swatch is the colour that material is drawn in. A material that is off keeps its normal pixels."));
		ImGui::PopID();
		ImGui::EndDisabled();
		return changed;
	}

	/** @brief Draws the dialogue section of the situation profiles; the crop override follows the crop's enable state. Returns true when the crop override changed. */
	bool DrawContextProfiles(NR::Context::Profiles& contexts, bool cropDisabled)
	{
		bool cropChanged = false;
		if (ImGui::CollapsingHeader(T(TKEY("dialogue"), "Dialogue"))) {
			ImGui::PushID("dialogueProfile");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("dialogue_tooltip"),
					"Changes only what Neural Rendering does while a dialogue is open. The default changes nothing: the normal frame and dialogue both follow the settings above."));
			bool onlyInDialogue = !contexts.normal.run;
			if (ImGui::Checkbox(T(TKEY("dialogue_only"), "Only in dialogue"), &onlyInDialogue)) {
				contexts.normal.run = !onlyInDialogue;
				contexts.dialogue.run = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("dialogue_only_tooltip"),
					"Evaluates Neural Rendering only while a dialogue is open, and leaves the frame's rendering untouched the rest of the time. The pass stays initialized, so opening a dialogue resumes it without a rebuild."));
			int scopeItem = static_cast<int>(contexts.dialogue.scope);
			const std::array<const char*, 4> dialogueScopeLabels{
				T(TKEY("dialogue_same_as_normal"), "Same as normal"),
				T(TKEY("dialogue_scope_everything"), "Everything"),
				T(TKEY("dialogue_scope_characters"), "Skin, hair and eyes"),
				T(TKEY("dialogue_scope_characters_foliage"), "Skin, hair, eyes and foliage"),
			};
			if (ImGui::Combo(T(TKEY("dialogue_scope"), "In dialogue, apply Neural Rendering to"), &scopeItem, dialogueScopeLabels.data(), static_cast<int>(dialogueScopeLabels.size())))
				contexts.dialogue.scope = static_cast<NR::Context::ScopeOverride>(scopeItem);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("dialogue_scope_tooltip"),
					"What Neural Rendering applies to while a dialogue is open. Choosing a scope keeps the by-material lane bound the whole time, at a cost of about 0.04 ms, so entering and leaving dialogue never rebuilds Neural Rendering."));
			ImGui::BeginDisabled(cropDisabled);
			int regionItem = static_cast<int>(contexts.dialogue.region);
			const std::array<const char*, 2> dialogueRegionLabels{
				T(TKEY("dialogue_same_as_normal"), "Same as normal"),
				T(TKEY("dialogue_region_full"), "Full frame"),
			};
			if (ImGui::Combo(T(TKEY("dialogue_region"), "In dialogue, crop"), &regionItem, dialogueRegionLabels.data(), static_cast<int>(dialogueRegionLabels.size()))) {
				contexts.dialogue.region = static_cast<NR::Context::RegionOverride>(regionItem);
				cropChanged = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("dialogue_region_tooltip"),
					"Full frame evaluates the whole view during dialogue instead of the character's crop. It needs Limit to Tracked Actor on; Same as normal follows that setting."));
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		return cropChanged;
	}
}

void NeuralRendering::DrawSettings(bool& enabled, uint32_t& placement, float& mix, NR::Context::Profiles& contexts, NR::Tuning& tuning)
{
	ImGui::PushID("NeuralRendering");
	const auto availability = GetRuntimeAvailability();
	const bool developerMode = globals::state->IsDeveloperMode();
	// An unavailable runtime must not strand a feature that is already on, so only the switch
	// from off is locked. The verdict is re-read when the file changes, so installing a build
	// mid-session unblocks the toggle without a restart.
	const bool loadable = availability.AllowsLoad(developerMode);
	const bool lockEnable = !loadable && !enabled;
	ImGui::BeginDisabled(lockEnable);
	if (ImGui::Checkbox(T(TKEY("enable"), "Enable Neural Rendering"), &enabled))
		retryRequested = resetHistory = true;
	ImGui::EndDisabled();
	// The same switch is on a hotkey (Settings > Keybindings); it takes effect on the next frame.
	ImGui::SameLine();
	Util::Text::Disabled("(%s)", Util::Input::KeyIdToString(globals::menu->GetSettings().NeuralRenderingToggleKey).c_str());
	if (lockEnable) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(availability.reason.c_str());
			ImGui::TextUnformatted(RuntimeFixHint(availability.state));
		}
	}
	if (!availability.Ready()) {
		if (loadable)
			Util::Text::WrappedWarning("%s", T(TKEY("runtime_developer_load"), "Loading this build because developer mode is on; its output is unverified."));
		else
			Util::Text::Disabled("%s", RuntimeFixHint(availability.state));
	}
	ImGui::TextWrapped("%s", T(TKEY("description"),
								 "Runs NVIDIA's neural rendering model on the scene, before upscaling or on the final image. Requires an NR-capable NVIDIA GPU and a validated 310.8 runtime build."));
	int placementItem = static_cast<int>(std::min(placement, kMaxPlacement));
	const std::array<const char*, kMaxPlacement + 1> placementLabels{
		T(TKEY("placement_before"), "Before upscaling"),
		T(TKEY("placement_final"), "Final image"),
	};
	if (ImGui::Combo(T(TKEY("placement"), "Placement"), &placementItem, placementLabels.data(), static_cast<int>(placementLabels.size()))) {
		placement = static_cast<uint32_t>(placementItem);
		resetHistory = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("placement_tooltip"),
			"Before upscaling: on the render-resolution scene before DLSS, as Open Shaders does; only brightness and luminance detail change, and DLSS and the grading run on top. Final image: after upscaling and tone mapping, on the frame DLSS frame generation receives without the UI, at output resolution; the model's result is shown as it is, colour included, and costs more GPU time. The final image needs DLSS frame generation and SDR output."));
	if (placement == static_cast<uint32_t>(Placement::kFinalImage)) {
		if (const char* unavailable = FinalImageUnavailableReason())
			Util::Text::WrappedWarning(T(TKEY("placement_final_unavailable"), "Final image is unavailable (%s), so Neural Rendering runs before upscaling."), unavailable);
	}
	float mixPercent = std::clamp(mix, 0.0f, NR::Tuning::kMaxMix) * 100.0f;
	if (ImGui::SliderFloat(T(TKEY("mix"), "Mix"), &mixPercent, 0.0f, NR::Tuning::kMaxMix * 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
		mix = mixPercent / 100.0f;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("mix_tooltip"),
			"How much of Neural Rendering's edit is shown: 0% leaves the frame as it was, 100% shows all of it.\n"
			"Above 100% strengthens the effect by extrapolating the edit; too much exaggerates colour and hair.\n"
			"It changes what is shown, not the GPU time."));
	int style = static_cast<int>(std::min(tuning.style, NR::Tuning::kMaxStyle));
	const std::array<const char*, NR::Tuning::kMaxStyle + 1> styleLabels{
		T(TKEY("style_0"), "Style 0"),
		T(TKEY("style_1"), "Style 1"),
		T(TKEY("style_2"), "Style 2"),
	};
	bool changed = ImGui::Combo(T(TKEY("style"), "Style"), &style, styleLabels.data(), static_cast<int>(styleLabels.size()));
	bool recreateTuning = changed;
	if (changed)
		tuning.style = static_cast<uint32_t>(style);
	changed |= ImGui::SliderFloat(T(TKEY("intensity"), "Intensity"), &tuning.intensity, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("local_tone"), "Local Tone Strength"), &tuning.localToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxLocalStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("local_tone_tooltip"), "Lighting and colour change NR makes; the strongest look control and most of its darkening.\nDefault 1; NVIDIA's dxvk-remix uses 0.3 with structure 0.7 for a lighter touch."));
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("local_structure"), "Local Structure Strength"), &tuning.localStructureStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxLocalStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("local_structure_tooltip"), "Fine detail NR adds: skin, hair strands, surfaces.\nDefault 1; dxvk-remix uses 0.7."));
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("skin_structure"), "Skin Structure Strength"), &tuning.skinStructureStrength, NR::Tuning::kAutomaticSkinStructure, NR::Tuning::kMaxStrength,
		tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure ? T(TKEY("skin_auto"), "Auto") : "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("skin_structure_tooltip"), "Runtime 310.8 changes the image only for 0.00 to 0.99; Auto (-1) and 1.00 leave skin as the other strengths make it."));
	const char* transferLabels[] = { T(TKEY("tone_transfer_gain"), "Luminance gain"), T(TKEY("tone_transfer_aces"), "ACES round trip") };
	int transfer = static_cast<int>(std::min(tuning.toneTransfer, NR::Tuning::kMaxToneTransfer));
	if (ImGui::Combo(T(TKEY("tone_transfer"), "Tone Transfer (before upscaling)"), &transfer, transferLabels, IM_ARRAYSIZE(transferLabels))) {
		tuning.toneTransfer = static_cast<uint32_t>(transfer);
		changed = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("tone_transfer_tooltip"), "Luminance gain applies only NR's bounded brightness change and keeps the game's colour.\nACES round trip gives NR an ACES-tonemapped copy and brings its colour back exactly, keeping the original in the highlights."));
	changed |= ImGui::SliderFloat(T(TKEY("final_scale"), "Final Image Resolution"), &tuning.finalScale, NR::Tuning::kMinFinalScale, NR::Tuning::kMaxFinalScale,
		"%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("final_scale_tooltip"), "Final image placement: the share of the output size, per axis, that the model works at.\nBelow 1 only NR's edit is upscaled onto the full frame; the cost falls with the pixel count (0.75 = 56 %)."));
	// 05-29 writes no material category lane, so the controls that act only through it are hidden
	// while it is missing and their saved values keep the neutral defaults.
	const bool categoryLane = impl->MaterialLane() != nullptr;
	if (!categoryLane)
		Util::Text::Disabled("%s", T(TKEY("category_lane_missing"), "Category and material controls need the deferred material lane, which this build does not write."));
	if (categoryLane && ImGui::CollapsingHeader(T(TKEY("category_tone"), "Category Tone Strengths"))) {
		ImGui::PushID("categoryTone");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("category_tone_tooltip"),
				"Scales Neural Rendering's tone edit per material: 0 leaves a material's brightness untouched, 1 is the normal edit, and 2 doubles it. Materials the deferred buffer does not label are unaffected."));
		changed |= ImGui::SliderFloat(T(TKEY("category_skin"), "Skin"), &tuning.skinToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderFloat(T(TKEY("category_hair"), "Hair"), &tuning.hairToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderFloat(T(TKEY("category_eyes"), "Eyes"), &tuning.eyeToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderFloat(T(TKEY("category_foliage"), "Foliage"), &tuning.foliageToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderFloat(T(TKEY("category_landscape"), "Landscape"), &tuning.landscapeToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::PopID();
	}
	if (categoryLane)
		changed |= DrawMaterialControls(tuning, materialStrengthAvailable.load(std::memory_order_relaxed));
	// The 05-29 port has no actor-tracking hook yet (InstallHooks is empty), so the crop stays off.
	ImGui::BeginDisabled(true);
	if (ImGui::Checkbox(T(TKEY("region_of_interest"), "Limit to Tracked Actor"), &tuning.regionOfInterest)) {
		changed = true;
		resetHistory = true;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	Util::Text::Disabled("%s", T(TKEY("region_of_interest_unported"), "(not in this build)"));
	if (globals::game::isVR) {
		if (ImGui::Checkbox(T(TKEY("region_follow_foveation"), "Follow Foveation"), &tuning.regionFollowFoveation)) {
			changed = true;
			resetHistory = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("region_follow_foveation_tooltip"),
				"With Foveation on, evaluates only the foveated region, narrowed to the tracked character's crop when that is set, so the periphery keeps its pre-Neural-Rendering content. The upscaler already replaces that periphery with the cheap stretched view."));
	}
	// The crop controls act only through a tracked crop, so they follow the toggle and are greyed
	// out without it instead of accepting edits that the pass ignores.
	const bool cropDisabled = !tuning.regionOfInterest;
	ImGui::BeginDisabled(cropDisabled);
	if (ImGui::Checkbox(T(TKEY("crop_group"), "Include Nearby Characters"), &tuning.regionGroup)) {
		changed = true;
		resetHistory = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("crop_group_tooltip"),
			"Grows the crop to also cover the next most prominent characters while it stays under half the view, so a group is evaluated together. Off tracks one character."));
	if (ImGui::Checkbox(T(TKEY("region_overlay"), "Show Region Overlay"), &tuning.regionOverlay))
		changed = true;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("region_overlay_tooltip"),
			"Draws the evaluated crop: a green outline in the game frame and the same rectangle over the preview below. Only meaningful with Limit to Tracked Actor on, and it draws nothing while no character is tracked, since the whole frame is evaluated then."));
	ImGui::EndDisabled();
	if (DrawContextProfiles(contexts, cropDisabled))
		resetHistory = true;
	if (ImGui::Button(T(TKEY("restore_defaults"), "Restore NR Defaults"))) {
		tuning = {};
		contexts = {};
		changed = recreateTuning = true;
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("reset_history"), "Reset NR History")))
		resetHistory = true;
	if (enabled && ImGui::Button(T(TKEY("retry"), "Retry NR")))
		RequestRetry();
	if (globals::state->IsDeveloperMode() && ImGui::TreeNodeEx(T(TKEY("developer"), "Developer"), ImGuiTreeNodeFlags_None)) {
		ImGui::SeparatorText(T(TKEY("developer_model"), "Model input"));
		const bool autoMaskChanged = ImGui::Checkbox(T(TKEY("use_auto_mask"), "Use NGX Automatic Mask"), &tuning.useAutoMask);
		changed |= autoMaskChanged;
		recreateTuning |= autoMaskChanged;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("use_auto_mask_tooltip"),
				"NVIDIA's built-in automatic mask. On is the normal setting; turning it off with no mask supplied is an untested configuration."));
		ImGui::SeparatorText(T(TKEY("developer_crop"), "Crop"));
		ImGui::BeginDisabled(cropDisabled);
		int fit = static_cast<int>(std::min(tuning.regionFit, NR::Tuning::kMaxRegionFit));
		const std::array<const char*, NR::Tuning::kMaxRegionFit + 1> fitLabels{
			T(TKEY("crop_fit_padded"), "Padded"),
			T(TKEY("crop_fit_tight"), "Tight"),
		};
		if (ImGui::Combo(T(TKEY("crop_fit"), "Crop Fit"), &fit, fitLabels.data(), static_cast<int>(fitLabels.size()))) {
			tuning.regionFit = static_cast<uint32_t>(fit);
			changed = true;
			resetHistory = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("crop_fit_tooltip"),
				"How much margin the crop keeps around the tracked character. Padded keeps the normal margin; Tight evaluates the character's own outline with no margin, for checking what the crop covers."));
		ImGui::EndDisabled();
		// The sweep forces its own centred crops, so it stays usable without a tracked actor.
		if (ImGui::Button(T(TKEY("crop_calibrate"), "Calibrate Crop Cost")))
			RequestCalibration();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("crop_calibrate_tooltip"),
				"Measures how Neural Rendering's GPU time falls as the crop shrinks, over about half a minute, and finds the largest crop that is still about as cheap as the smallest. Turn frame generation off first; it skews the timing."));
		const auto calibrationState = GetCalibration();
		if (calibrationState.state == NR::CropCalibration::State::kRunning) {
			ImGui::TextUnformatted(T(TKEY("crop_calibrate_running"), "Calibrating..."));
		} else if (calibrationState.state == NR::CropCalibration::State::kFailed) {
			ImGui::TextUnformatted(T(TKEY("crop_calibrate_failed"), "Calibration failed: frame generation on, Neural Rendering not running, or no timing data."));
		} else if (calibrationState.state == NR::CropCalibration::State::kDone) {
			for (size_t step = 0; step < NR::CropCalibration::kSteps; ++step)
				ImGui::Text("%3.0f%%: %.2f ms", NR::CropCalibration::kFractions[step] * 100.0f, calibrationState.stepMs[step]);
			ImGui::Text(T(TKEY("crop_calibrate_knee"), "Largest crop that is still cheap: %.0f%%"), calibrationState.kneeFraction * 100.0f);
			if (calibrationState.stabilityRatio > kCalibrationUnstableRatio)
				ImGui::TextUnformatted(T(TKEY("crop_calibrate_unstable"), "Timing was unsteady between passes; run it again."));
		}
		ImGui::SeparatorText(T(TKEY("developer_diagnostics"), "Diagnostics"));
		if (ImGui::Checkbox("Use resolution-scaled NR motion", &impl->useResolutionMotionScale))
			resetHistory = true;
		diagnostics.DrawSettings();
		ImGui::TreePop();
	}
	if (changed)
		tuning.Sanitize();
	if (recreateTuning)
		recreate = resetHistory = true;
	const auto current = GetStatus();
	ImGui::TextWrapped("%s", current.text.c_str());
	if (tuning.regionOverlay)
		DrawRegionPreview();
	ImGui::PopID();
}

void NeuralRendering::DrawRegionPreview()
{
	ImGui::Separator();
	// Panels draw on the rendering thread after the NR pass, so impl's preview texture needs no lock.
	const auto tracked = GetRegionOfInterest();
	const auto trackedBox = GetActorBox();
	const uint32_t sourceWidth = impl->width * impl->eyeCount;
	const uint32_t sourceHeight = impl->height;
	auto* preview = impl->original ? impl->original->srv.get() : nullptr;
	if (!preview || !sourceWidth || !sourceHeight) {
		ImGui::TextDisabled("%s", T(TKEY("region_overlay_unavailable"), "Crop preview appears once Neural Rendering runs a frame."));
		return;
	}
	const float maxWidth = std::min(kRegionPreviewMaxWidth, ImGui::GetContentRegionAvail().x);
	const float aspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
	const ImVec2 imageSize(maxWidth, maxWidth / aspect);
	const ImVec2 imageMin = ImGui::GetCursorScreenPos();
	ImGui::Image(reinterpret_cast<ImTextureID>(preview), imageSize);
	std::array<Util::RegionOverlay::Region, kMaxPreviewRegions> rects{};
	size_t count = 0;
	const uint32_t eyes = std::min<uint32_t>(impl->eyeCount, 2);
	if (tracked.active) {
		for (uint32_t eye = 0; eye < eyes; ++eye) {
			const auto& crop = tracked.eye[eye];
			rects[count].rect = Util::Subrect::PixelRegion{ crop.x + eye * impl->width, crop.y, crop.w, crop.h };
			rects[count].label = eyes > 1 ? (eye == 0 ? "L" : "R") : nullptr;
			++count;
		}
	}
	if (trackedBox.active) {
		for (uint32_t eye = 0; eye < eyes; ++eye) {
			const auto& box = trackedBox.eye[eye];
			rects[count].rect = Util::Subrect::PixelRegion{ box.x + eye * impl->width, box.y, box.w, box.h };
			rects[count].color = kActorBoxPreviewColor;
			rects[count].label = "actor";
			++count;
		}
	}
	Util::RegionOverlay::Draw(imageMin, imageSize, sourceWidth, sourceHeight, std::span(rects.data(), count));
}

void NeuralRendering::DrawDiagnosticsOverlay()
{
	diagnostics.DrawOverlay(GetStatus().text);
}

void NeuralRendering::RecordStage(bool finishedPost)
{
	auto* main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
	diagnostics.Stage(globals::state->frameCount, finishedPost, reinterpret_cast<uintptr_t>(main));
}

void NeuralRendering::CaptureBeforeUpscaling()
{
	if (!globals::state)
		return;
	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!diagnostics.CaptureActive(globals::state->frameCount) && diagnostics.BeginCapture(globals::state->frameCount))
		diagnostics.CaptureStage("00_original_scene", Util::AsReal(main.texture), globals::state->frameCount);
	diagnostics.CaptureStage("05_pre_sr", Util::AsReal(main.texture), globals::state->frameCount);
}

void NeuralRendering::CaptureAfterUpscaling()
{
	if (!globals::state)
		return;
	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	diagnostics.CaptureStage("06_post_sr", Util::AsReal(main.texture), globals::state->frameCount);
	diagnostics.FinishCapture(globals::state->frameCount);
}

void NeuralRendering::DrawBeforeUpscaling(bool enabled, Placement placement, float mix, const NR::Context::Profiles& contexts, const NR::Tuning& tuning, uint32_t target, float2 renderSize)
{
	using Outcome = NR::Diagnostics::Outcome;
	// Last frame's final-image request is still here when DLSS-G's compose pass never ran it.
	if (std::exchange(finalRequest.pending, false) && finalMissed++ == 0)
		logger::info("[NeuralRendering] final-image pass skipped on frame {}: DLSS-G did not compose that frame", finalRequest.frame);
	enabled = ApplyTestCycle(enabled, placement);
	ReportStatus();
	if (placement == Placement::kFinalImage) {
		const char* unavailable = FinalImageUnavailableReason();
		if (unavailable != finalUnavailableLogged) {
			if (unavailable)
				logger::warn("[NeuralRendering] final-image placement unavailable: {}; NR runs before upscaling", unavailable);
			else if (finalUnavailableLogged)
				logger::info("[NeuralRendering] final-image placement available again");
			finalUnavailableLogged = unavailable;
		}
		if (unavailable)
			placement = Placement::kBeforeUpscaling;
	}
	const auto context = NR::Context::Resolve(contexts, DialogueOpen(), contextState);
	auto& diagnostic = diagnostics.BeginHook(globals::state->frameCount, target);
	const auto action = NR::DecideFrame({ enabled, context.suspended, globals::state->worldRenderedThisFrame, impl->failed,
		retryRequested.load(), impl->ready, impl->lastFrame, globals::state->frameCount });
	if (context.resetHistory)
		resetHistory = true;
	// Without a usable runtime NR stays off quietly: the runtime check at load already logged why, and an
	// initialization that can only fail would add an error line and a "stopped" status for testers to ask about.
	if ((action == NR::FrameAction::InitializeThenRun || action == NR::FrameAction::RebuildThenRun) &&
		!GetRuntimeAvailability().AllowsLoad(globals::state->IsDeveloperMode())) {
		diagnostic.outcome = NR::Diagnostics::Outcome::Disabled;
		return;
	}
	if (action == NR::FrameAction::ReleasePassResources) {
		diagnostic.outcome = Outcome::Disabled;
		// A latched failure may be a wedged queue, so its resources wait for Retry's draining teardown.
		if (impl->HasPassResources() && !impl->failed) {
			std::string failure;
			try {
				impl->ReleasePassResources();
			} catch (const winrt::hresult_error& error) {
				failure = winrt::to_string(error.message());
			} catch (const std::exception& error) {
				failure = error.what();
			}
			if (!failure.empty()) {
				diagnostic.outcome = Outcome::Error;
				LatchFailure();
				PublishFailure(failure);
				logger::error("[NeuralRendering] {}", failure);
				retryRequested = resetHistory = true;
				return;
			}
		}
		if (!impl->failed && publishedState.load(std::memory_order_relaxed) != Status::State::kOff) {
			logger::debug("[NeuralRendering] Disabled");
			PublishStatus(Status::State::kOff, T(TKEY("status_off"), "Off"));
		}
		retryRequested = resetHistory = true;
		return;
	}
	if (action == NR::FrameAction::Suspend) {
		diagnostic.outcome = Outcome::Suspended;
		if (!impl->failed && publishedState.load(std::memory_order_relaxed) != Status::State::kSuspended) {
			logger::debug("[NeuralRendering] Suspended: waiting for dialogue");
			PublishStatus(Status::State::kSuspended, T(TKEY("status_waiting_dialogue"), "Waiting for dialogue"));
		}
		return;
	}
	auto* state = globals::state;
	if (action == NR::FrameAction::SkipNoWorld) {
		diagnostic.outcome = Outcome::NoWorld;
		if (publishedState.load(std::memory_order_relaxed) == Status::State::kOff)
			PublishStatus(Status::State::kStarting, T(TKEY("status_starting"), "Starting..."));
		resetHistory = true;
		return;
	}
	const auto effective = NR::Context::EffectiveTuning(tuning, contexts, context.kind);
	const auto materialStrengths = effective.MaterialStrengths();
	const auto publishMaterialStrength = [this, &materialStrengths] {
		materialStrengthActive.store(impl->materialStrengthBound, std::memory_order_relaxed);
		materialStrengthAvailable.store(!impl->materialStrengthDegraded && !materialStrengthRejected.load(std::memory_order_relaxed), std::memory_order_relaxed);
		for (size_t i = 0; i < materialStrengths.strength.size(); ++i)
			materialStrengthValues[i].store(materialStrengths.strength[i], std::memory_order_relaxed);
		materialEdgeSoftness.store(materialStrengths.edgeSoftness, std::memory_order_relaxed);
	};
	try {
		retryRequested = false;
		if (action == NR::FrameAction::RebuildThenRun) {
			// Retire both APIs before releasing a failed runtime and its shared resources.
			impl->interop.Drain();
			impl = std::make_unique<Impl>();
			resetHistory = true;
		}
		auto& work = *impl;
		const auto selected = diagnostics.Selected();
		diagnostic.options = selected.options;
		if (diagnostic.options != work.lastDiagnosticOptions) {
			resetHistory = true;
			if ((diagnostic.options ^ work.lastDiagnosticOptions) & NR::Diagnostics::FeedCameraData) {
				work.interop.Drain();
				work.runtime.ResetFeatures();
			}
			work.lastDiagnosticOptions = diagnostic.options;
		}
		if (action == NR::FrameAction::SkipLatched) {
			diagnostic.outcome = Outcome::FailedLatch;
			return;
		}
		if (action == NR::FrameAction::SkipDuplicate) {
			++diagnostic.duplicates;
			return;
		}
		// A rebuilt runtime starts uninitialized, so both actions end in Initialize().
		if (action == NR::FrameAction::InitializeThenRun || action == NR::FrameAction::RebuildThenRun) {
			work.Initialize();
			const auto luid = work.interop.AdapterLuid();
			logger::debug("[NeuralRendering] D3D12 device on renderer adapter LUID {:08X}:{:08X}", luid.HighPart, luid.LowPart);
			logger::info("[NeuralRendering] material category lane {}", work.MaterialLane() ? "present" : "absent; category and material edits stay neutral");
		}
		if (clearShaders.exchange(false)) {
			work.prepareColor.Reset();
			work.prepareToneData.Reset();
			work.compositeColor.Reset();
			work.materialAlphaShader.Reset();
			work.sceneKeyReduce.Reset();
			work.sceneKeyAdapt.Reset();
			work.finalPrepare.Reset();
			work.finalComposite.Reset();
		}
		work.mix = std::clamp(mix, 0.0f, NR::Tuning::kMaxMix);
		if (placement == Placement::kFinalImage) {
			// The other placement's resources go once; the final-image pass makes its own at output size.
			if (work.HasBeforeUpscalingResources())
				work.ReleasePassResources();
			diagnostic.outcome = Outcome::Bypassed;
			// Menus and pauses skip DLSS-G's preparation, and with it this frame's final image.
			if (!globals::features::upscaling.ShouldPrepareFrameGeneration())
				return;
			auto boundedTuning = effective;
			boundedTuning.Sanitize();
			if (diagnostic.options & NR::Diagnostics::DisableTone)
				boundedTuning.localToneStrength = 0.0f;
			if (diagnostic.options & NR::Diagnostics::DisableStructure)
				boundedTuning.localStructureStrength = 0.0f;
			if (diagnostic.options & NR::Diagnostics::DisableSkin)
				boundedTuning.skinStructureStrength = NR::Tuning::kAutomaticSkinStructure;
			if (testCycleScale >= 0) {
				boundedTuning.finalScale = kTestCycleScales[testCycleScale].scale;
				boundedTuning.skinStructureStrength = kTestCycleScales[testCycleScale].skinStructure;
			}
			if (testCycleMix >= 0) {
				const auto& variant = kTestCycleMixes[testCycleMix];
				work.mix = variant.mix;
				boundedTuning.localToneStrength = variant.localTone;
				boundedTuning.localStructureStrength = variant.localStructure;
			}
			finalRequest.pending = true;
			finalRequest.frame = state->frameCount;
			finalRequest.renderWidth = NR::EyeRenderWidth(static_cast<uint32_t>(renderSize.x), 1);
			finalRequest.renderHeight = static_cast<uint32_t>(renderSize.y);
			finalRequest.reset = resetHistory.exchange(false) ? NR::Diagnostics::Requested : 0u;
			finalRequest.options = diagnostic.options;
			finalRequest.mix = work.mix;
			finalRequest.tuning = boundedTuning;
			work.lastFrame = state->frameCount;
			return;
		}
		auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		auto* color = Util::AsReal(targets[RE::RENDER_TARGETS::kMAIN].texture);
		Upscaling::EncodeInputViews inputs{};
		const char* missingInput = nullptr;
		if (!globals::features::upscaling.GetEncodeInputs(inputs, missingInput))
			throw std::runtime_error(std::format("Missing NR guide input ({})", missingInput));
		const auto count = globals::game::isVR ? 2u : 1u;
		const auto gw = NR::EyeRenderWidth(static_cast<uint32_t>(renderSize.x), count);
		const auto gh = static_cast<uint32_t>(renderSize.y);
		D3D11_TEXTURE2D_DESC desc{};
		if (color)
			color->GetDesc(&desc);
		const auto w = gw;
		const auto h = gh;
		diagnostic.width = w;
		diagnostic.height = h;
		diagnostic.eyeCount = count;
		diagnostic.format = desc.Format;
		diagnostic.proxyFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		diagnostic.source = reinterpret_cast<uintptr_t>(color);
		if (!NR::IsSupportedOutput(desc, w, h, count))
			throw std::runtime_error(std::format("Unsupported NR output: {}x{}, DXGI format {}, array {}, samples {}, guides {}x{}",
				desc.Width, desc.Height, static_cast<uint32_t>(desc.Format), desc.ArraySize, desc.SampleDesc.Count, gw, gh));
		for (auto* input : inputs) {
			D3D11_TEXTURE2D_DESC guide{};
			if (!input || !Util::GetTexture2DDesc(input, guide) || guide.Width < gw * count || guide.Height < gh ||
				guide.ArraySize != 1 || guide.SampleDesc.Count != 1)
				throw std::runtime_error("Missing or incompatible NR guide texture");
		}
		const bool materialStrengthWanted = effective.materialStrength && !materialStrengthRejected.load(std::memory_order_relaxed);
		if (materialStrengthWanted != work.materialStrengthOn) {
			recreate = resetHistory = true;
			work.materialStrengthOn = materialStrengthWanted;
		}
		const bool forceRecreate = recreate.exchange(false);
		diagnostic.recreated = forceRecreate || work.width != w || work.height != h || work.guideWidth != gw || work.guideHeight != gh || work.eyeCount != count || work.format != desc.Format;
		work.EnsureResources(w, h, gw, gh, count, desc.Format, forceRecreate);
		if (diagnostic.recreated)
			PublishResources();
		const bool dilateMotion = (diagnostic.options & NR::Diagnostics::DilateMotion) != 0;
		auto* shader = globals::features::upscaling.GetEncodeTexturesCS(dilateMotion ? Upscaling::UpscaleMethod::kDLSS : Upscaling::UpscaleMethod::kNONE,
			Upscaling::EncodeOutput::kTypedDepth);
		auto* prepare = work.prepareColor.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
			{}, "cs_5_0", "Prepare", "NeuralRendering::PrepareColor CS");
		auto* composite = work.compositeColor.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
			{}, "cs_5_0", "Composite", "NeuralRendering::CompositeHDR CS");
		if (!shader || !prepare || !composite)
			throw std::runtime_error("NR encoder or color-transfer shader unavailable");
		work.debugOptions = diagnostic.options;
		work.conversionMode = selected.conversion;
		work.exposureMode = selected.exposure;
		work.compositeMode = selected.composite;
		work.visualMode = selected.visual;
		work.manualExposure = selected.manualExposure;
		work.differenceStrength = selected.differenceStrength;
		work.splitPosition = selected.splitPosition;
		work.shadowProtect = selected.shadowProtect;
		work.highlightProtect = selected.highlightProtect;
		work.toneRadius = selected.toneRadius;
		uint32_t reset = NR::Diagnostics::FrameResetReasons(resetHistory.exchange(false), work.lastFrame, state->frameCount);
		auto boundedTuning = effective;
		boundedTuning.Sanitize();
		const bool calibrationRunning = GetCalibration().state == NR::CropCalibration::State::kRunning;
		const bool cropActive = boundedTuning.regionOfInterest || calibrationRunning;
		work.region = cropActive ? GetRegionOfInterest() : Util::Region::StereoRegion{};
		work.actorBox = cropActive ? GetActorBox() : Util::Region::StereoRegion{};
		const bool actorCrop = work.region.active && !calibrationRunning;
		const bool foveatedRouteSuspended = state->IsMainOrLoadingMenuOpen();
		bool foveaClip = false;
		(void)foveatedRouteSuspended;  // no foveated renderer in the 05-29 tree
		auto source = RegionSource::kNone;
		if (actorCrop)
			source = foveaClip ? RegionSource::kBoth : RegionSource::kActor;
		else if (foveaClip)
			source = RegionSource::kFovea;
		regionSource.store(static_cast<uint32_t>(source), std::memory_order_relaxed);
		if (Util::Region::ShouldResetForRegion(NR::ActorRegion::kResetPolicy, work.region, work.lastRegion, NR::ActorRegion::kHistoryTolerancePixels))
			reset |= NR::Diagnostics::RegionChanged;
		work.lastRegion = work.region;
		if (diagnostic.options & NR::Diagnostics::DisableTone)
			boundedTuning.localToneStrength = 0.0f;
		if (diagnostic.options & NR::Diagnostics::DisableStructure)
			boundedTuning.localStructureStrength = 0.0f;
		if (diagnostic.options & NR::Diagnostics::DisableSkin)
			boundedTuning.skinStructureStrength = NR::Tuning::kAutomaticSkinStructure;
		work.withoutDepth = false;
		if (testCycleVariant >= 0) {
			const auto& variant = kTestCycleVariants[testCycleVariant];
			boundedTuning.skinStructureStrength = variant.skinStructure;
			boundedTuning.toneTransfer = variant.toneTransfer;
			work.withoutDepth = variant.withoutDepth;
		}
		work.toneTransfer = boundedTuning.toneTransfer;
		work.toneLowStrength = boundedTuning.localToneStrength;
		work.toneHighStrength = boundedTuning.localStructureStrength;
		work.categoryToneStrength = { boundedTuning.skinToneStrength, boundedTuning.hairToneStrength,
			boundedTuning.eyeToneStrength, boundedTuning.foliageToneStrength, boundedTuning.landscapeToneStrength };
		work.regionOverlay = boundedTuning.regionOverlay;
		work.materialMapEnabled = boundedTuning.showMaterialMap;
		work.materialMapMode = boundedTuning.materialMapMode;
		work.materialMapFilter = boundedTuning.materialMapFilter;
		work.materialMapStrength = materialStrengths.strength;
		diagnostic.conversion = static_cast<uint32_t>(work.conversionMode);
		diagnostic.exposureMode = static_cast<uint32_t>(work.exposureMode);
		diagnostic.compositeMode = static_cast<uint32_t>(work.compositeMode);
		diagnostic.visualMode = static_cast<uint32_t>(work.visualMode);
		diagnostic.manualExposure = work.manualExposure;
		diagnostic.differenceStrength = work.differenceStrength;
		diagnostic.splitPosition = work.splitPosition;
		diagnostic.intensity = boundedTuning.intensity;
		diagnostic.localTone = boundedTuning.localToneStrength;
		diagnostic.localStructure = boundedTuning.localStructureStrength;
		diagnostic.skinStructure = boundedTuning.skinStructureStrength;
		const auto testCapture = testCaptureFrames.load(std::memory_order_relaxed);
		if (testCapture && appliedFrames.load(std::memory_order_relaxed) >= testCaptureAt) {
			testCaptureAt = UINT32_MAX;
			if (work.StartTestCapture(testCapture, work.original->resource.get(), work.eyes[0].resolved->resource.get(), "test"))
				logger::info("[NeuralRendering] test capture: the next {} frames, before and after NR", testCapture);
			else
				logger::warn("[NeuralRendering] test capture skipped: its copy textures could not be created");
		}
		work.unitExposure = unitExposure.load(std::memory_order_relaxed);
		bool processed = false;
		{
			const PassTimer::Scope timing(work.passTimer);
			processed = work.Draw(color, inputs.data(), shader, reset, materialStrengthWanted, materialStrengths, boundedTuning, diagnostic, diagnostics);
		}
		if (work.TestCaptureComplete())
			work.WriteTestCapture(diagnostics);
		publishMaterialStrength();
		if (!processed)
			throw std::runtime_error(std::format("the NVIDIA runtime failed to process a frame. Update the GPU driver, then press Retry (NGX L/R 0x{:08X}/0x{:08X})",
				diagnostic.result[0], diagnostic.result[1]));
		appliedFrame.store(state->frameCount, std::memory_order_relaxed);
		appliedFrames.fetch_add(1, std::memory_order_relaxed);
		const uint32_t frameResets = diagnostic.reset[0] | diagnostic.reset[1];
		diagnostics.RecordFrame(frameResets, diagnostic.resetDrainMs);
		lastNgxResult[0].store(diagnostic.result[0], std::memory_order_relaxed);
		lastNgxResult[1].store(diagnostic.result[1], std::memory_order_relaxed);
		if (publishedState.load(std::memory_order_relaxed) != Status::State::kActive || activePlacement != Placement::kBeforeUpscaling) {
			activePlacement = Placement::kBeforeUpscaling;
			const auto runtime = work.runtime.Version();
			const auto luid = work.interop.AdapterLuid();
			PublishStatus(Status::State::kActive, FormatActiveStatus(runtime, false));
			logger::info("[NeuralRendering] active before upscaling: runtime {} on adapter LUID {:08X}:{:08X}", runtime, luid.HighPart, luid.LowPart);
			LogTesterSummary("before upscaling", runtime, boundedTuning, work.mix, work.width, work.height, work.width, work.height);
			// Every start, at load or switched back on, gets its own test capture once the history settles.
			if (testCaptureFrames.load(std::memory_order_relaxed))
				testCaptureAt = appliedFrames.load(std::memory_order_relaxed) + kTestCaptureAfterFrames;
		}
		diagnostic.outcome = (diagnostic.options & NR::Diagnostics::BypassWriteback) ? Outcome::Bypassed : Outcome::Applied;
		work.lastFrame = state->frameCount;
	} catch (const winrt::hresult_error& error) {
		diagnostic.outcome = Outcome::Error;
		const bool strengthHandled = RevertMaterialStrengthOnFailure(winrt::to_string(error.message()).c_str());
		publishMaterialStrength();
		if (strengthHandled)
			return;
		LatchFailure();
		PublishFailure(winrt::to_string(error.message()));
		logger::error("[NeuralRendering] D3D initialization/dispatch failed: 0x{:08X}", static_cast<uint32_t>(error.code().value));
	} catch (const std::exception& error) {
		diagnostic.outcome = Outcome::Error;
		const bool strengthHandled = RevertMaterialStrengthOnFailure(error.what());
		publishMaterialStrength();
		if (strengthHandled)
			return;
		LatchFailure();
		PublishFailure(error.what());
		logger::error("[NeuralRendering] {}", error.what());
	}
}

void NeuralRendering::DrawOnFinalImage(ID3D11Texture2D* a_hudless, ID3D11UnorderedAccessView* a_hudlessUAV, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_motion)
{
	if (!finalRequest.pending)
		return;
	const auto request = finalRequest;
	finalRequest.pending = false;
	if (!a_hudless || !a_hudlessUAV || !a_depth || !a_motion || !impl->ready || impl->failed)
		return;
	try {
		auto& work = *impl;
		D3D11_TEXTURE2D_DESC desc{};
		a_hudless->GetDesc(&desc);
		const bool forceRecreate = recreate.exchange(false);
		const auto& pass = work.finalImage;
		const auto [workWidth, workHeight] = Impl::FinalWorkSize(desc.Width, desc.Height, request.tuning.finalScale);
		const bool recreated = forceRecreate || pass.outputWidth != desc.Width || pass.outputHeight != desc.Height || pass.width != workWidth ||
		                       pass.height != workHeight || pass.format != desc.Format;
		work.EnsureFinalResources(desc.Width, desc.Height, desc.Format, request.tuning.finalScale, forceRecreate);
		if (recreated)
			PublishResources();
		// A gap since the pass last ran (a menu, a placement switch) starts a fresh history, as before upscaling.
		const uint32_t reset = request.reset | NR::Diagnostics::FrameResetReasons(false, pass.lastFrame, request.frame);
		const auto testCapture = testCaptureFrames.load(std::memory_order_relaxed);
		if (testCapture && appliedFrames.load(std::memory_order_relaxed) >= testCaptureAt) {
			testCaptureAt = UINT32_MAX;
			if (work.StartTestCapture(testCapture, pass.original->resource.get(), a_hudless, "final"))
				logger::info("[NeuralRendering] test capture: the next {} frames, before and after NR on the final image", testCapture);
			else
				logger::warn("[NeuralRendering] test capture skipped: its copy textures could not be created");
		}
		work.mix = request.mix;
		bool processed = false;
		{
			const PassTimer::Scope timing(work.finalPassTimer);
			processed = work.DrawFinal(a_hudless, a_hudlessUAV, a_depth, a_motion, request.renderWidth, request.renderHeight, reset, request.options, request.tuning, request.frame);
		}
		if (work.TestCaptureComplete())
			work.WriteTestCapture(diagnostics);
		if (!processed)
			throw std::runtime_error(std::format("the NVIDIA runtime failed to process a frame. Update the GPU driver, then press Retry (NGX 0x{:08X})", pass.frame.result));
		if (finalMissed) {
			logger::info("[NeuralRendering] final-image pass resumed after {} skipped frames", finalMissed);
			finalMissed = 0;
		}
		appliedFrame.store(request.frame, std::memory_order_relaxed);
		appliedFrames.fetch_add(1, std::memory_order_relaxed);
		lastNgxResult[0].store(pass.frame.result, std::memory_order_relaxed);
		lastNgxResult[1].store(0, std::memory_order_relaxed);
		if (publishedState.load(std::memory_order_relaxed) != Status::State::kActive || activePlacement != Placement::kFinalImage) {
			activePlacement = Placement::kFinalImage;
			const auto runtime = work.runtime.Version();
			PublishStatus(Status::State::kActive, FormatActiveStatus(runtime, true));
			logger::info("[NeuralRendering] active on the final image: runtime {}, {}x{} (model {}x{}) from a {}x{} render", runtime, pass.outputWidth,
				pass.outputHeight, pass.width, pass.height, request.renderWidth, request.renderHeight);
			LogTesterSummary("final image", runtime, request.tuning, request.mix, pass.width, pass.height, pass.outputWidth, pass.outputHeight);
			// Every start gets its own test capture once the history settles, as before upscaling.
			if (testCaptureFrames.load(std::memory_order_relaxed))
				testCaptureAt = appliedFrames.load(std::memory_order_relaxed) + kTestCaptureAfterFrames;
		}
	} catch (const winrt::hresult_error& error) {
		LatchFailure();
		PublishFailure(winrt::to_string(error.message()));
		logger::error("[NeuralRendering] final-image pass failed: 0x{:08X}", static_cast<uint32_t>(error.code().value));
	} catch (const std::exception& error) {
		LatchFailure();
		PublishFailure(error.what());
		logger::error("[NeuralRendering] final-image pass failed: {}", error.what());
	}
}

bool NeuralRendering::RevertMaterialStrengthOnFailure(const char* detail)
{
	if (!impl->materialStrengthInFlight || impl->interop.DeviceRemoved())
		return false;
	impl->materialStrengthInFlight = false;
	impl->materialStrengthBound = false;
	materialStrengthRejected.store(true, std::memory_order_relaxed);
	logger::warn("[NeuralRendering] material strength rejected ({}); Neural Rendering by material is off for this session", detail);
	return true;
}

void NeuralRendering::LatchFailure()
{
	if (NR::OnFailure(impl->interop.DeviceRemoved()) == NR::FailureAction::TeardownThenLatch) {
		// A removed device cannot be reused: the replacement starts latched, so only Retry rebuilds it.
		impl = std::make_unique<Impl>();
	}
	impl->failed = true;
}

#undef I18N_KEY_PREFIX
