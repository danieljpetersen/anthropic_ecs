#pragma once

#include <vector>
#include <unordered_map>
#include <algorithm>
#include <bitset>
#include <iostream>
#include <optional>
#include <functional>
#include <type_traits>
#include <tuple>
#include <cassert>

/*
ECS SUMMARY:
- Entities: unique IDs, belong to specific ComponentPool based on their component makeup
  - EntityId: unstableIndex (index in pool), version (unique per entity), poolKey (which pool it belongs to)m dead
  - Remapping: if EntityId is stale (happens after: pool->destroy, via add/remove component, and removeEntity(due to swapping back)), consult entityRemappings[version] for updated ID
- ComponentPools: store entities with the same component setup
  - Each pool contains a vector for every component type in the ECS
  - Only the component vectors relevant to the pool's archetype are used; others remain unused
  - This structure was done because it makes expressing the archetype easier with C++ static typing
- Pools exist for each unique combination of components (entity archetypes)
- Registry: manages all pools, entities, and components
- Operations on pools only involve relevant component vectors, despite the fact that they have a vector for all

EntityId Remapping Triggers:
- removeEntity: triggers remapping for swapped entity
- removeComponent: changes entity's archetype, moves to new pool, triggers remapping(s) due to swap pop from this pool to another pool. One remapping for moved entity, one remapping if swapped entity on originating pool
- addComponent: changes entity's archetype, moves to new pool, triggers remapping(s) due to swap pop from this pool to another pool. One remapping for moved entity, one remapping if swapped entity on originating pool

EntityId Lookups:
- Registry takes all EntityId by reference to potentially update stale IDs
- Check poolKey and unstableIndex first, 99% of time we should be able to index directly into the components
- If not found or dead flag set, consult entityRemappings[version] and set &EntityId (sad path, but only happens for first lookup of stale ID)

A consequence of this design is that we need to know all component types at compile time.

Usage Example:
	struct CmpVelocity { float x, y; };
	struct CmpPosition { float x, y; };
	struct CmpHp { float hp; };

	Registry<CmpPosition, CmpVelocity, CmpHp> registry;

	EntityId entity1 = registry.createEntity<CmpPosition, CmpVelocity>();
	EntityId entity2 = registry.createEntity<CmpPosition, CmpVelocity>();
	EntityId entity3 = registry.createEntity<CmpPosition, CmpHp>();

	registry.addComponent<CmpHp>(entity1, CmpHp{50.0f});

	for (int i = 0; i < 10; i++) {
		registry.forEachComponents<CmpPosition, CmpVelocity>([&](EntityId entityId, CmpPosition& position, CmpVelocity& velocity) {
			std::cout << "forEachComponent Entity ID: " << entityId.unstableIndex << ", Version: " << entityId.version << ", poolKey: " << entityId.poolKey << std::endl;
			std::cout << "Position: " << position.x << ", " << position.y << std::endl;
			position.x += 1.0f;
		});
	}


Things I may still do:
	- Allow for entity/component addition/removal during iteration
	- Add a batch removeEntity
	- Add a batch createEntity
	- Add a batch removeComponents<Components>(e)
	- Add a batch addComponents<Components>(e) and addComponents<Components>(e, Components&&...)

Things which would be nice but I am not going to do:
	- Remove the template<SetOfAllComponents> from the ComponentPool class. This simplified the implementation, and I don't see much gain from removing it
	- Add a ctx() similar to entt. In other words, singleton components.
	- Anything regarding multithreading
*/

// ----

namespace fi {

inline void fi_assert(bool condition, const char* message) {
    if (! condition) {
        std::cerr << "Assertion failed: " << message << std::endl;
        std::abort();
    }
}

// ----
// get the index for a given component in a list of template types
template<std::size_t Index, typename T, typename... Types>
struct IndexOfType;

// Base case: If the type is not found, return an index equal to the number of types.
template<std::size_t Index, typename T>
struct IndexOfType<Index, T> {
	static constexpr std::size_t value = Index;
};

// Recursive step: Check if T is the same as the first type in Types...
template<std::size_t Index, typename T, typename First, typename... Rest>
struct IndexOfType<Index, T, First, Rest...> {
	// If T is the same as First, return the current index, otherwise, continue with the rest.
	static constexpr std::size_t value = std::is_same<T, First>::value ? Index : IndexOfType<Index + 1, T, Rest...>::value;
};

template<typename T, typename... Types>
constexpr std::size_t getIndexInTypeList() {
	return IndexOfType<0, T, Types...>::value;
}

// ----

inline void hashCombine(std::size_t& seed, const std::size_t& hash) {
	seed ^= hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

// ----
// Function to combine all hashes in a vector, needed for generating a unique key for each pool + deriving pool key when template types not available
inline std::size_t combineHashes(std::vector<std::size_t> hashes) {
	std::size_t seed = 0;
	// sort required for consistent hash order, <CmpVelocity, CmpPosition> should hash the same as <CmpPosition, CmpVelocity>
	std::sort(hashes.begin(), hashes.end());
	for (const auto& hash : hashes) {
		hashCombine(seed, hash);
	}
	return seed;
}

// ----

struct EntityId {
	size_t unstableIndex{}; // the index into all components on this entities pool belonging to the entity
	size_t version{}; // each entity gets a unique version
	size_t poolKey{}; // the lookup key for this entities pool
	bool dead{}; // if true, this entity is no longer valid

	// NOTE: in order to avoid a lookup map for entities in every get case, we allow them to become stale, hence unstableIndex.
	// it works via a version check match on get. if mismatch we go through the remapping and update the ref arg (&entity id)

	bool operator==(const EntityId& other) const {
		return version == other.version;
	}

	// needed internally for the ecs. intentionally leaving out dead
	bool isIdentical(const EntityId& other) const {
		return unstableIndex == other.unstableIndex &&
			   version == other.version &&
			   poolKey == other.poolKey;
	}
};

// we want to know how the pool resolved a destroy entity call so that we can update the remapping and entity id if the pool did internally made any existing id stale
// I opted to allow id's to go stale and remap them on encountering staleness rather than forcing a lookup map for every get call. Thought it probably more efficient, maybe i'm wrong
struct RemoveEntityResult {
	bool success=false;
	bool wasSwapped=false;
	std::optional<size_t> swappedEntityVersion{}; // Only present when wasSwapped is true
	std::optional<size_t> swappedEntityUnstableIndex{}; // Only present when wasSwapped is true
};

// ----
// a template rather than a baseclass or the like is the central idea of this ECS. I was wondering if it'd make it easier to express archetypes with C++ static typing
// this results in each pool technically having more vectors than strictly needed, but unused ones are effectively ignored
template<typename... SetOfAllComponents>
class ComponentPool {
public:
	std::tuple<std::vector<SetOfAllComponents>...> components;
	std::bitset<sizeof...(SetOfAllComponents)> componentsInUseBitmask; // bitset representing the components in use
	std::vector<std::size_t> componentsInUseIndices; // indices of components in the pool
	std::vector<size_t> componentHashes; // needed for determining new pool when transferring entities between pools
	std::vector<size_t> versions; // version of each entity in the pool. used to resolve entity id get, used to check if entity is stale
	std::size_t poolSize; // number of entities in the pool
	size_t poolKey = 0;

	ComponentPool() : poolSize(0) {}

	template<typename... Components>
	void initFromTemplate(size_t entityPoolKey, const std::vector<size_t>& _componentHashes) {
		this->poolKey = entityPoolKey;
		this->componentHashes = _componentHashes;
		componentsInUseIndices = getComponentIndices<Components...>(std::make_index_sequence<sizeof...(Components)>{});
		(componentsInUseBitmask.set(getIndexInTypeList<std::decay_t<Components>, SetOfAllComponents...>()), ...);
		reserveVectors();
	}

	void initFromBitmask(size_t entityPoolKey, const std::vector<size_t>& _componentHashes, const std::bitset<sizeof...(SetOfAllComponents)>& bitmask) {
		this->poolKey = entityPoolKey;
		this->componentHashes = _componentHashes;
		this->componentsInUseBitmask = bitmask;
		componentsInUseIndices.clear();
		for (std::size_t i = 0; i < bitmask.size(); ++i) {
			if (bitmask.test(i)) {
				componentsInUseIndices.push_back(i);
			}
		}
		reserveVectors();
	}

	void reserveVectors() {
		const size_t reserveCount = 1000; // todo: make parameter somewhere in registry
		for (std::size_t index : componentsInUseIndices) {
			accessComponentsVecByIndex(index, [&](auto &componentVector) {
				componentVector.reserve(reserveCount);
				versions.reserve(reserveCount);
			});
		}
	}

	template<typename... Components>
	void createEntity(EntityId &expectedEntityId, Components... entityComponents) {
		(std::get<std::vector<Components>>(components).push_back(entityComponents), ...);
		fi_assert(expectedEntityId.unstableIndex == poolSize, "Unexpected entity index");
		fi_assert(poolSize == versions.size(), "Unexpected versions size");
		versions.push_back(expectedEntityId.version);
		poolSize++;
	}

	void createEntityFromBitmask(EntityId &expectedEntityId) {
		for (std::size_t index : componentsInUseIndices) {
			accessComponentsVecByIndex(index, [&](auto &componentVector) {
				using ComponentType = typename std::decay_t<decltype(componentVector)>::value_type;
				componentVector.push_back(ComponentType{});
			});
		}

		fi_assert(expectedEntityId.unstableIndex == poolSize, "Unexpected entity index");
		fi_assert(poolSize == versions.size(), "Unexpected versions size");
		versions.push_back(expectedEntityId.version);
		poolSize++;
	}

	RemoveEntityResult removeEntity(EntityId entityId) {
		RemoveEntityResult result{
				.success = false,
				.wasSwapped = false,
				.swappedEntityVersion = {},
				.swappedEntityUnstableIndex = {},
		};

		if (poolSize == 0) {
			return result;
		}

		if (! isValid(entityId)) {
			return result;
		}

		result.success = true;
		if (poolSize > 1) {
			result.wasSwapped = true;
			result.swappedEntityUnstableIndex = entityId.unstableIndex;
			result.swappedEntityVersion = versions.back();
		}

		for (std::size_t componentIndex: componentsInUseIndices) {
			accessComponentsVecByIndex(componentIndex, [&](auto &vec) {
				if (entityId.unstableIndex < vec.size() - 1) {
					std::swap(vec[entityId.unstableIndex], vec.back());
				}
				vec.pop_back();
			});
		}
		std::swap(versions[entityId.unstableIndex], versions.back());
		versions.pop_back();
		poolSize--;

		return result;
	}

	inline bool isValid(EntityId id) {
		if (id.dead)
			return false;
		if (id.unstableIndex >= poolSize)
			return false;
		if (id.version != versions[id.unstableIndex])
			return false;

		return true;
	}

	// NOTE: addComponent and removeComponent do not make sense on this object as each pool is a specific collection of components. Use registry instead.

	template<typename Component>
	bool hasComponent() const {
		std::size_t componentIndex = getIndexInTypeList<std::decay_t<Component>, SetOfAllComponents...>();
		return componentsInUseBitmask.test(componentIndex);
	}

	template<typename... Components>
	bool hasComponents() const {
		std::bitset<sizeof...(SetOfAllComponents)> checkMask;
		(checkMask.set(getIndexInTypeList<std::decay_t<Components>, SetOfAllComponents...>()), ...);

		// Perform a logical AND between the pool's component mask and the check mask
		return (componentsInUseBitmask & checkMask) == checkMask;
	}

	template<typename... Components, typename Func>
	void forEach(Func callback) {
		for (std::size_t i = 0; i < poolSize; ++i) {
			EntityId id;
			id.unstableIndex = i;
			id.version = versions[i];
			id.poolKey = poolKey;
			id.dead = false;
			callback(id, std::get<std::vector<Components>>(components)[i]...);
		}
	}

	template<typename... Components, typename Func>
	bool forEachEarlyReturn(Func callback) {
		for (std::size_t i = 0; i < poolSize; ++i) {
			EntityId id;
			id.unstableIndex = i;
			id.version = versions[i];
			id.poolKey = poolKey;
			id.dead = false;
			auto result = callback(id, std::get<std::vector<Components>>(components)[i]...);

			if (result) {
				return true;
			}
		}

		return false;
	}

	template<typename Component>
	Component* getComponent(EntityId entityId) {
		if (!hasComponents<Component>()) {
			return nullptr;
		}

		if (isValid(entityId)) {
			return &std::get<std::vector<Component>>(components)[entityId.unstableIndex];
		}
		return nullptr;
	}

	std::size_t size() const {
		return poolSize;
	}

	template<typename Func>
	void accessComponentsVecByIndex(size_t index, Func&& func) {
		accessComponentsVecByIndexImpl(index, std::forward<Func>(func), std::index_sequence_for<SetOfAllComponents...>{});
	}

	template<typename Component>
	std::vector<Component>* getComponentVector() {
		return &std::get<getIndexInTypeList<Component, SetOfAllComponents...>()>(components);
	}

private:
	// required to call the functor with the right component vector based on runtime index
	template<typename Func, size_t... Is>
	void accessComponentsVecByIndexImpl(size_t index, Func&& func, std::index_sequence<Is...>) {
		// List of lambdas that call the functor with the corresponding component vector
		std::initializer_list<int>{(index == Is ? (func(randomAccessComponentTuple<Is>()), 0) : 0)...};
	}

	template<size_t index>
	auto& randomAccessComponentTuple() {
		return std::get<index>(components);
	}

	template<typename... Components, std::size_t... Indices>
	std::vector<std::size_t> getComponentIndices(std::index_sequence<Indices...>) {
		return {getIndexInTypeList<std::decay_t<Components>, SetOfAllComponents...>()...};
	}
};

template<typename... SetOfAllComponents>
class Registry {
private:
	std::unordered_map<size_t, ComponentPool<SetOfAllComponents...>> pools;
	std::unordered_map<std::size_t, EntityId> entityRemappings;
	int nextVersionIndex = 0;

	// It would heavily complicate things to allow for entity removal/addition or component addition/removal during iteration.
	// Therefore, we static_assert isIterating == false when these operations occur. user code will need to defer
	bool isIterating = false;

	std::pair<size_t, std::vector<size_t>> generateComponentPoolKeyFromHashes(std::vector<size_t> typeHashes) {
		size_t combinedHash = combineHashes(typeHashes);
		return {(combinedHash), typeHashes};
	}

	template<typename... Components>
	std::pair<size_t, std::vector<size_t>> generateComponentPoolKeyFromTemplate() {
		std::vector<size_t> typeHashes = {typeid(Components).hash_code()...};
		return generateComponentPoolKeyFromHashes(typeHashes);
	}

	void handleRemoveResult(RemoveEntityResult& removeResult, size_t poolKey) {
		if (! removeResult.success) {
			return;
		}

		if (removeResult.wasSwapped) {
			if (removeResult.swappedEntityVersion.has_value() && removeResult.swappedEntityUnstableIndex.has_value()) {
				EntityId swappedEntityId = {
						.unstableIndex = removeResult.swappedEntityUnstableIndex.value(),
						.version = removeResult.swappedEntityVersion.value(),
						.poolKey = poolKey,
						.dead = false
				};
				entityRemappings[removeResult.swappedEntityVersion.value()] = swappedEntityId;
			}
		}
	}

	template <typename ComponentToSkip>
	void transferEntityToNewPool(EntityId& oldEntityId, EntityId& newEntityId, ComponentPool<SetOfAllComponents...>& oldPool, ComponentPool<SetOfAllComponents...>& newPool) {
		fi_assert(!isIterating, "Cannot add/remove entities, and cannot add/remove components during iteration.");

		newPool.createEntityFromBitmask(newEntityId);
		fi_assert(newEntityId.unstableIndex == newPool.size() - 1, "Unexpected new entity index");
		fi_assert(newEntityId.version == newPool.versions.back(), "Unexpected new entity version");

		for (std::size_t componentIndex : newPool.componentsInUseIndices) {
			if (componentIndex == getIndexInTypeList<std::decay_t<ComponentToSkip>, SetOfAllComponents...>()) {
				continue;
			}

			oldPool.accessComponentsVecByIndex(componentIndex, [&](auto &oldComponentVector) {
				newPool.accessComponentsVecByIndex(componentIndex, [&](auto &newComponentVector) {
					using ComponentTypeOld = typename std::decay_t<decltype(oldComponentVector)>::value_type;
					using ComponentTypeNew = typename std::decay_t<decltype(newComponentVector)>::value_type;

					// i'm not sure why this constexpr is necessary, runtime we only hit the proper type but i guess it still generates all the cases regardless leading to compiler errors w/o the constexpr
					if constexpr (std::is_same_v<ComponentTypeOld, ComponentTypeNew>) {
						newComponentVector[newEntityId.unstableIndex] = oldComponentVector[oldEntityId.unstableIndex];
					}
				});
			});
		}

		RemoveEntityResult removeResult = oldPool.removeEntity(oldEntityId);
		handleRemoveResult(removeResult, oldPool.poolKey);
		entityRemappings[newEntityId.version] = newEntityId;
		oldEntityId = newEntityId;
	}

	bool resolveEntityId(EntityId& entityId, ComponentPool<SetOfAllComponents...>*& pool) {
		if (entityId.dead) {
			return false;
		}

		auto currentPoolIt = pools.find(entityId.poolKey);
		if (currentPoolIt != pools.end()) {
			if (currentPoolIt->second.isValid(entityId)) {
				pool = &currentPoolIt->second;
				return true;
			}
		}

		auto remapIt = entityRemappings.find(entityId.version);
		if (remapIt != entityRemappings.end()) {
			if (remapIt->second.isIdentical(entityId)) {
				entityId.dead = true;
				remapIt->second.dead = true;
				return false;
			}

			entityId = remapIt->second;
			return resolveEntityId(entityId, pool);
		}

		return false;
	}

public:
	template<typename... Components>
	EntityId createEntity() {
		fi_assert(!isIterating, "Cannot add/remove entities, and cannot add/remove components during iteration.");

		auto [key, representation] = generateComponentPoolKeyFromTemplate<Components...>();
		auto it = pools.find(key);

		if (it == pools.end()) {
			auto result = pools.emplace(key, ComponentPool<SetOfAllComponents...>());
			it = result.first;
			it->second.template initFromTemplate<Components...>(key, representation);
		}

		EntityId entityId;
		entityId.unstableIndex = it->second.size();
		entityId.version = nextVersionIndex++;
		entityId.poolKey = it->first;
		entityId.dead = false;

		it->second.template createEntity(entityId, Components{}...);

		return entityId;
	}

	template<typename... Components>
	EntityId createEntity(Components&&... components) {
		static_assert((... && std::is_constructible_v<Components>), "All components must be constructible with provided arguments.");

		fi_assert(!isIterating, "Cannot add/remove entities, and cannot add/remove components during iteration.");

		auto [key, representation] = generateComponentPoolKeyFromTemplate<Components...>();
		auto it = pools.find(key);

		if (it == pools.end()) {
			auto result = pools.emplace(key, ComponentPool<SetOfAllComponents...>());
			it = result.first;
			it->second.template initFromTemplate<Components...>(key, representation);
		}

		EntityId entityId;
		entityId.unstableIndex = it->second.size();
		entityId.version = nextVersionIndex++;
		entityId.poolKey = it->first;
		entityId.dead = false;

		it->second.template createEntity<Components...>(entityId, std::forward<Components>(components)...);

		return entityId;
	}

	void removeEntity(EntityId &entityId) {
		fi_assert(!isIterating, "Cannot add/remove entities, and cannot add/remove components during iteration.");

		ComponentPool<SetOfAllComponents...>* pool = nullptr;
		if (resolveEntityId(entityId, pool)) {
			RemoveEntityResult removeResult = pool->removeEntity(entityId);

			if (removeResult.success) {
				entityId.dead = true;
			}

			handleRemoveResult(removeResult, pool->poolKey);
		}
	}

	template<typename ComponentToAdd>
	void addComponent(EntityId& entityId, const ComponentToAdd& component) {
		fi_assert(!isIterating, "Cannot add/remove entities, and cannot add/remove components during iteration.");

		ComponentPool<SetOfAllComponents...>* oldPool = nullptr;
		if (resolveEntityId(entityId, oldPool)) {
			if (oldPool->template hasComponent<ComponentToAdd>()) {
				*this->get<ComponentToAdd>(entityId) = component;
				return;
			}

			std::vector<size_t> intermediateHashes = oldPool->componentHashes;
			intermediateHashes.push_back(typeid(ComponentToAdd).hash_code());
			auto [newPoolKey, newRepresentation] = generateComponentPoolKeyFromHashes(intermediateHashes);

			auto newPoolIt = pools.find(newPoolKey);
			if (newPoolIt == pools.end()) {
				auto [it, inserted] = pools.emplace(newPoolKey, ComponentPool<SetOfAllComponents...>());
				newPoolIt = it;

				oldPool = &pools.find(entityId.poolKey)->second;

				auto nextBitmask = oldPool->componentsInUseBitmask;
				nextBitmask.set(getIndexInTypeList<std::decay_t<ComponentToAdd>, SetOfAllComponents...>(), true);
				newPoolIt->second.initFromBitmask(newPoolKey, newRepresentation, nextBitmask);
			}

			EntityId newEntityId;
			newEntityId.unstableIndex = newPoolIt->second.size();
			newEntityId.version = entityId.version;
			newEntityId.poolKey = newPoolKey;
			newEntityId.dead = false;

			transferEntityToNewPool<ComponentToAdd>(entityId, newEntityId, *oldPool, newPoolIt->second);

			constexpr std::size_t newComponentIndex = getIndexInTypeList<std::decay_t<ComponentToAdd>, SetOfAllComponents...>();
			newPoolIt->second.accessComponentsVecByIndex(newComponentIndex, [&](auto &newComponentVector) {
				using ComponentType = typename std::decay_t<decltype(newComponentVector)>::value_type;
				if constexpr (std::is_same_v<std::decay_t<ComponentType>, std::decay_t<ComponentToAdd>>) {
					newComponentVector[newEntityId.unstableIndex] = component;
				} else {
					std::cout << "Error: Component type mismatch in addComponent\n";
					std::abort(); // there's a logical error in the ecs code if we hit this. it should be unreachable
				}
			});
		}
	}

	template<typename ComponentToRemove>
	void removeComponent(EntityId& entityId) {
		fi_assert(!isIterating, "Cannot add/remove entities, and cannot add/remove components during iteration.");

		ComponentPool<SetOfAllComponents...>* oldPool = nullptr;
		if (!resolveEntityId(entityId, oldPool)) {
			return;
		}

		if (!oldPool->template hasComponent<ComponentToRemove>()) {
			return;
		}

		std::vector<size_t> intermediateHashes = oldPool->componentHashes;
		intermediateHashes.erase(std::remove(intermediateHashes.begin(), intermediateHashes.end(), typeid(ComponentToRemove).hash_code()), intermediateHashes.end());
		auto [newPoolKey, newRepresentation] = generateComponentPoolKeyFromHashes(intermediateHashes);

		auto newPoolIt = pools.find(newPoolKey);
		if (newPoolIt == pools.end()) {
			auto [it, inserted] = pools.emplace(newPoolKey, ComponentPool<SetOfAllComponents...>());
			newPoolIt = it;

			oldPool = &pools.find(entityId.poolKey)->second;

			auto nextBitmask = oldPool->componentsInUseBitmask;
			nextBitmask.set(getIndexInTypeList<std::decay_t<ComponentToRemove>, SetOfAllComponents...>(), false);
			newPoolIt->second.initFromBitmask(newPoolKey, newRepresentation, nextBitmask);
		}

		EntityId newEntityId;
		newEntityId.unstableIndex = newPoolIt->second.size();
		newEntityId.version = entityId.version;
		newEntityId.poolKey = newPoolKey;
		newEntityId.dead = false;

		transferEntityToNewPool<ComponentToRemove>(entityId, newEntityId, *oldPool, newPoolIt->second);
	}

	template<typename Component>
	void set(EntityId& entityId, Component&& component) {
		ComponentPool<SetOfAllComponents...>* pool = nullptr;
		if (resolveEntityId(entityId, pool)) {
			auto componentPtr = pool->template getComponent<Component>(entityId);
			if (componentPtr) {
				*componentPtr = std::forward<Component>(component);
			}
		}
	}

	template<typename Component>
	Component* get(EntityId& entityId) {
		ComponentPool<SetOfAllComponents...>* pool = nullptr;
		if (resolveEntityId(entityId, pool)) {
			return pool->template getComponent<Component>(entityId);
		}

		return nullptr;
	}

	template<typename... Components>
	ComponentPool<SetOfAllComponents...>* getPool() {
		auto [key, representation] = generateComponentPoolKeyFromTemplate<Components...>();
		auto it = pools.find(key);

		if (it != pools.end()) {
			return &(it->second);
		}

		return nullptr;
	}

	void forEachPool(std::function<void(ComponentPool<SetOfAllComponents...>&)> callback) {
		isIterating = true;
		for (auto& poolPair : pools) {
			if (poolPair.second.size() == 0) {
				continue;
			}
			ComponentPool<SetOfAllComponents...>& pool = poolPair.second;
			callback(pool);
		}
		isIterating = false;
	}

	template<typename... Components, typename Func>
	void forEachComponents(Func callback) {
		isIterating = true;
		for (auto& poolPair : pools) {
			auto& pool = poolPair.second;

			if (pool.template hasComponents<Components...>()) {
				pool.template forEach<Components...>(callback);
			}
		}
		isIterating = false;
	}

	template<typename... Components, typename Func>
	void forEachComponentsEarlyReturn(Func callback) {
		isIterating = true;
		for (auto& poolPair : pools) {
			auto& pool = poolPair.second;

			if (pool.template hasComponents<Components...>()) {
				if (pool.template forEachEarlyReturn<Components...>(callback)) {
					break;
				}
			}
		}
		isIterating = false;
	}

	void forEachEntity(const std::function<void(EntityId)> &callback) {
		isIterating = true;
		for (auto& poolPair : pools) {
			auto& pool = poolPair.second;

			for (std::size_t i = 0; i < pool.size(); ++i) {
				EntityId entityId;
				entityId.unstableIndex = i;
				entityId.version = pool.versions[i];
				entityId.poolKey = poolPair.first;
				entityId.dead = false;

				callback(entityId);
			}
		}
		isIterating = true;
	}
};

}