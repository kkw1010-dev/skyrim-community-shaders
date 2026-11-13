#pragma once

#include <cstdint>

/**
 * Answers the engine's "is this shape already in this light's list" search without walking the whole list.
 *
 * Every frame the engine refreshes the lights of each shape of the player, and of every object that moved: for each
 * shape and each light in reach it walks that light's list of shapes from the head to see whether the shape is there.
 * In a room with many lights that each cover the room, that walk takes a tenth of the main thread and more.
 *
 * Inside one refresh the search remembers, per light, the node it found last and looks beside it first: the shapes of
 * one object sit together. On a hit it does what the engine does on a hit (its validity call, then AddLight). On a
 * miss, a light under rebuild, or any doubt, the engine's own routine runs. Nothing is remembered from one refresh to
 * the next, and the engine's lists are never written.
 *
 * On by default; the switch is in the Light Limit Fix settings and is read at the start of every refresh. In verify
 * mode the search only compares its answers with a full walk and the engine does all the work.
 */
namespace LightShapeSearch
{
	/** @brief Totals since the game started. */
	struct Counters
	{
		std::uint64_t lookups = 0;      // searches answered by the search (found or not)
		std::uint64_t found = 0;        // of those, the shape was in the light's list
		std::uint64_t notFound = 0;     // of those, it was not (the engine then adds it)
		std::uint64_t steps = 0;        // nodes the search compared
		std::uint64_t engineSteps = 0;  // verify mode: nodes the engine's walk compares for the same lookups
		std::uint64_t mismatches = 0;   // verify mode: answers that differ from the engine's walk
		std::uint64_t stale = 0;        // a remembered node no longer held its shape; turns the search off
		std::uint64_t nested = 0;       // a refresh started inside a refresh; the engine did that one
		std::uint64_t fenced = 0;       // the light was being rebuilt; the engine did that call
		std::uint64_t refused = 0;      // the engine's own early exits (no light data, or the validity call said no)
		std::uint64_t blocked = 0;      // calls left to the engine for a light that missed or was fenced in this refresh
	};

	/** @brief True once both call sites are replaced, which needs Skyrim 1.6.640 or 1.6.1170 with the engine's own code at every checked place. */
	[[nodiscard]] bool IsSupported();

	/** @brief True after the search turned itself off because a remembered node went stale or the validity call could not be read. */
	[[nodiscard]] bool HasTripped();

	[[nodiscard]] Counters GetCounters();

	/** @brief Checks the engine's code and replaces the two call sites. Does nothing, and says why in the log, on any mismatch. */
	void Install();
}
