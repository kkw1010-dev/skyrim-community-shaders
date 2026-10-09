#pragma once

namespace Util
{
	/**
	 * @brief Makes a BSResourceNiBinaryStream's destruction release its stream only once.
	 *
	 * CommonLib-NG's ~BSResourceNiBinaryStream calls the game's destructor, which releases (and at zero deletes) the
	 * stream but leaves the member set, and then the BSTSmartPointer member releases the same pointer again: a
	 * write into freed memory that the engine allocator can turn into a crash (Crash Triage f301562, s003d2). One
	 * extra reference, taken right after opening, makes the two releases reach zero together. Call it once per
	 * stream, before any early return.
	 */
	inline void BalanceStreamRelease(RE::BSResourceNiBinaryStream& a_stream)
	{
		if (a_stream.stream)
			a_stream.stream->IncRef();
	}
}
