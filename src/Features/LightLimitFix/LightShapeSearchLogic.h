#pragma once

#include <cstddef>
#include <cstdint>

// Pure search logic of the light shape search, kept free of engine types so it can be tested offline.
namespace LightShapeSearchLogic
{
	/** @brief Layout of the engine's list node (next, previous, element). Only next and item are ever read. */
	struct Node
	{
		Node* next;
		Node* prev;
		const void* item;
	};
	static_assert(sizeof(Node) == 0x18);
	static_assert(offsetof(Node, item) == 0x10);

	/** @brief The engine's own walk: the first node from a_head that holds a_item. a_steps grows by the nodes compared. */
	[[nodiscard]] inline const Node* FindFromHead(const Node* a_head, const void* a_item, std::uint64_t& a_steps) noexcept
	{
		for (const Node* node = a_head; node; node = node->next) {
			++a_steps;
			if (node->item == a_item)
				return node;
		}
		return nullptr;
	}

	/**
	 * @brief A node of the list at a_head that holds a_item, or nullptr when the list holds none.
	 *
	 * a_cursor must be a node of that same list (or nullptr). The shapes of one object sit next to each other in a light's
	 * list, in the order a pass visited them or in the reverse order when they were added one by one at the head. So the
	 * cursor's two neighbours are tried first. After that the search steps forward from behind the cursor and forward
	 * from the head up to the cursor, in turn, so it ends close to whichever side holds the item. Every node is compared
	 * at most once: the answer is the one a full walk gives, and the cost never exceeds a full walk.
	 *
	 * The previous-node link is used for that one neighbour only, and only when the neighbour links back to the cursor.
	 */
	[[nodiscard]] inline const Node* Find(const Node* a_head, const Node* a_cursor, const void* a_item, std::uint64_t& a_steps) noexcept
	{
		if (!a_cursor)
			return FindFromHead(a_head, a_item, a_steps);

		++a_steps;
		if (a_cursor->item == a_item)
			return a_cursor;

		const Node* after = a_cursor->next;
		if (after) {
			++a_steps;
			if (after->item == a_item)
				return after;
			after = after->next;
		}

		const Node* stop = a_cursor;  // the walk from the head ends in front of this node
		if (const Node* previous = a_cursor->prev; previous && previous->next == a_cursor) {
			++a_steps;
			if (previous->item == a_item)
				return previous;
			stop = previous;
		}

		const Node* before = a_head == stop ? nullptr : a_head;
		while (after || before) {
			if (after) {
				++a_steps;
				if (after->item == a_item)
					return after;
				after = after->next;
			}
			if (before) {
				++a_steps;
				if (before->item == a_item)
					return before;
				before = before->next;
				if (before == stop)
					before = nullptr;
			}
		}
		return nullptr;
	}

	/**
	 * @brief What one refresh remembers for each light: the node it found last, and the item that node held.
	 *
	 * Begin() empties the table in constant time. A full table stops taking new lights; they are then searched from the
	 * head every time, which is what the engine does.
	 */
	class Memo
	{
	public:
		static constexpr std::size_t kSlots = 128;
		static constexpr std::size_t kMaxUsed = 96;

		struct Entry
		{
			const void* light = nullptr;
			const Node* node = nullptr;
			const void* item = nullptr;
			std::uint64_t epoch = 0;
			bool blocked = false;
		};

		/** @brief Starts a new refresh: nothing remembered before this call is returned again. */
		void Begin() noexcept
		{
			++epoch;
			used = 0;
		}

		/** @brief The entry of a_light in this refresh, or nullptr when the light is not remembered. */
		[[nodiscard]] Entry* Find(const void* a_light) noexcept
		{
			for (std::size_t i = Slot(a_light), probes = 0; probes < kSlots; i = (i + 1) & (kSlots - 1), ++probes) {
				Entry& entry = entries[i];
				if (entry.epoch != epoch)
					return nullptr;
				if (entry.light == a_light)
					return &entry;
			}
			return nullptr;
		}

		/** @brief Remembers a_node (holding a_item) as the last node found for a_light. A blocked light stays blocked. */
		void Remember(const void* a_light, const Node* a_node, const void* a_item) noexcept
		{
			if (Entry* entry = Claim(a_light); entry && !entry->blocked) {
				entry->node = a_node;
				entry->item = a_item;
			}
		}

		/** @brief Stops a_light from being remembered or returned with a node for the rest of this refresh. */
		void Block(const void* a_light) noexcept
		{
			if (Entry* entry = Claim(a_light)) {
				entry->node = nullptr;
				entry->item = nullptr;
				entry->blocked = true;
			}
		}

	private:
		[[nodiscard]] static std::size_t Slot(const void* a_light) noexcept
		{
			const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_light)) >> 4;
			return static_cast<std::size_t>((value * 0x9E3779B97F4A7C15ull) >> 57) & (kSlots - 1);
		}

		/** @brief The entry of a_light, made when missing. nullptr when the table has taken all the lights it will take. */
		[[nodiscard]] Entry* Claim(const void* a_light) noexcept
		{
			for (std::size_t i = Slot(a_light), probes = 0; probes < kSlots; i = (i + 1) & (kSlots - 1), ++probes) {
				Entry& entry = entries[i];
				if (entry.epoch != epoch) {
					if (used >= kMaxUsed)
						return nullptr;
					entry = Entry{ a_light, nullptr, nullptr, epoch, false };
					++used;
					return &entry;
				}
				if (entry.light == a_light)
					return &entry;
			}
			return nullptr;
		}

		Entry entries[kSlots]{};
		std::uint64_t epoch = 1;
		std::size_t used = 0;
	};
}
