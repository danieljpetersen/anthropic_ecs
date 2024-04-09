#include "anthropic_ecs.h"
#include <iostream>

struct ComponentPosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct ComponentVelocity {
    float vx = 0.0f;
    float vy = 0.0f;
};

struct ComponentExtra {
    bool flag = true;
};

#define ALL_COMPONENTS ComponentPosition, ComponentVelocity, ComponentExtra

int main() {
    fi::Registry<ALL_COMPONENTS> registry;

    fi::EntityId entity1 = registry.createEntity<ComponentPosition, ComponentVelocity>();
    fi::EntityId entity2 = registry.createEntity<ComponentPosition>();
    fi::EntityId entity3 = registry.createEntity<ComponentPosition, ComponentExtra>();

    registry.addComponent<ComponentVelocity>(entity2, {1.0f, 1.0f});
    registry.addComponent<ComponentExtra>(entity1, {});

    registry.forEachComponents<ComponentPosition, ComponentVelocity>(
        [&](fi::EntityId id, ComponentPosition &pos, ComponentVelocity &vel) {
            pos.x += vel.vx;
            pos.y += vel.vy;
        }
    );

    registry.forEachEntity([&](fi::EntityId id) {
        std::cout << "Entity: " << id.version << " processed\n";
    });

    registry.forEachPool([&](auto &pool) {
        std::cout << "A component pool processed\n";
    });

    registry.forEachComponentsEarlyReturn<ComponentPosition>(
        [&](fi::EntityId id, ComponentPosition &pos) {
            return true; // Stops after one iteration
        }
    );

    registry.removeComponent<ComponentExtra>(entity3);
    registry.removeEntity(entity1);

    registry.set<ComponentVelocity>(entity2, {0.0f, -1.0f});

	auto entity2Position = registry.get<ComponentPosition>(entity2);
	auto entity2Velocity = registry.get<ComponentVelocity>(entity2);
    std::cout << "Position.x: " << entity2Position->x
			  << ", Position.y: " << entity2Position->y << std::endl
			  << "Velocity.vx: " << entity2Velocity->vx
			  << ", Velocity.vy: " << entity2Velocity->vy
			  << std::endl;

    return 0;
}
