# Anthropic ECS

A small (600ish lines of code) experiment in writing an Entity Component System (ECS) for C++. The name is both because I wrote it with the help of [Claude](https://claude.ai/) (an LLM from a company called Anthropic), and from the realization I had while writing it that C++ is a deeply anti-human language. Something broke in my brain working on this, and now it keeps echoing in my head that this isn't a language designed for human beings. I thought the name ironic in that respect.

## WTF is an Entity Component System?

It's a container type that's often used in gamedev. Generally people like it both because it's reasonably fast but also because it's an interesting way of composing logic in your app.

You can think of your app as a series of structs or pieces of data which belong to an object (an entity). The entity being just a tag (usually an integer) which provides a way to lookup the specific components belonging to it. You can then write systems which operate against a specific set of data, but operates on them in a way that is agnostic to entity type. In other words, it lets you write code that operates against each entity that has a given component set.

For example, you might have a system which is responsible for updating movement. You might write a system like


```cpp
registry.forEachComponent<Position, Velocity>([](EntityId id, Position &pos, Velocity &vel) {
	pos.x += vel.x;
	pos.y += vel.y;
});
```

It's cool because it operates against every entity which has both Position and Velocity, so you can have an entity which has like <Position, Velocity, Hp, etc> and an entity which has like <Position, Velocity, Text> and they'll now both get hit and update according to the logic there. 

It gives you a combinatorial explosion of interactions between things. I kind of view it like adjusting dials on a synthesizer. Just like adjusting different dials on a synthesizer can create a wide range of sounds, combining different components on entities can lead to emergent behaviors and interactions in the game world.

## Different types of ECS systems

This is an archetype ECS. There are two popular ways of writing an ECS system, archetypes and sparse sets.

Archetype-based systems group entities with identical sets of components together, allowing for contiguous memory storage and fast access patterns during iteration. It's nice in that you don't need to check whether a given entity has certain components for each loop of iteration. Adding / removing components is generally going to be more expensive than sparse sets though, because you need to transfer all components for an entity from one pool to another.

Sparse set systems manage components individually, using a dense array for each component type and a sparse array for existence checks. It's generally going to be better in scenarios where components are added or removed frequently because you don't need to move all components an entity has, but iteration is more expensive as it requires checks to ensure each entity has the components requested.

Simplified example to get the idea across:

### Sparse Set Representation
```
Sparse Array (Entity ID -> Index):
| Entity ID | Index for Position | Index for Velocity |
|-----------|--------------------|--------------------|
| E1        | 0                  | -                  |
| E2        | 1                  | 0                  |
| E3        | 2                  | 1                  |
| E4        | 3                  | -                  |

Dense Array for Position:
| Index |    Position    |
|-------|----------------|
| 0     | (x1, y1)       |
| 1     | (x2, y2)       |
| 2     | (x3, y3)       |
| 3     | (x4, y4)       |

Dense Array for Velocity:
| Index |    Velocity    |
|-------|----------------|
| 0     | (vx2, vy2)     |
| 1     | (vx3, vy3)     |
```

```
Iteration Example for Sparse Sets:

1. Start with the smaller dense array to minimize checks (in this case, Velocity).
2. Iterate through each entry in the Velocity Dense Array:
    a. Use the Velocity index to find the corresponding entity in the Sparse Array.
    b. Verify if a Position component exists for this entity by checking the Sparse Array.
    c. If both Position and Velocity components are confirmed, access them for processing.

```

### Archetype System Representation:
```
Archetype A: [Position, Velocity]
Entities: E1, E4

| Index | Entity ID |         Components           |
|-------|-----------|------------------------------|
| 0     | E1        | <E1_pos, E1_vel>             |
| 1     | E4        | <E4_pos, E4_vel>             |

Archetype B: [Position, Health]
Entities: E2, E3

| Index | Entity ID |          Components          |
|-------|-----------|------------------------------|
| 0     | E2        | <(E2_pos, E2_health>         |
| 1     | E3        | <(E3_pos, E3_health>         |
```

```
Iteration Example: Processing Entities With specific components (e.g., Position and Velocity)

1. For each pool, check if contains the relevant components.
    a. If so, iterate over each entity within the archetype.
```
There are no further checks needed once iteration starts in a specific pool. All entities are guaranteed to have the required components from the initial check on that pool.

## Architecture

This is a bit of a weird ECS implementation. In the past I've tried to write an archetype ECS, and had a lot of trouble expressing the concept in C++. It doesn't like you storing a dynamic array to things which are different with each item (which is going to be the case for the component pools as each pool you want holding a different set of components). Usually you solve this via type erasure, where you basically create a base class and your actual pool implementation inherits from this, so something like the ecs system holds a vector of ComponentPoolBase. There are many other ways to tackle it, but I didn't see any way of doing it without complications or annoyances.

I was sitting around waiting during jury duty and I realized that if I had the same type for the pool container the implementation would be significantly easier. This can be accomplished by just making the registry a template taking the set of all components. Then each component gets its own dynamic array in each pool, but you can just leave them at size 0 and never interact with them if they're not relevant to the pool in question.

This comes with important limitations. The biggest one being that you need to specify all components used by the ECS up front. Each pool also technically has more vectors than strictly needed, but the unused vectors should be effectively ignored.

It consists of the following main components:
- ComponentPools: Store entities with the same (unique) component set. Each pool contains a vector for every component type in the ECS, but again, only the component set for the entity type in question are used.
- Registry: Stores and manages all ComponentPools
- EntityId: UniqueId to retrieve components belonging to a specific entities. Consists of an unstableIndex (index in the pool), version (unique per entity), poolKey (which pool it belongs to), and a dead flag. UnstableIndex because destroying or moving entities (via adding / removing components) can invalidate this index, but we flag it when this happens, and id's are remapped when relevant. 

## Disclaimer

As I've said, this was an experiment for an idea that I had while waiting in jury duty. 99% of it was written over the course of 9 hours on the day after. It is not thoroughly tested and no doubt contains bugs or inefficiencies. I'm sure this is unnecessary to say, but you should not use this. I would recommend [EnTT](https://github.com/skypjack/entt) or [flecs](https://github.com/SanderMertens/flecs).

## Compiler Support

Developed with C++20 against GCC 13.1. In truth I have no idea if it will work with a C++17 compiler, nor do I know whether it will work with msvc or clang.

### Usage

Every public function:

```cpp
#include "anthropic_ecs.h"
#include <iostream>

struct Component1 {
    float example = 2.0f;
};

struct Component2 {
    int whatever = 9;
};

struct Component3 {
    bool youGetTheIdea = true;
};

#define ALL_COMPONENTS Component1, Component2, Component3

int main() {
    Registry<ALL_COMPONENTS> registry;
    EntityId entity1 = registry.createEntity<Component1, Component2>();
    EntityId entity2 = registry.createEntity<Component1, Component3>();
    EntityId entity3 = registry.createEntity<Component1>();
    registry.addComponent<Component3>(entity1, Component3{});

    int i = 0;
    registry.forEachComponents<Component1, Component2>([&](EntityId entityId, Component1 &component1, Component2 &component2) {
        component1.example += 2;

        std::cout << i++ << " (forEachComponents<Component1, Component2>): entity ID: " << entityId.unstableIndex << ", Version: " << entityId.version << ", poolKey: " << entityId.poolKey
                  << " (component1.example): " << component1.example << std::endl;
    });

    ComponentPool<Component1, Component2, Component3> *pool = registry.getPool<Component1, Component2, Component3>();
    if (pool) {
        std::cout << "This pool size: " << pool->size() << ". Note that it is size 0. It returns the entity count inside this pool, not within the registry" << std::endl;

        if (pool->hasComponent<Component1>()) {
            std::cout << "Just showing the hasComponent function" << std::endl;
        }

        if (pool->hasComponents<Component1, Component2>()) {
            std::cout << "Just showing the hasComponents function" << std::endl;
        }
    }

    i = 0;
    registry.forEachEntity([&](EntityId entityId) {
        std::cout << i++ << " (registry.forEachEntity): entity ID: " << entityId.unstableIndex << ", Version: " << entityId.version << ", poolKey: " << entityId.poolKey << std::endl;
    });

    i = 0;
    registry.forEachPool([&](ComponentPool<ALL_COMPONENTS> &pool) {
        std::cout << i++ << " (registry.forEachPool)" << std::endl;
    });

    i = 0;
    registry.forEachComponentsEarlyReturn<Component1>([&](EntityId id, Component1 &component1) {
        std::cout << i++ << " (forEachComponentsEarlyReturn)" << std::endl;

        return true; // true for early return. this does one iteration
    });

    registry.removeComponent<Component3>(entity1);

    registry.removeEntity(entity3);
    registry.addComponent<Component2>(entity2, {
            .whatever = 97
    });

    std::cout << "(Entity1->example) " << registry.get<Component1>(entity1)->example << std::endl;

    registry.set<Component2>(entity2, Component2 {
        .whatever = 37
    });

    std::cout << "(entity2->whatever): " << registry.get<Component2>(entity2)->whatever << std::endl;
}
```

## Future

Things I may still do:
    - Add a ctx() similar to entt. In other words, singleton components (they would still be able to be assigned to entities, but the one in the ctx() would be unique)
    - Add an overload to createEntity which takes Components&& and forwards to the relevant constructor
    - Add a batch removeEntity
    - Add a batch createEntity
    - Add a batch removeComponents<Components>(e)
    - Add a batch addComponents<Components>(e) and addComponents<Components>(e, Components&&...)
	- Add tests

Things which would be nice but I am not going to do:
    - Allow for entity and component addition/removal during iteration. This would complicate things too much
    - Remove the template<SetOfAllComponents> from the ComponentPool class. This simplified the implementation, and I don't see much gain from removing it


## License

This project is licensed under the MIT License.
