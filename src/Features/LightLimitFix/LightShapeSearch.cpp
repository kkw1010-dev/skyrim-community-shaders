#include "Features/LightLimitFix/LightShapeSearch.h"

#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/LightShapeSearchLogic.h"
#include "Globals.h"
#include "Utils/VersionedRelocation.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace LightShapeSearch
{
	namespace
	{
		using LightShapeSearchLogic::Memo;
		using LightShapeSearchLogic::Node;

		// Skyrim 1.6.1170 only. Every id, offset and field offset below is confirmed at install by the byte patterns.
		constexpr std::uint64_t kRefreshId = 106350;       // refreshes the lights of one object: collects and sorts the lights in reach
		constexpr std::uint64_t kRefreshWalkId = 106353;   // walks the object's shapes; per shape: begin, every light, finish
		constexpr std::uint64_t kAddGeometryId = 108283;   // BSLight: searches the light's shape list, adds the shape when missing
		constexpr std::uint64_t kAddLightId = 107777;      // BSShaderPropertyLightData::AddLight
		constexpr std::uint64_t kBeginLightsId = 107782;   // starts the rebuild of a shape's light array
		constexpr std::uint64_t kFinishLightsId = 107783;  // drops the lights that were not added again

		constexpr std::uintptr_t kRefreshCallArgs = 0x407;    // 106350: the arguments of the call to 106353
		constexpr std::uintptr_t kRefreshWalkCall = 0x424;    // 106350: call 106353
		constexpr std::uintptr_t kShapeBlock = 0x126;         // 106353: the per-shape block, up to the begin call
		constexpr std::uintptr_t kShapeBlockLights = 0x140;   // 106353: the loop over the lights, up to the add-geometry call
		constexpr std::uintptr_t kShapeBlockNext = 0x153;     // 106353: the next light, then up to the finish call
		constexpr std::uintptr_t kShapeBlockEnd = 0x16C;      // 106353: after the finish call
		constexpr std::uintptr_t kBeginLightsCall = 0x13B;    // 106353: call 107782
		constexpr std::uintptr_t kAddGeometryCall = 0x14E;    // 106353: call 108283
		constexpr std::uintptr_t kFinishLightsCall = 0x167;   // 106353: call 107783
		constexpr std::uintptr_t kChecks = 0x5D;              // 108283: early exits, up to the validity call
		constexpr std::uintptr_t kSearch = 0x83;              // 108283: after the validity call: fence load and search loop
		constexpr std::uintptr_t kValidityCall = 0x7E;        // 108283: the validity call (Light Limit Fix replaces its target)
		constexpr std::uintptr_t kFoundBranch = 0x11D;        // 108283: found; without a fence it jumps to the AddLight call
		constexpr std::uintptr_t kFoundAddLight = 0x184;      // 108283: found: AddLight, then the exit
		constexpr std::uintptr_t kFoundAddLightCall = 0x192;  // 108283: call 107777
		constexpr std::uintptr_t kExit = 0x1FF;               // 108283: the exit

		constexpr std::ptrdiff_t kShapeShaderProperty = 0x128;  // BSGeometry -> BSShaderProperty
		constexpr std::ptrdiff_t kShapeWorldBound = 0xE4;       // BSGeometry world bound, the unused third argument of AddLight
		constexpr std::ptrdiff_t kPropertyLightData = 0x70;     // BSShaderProperty -> BSShaderPropertyLightData
		constexpr std::ptrdiff_t kLightListHead = 0x18;         // BSLight::geomList head
		constexpr std::ptrdiff_t kLightListFence = 0x68;        // BSLight::geomListFence

		constexpr auto RefreshCallArgs = REL::make_pattern<"48 8B 94 24 A8 00 00 00 45 0F B6 CF 49 63 C4 4D 8B C2 49 8B CD 49 C7 04 C2 00 00 00 00 E8">();
		constexpr auto RefreshWalkEntry = REL::make_pattern<"48 89 54 24 10 53 56 41 55 41 56 48 83 EC 28 45 0F B6 E9 49 8B F0 4C 8B F2 48 8B D9">();
		constexpr auto ShapeBlock = REL::make_pattern<"4C 8B B7 28 01 00 00 4D 85 F6 74 46 49 8B 4E 70 48 85 C9 74 3D E8">();
		constexpr auto ShapeBlockLights = REL::make_pattern<"48 8B 06 48 85 C0 74 18 48 8B D7 48 8B C8 E8">();
		constexpr auto ShapeBlockNext = REL::make_pattern<"48 8B 44 DE 08 48 FF C3 48 85 C0 75 E8 49 8B 4E 70 48 8B D7 E8">();
		constexpr auto ShapeBlockEnd = REL::make_pattern<"81 8F F4 00 00 00 00 00 40 00">();
		constexpr auto BeginLightsBody = REL::make_pattern<"C7 41 18 00 00 00 00 C3">();
		constexpr auto FinishLightsEntry = REL::make_pattern<"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 83 79 18 FF 48 8B EA 44 8B 41 10 48 8B F9">();
		constexpr auto AddGeometryEntry = REL::make_pattern<"40 56 57 41 56 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 8B F2 48 8B F9">();
		constexpr auto Checks = REL::make_pattern<"48 8B 9E 28 01 00 00 48 85 DB 0F 84 92 01 00 00 48 83 7B 70 00 0F 84 87 01 00 00 48 8B D7 48 8B CB E8">();
		constexpr auto Search = REL::make_pattern<"84 C0 0F 84 74 01 00 00 4C 8B 47 68 4D 85 C0 49 8B C8 75 04 48 8B 4F 18 48 85 C9 74 14 48 8B D1 48 8D 41 10 48 8B 09 48 39 30 74 6E 48 85 C9 75 EC">();
		constexpr auto FoundBranch = REL::make_pattern<"4D 85 C0 74 62">();
		constexpr auto FoundAddLight = REL::make_pattern<"4C 8D 86 E4 00 00 00 48 8B D7 48 8B 4B 70 E8 ?? ?? ?? ?? EB 66">();
		constexpr auto Exit = REL::make_pattern<"8B 44 24 58 41 89 06 48 8B 5C 24 50 48 83 C4 30 41 5E 5F 5E C3">();

		// What one call hook costs in the shared trampoline (a 5-byte call needs a 14-byte stub).
		constexpr std::size_t kTrampolineBytesPerHook = 14;
		// Seconds between log lines: verify mode, then live mode for its first lines and after them.
		constexpr double kVerifyReportSeconds = 30.0;
		constexpr double kLiveReportSeconds = 60.0;
		constexpr double kLiveLaterReportSeconds = 600.0;
		constexpr std::uint32_t kLiveFirstReports = 10;

		enum class Mode : std::uint8_t
		{
			Off,
			Verify,
			Live
		};

		using ValidityFn = bool(void* a_property, RE::BSLight* a_light);
		using AddLightFn = void(void* a_lightData, RE::BSLight* a_light, const void* a_bound);

		std::atomic<bool> installed{ false };
		std::atomic<bool> tripped{ false };
		std::atomic<ValidityFn*> validity{ nullptr };
		std::uintptr_t validityCallSite = 0;
		AddLightFn* addLight = nullptr;

		struct Totals
		{
			std::atomic<std::uint64_t> lookups{ 0 }, found{ 0 }, notFound{ 0 }, steps{ 0 }, engineSteps{ 0 }, mismatches{ 0 }, stale{ 0 }, nested{ 0 },
				fenced{ 0 }, refused{ 0 }, blocked{ 0 };
		};
		Totals totals;

		std::mutex reportLock;
		std::chrono::steady_clock::time_point lastReport{};
		Counters lastReported{};
		Mode lastReportedMode = Mode::Off;
		std::uint32_t liveReports = 0;

		/** @brief What one thread keeps for the refresh it is in. */
		struct ThreadState
		{
			Memo memo;
			Counters counters;
			std::uint32_t depth = 0;
			std::uint32_t flushes = 0;
			Mode mode = Mode::Off;
			bool tainted = false;
		};
		thread_local ThreadState threadState;

		template <class T>
		[[nodiscard]] T Read(const void* a_object, std::ptrdiff_t a_offset) noexcept
		{
			return *reinterpret_cast<const T*>(reinterpret_cast<std::uintptr_t>(a_object) + a_offset);
		}

		/** @brief Turns the search off for the rest of the session. The engine's routine then runs for every call. */
		void Trip(std::string_view a_reason)
		{
			if (!tripped.exchange(true))
				logger::warn("[LLF] Light shape search turned itself off: {}. The game's own search runs from here on.", a_reason);
		}

		[[nodiscard]] Mode CurrentMode() noexcept
		{
			if (tripped.load(std::memory_order_relaxed))
				return Mode::Off;
			const auto& feature = globals::features::lightLimitFix;
			if (!feature.loaded)
				return Mode::Off;
			if (feature.settings.VerifyLightShapeSearch)
				return Mode::Verify;
			return feature.settings.FastLightShapeSearch ? Mode::Live : Mode::Off;
		}

		/** @brief The function the engine's validity call reaches now, read once from the call itself so every hook on it stays in force. */
		[[nodiscard]] ValidityFn* ResolveValidity()
		{
			if (auto* known = validity.load(std::memory_order_acquire))
				return known;
			const auto target = Util::VersionedRelocation::GetCallTarget(validityCallSite);
			if (!target) {
				Trip("the validity call inside the engine's routine is no longer a call");
				return nullptr;
			}
			auto* resolved = reinterpret_cast<ValidityFn*>(target);
			validity.store(resolved, std::memory_order_release);
			return resolved;
		}

		[[nodiscard]] Counters LoadTotals() noexcept
		{
			Counters c;
			c.lookups = totals.lookups.load(std::memory_order_relaxed);
			c.found = totals.found.load(std::memory_order_relaxed);
			c.notFound = totals.notFound.load(std::memory_order_relaxed);
			c.steps = totals.steps.load(std::memory_order_relaxed);
			c.engineSteps = totals.engineSteps.load(std::memory_order_relaxed);
			c.mismatches = totals.mismatches.load(std::memory_order_relaxed);
			c.stale = totals.stale.load(std::memory_order_relaxed);
			c.nested = totals.nested.load(std::memory_order_relaxed);
			c.fenced = totals.fenced.load(std::memory_order_relaxed);
			c.refused = totals.refused.load(std::memory_order_relaxed);
			c.blocked = totals.blocked.load(std::memory_order_relaxed);
			return c;
		}

		/** @brief A log line from time to time while the search runs: what it answered and what that cost. */
		void Report(Mode a_mode)
		{
			std::unique_lock lock(reportLock, std::try_to_lock);
			if (!lock)
				return;
			const auto now = std::chrono::steady_clock::now();
			if (lastReport == std::chrono::steady_clock::time_point{}) {
				lastReport = now;
				lastReported = LoadTotals();
				lastReportedMode = a_mode;
				return;
			}
			const double seconds = std::chrono::duration<double>(now - lastReport).count();
			const double interval = lastReportedMode == Mode::Verify ? kVerifyReportSeconds :
			                        liveReports < kLiveFirstReports  ? kLiveReportSeconds :
			                                                           kLiveLaterReportSeconds;
			if (seconds < interval && a_mode == lastReportedMode)
				return;
			const Counters current = LoadTotals();
			const Counters& was = lastReported;
			const std::uint64_t lookups = current.lookups - was.lookups;
			if (lookups || current.mismatches != was.mismatches || current.stale != was.stale) {
				const double perLookup = lookups ? static_cast<double>(current.steps - was.steps) / static_cast<double>(lookups) : 0.0;
				if (lastReportedMode == Mode::Verify) {
					logger::info("[LLF] Light shape search (verify) over {:.0f} s: {} lookups, {} found, {} not found, {:.1f} nodes a lookup against {:.1f} for the game's walk, {} mismatches, {} stale, {} nested, {} fenced, {} left to the game, {} refused",
						seconds, lookups, current.found - was.found, current.notFound - was.notFound, perLookup,
						lookups ? static_cast<double>(current.engineSteps - was.engineSteps) / static_cast<double>(lookups) : 0.0,
						current.mismatches - was.mismatches, current.stale - was.stale, current.nested - was.nested, current.fenced - was.fenced, current.blocked - was.blocked,
						current.refused - was.refused);
				} else {
					++liveReports;
					logger::info("[LLF] Light shape search (on) over {:.0f} s: {} lookups, {} found, {} not found, {:.1f} nodes a lookup, {} stale, {} nested, {} fenced, {} left to the game, {} refused",
						seconds, lookups, current.found - was.found, current.notFound - was.notFound, perLookup, current.stale - was.stale, current.nested - was.nested,
						current.fenced - was.fenced, current.blocked - was.blocked, current.refused - was.refused);
				}
			}
			lastReport = now;
			lastReported = current;
			lastReportedMode = a_mode;
		}

		/** @brief Writes a verify-mode disagreement to the log when it happens, the first few in full. */
		void ReportMismatch(const void* a_light, const void* a_shape, const Node* a_found, const Node* a_walked, const Node* a_cursor)
		{
			static std::atomic<std::uint32_t> written{ 0 };
			constexpr std::uint32_t kMostWritten = 20;
			if (const auto count = written.fetch_add(1, std::memory_order_relaxed); count < kMostWritten) {
				logger::warn("[LLF] Light shape search MISMATCH {}: light {:X}, shape {:X}: the search says {} (node {:X}), the game's walk says {} (node {:X}); cursor {:X}",
					count + 1, reinterpret_cast<std::uintptr_t>(a_light), reinterpret_cast<std::uintptr_t>(a_shape), a_found ? "found" : "not found",
					reinterpret_cast<std::uintptr_t>(a_found), a_walked ? "found" : "not found", reinterpret_cast<std::uintptr_t>(a_walked),
					reinterpret_cast<std::uintptr_t>(a_cursor));
			}
		}

		/** @brief Adds a finished refresh's counts to the totals. */
		void Flush(ThreadState& a_state)
		{
			Counters& c = a_state.counters;
			if ((c.lookups | c.found | c.notFound | c.steps | c.engineSteps | c.mismatches | c.stale | c.nested | c.fenced | c.refused | c.blocked) != 0) {
				totals.lookups.fetch_add(c.lookups, std::memory_order_relaxed);
				totals.found.fetch_add(c.found, std::memory_order_relaxed);
				totals.notFound.fetch_add(c.notFound, std::memory_order_relaxed);
				totals.steps.fetch_add(c.steps, std::memory_order_relaxed);
				totals.engineSteps.fetch_add(c.engineSteps, std::memory_order_relaxed);
				totals.mismatches.fetch_add(c.mismatches, std::memory_order_relaxed);
				totals.stale.fetch_add(c.stale, std::memory_order_relaxed);
				totals.nested.fetch_add(c.nested, std::memory_order_relaxed);
				totals.fenced.fetch_add(c.fenced, std::memory_order_relaxed);
				totals.refused.fetch_add(c.refused, std::memory_order_relaxed);
				totals.blocked.fetch_add(c.blocked, std::memory_order_relaxed);
				c = Counters{};
			}
			if (a_state.mode != Mode::Off && (++a_state.flushes & 63) == 0)
				Report(a_state.mode);
		}

		// 106350 + 0x424: the one outside call to the shape walk. It marks where a refresh starts and ends.
		struct RefreshWalk
		{
			static bool thunk(RE::NiAVObject* a_object, void* a_scene, RE::BSLight** a_lights, bool a_flag)
			{
				ThreadState& state = threadState;
				if (state.depth++ == 0) {
					state.mode = CurrentMode();
					state.tainted = false;
					if (state.mode != Mode::Off)
						state.memo.Begin();
				} else {
					// Not seen in the engine's code. If a refresh ever starts inside another, the engine does all of both from here.
					state.tainted = true;
					++state.counters.nested;
				}
				const bool result = func(a_object, a_scene, a_lights, a_flag);
				if (--state.depth == 0)
					Flush(state);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// 106353 + 0x14E: the call to BSLight's add-geometry routine, once per shape and light.
		struct AddGeometry
		{
			static void thunk(RE::BSLight* a_light, RE::BSGeometry* a_shape)
			{
				ThreadState& state = threadState;
				if (state.depth != 1 || state.tainted || state.mode == Mode::Off || !a_light || !a_shape) {
					func(a_light, a_shape);
					return;
				}
				Counters& c = state.counters;

				// The routine's own early exits, in its order (108283 + 0x5D). It does nothing in these cases; it is still the one to say so.
				void* property = Read<void*>(a_shape, kShapeShaderProperty);
				void* lightData = property ? Read<void*>(property, kPropertyLightData) : nullptr;
				ValidityFn* valid = lightData ? ResolveValidity() : nullptr;
				if (!valid || !valid(property, a_light)) {
					++c.refused;
					func(a_light, a_shape);
					return;
				}

				// A light under rebuild moves the nodes it finds in front of its fence. That is the routine's work.
				if (Read<const Node*>(a_light, kLightListFence)) {
					++c.fenced;
					state.memo.Block(a_light);
					func(a_light, a_shape);
					return;
				}

				Memo::Entry* entry = state.memo.Find(a_light);
				if (entry && entry->blocked) {
					++c.blocked;
					func(a_light, a_shape);
					return;
				}
				const Node* cursor = entry ? entry->node : nullptr;
				if (cursor && cursor->item != entry->item) {
					// Cannot happen inside one refresh: the node was found in it and its light was added to its shape again.
					++c.stale;
					state.tainted = true;
					Trip("a remembered list node no longer holds its shape");
					func(a_light, a_shape);
					return;
				}

				const Node* head = Read<const Node*>(a_light, kLightListHead);
				++c.lookups;
				const Node* node = LightShapeSearchLogic::Find(head, cursor, a_shape, c.steps);

				if (state.mode == Mode::Verify) {
					const Node* walked = LightShapeSearchLogic::FindFromHead(head, a_shape, c.engineSteps);
					if ((node != nullptr) != (walked != nullptr) || (node && node->item != a_shape)) {
						++c.mismatches;
						ReportMismatch(a_light, a_shape, node, walked, cursor);
					}
					func(a_light, a_shape);  // the engine does the work
					if (node && walked) {
						++c.found;
						state.memo.Remember(a_light, node, a_shape);
					} else {
						++c.notFound;
						state.memo.Block(a_light);
					}
					return;
				}

				if (node) {
					// All the routine does for a shape it finds while no fence is set (108283 + 0x184): AddLight, then the exit.
					++c.found;
					addLight(lightData, a_light, reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(a_shape) + kShapeWorldBound));
					state.memo.Remember(a_light, node, a_shape);
					return;
				}

				// Not in the list: the routine adds it. A light that misses once is new to this object, so the rest of this refresh is the routine's too.
				++c.notFound;
				state.memo.Block(a_light);
				func(a_light, a_shape);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool IsSupported()
	{
		return installed.load(std::memory_order_relaxed);
	}

	bool HasTripped()
	{
		return tripped.load(std::memory_order_relaxed);
	}

	Counters GetCounters()
	{
		return LoadTotals();
	}

	void Install()
	{
		if (!REL::Module::IsAE() || REL::Module::get().version() != SKSE::RUNTIME_SSE_1_6_1170) {
			logger::info("[LLF] Light shape search not applied: it is only verified on Skyrim 1.6.1170");
			return;
		}

		using Util::VersionedRelocation::IsCallTo;
		const std::uintptr_t refresh = REL::ID(kRefreshId).address();
		const std::uintptr_t refreshWalk = REL::ID(kRefreshWalkId).address();
		const std::uintptr_t addGeometry = REL::ID(kAddGeometryId).address();
		const std::uintptr_t addLightAddress = REL::ID(kAddLightId).address();
		const std::uintptr_t beginLights = REL::ID(kBeginLightsId).address();
		const std::uintptr_t finishLights = REL::ID(kFinishLightsId).address();

		const char* mismatch = nullptr;
		if (!REL::verify_code(refresh + kRefreshCallArgs, RefreshCallArgs) || !IsCallTo(refresh + kRefreshWalkCall, refreshWalk))
			mismatch = "the per-object light refresh (its call to the shape walk)";
		else if (!REL::verify_code(refreshWalk, RefreshWalkEntry) || !REL::verify_code(refreshWalk + kShapeBlock, ShapeBlock) ||
				 !REL::verify_code(refreshWalk + kShapeBlockLights, ShapeBlockLights) || !REL::verify_code(refreshWalk + kShapeBlockNext, ShapeBlockNext) ||
				 !REL::verify_code(refreshWalk + kShapeBlockEnd, ShapeBlockEnd))
			mismatch = "the shape walk";
		else if (!IsCallTo(refreshWalk + kBeginLightsCall, beginLights) || !IsCallTo(refreshWalk + kAddGeometryCall, addGeometry) ||
				 !IsCallTo(refreshWalk + kFinishLightsCall, finishLights))
			mismatch = "the shape walk's three calls (another plugin's hook)";
		else if (!REL::verify_code(beginLights, BeginLightsBody) || !REL::verify_code(finishLights, FinishLightsEntry))
			mismatch = "the begin / finish of a shape's light array";
		else if (!REL::verify_code(addGeometry, AddGeometryEntry) || !REL::verify_code(addGeometry + kChecks, Checks) ||
				 !REL::verify_code(addGeometry + kSearch, Search) || !REL::verify_code(addGeometry + kFoundBranch, FoundBranch) ||
				 !REL::verify_code(addGeometry + kFoundAddLight, FoundAddLight) || !REL::verify_code(addGeometry + kExit, Exit))
			mismatch = "BSLight's add-geometry routine";
		else if (!IsCallTo(addGeometry + kFoundAddLightCall, addLightAddress))
			mismatch = "the AddLight call of BSLight's add-geometry routine (another plugin's hook)";
		if (mismatch) {
			logger::warn("[LLF] Light shape search not applied: unexpected code at {} (another mod or an unverified runtime)", mismatch);
			return;
		}

		auto& trampoline = SKSE::GetTrampoline();
		if (trampoline.free_size() < 2 * kTrampolineBytesPerHook) {
			logger::warn("[LLF] Light shape search not applied: the hook trampoline has {} bytes left, it needs {}", trampoline.free_size(),
				2 * kTrampolineBytesPerHook);
			return;
		}

		validityCallSite = addGeometry + kValidityCall;
		addLight = reinterpret_cast<AddLightFn*>(addLightAddress);
		stl::write_thunk_call<RefreshWalk>(refresh + kRefreshWalkCall);
		stl::write_thunk_call<AddGeometry>(refreshWalk + kAddGeometryCall);
		installed = true;
		logger::info("[LLF] Installed light shape search (on by default, switch in the Light Limit Fix settings; trampoline {} bytes left)", trampoline.free_size());
	}
}
