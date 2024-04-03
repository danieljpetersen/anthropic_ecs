// bit of an afterthought but I used ankerl::unordered_dense::map in the project as a drop in replacement for std::unordered_map.
// much faster iteration times compared to unordered_map. you can opt into using it via this define. https://github.com/martinus/unordered_dense
// not included in this repo
// #define ANTHROPIC_USE_ANKERL
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