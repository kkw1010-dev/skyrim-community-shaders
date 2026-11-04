#pragma once

#include <algorithm>
#include <d3d11.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

/**
 * @brief GPU and CPU profiler using D3D11 timestamp queries.
 *
 * Maintains a ring buffer of frames with paired begin/end timestamp queries
 * and rolling statistics (average, p95, p99) per named pass.
 */
class Profiler
{
public:
	static constexpr uint32_t kMaxTimers = 128;
	static constexpr uint32_t kFrameLatency = 3;
	static constexpr uint32_t kHistorySize = 300;
	// Must exceed the longest legitimate gap between samples of a still-running pass;
	// the DynamicCubemaps state machine spreads its passes over 6 frames.
	static constexpr uint64_t kTimerRetireFrames = 60;

	using PerfEventCallback = std::function<void(std::string_view)>;

	/** @brief Circular buffer tracking per-timer timing samples with statistics. */
	struct RollingHistory
	{
		float history[kHistorySize]{};
		uint32_t head = 0;
		uint32_t count = 0;
		float lastMs = 0.0f;

		/** @brief Appends a timing sample, overwriting the oldest if full. */
		void PushSample(float ms)
		{
			history[head] = ms;
			head = (head + 1) % kHistorySize;
			if (count < kHistorySize)
				count++;
			lastMs = ms;
		}

		/** @brief Gets the arithmetic mean of all buffered samples. */
		float GetAverage() const;

		/**
		 * @brief Gets an interpolated percentile from the buffered samples.
		 * @param p Percentile in [0, 100].
		 */
		float GetPercentile(float p) const;

		/**
		 * @brief Gets two interpolated percentiles from one sort of the buffered samples.
		 * @param p1 First percentile in [0, 100].
		 * @param p2 Second percentile in [0, 100].
		 * @param out1 Receives the first percentile.
		 * @param out2 Receives the second percentile.
		 */
		void GetPercentiles(float p1, float p2, float& out1, float& out2) const;
	};

	/** @brief Snapshot of GPU/CPU timing data for a single named pass. */
	struct TimerResult
	{
		std::string name;
		float gpuTimeMs = 0.0f;
		float avgMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		float cpuTimeMs = 0.0f;
		float cpuAvgMs = 0.0f;
		float cpuP95Ms = 0.0f;
		float cpuP99Ms = 0.0f;
		bool valid = false;

		const float* historyBuffer = nullptr;
		uint32_t historyHead = 0;
		uint32_t historyCount = 0;

		/**
		 * @brief Gets a history sample by age-ordered index (0 = oldest).
		 * @param index Zero-based index into the history ring buffer.
		 */
		float GetHistorySample(uint32_t index) const
		{
			if (!historyBuffer || index >= historyCount)
				return 0.0f;
			return historyBuffer[(historyHead - historyCount + index + kHistorySize) % kHistorySize];
		}
	};

	/**
	 * @brief Creates timestamp query objects and prepares the frame ring buffer.
	 * @param device D3D11 device used to create query objects.
	 * @param context Device context used for issuing and collecting queries.
	 */
	void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

	/** @brief Releases all D3D11 query objects and resets state. */
	void Release();

	/**
	 * @brief Registers optional callbacks invoked at pass begin/end (e.g. for RenderDoc markers).
	 * @param beginCb Called with the pass name when a pass begins.
	 * @param endCb Called when a pass ends.
	 */
	void SetPerfEventCallbacks(PerfEventCallback beginCb, PerfEventCallback endCb)
	{
		beginPerfEvent = std::move(beginCb);
		endPerfEvent = std::move(endCb);
	}

	/** @brief Begins a new profiling frame; collects results from the oldest in-flight frame. */
	void BeginFrame();

	/**
	 * @brief Begins a named GPU/CPU timing pass within the current frame.
	 * @param name Pass identifier (e.g. "Feature::PassName"). Implicitly calls BeginFrame if needed.
	 */
	void BeginPass(const std::string& name);

	/** @brief Ends the current timing pass and records the CPU/GPU timestamps. */
	void EndPass();

	/** @brief Ends the current profiling frame and advances the ring buffer write cursor. */
	void EndFrame();

	/**
	 * @brief Turns pass timing on or off from the next frame; call it at a frame boundary.
	 *
	 * Off, a pass still raises its perf events but issues no queries and reads no clock, and no
	 * results are collected: nothing shows them, and collecting cost 2-5 % of the main thread in
	 * play (sorting every timer's history each frame).
	 * @param a_active True while something shows the timings (the menu, the performance overlay).
	 */
	void SetActive(bool a_active);

	/**
	 * @brief Keeps pass timing on for the next frames, for a reader that asks on demand.
	 * @param a_frames Number of frames to keep timing on.
	 */
	void KeepActive(uint32_t a_frames) { keepActiveFrames = std::max(keepActiveFrames, a_frames); }

	/** @brief Whether passes are timed this frame. */
	bool IsActive() const { return active; }

	/** @brief Gets the per-pass timing results from the last collected frame. */
	const std::vector<TimerResult>& GetResults() const { return results; }

	/** @brief Gets the total GPU time in milliseconds for the last collected frame. */
	float GetTotalTimeMs() const { return totalTimeMs; }

	/** @brief Gets the total CPU time in milliseconds for the last collected frame. */
	float GetCpuTotalTimeMs() const { return cpuTotalTimeMs; }

	/** @brief Resets all timer history and results. */
	void ClearTimers()
	{
		results.clear();
		knownTimers.clear();
		knownTimerIndex.clear();
		collectedFrames = 0;
		totalTimeMs = 0.0f;
		cpuTotalTimeMs = 0.0f;
	}

	/**
	 * @brief Removes all timers whose names start with the given feature prefix.
	 * @param featureName Feature name; timers matching "featureName::*" are removed.
	 */
	void ClearTimersForFeature(const std::string& featureName)
	{
		std::string prefix = featureName + "::";
		std::erase_if(knownTimers, [&prefix](const KnownTimer& kt) {
			return kt.name.starts_with(prefix);
		});
		RebuildTimerIndex();
	}

private:
	struct FrameQueries
	{
		winrt::com_ptr<ID3D11Query> disjoint;
		struct TimerPair
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
			std::string name;
			LARGE_INTEGER cpuBegin{};
			float cpuMs = 0.0f;
		};
		std::vector<TimerPair> timers;
		uint32_t activeCount = 0;
		bool inFlight = false;
	};

	ID3D11DeviceContext* context = nullptr;

	FrameQueries frames[kFrameLatency];
	uint32_t writeFrame = 0;
	uint32_t readFrame = 0;
	uint32_t framesSinceInit = 0;
	bool initialized = false;
	bool frameActive = false;
	bool active = false;
	uint32_t keepActiveFrames = 0;
	double cpuTicksToMs = 0.0;

	PerfEventCallback beginPerfEvent;
	PerfEventCallback endPerfEvent;

	std::vector<TimerResult> results;

	struct KnownTimer
	{
		std::string name;
		RollingHistory gpu;
		RollingHistory cpu;
		uint64_t lastSampleFrame = 0;
	};
	std::vector<KnownTimer> knownTimers;
	std::unordered_map<std::string, size_t> knownTimerIndex;
	uint64_t collectedFrames = 0;
	float totalTimeMs = 0.0f;
	float cpuTotalTimeMs = 0.0f;

	void CollectResults();

	/** @brief Drops timers that have not been sampled for kTimerRetireFrames, so disabled passes stop reporting stale values. */
	void RetireStaleTimers();

	/** @brief Repoints knownTimerIndex at the current knownTimers positions after an erase. */
	void RebuildTimerIndex();
};
