// Offline test of the light shape search (src/Features/LightLimitFix/LightShapeSearchLogic.h). No engine, no game.
//
// Build and run (any C++20 compiler):
//   cl /nologo /std:c++20 /O2 /EHsc /W4 tests\light_shape_search_test.cpp /Fe:light_shape_search_test.exe
//   light_shape_search_test.exe [seed] [rounds]
//
// Part 1 checks Find against the engine's walk on random lists and cursors.
// Part 2 runs a model of the engine's light lists twice with the same random script: once as the engine does it, once
// through the search with its per-refresh memo. After every step both worlds must be identical (every light's list in
// order, every shape's light array in order), and every remembered node the search uses must still be a live node of
// that light's list. The model follows SkyrimSE 1.6.1170: BSLight::AddGeometry (108283), the fence pass (108295 /
// 108296), BSShaderPropertyLightData::AddLight (107777), the per-shape begin / finish (107782 / 107783), and a node
// pool that hands the most recently freed node out first, so a stale pointer would be reused at once.

#include "../src/Features/LightLimitFix/LightShapeSearchLogic.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <unordered_set>
#include <vector>

using LightShapeSearchLogic::Find;
using LightShapeSearchLogic::FindFromHead;
using LightShapeSearchLogic::Memo;
using LightShapeSearchLogic::Node;

namespace
{
	int failures = 0;

	void Check(bool a_ok, const char* a_what, int a_line)
	{
		if (!a_ok) {
			if (++failures <= 20)
				std::printf("FAIL line %d: %s\n", a_line, a_what);
		}
	}
#define CHECK(x) Check((x), #x, __LINE__)

	struct Shape;

	struct Light
	{
		int id = 0;
		bool dynamic = false;
		Node* head = nullptr;
		Node* tail = nullptr;
		int count = 0;
		Node* fence = nullptr;
		Node fenceNode{};
	};

	struct Shape
	{
		int id = 0;
		std::uint32_t flags = 0;
		std::vector<Light*> lights;
		int fence = -1;
		bool changed = false;
	};

	struct Stats
	{
		std::uint64_t lookups = 0, hits = 0, misses = 0, steps = 0, engineSteps = 0, fallbackSteps = 0, fenced = 0, invalid = 0, remembered = 0;
	};

	struct World
	{
		std::vector<std::unique_ptr<Node>> storage;
		std::vector<Node*> freeNodes;
		std::unordered_set<const Node*> live;
		std::vector<std::unique_ptr<Light>> lights;
		std::vector<std::unique_ptr<Shape>> shapes;
		bool useSearch = false;
		Memo memo;
		Stats stats;

		Node* Alloc()
		{
			Node* node;
			if (!freeNodes.empty()) {
				node = freeNodes.back();  // the most recently freed node first
				freeNodes.pop_back();
			} else {
				storage.push_back(std::make_unique<Node>());
				node = storage.back().get();
			}
			*node = Node{};
			live.insert(node);
			return node;
		}

		void Free(Node* a_node)
		{
			live.erase(a_node);
			a_node->next = reinterpret_cast<Node*>(std::uintptr_t{ 0xDEAD0000 });  // a freed node must never be followed
			a_node->prev = nullptr;
			a_node->item = nullptr;
			freeNodes.push_back(a_node);
		}

		static bool Valid(const Shape& a_shape, const Light& a_light)
		{
			const std::uint32_t h = (static_cast<std::uint32_t>(a_shape.id) * 2654435761u) ^ (static_cast<std::uint32_t>(a_light.id) * 40503u) ^ (a_shape.flags * 97u);
			return (h >> 7) % 8 != 0;  // one pair in eight is refused; a shape's flags can change over time
		}

		bool InList(const Light& a_light, const Node* a_node) const
		{
			for (const Node* n = a_light.head; n; n = n->next)
				if (n == a_node)
					return true;
			return false;
		}

		void Unlink(Light& a_light, Node* a_node)
		{
			if (a_node->prev)
				a_node->prev->next = a_node->next;
			else
				a_light.head = a_node->next;
			if (a_node->next)
				a_node->next->prev = a_node->prev;
			else
				a_light.tail = a_node->prev;
			--a_light.count;
		}

		// BSShaderPropertyLightData::AddLight (107777)
		void AddLight(Shape& a_shape, Light& a_light)
		{
			auto& arr = a_shape.lights;
			const auto it = std::find(arr.begin(), arr.end(), &a_light);
			if (a_shape.fence == -1) {
				if (it == arr.end()) {
					arr.push_back(&a_light);
					a_shape.changed = true;
				}
				return;
			}
			if (it != arr.end())
				arr.erase(it);
			else
				a_shape.changed = true;
			arr.insert(arr.begin() + std::min<std::size_t>(static_cast<std::size_t>(a_shape.fence), arr.size()), &a_light);
			++a_shape.fence;
		}

		// BSLight::AddGeometry (108283), as the engine runs it
		void AddGeometry(Light& a_light, Shape& a_shape)
		{
			if (!Valid(a_shape, a_light))
				return;
			Node* found = nullptr;
			for (Node* n = a_light.fence ? a_light.fence : a_light.head; n; n = n->next) {
				++stats.engineSteps;
				if (n->item == &a_shape) {
					found = n;
					break;
				}
			}
			if (found) {
				if (a_light.fence && found != a_light.fence) {  // move in front of the fence
					Node* fence = a_light.fence;
					Unlink(a_light, found);
					found->prev = fence->prev;
					found->next = fence;
					if (fence->prev)
						fence->prev->next = found;
					else
						a_light.head = found;
					fence->prev = found;
					++a_light.count;
				}
				AddLight(a_shape, a_light);
				return;
			}
			AddLight(a_shape, a_light);
			Node* node = Alloc();
			node->item = &a_shape;
			node->prev = nullptr;
			node->next = a_light.head;
			if (a_light.head)
				a_light.head->prev = node;
			else
				a_light.tail = node;
			a_light.head = node;
			++a_light.count;
		}

		// What the hook does in place of the call to 108283 inside the per-object refresh
		void AddGeometryThroughSearch(Light& a_light, Shape& a_shape)
		{
			if (!Valid(a_shape, a_light)) {
				++stats.invalid;
				AddGeometry(a_light, a_shape);
				return;
			}
			if (a_light.fence) {
				++stats.fenced;
				memo.Block(&a_light);
				AddGeometry(a_light, a_shape);
				return;
			}
			Memo::Entry* entry = memo.Find(&a_light);
			if (entry && entry->blocked) {
				AddGeometry(a_light, a_shape);
				return;
			}
			const Node* cursor = entry ? entry->node : nullptr;
			if (cursor) {
				CHECK(live.count(cursor) == 1);      // never a freed node
				CHECK(InList(a_light, cursor));      // never a node of another list
				CHECK(cursor->item == entry->item);  // the tripwire of the real hook must stay silent
			}
			++stats.lookups;
			const Node* node = Find(a_light.head, cursor, &a_shape, stats.steps);
			std::uint64_t reference = 0;
			const Node* walked = FindFromHead(a_light.head, &a_shape, reference);
			CHECK((node != nullptr) == (walked != nullptr));
			CHECK(!node || node->item == &a_shape);
			if (node) {
				++stats.hits;
				AddLight(a_shape, a_light);  // all the engine does on a hit without a fence
				memo.Remember(&a_light, node, &a_shape);
				++stats.remembered;
				return;
			}
			++stats.misses;
			memo.Block(&a_light);  // a light that misses once is new to this object: the engine does the rest of this refresh
			AddGeometry(a_light, a_shape);
		}

		// BSShaderPropertyLightData finish (107783)
		void Finish(Shape& a_shape)
		{
			a_shape.changed = a_shape.fence != -1;
			for (int i = static_cast<int>(a_shape.lights.size()) - 1; i >= a_shape.fence && i >= 0; --i) {
				Light* light = a_shape.lights[static_cast<std::size_t>(i)];
				if (!light->dynamic) {
					for (Node* n = light->head; n; n = n->next) {
						if (n->item == &a_shape) {
							Unlink(*light, n);
							Free(n);
							break;
						}
					}
					a_shape.lights.erase(a_shape.lights.begin() + i);
				}
			}
			a_shape.fence = -1;
		}

		// The per-object refresh (106350 -> 106353)
		void Refresh(const std::vector<int>& a_shapes, const std::vector<int>& a_lights)
		{
			if (useSearch)
				memo.Begin();
			for (const int s : a_shapes) {
				Shape& shape = *shapes[static_cast<std::size_t>(s)];
				shape.fence = 0;  // 107782
				for (const int l : a_lights) {
					Light& light = *lights[static_cast<std::size_t>(l)];
					if (useSearch)
						AddGeometryThroughSearch(light, shape);
					else
						AddGeometry(light, shape);
				}
				Finish(shape);
			}
		}

		void BeginPass(Light& a_light)  // 108295
		{
			Node* fence = &a_light.fenceNode;
			fence->item = nullptr;
			fence->prev = nullptr;
			fence->next = a_light.head;
			if (a_light.head)
				a_light.head->prev = fence;
			else
				a_light.tail = fence;
			a_light.head = fence;
			++a_light.count;
			a_light.fence = fence;
		}

		void EndPass(Light& a_light)  // 108296
		{
			Node* fence = a_light.fence;
			Node* stale = fence->next;
			Unlink(a_light, fence);
			a_light.fence = nullptr;
			while (stale) {
				Node* next = stale->next;
				if (stale->item) {
					auto* shape = const_cast<Shape*>(static_cast<const Shape*>(stale->item));
					const auto it = std::find(shape->lights.begin(), shape->lights.end(), &a_light);
					if (it != shape->lights.end())
						shape->lights.erase(it);
				}
				Unlink(a_light, stale);
				Free(stale);
				stale = next;
			}
		}

		// A light's own rebuild pass over the shapes in its reach, in scene order; a refresh may run inside it
		void Pass(int a_light, const std::vector<int>& a_reach, const std::vector<int>* a_nestedShapes, const std::vector<int>* a_nestedLights)
		{
			Light& light = *lights[static_cast<std::size_t>(a_light)];
			BeginPass(light);
			std::size_t i = 0;
			for (const int s : a_reach) {
				if (a_nestedShapes && i++ == a_reach.size() / 2)
					Refresh(*a_nestedShapes, *a_nestedLights);
				AddGeometry(light, *shapes[static_cast<std::size_t>(s)]);
			}
			EndPass(light);
		}

		// A shape leaves the scene: every light lets it go
		void Drop(int a_shape)
		{
			Shape& shape = *shapes[static_cast<std::size_t>(a_shape)];
			for (auto& light : lights) {
				for (Node* n = light->head; n; n = n->next) {
					if (n->item == &shape) {
						Unlink(*light, n);
						Free(n);
						break;
					}
				}
			}
			shape.lights.clear();
			shape.fence = -1;
		}

		void Dump(std::vector<int>& a_out) const
		{
			for (const auto& light : lights) {
				a_out.push_back(-1000 - light->id);
				a_out.push_back(light->count);
				int n = 0;
				const Node* last = nullptr;
				for (const Node* node = light->head; node; node = node->next) {
					a_out.push_back(node->item ? static_cast<const Shape*>(node->item)->id : -1);
					CHECK(node->prev == last);
					last = node;
					++n;
				}
				CHECK(n == light->count);
				CHECK(light->tail == last);
			}
			for (const auto& shape : shapes) {
				a_out.push_back(-5000 - shape->id);
				a_out.push_back(shape->fence);
				for (const Light* light : shape->lights)
					a_out.push_back(light->id);
			}
		}
	};

	void TestFind(std::mt19937& a_rng)
	{
		for (int round = 0; round < 20000; ++round) {
			const int length = static_cast<int>(a_rng() % 40);
			std::vector<Node> nodes(static_cast<std::size_t>(length));
			std::vector<int> items(static_cast<std::size_t>(length));
			const bool duplicates = a_rng() % 4 == 0;
			for (int i = 0; i < length; ++i) {
				items[static_cast<std::size_t>(i)] = duplicates ? static_cast<int>(a_rng() % 8) : i;
				nodes[static_cast<std::size_t>(i)].item = &items[static_cast<std::size_t>(i)];
				nodes[static_cast<std::size_t>(i)].next = i + 1 < length ? &nodes[static_cast<std::size_t>(i) + 1] : nullptr;
				nodes[static_cast<std::size_t>(i)].prev = i ? &nodes[static_cast<std::size_t>(i) - 1] : nullptr;
			}
			// with duplicates the item is the value, not the slot: point equal values at one object
			std::vector<int> values(8);
			if (duplicates)
				for (int i = 0; i < length; ++i)
					nodes[static_cast<std::size_t>(i)].item = &values[static_cast<std::size_t>(items[static_cast<std::size_t>(i)])];
			const Node* head = length ? nodes.data() : nullptr;
			for (int query = 0; query < 6; ++query) {
				static int absent;
				const void* item = &absent;
				if (length && a_rng() % 5 != 0)
					item = nodes[a_rng() % static_cast<unsigned>(length)].item;
				const Node* cursor = length && a_rng() % 6 != 0 ? &nodes[a_rng() % static_cast<unsigned>(length)] : nullptr;
				std::uint64_t steps = 0, reference = 0;
				const Node* found = Find(head, cursor, item, steps);
				const Node* walked = FindFromHead(head, item, reference);
				CHECK((found != nullptr) == (walked != nullptr));
				CHECK(!found || found->item == item);
				CHECK(duplicates || found == walked);
				CHECK(steps <= static_cast<std::uint64_t>(length) + 1);
			}
		}
	}

	void TestMemo()
	{
		Memo memo;
		static int lightsStorage[400];
		Node node{};
		int item = 0;
		memo.Begin();
		CHECK(memo.Find(&lightsStorage[0]) == nullptr);
		int taken = 0;
		for (int i = 0; i < 400; ++i) {
			memo.Remember(&lightsStorage[i], &node, &item);
			if (memo.Find(&lightsStorage[i]))
				++taken;
		}
		CHECK(taken == static_cast<int>(Memo::kMaxUsed));  // a full table takes no more lights
		for (int i = 0; i < 400; ++i) {
			const Memo::Entry* entry = memo.Find(&lightsStorage[i]);
			CHECK(!entry || (entry->node == &node && entry->item == &item && !entry->blocked));
		}
		memo.Begin();
		for (int i = 0; i < 400; ++i)
			CHECK(memo.Find(&lightsStorage[i]) == nullptr);  // nothing survives a new refresh
		memo.Block(&lightsStorage[5]);
		memo.Remember(&lightsStorage[5], &node, &item);
		const Memo::Entry* blocked = memo.Find(&lightsStorage[5]);
		CHECK(blocked && blocked->blocked && blocked->node == nullptr);  // a blocked light stays blocked
	}

	void TestWorlds(std::mt19937& a_rng, int a_rounds, Stats& a_total)
	{
		for (int round = 0; round < a_rounds; ++round) {
			World engine, search;
			search.useSearch = true;
			const int lightCount = 2 + static_cast<int>(a_rng() % 12);
			const int objectCount = 1 + static_cast<int>(a_rng() % 8);
			std::vector<std::vector<int>> objects(static_cast<std::size_t>(objectCount));
			int shapeCount = 0;
			for (auto& object : objects) {
				const int n = 1 + static_cast<int>(a_rng() % (a_rng() % 3 == 0 ? 60 : 12));
				for (int i = 0; i < n; ++i)
					object.push_back(shapeCount++);
			}
			for (World* world : { &engine, &search }) {
				for (int i = 0; i < lightCount; ++i) {
					world->lights.push_back(std::make_unique<Light>());
					world->lights.back()->id = i;
					world->lights.back()->dynamic = (i % 4) == 3;
				}
				for (int i = 0; i < shapeCount; ++i) {
					world->shapes.push_back(std::make_unique<Shape>());
					world->shapes.back()->id = i;
				}
			}
			const auto randomLights = [&](int a_most) {
				std::vector<int> picked;
				for (int i = 0; i < lightCount; ++i)
					if (static_cast<int>(a_rng() % 100) < a_most)
						picked.push_back(i);
				std::shuffle(picked.begin(), picked.end(), a_rng);
				return picked;
			};
			for (int step = 0; step < 220; ++step) {
				const unsigned op = a_rng() % 100;
				if (op < 62) {  // the usual frame: one object's refresh
					const auto& object = objects[a_rng() % objects.size()];
					const auto picked = randomLights(step % 40 < 30 ? 80 : 35);
					engine.Refresh(object, picked);
					search.Refresh(object, picked);
				} else if (op < 80) {  // a light rebuilds its list in scene order
					const int light = static_cast<int>(a_rng() % static_cast<unsigned>(lightCount));
					std::vector<int> reach;
					for (const auto& object : objects)
						if (a_rng() % 4 != 0)
							for (const int s : object)
								if (a_rng() % 10 != 0)
									reach.push_back(s);
					const bool nested = a_rng() % 5 == 0;
					const auto& object = objects[a_rng() % objects.size()];
					const auto picked = randomLights(70);
					engine.Pass(light, reach, nested ? &object : nullptr, nested ? &picked : nullptr);
					search.Pass(light, reach, nested ? &object : nullptr, nested ? &picked : nullptr);
				} else if (op < 90) {  // a shape's flags change, so some lights refuse it from now on
					const int s = static_cast<int>(a_rng() % static_cast<unsigned>(shapeCount));
					const std::uint32_t flags = a_rng();
					engine.shapes[static_cast<std::size_t>(s)]->flags = flags;
					search.shapes[static_cast<std::size_t>(s)]->flags = flags;
				} else {  // a shape leaves the scene
					const int s = static_cast<int>(a_rng() % static_cast<unsigned>(shapeCount));
					engine.Drop(s);
					search.Drop(s);
				}
				std::vector<int> a, b;
				engine.Dump(a);
				search.Dump(b);
				CHECK(a == b);
				if (failures)
					return;
			}
			a_total.lookups += search.stats.lookups;
			a_total.hits += search.stats.hits;
			a_total.misses += search.stats.misses;
			a_total.steps += search.stats.steps;
			a_total.engineSteps += engine.stats.engineSteps;
			a_total.fallbackSteps += search.stats.engineSteps;
			a_total.fenced += search.stats.fenced;
			a_total.invalid += search.stats.invalid;
			a_total.remembered += search.stats.remembered;
		}
	}
}

namespace
{
	// A room as the game builds it: every light has run its pass over the room's statics and then the actors more than
	// once (a light that already holds a shape moves it in front of the fence, so a repeated pass leaves the list in the
	// order visited, actors last), and from then on the player and one moving NPC are refreshed every frame while two
	// lights repeat their pass every tenth frame. Later five more lights come into reach.
	void TestSteadyRoom()
	{
		constexpr int kLights = 20, kLateLights = 5, kRoom = 400, kNpc = 30, kPlayer = 60, kFrames = 100;
		World engine, search;
		search.useSearch = true;
		std::vector<int> room, npc, player, scene, firstLights, allLights;
		for (int i = 0; i < kRoom + kNpc + kPlayer; ++i) {
			(i < kRoom ? room : i < kRoom + kNpc ? npc :
												   player)
				.push_back(i);
			scene.push_back(i);
		}
		for (World* world : { &engine, &search }) {
			for (int i = 0; i < kLights + kLateLights; ++i) {
				world->lights.push_back(std::make_unique<Light>());
				world->lights.back()->id = i;
			}
			for (int i = 0; i < kRoom + kNpc + kPlayer; ++i) {
				world->shapes.push_back(std::make_unique<Shape>());
				world->shapes.back()->id = i;
			}
		}
		for (int i = 0; i < kLights + kLateLights; ++i) {
			(i < kLights ? firstLights : allLights).push_back(i);
			for (World* world : { &engine, &search }) {
				world->Pass(i, i < kLights ? scene : room, nullptr, nullptr);
				world->Pass(i, i < kLights ? scene : room, nullptr, nullptr);
			}
		}
		allLights.insert(allLights.begin(), firstLights.begin(), firstLights.end());

		// the refreshes of a frame, counted apart from the lights' own passes
		struct Count
		{
			std::uint64_t engine = 0, search = 0;
		};
		const auto frames = [&](const std::vector<int>& a_lights, const char* a_label) {
			Count count;
			const std::uint64_t lookupsBefore = search.stats.lookups, missesBefore = search.stats.misses;
			for (int frame = 0; frame < kFrames; ++frame) {
				if (frame % 10 == 9) {
					for (const int light : { frame % kLights, (frame + 7) % kLights })
						for (World* world : { &engine, &search })
							world->Pass(light, scene, nullptr, nullptr);
				}
				const std::uint64_t engineBefore = engine.stats.engineSteps;
				const std::uint64_t searchBefore = search.stats.steps + search.stats.engineSteps;
				for (World* world : { &engine, &search }) {
					world->Refresh(player, a_lights);
					world->Refresh(npc, a_lights);
				}
				count.engine += engine.stats.engineSteps - engineBefore;
				count.search += search.stats.steps + search.stats.engineSteps - searchBefore;
				std::vector<int> a, b;
				engine.Dump(a);
				search.Dump(b);
				CHECK(a == b);
			}
			const double engineSteps = static_cast<double>(count.engine) / kFrames;
			const double searchSteps = static_cast<double>(count.search) / kFrames;
			std::printf("  %-34s nodes compared a frame: engine %.0f, with the search %.0f (%.1f %%); lookups a frame %.0f, not found %llu in %d frames\n",
				a_label, engineSteps, searchSteps, 100.0 * searchSteps / engineSteps,
				static_cast<double>(search.stats.lookups - lookupsBefore) / kFrames,
				static_cast<unsigned long long>(search.stats.misses - missesBefore), kFrames);
		};
		frames(firstLights, "20 lights after their passes:");
		frames(allLights, "5 more lights come into reach:");
		frames(allLights, "the same 25 lights, later:");
	}
}

int main(int argc, char** argv)
{
	const unsigned seed = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 20261113u;
	const int rounds = argc > 2 ? std::atoi(argv[2]) : 400;
	std::mt19937 rng(seed);
	TestFind(rng);
	std::printf("part 1 (Find against the engine's walk, 120000 queries): %s\n", failures ? "FAIL" : "ok");
	TestMemo();
	std::printf("part 1b (memo table): %s\n", failures ? "FAIL" : "ok");
	Stats total;
	TestWorlds(rng, rounds, total);
	std::printf("part 2 (two worlds, %d rounds x 220 steps, seed %u): %s\n", rounds, seed, failures ? "FAIL" : "ok");
	std::printf("  lookups %llu, found %llu, not found %llu, refused by the validity check %llu, fenced %llu\n",
		static_cast<unsigned long long>(total.lookups), static_cast<unsigned long long>(total.hits),
		static_cast<unsigned long long>(total.misses), static_cast<unsigned long long>(total.invalid),
		static_cast<unsigned long long>(total.fenced));
	std::printf("  nodes compared in the search world: %llu by the search (%.2f a lookup) + %llu by the engine's own code; in the engine world: %llu\n",
		static_cast<unsigned long long>(total.steps), total.lookups ? static_cast<double>(total.steps) / static_cast<double>(total.lookups) : 0.0,
		static_cast<unsigned long long>(total.fallbackSteps), static_cast<unsigned long long>(total.engineSteps));
	std::printf("part 3 (a room of 400 shapes, a player of 60 and an NPC of 30 shapes, refreshed every frame):\n");
	TestSteadyRoom();
	std::printf("%s (%d failed checks)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
