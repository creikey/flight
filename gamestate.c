#include <chipmunk.h>
#include "types.h"

#include "ipsettings.h" // debug/developer settings

#include <stdio.h>  // assert logging
#include <string.h> // memset

// do not use any global variables to process gamestate

// super try not to depend on external libraries like enet or sokol to keep build process simple,
// gamestate its own portable submodule. If need to link to other stuff document here:
// - debug.c for debug drawing
// - chipmunk

enum
{
	PLAYERS = 1 << 0,
	BOXES = 1 << 1,
};

void __assert(bool cond, const char* file, int line, const char* cond_string)
{
	if (!cond)
	{
		fprintf(stderr, "%s:%d | Assertion %s failed\n", file, line, cond_string);
	}
}

#define assert(condition) __assert(condition, __FILE__, __LINE__, #condition)

static V2 cp_to_v2(cpVect v)
{
	return (V2) { .x = (float)v.x, .y = (float)v.y };
}

static cpVect v2_to_cp(V2 v)
{
	return cpv(v.x, v.y);
}

bool was_entity_deleted(GameState* gs, EntityID id)
{
	if (id.generation == 0) return false; // generation 0 means null entity ID, not a deleted entity
	Entity* the_entity = &gs->entities[id.index];
	return (!the_entity->exists || the_entity->generation != id.generation);
}

// may return null if it doesn't exist anymore
Entity* get_entity(GameState* gs, EntityID id)
{
	if (id.generation == 0)
	{
		return NULL;
	}
	assert(id.index < gs->cur_next_entity);
	assert(id.index < gs->max_entities);
	Entity* to_return = &gs->entities[id.index];
	if (was_entity_deleted(gs, id))
		return NULL;
	return to_return;
}

EntityID get_id(GameState* gs, Entity* e)
{
	if (e == NULL)
		return (EntityID) { 0 };

	size_t index = (e - gs->entities);
	assert(index >= 0);
	assert(index < gs->cur_next_entity);

	return (EntityID) {
		.generation = e->generation,
			.index = (unsigned int)index,
	};
}

static Entity* cp_shape_entity(cpShape* shape)
{
	return (Entity*)cpShapeGetUserData(shape);
}

static Entity* cp_body_entity(cpBody* body)
{
	return (Entity*)cpBodyGetUserData(body);
}

static GameState* cp_space_gs(cpSpace* space)
{
	return (GameState*)cpSpaceGetUserData(space);
}

int grid_num_boxes(GameState* gs, Entity* e)
{
	assert(e->is_grid);
	int to_return = 0;

	BOXES_ITER(gs, cur, e)
		to_return++;

	return to_return;
}

void box_remove_from_boxes(GameState* gs, Entity* box)
{
	assert(box->is_box);
	Entity* prev_box = get_entity(gs, box->prev_box);
	Entity* next_box = get_entity(gs, box->next_box);
	if (prev_box != NULL)
	{
		if (prev_box->is_box)
			prev_box->next_box = get_id(gs, next_box);
		else if (prev_box->is_grid)
			prev_box->boxes = get_id(gs, next_box);
	}
	if (next_box != NULL)
	{
		assert(next_box->is_box);
		next_box->prev_box = get_id(gs, prev_box);
	}
	box->next_box = (EntityID){ 0 };
	box->prev_box = (EntityID){ 0 };
}

void on_entity_child_shape(cpBody* body, cpShape* shape, void* data);

// gs is for iterating over all child shapes and destroying those, too
static void destroy_body(GameState* gs, cpBody** body)
{
	if (*body != NULL)
	{
		cpBodyEachShape(*body, on_entity_child_shape, (void*)gs);
		cpSpaceRemoveBody(gs->space, *body);
		cpBodyFree(*body);
		*body = NULL;
	}
	*body = NULL;
}

void entity_destroy(GameState* gs, Entity* e)
{
	assert(e->exists);

	if (e->is_grid)
	{
		BOXES_ITER(gs, cur, e)
			entity_destroy(gs, cur);
	}
	if (e->is_box)
	{
		box_remove_from_boxes(gs, e);
	}

	if (e->shape != NULL)
	{
		cpSpaceRemoveShape(gs->space, e->shape);
		cpShapeFree(e->shape);
		e->shape = NULL;
	}
	destroy_body(gs, &e->body);

	Entity* front_of_free_list = get_entity(gs, gs->free_list);
	if (front_of_free_list != NULL)
		assert(!front_of_free_list->exists);
	int gen = e->generation;
	*e = (Entity){ 0 };
	e->generation = gen;
	e->next_free_entity = gs->free_list;
	gs->free_list = get_id(gs, e);
}

void on_entity_child_shape(cpBody* body, cpShape* shape, void* data)
{
	entity_destroy((GameState*)data, cp_shape_entity(shape));
}

Entity* new_entity(GameState* gs)
{
	Entity* to_return = NULL;
	if (get_entity(gs, gs->free_list) != NULL)
	{
		to_return = get_entity(gs, gs->free_list);
		assert(!to_return->exists);
		gs->free_list = to_return->next_free_entity;
	}
	else
	{
		assert(gs->cur_next_entity < gs->max_entities); // too many entities if fails
		to_return = &gs->entities[gs->cur_next_entity];
		gs->cur_next_entity++;
	}

	to_return->generation++;
	to_return->exists = true;
	return to_return;
}

void create_body(GameState* gs, Entity* e)
{
	assert(gs->space != NULL);

	if (e->body != NULL)
	{
		cpSpaceRemoveBody(gs->space, e->body);
		cpBodyFree(e->body);
		e->body = NULL;
	}

	cpBody* body = cpSpaceAddBody(gs->space, cpBodyNew(0.0, 0.0)); // zeros for mass/moment of inertia means automatically calculated from its collision shapes
	e->body = body;
	cpBodySetUserData(e->body, (void*)e);
}

V2 player_vel(GameState* gs, Entity* player)
{
	assert(player->is_player);
	Entity* potential_seat = get_entity(gs, player->currently_inside_of_box);
	if (potential_seat != NULL)
	{
		return cp_to_v2(cpBodyGetVelocity(get_entity(gs, potential_seat->shape_parent_entity)->body));
	}
	else {
		return cp_to_v2(cpBodyGetVelocity(player->body));
	}
}

void grid_create(GameState* gs, Entity* e)
{
	e->is_grid = true;
	create_body(gs, e);
}

void entity_set_rotation(Entity* e, float rot)
{
	assert(e->body != NULL);
	cpBodySetAngle(e->body, rot);
}

void entity_set_pos(Entity* e, V2 pos)
{
	assert(e->is_grid);
	assert(e->body != NULL);
	cpBodySetPosition(e->body, v2_to_cp(pos));
}

// size is (1/2 the width, 1/2 the height)
void create_rectangle_shape(GameState* gs, Entity* e, Entity* parent, V2 pos, V2 size, float mass)
{
	if (e->shape != NULL)
	{
		cpSpaceRemoveShape(gs->space, e->shape);
		cpShapeFree(e->shape);
		e->shape = NULL;
	}

	cpBB box = cpBBNew(-size.x + pos.x, -size.y + pos.y, size.x + pos.x, size.y + pos.y);
	cpVect verts[4] = {
		cpv(box.r, box.b),
		cpv(box.r, box.t),
		cpv(box.l, box.t),
		cpv(box.l, box.b),
	};

	e->shape_size = size;
	e->shape_parent_entity = get_id(gs, parent);
	e->shape = (cpShape*)cpPolyShapeInitRaw(cpPolyShapeAlloc(), parent->body, 4, verts, 0.0f); // this cast is done in chipmunk, not sure why it works
	cpShapeSetUserData(e->shape, (void*)e);
	cpShapeSetMass(e->shape, mass);
	cpSpaceAddShape(gs->space, e->shape);
}

#define PLAYER_SHAPE_FILTER cpShapeFilterNew(CP_NO_GROUP, PLAYERS, CP_ALL_CATEGORIES)

void create_player(GameState* gs, Entity* e)
{
	e->is_player = true;
	create_body(gs, e);
	create_rectangle_shape(gs, e, e, (V2) { 0 }, V2scale(PLAYER_SIZE, 0.5f), PLAYER_MASS);
	cpShapeSetFilter(e->shape, PLAYER_SHAPE_FILTER);
}

// box must be passed as a parameter as the box added to chipmunk uses this pointer in its
// user data. pos is in local coordinates. Adds the box to the grid's chain of boxes
void box_create(GameState* gs, Entity* new_box, Entity* grid, V2 pos)
{
	new_box->is_box = true;
	assert(gs->space != NULL);
	assert(grid->is_grid);

	float halfbox = BOX_SIZE / 2.0f;

	create_rectangle_shape(gs, new_box, grid, pos, (V2) { halfbox, halfbox }, 1.0f);

	cpShapeSetFilter(new_box->shape, cpShapeFilterNew(CP_NO_GROUP, BOXES, CP_ALL_CATEGORIES));

	new_box->next_box = get_id(gs, get_entity(gs, grid->boxes));
	new_box->prev_box = get_id(gs, grid);
	if (get_entity(gs, new_box->next_box) != NULL)
	{
		get_entity(gs, new_box->next_box)->prev_box = get_id(gs, new_box);
	}
	grid->boxes = get_id(gs, new_box);
}

// removes boxes from grid, then ensures that the rule that grids must not have
// holes in them is applied.
static void grid_correct_for_holes(GameState* gs, struct Entity* grid)
{
	int num_boxes = grid_num_boxes(gs, grid);
	if (num_boxes == 0)
	{
		entity_destroy(gs, grid);
		return;
	}
	if (num_boxes == 1)
		return;

	// could be a gap between boxes in the grid, separate into multiple grids

	// goal: create list of "real grids" from this grid that have boxes which are
	// ONLY connected horizontally and vertically. whichever one of these "real grids"
	// has the most blocks stays the current grid, so
	// if a player is inhabiting this ship it stays that ship.
	// The other "real grids" are allocated as new grids

#define MAX_SEPARATE_GRIDS 8
	EntityID separate_grids[MAX_SEPARATE_GRIDS] = { 0 };
	int cur_separate_grid_index = 0;
	int cur_separate_grid_size = 0;
	int processed_boxes = 0;

	int biggest_separate_grid_index = 0;
	int biggest_separate_grid_length = 0;

	// process all boxes into separate, but correctly connected, grids
	while (processed_boxes < num_boxes)
	{
		// grab an unprocessed box, one not in separate_grids, to start the flood fill
		Entity* unprocessed = get_entity(gs, grid->boxes);
		assert(unprocessed != NULL);
		assert(unprocessed->is_box);
		box_remove_from_boxes(gs, unprocessed); // no longer in the boxes list of the grid

		// flood fill from this unprocessed box, adding each result to cur_separate_grid_index,
		// removing each block from the grid
		// https://en.wikipedia.org/wiki/Flood_fill
		{
			// queue stuff @Robust use factored datastructure
			EntityID Q = get_id(gs, unprocessed);
			Entity* N = NULL;
			while (true)
			{
				assert(!was_entity_deleted(gs, Q));
				N = get_entity(gs, Q);
				if (N == NULL) // must mean that the queue is empty
					break;
				Q = N->next_box;
				if (true) // if node "inside", this is always true
				{
					N->next_box = separate_grids[cur_separate_grid_index];
					separate_grids[cur_separate_grid_index] = get_id(gs, N);
					cur_separate_grid_size++;
					processed_boxes++;

					V2 cur_local_pos = entity_shape_pos(N);
					const V2 dirs[] = {
						(V2) {
							.x = -1.0f, .y = 0.0f
						},
					   (V2) {
						.x = 1.0f, .y = 0.0f
						},
						(V2) {
						.x = 0.0f, .y = 1.0f
						},
						(V2) {
						.x = 0.0f, .y = -1.0f
						},
					};
					int num_dirs = sizeof(dirs) / sizeof(*dirs);

					for (int ii = 0; ii < num_dirs; ii++)
					{
						V2 dir = dirs[ii];
						// @Robust @Speed faster method, not O(N^2), of getting the box
						// in the direction currently needed
						V2 wanted_local_pos = V2add(cur_local_pos, V2scale(dir, BOX_SIZE));
						EntityID box_in_direction = (EntityID){ 0 };
						BOXES_ITER(gs, cur, grid)
						{
							if (V2equal(entity_shape_pos(cur), wanted_local_pos, 0.01f))
							{
								box_in_direction = get_id(gs, cur);
								break;
							}
						}

						Entity* newbox = get_entity(gs, box_in_direction);
						if (newbox != NULL)
						{
							box_remove_from_boxes(gs, newbox);
							newbox->next_box = Q;
							Q = box_in_direction;
						}
					}
				}
			}
		}

		if (cur_separate_grid_size > biggest_separate_grid_length)
		{
			biggest_separate_grid_length = cur_separate_grid_size;
			biggest_separate_grid_index = cur_separate_grid_index;
		}
		cur_separate_grid_index++;
		assert(cur_separate_grid_index < MAX_SEPARATE_GRIDS);
		cur_separate_grid_size = 0;
	}

	// create new grids for all lists of boxes except for the biggest one.
	// delete the boxes out of the current grid as I pull boxes into separate ones
	// which are no longer connected
	for (int sepgrid_i = 0; sepgrid_i < MAX_SEPARATE_GRIDS; sepgrid_i++)
	{
		EntityID cur_separate_grid = separate_grids[sepgrid_i];
		if (get_entity(gs, cur_separate_grid) == NULL)
			continue; // this separate grid is empty

		Entity* new_grid;
		if (sepgrid_i == biggest_separate_grid_index)
		{
			new_grid = grid;
		}
		else
		{
			new_grid = new_entity(gs);
			grid_create(gs, new_grid);
			cpBodySetPosition(new_grid->body, cpBodyGetPosition(grid->body));
			cpBodySetAngle(new_grid->body, cpBodyGetAngle(grid->body));
		}

		Entity* cur = get_entity(gs, cur_separate_grid);
		while (cur != NULL)
		{
			Entity* next = get_entity(gs, cur->next_box);
			box_create(gs, cur, new_grid, entity_shape_pos(cur)); // destroys next/prev fields on cur
			cur = next;
		}

		cpBodySetVelocity(new_grid->body, cpBodyGetVelocityAtWorldPoint(grid->body, v2_to_cp(grid_com(new_grid))));
		cpBodySetAngularVelocity(new_grid->body, entity_angular_velocity(grid));
	}
}

static void grid_remove_box(GameState* gs, struct Entity* grid, struct Entity* box)
{
	assert(grid->is_grid);
	assert(box->is_box);
	entity_destroy(gs, box);
	grid_correct_for_holes(gs, grid);
}

static cpBool on_damage(cpArbiter* arb, cpSpace* space, cpDataPointer userData)
{
	cpShape* a, * b;
	cpArbiterGetShapes(arb, &a, &b);

	Entity* entity_a, * entity_b;
	entity_a = cp_shape_entity(a);
	entity_b = cp_shape_entity(b);

	float damage = V2length(cp_to_v2(cpArbiterTotalImpulse(arb))) * COLLISION_DAMAGE_SCALING;

	if (entity_a->is_box && entity_a->box_type == BoxExplosive)
		entity_a->damage += 2.0f*EXPLOSION_DAMAGE_THRESHOLD;
	if (entity_b->is_box && entity_b->box_type == BoxExplosive)
		entity_b->damage += 2.0f*EXPLOSION_DAMAGE_THRESHOLD;

	if (damage > 0.05f)
	{
		// Log("Collision with damage %f\n", damage);
		entity_a->damage += damage;
		entity_b->damage += damage;

	}

	// b must be the key passed into the post step removed, the key is cast into its shape
	//cpSpaceAddPostStepCallback(space, (cpPostStepFunc)postStepRemove, b, NULL);
	//cpSpaceAddPostStepCallback(space, (cpPostStepFunc)postStepRemove, a, NULL);

	return true; // keep colliding
}

void initialize(GameState* gs, void* entity_arena, size_t entity_arena_size)
{
	*gs = (GameState){ 0 };
	memset(entity_arena, 0, entity_arena_size); // SUPER critical. Random vals in the entity data causes big problem
	gs->entities = (Entity*)entity_arena;
	gs->max_entities = (unsigned int)(entity_arena_size / sizeof(Entity));
	gs->space = cpSpaceNew();
	cpSpaceSetUserData(gs->space, (cpDataPointer)gs);                          // needed in the handler
	cpCollisionHandler* handler = cpSpaceAddCollisionHandler(gs->space, 0, 0); // @Robust limit collision type to just blocks that can be damaged
	handler->postSolveFunc = on_damage;
}
void destroy(GameState* gs)
{
	// can't zero out gs data because the entity memory arena is reused
	// on deserialization
	for (size_t i = 0; i < gs->max_entities; i++)
	{
		if (gs->entities[i].exists)
			entity_destroy(gs, &gs->entities[i]);
	}
	cpSpaceFree(gs->space);
	gs->space = NULL;
	gs->cur_next_entity = 0;
}
// center of mass, not the literal position
V2 grid_com(Entity* grid)
{
	return cp_to_v2(cpBodyLocalToWorld(grid->body, cpBodyGetCenterOfGravity(grid->body)));
}

V2 grid_vel(Entity* grid)
{
	return cp_to_v2(cpBodyGetVelocity(grid->body));
}
V2 grid_world_to_local(Entity* grid, V2 world)
{
	return cp_to_v2(cpBodyWorldToLocal(grid->body, v2_to_cp(world)));
}
V2 grid_local_to_world(Entity* grid, V2 local)
{
	return cp_to_v2(cpBodyLocalToWorld(grid->body, v2_to_cp(local)));
}
// returned snapped position is in world coordinates
V2 grid_snapped_box_pos(Entity* grid, V2 world)
{
	V2 local = grid_world_to_local(grid, world);
	local.x /= BOX_SIZE;
	local.y /= BOX_SIZE;
	local.x = roundf(local.x);
	local.y = roundf(local.y);
	local.x *= BOX_SIZE;
	local.y *= BOX_SIZE;

	return cp_to_v2(cpBodyLocalToWorld(grid->body, v2_to_cp(local)));
}
float entity_rotation(Entity* grid)
{
	return (float)cpBodyGetAngle(grid->body);
}
float entity_angular_velocity(Entity* grid)
{
	return (float)cpBodyGetAngularVelocity(grid->body);
}
Entity* box_grid(Entity* box)
{
	return (Entity*)cpBodyGetUserData(cpShapeGetBody(box->shape));
}
// in local space
V2 entity_shape_pos(Entity* box)
{
	return cp_to_v2(cpShapeGetCenterOfGravity(box->shape));
}
float entity_shape_mass(Entity* box)
{
	assert(box->shape != NULL);
	return (float)cpShapeGetMass(box->shape);
}
float box_rotation(Entity* box)
{
	return (float)cpBodyGetAngle(cpShapeGetBody(box->shape));
}
V2 entity_pos(Entity* e)
{
	if (e->is_box) {
		return V2add(entity_pos(box_grid(e)), V2rotate(entity_shape_pos(e), entity_rotation(box_grid(e))));
	}
	else {
		assert(e->body != NULL);
		return cp_to_v2(cpBodyGetPosition(e->body));
	}
}

struct BodyData
{
	V2 pos;
	V2 vel;
	float rotation;
	float angular_velocity;
};

void populate(cpBody* body, struct BodyData* data)
{
	data->pos = cp_to_v2(cpBodyGetPosition(body));
	data->vel = cp_to_v2(cpBodyGetVelocity(body));
	data->rotation = (float)cpBodyGetAngle(body);
	data->angular_velocity = (float)cpBodyGetAngularVelocity(body);
}

void update_from(cpBody* body, struct BodyData* data)
{
	cpBodySetPosition(body, v2_to_cp(data->pos));
	cpBodySetVelocity(body, v2_to_cp(data->vel));
	cpBodySetAngle(body, data->rotation);
	cpBodySetAngularVelocity(body, data->angular_velocity);
}

//#define WRITE_VARNAMES // debugging feature horrible for network
typedef struct SerState
{
	char* bytes;
	bool serializing;
	size_t cursor; // points to next available byte, is the size of current message after serializing something
	size_t max_size;
} SerState;
void ser_var(SerState* ser, char* var_pointer, size_t var_size, const char* name, const char* file, int line)
{
#ifdef WRITE_VARNAMES
	char var_name[512] = { 0 };
	snprintf(var_name, 512, "%d%s", line, name); // can't have separator before the name, when comparing names skips past the digit
	size_t var_name_len = strlen(var_name);
#endif
	if (ser->serializing)
	{

#ifdef WRITE_VARNAMES
		memcpy(ser->bytes + ser->cursor, var_name, var_name_len);
		ser->cursor += var_name_len;
#endif
		for (int b = 0; b < var_size; b++)
		{
			ser->bytes[ser->cursor] = var_pointer[b];
			ser->cursor += 1;
			assert(ser->cursor < ser->max_size);
		}
	}
	else
	{
#ifdef WRITE_VARNAMES
		{
			char read_name[512] = { 0 };

			for (int i = 0; i < var_name_len; i++)
			{
				read_name[i] = ser->bytes[ser->cursor];
				ser->cursor += 1;
				assert(ser->cursor < ser->max_size);
			}
			read_name[var_name_len] = '\0';
			// advance past digits
			char* read = read_name;
			char* var = var_name;
			while (*read >= '0' && *read <= '9')
				read++;
			while (*var >= '0' && *var <= '9')
				var++;
			if (strcmp(read, var) != 0)
			{
				printf("%s:%d | Expected variable %s but got %sn\n", file, line, var_name, read_name);
				*(char*)NULL = 0;
			}
		}
#endif
		for (int b = 0; b < var_size; b++)
		{
			var_pointer[b] = ser->bytes[ser->cursor];
			ser->cursor += 1;
			assert(ser->cursor < ser->max_size);
		}
	}

}
#define SER_VAR_NAME(var_pointer, name) ser_var(ser, (char*)var_pointer, sizeof(*var_pointer), name, __FILE__, __LINE__)
#define SER_VAR(var_pointer) SER_VAR_NAME(var_pointer, #var_pointer)

// @Robust probably get rid of this as separate function, just use SER_VAR
void ser_V2(SerState* ser, V2* var)
{
	SER_VAR(&var->x);
	SER_VAR(&var->y);
}

void ser_bodydata(SerState* ser, struct BodyData* data)
{
	ser_V2(ser, &data->pos);
	ser_V2(ser, &data->vel);
	SER_VAR(&data->rotation);
	SER_VAR(&data->angular_velocity);
}

void ser_entityid(SerState* ser, EntityID* id)
{
	SER_VAR(&id->generation);
	SER_VAR(&id->index);
}

void ser_inputframe(SerState* ser, InputFrame* i)
{
	SER_VAR(&i->tick);
	SER_VAR(&i->movement);

	SER_VAR(&i->seat_action);
	ser_entityid(ser, &i->seat_to_inhabit);
	SER_VAR(&i->hand_pos);
	ser_entityid(ser, &i->grid_hand_pos_local_to);

	SER_VAR(&i->dobuild);
	SER_VAR(&i->build_type);
	SER_VAR(&i->build_rotation);
}

void ser_player(SerState* ser, Player* p)
{
	SER_VAR(&p->connected);
	if (p->connected)
	{
		SER_VAR(&p->unlocked_bombs);
		ser_entityid(ser, &p->entity);
		ser_inputframe(ser, &p->input);
	}
}

void ser_entity(SerState* ser, GameState* gs, Entity* e)
{
	SER_VAR(&e->generation);
	SER_VAR(&e->damage);

	bool has_body = ser->serializing && e->body != NULL;
	SER_VAR(&has_body);

	if (has_body)
	{
		struct BodyData body_data;
		if (ser->serializing)
			populate(e->body, &body_data);
		ser_bodydata(ser, &body_data);
		if (!ser->serializing)
		{
			create_body(gs, e);
			update_from(e->body, &body_data);
		}
	}

	bool has_shape = ser->serializing && e->shape != NULL;
	SER_VAR(&has_shape);

	if (has_shape)
	{
		ser_V2(ser, &e->shape_size);
		ser_entityid(ser, &e->shape_parent_entity);
		Entity* parent = get_entity(gs, e->shape_parent_entity);
		assert(parent != NULL);

		V2 shape_pos;
		if (ser->serializing)
			shape_pos = entity_shape_pos(e);
		SER_VAR(&shape_pos);

		float shape_mass;
		if (ser->serializing)
			shape_mass = entity_shape_mass(e);
		SER_VAR(&shape_mass);

		cpShapeFilter filter;
		if (ser->serializing)
		{
			filter = cpShapeGetFilter(e->shape);
		}
		SER_VAR(&filter.categories);
		SER_VAR(&filter.group);
		SER_VAR(&filter.mask);
		if (!ser->serializing)
		{
			create_rectangle_shape(gs, e, parent, shape_pos, e->shape_size, shape_mass);
			cpShapeSetFilter(e->shape, filter);
		}
	}

	SER_VAR(&e->is_player);
	if (e->is_player)
	{
		ser_entityid(ser, &e->currently_inside_of_box);
		SER_VAR(&e->goldness);
	}

	SER_VAR(&e->is_explosion);
	if (e->is_explosion)
	{
		ser_V2(ser, &e->explosion_pos);
		ser_V2(ser, &e->explosion_vel);
		SER_VAR(&e->explosion_progresss);
	}

	SER_VAR(&e->is_grid);
	if (e->is_grid)
	{
		SER_VAR(&e->total_energy_capacity);
		ser_entityid(ser, &e->boxes);
	}

	SER_VAR(&e->is_box);
	if (e->is_box)
	{
		SER_VAR(&e->box_type);
		SER_VAR(&e->is_explosion_unlock);
		ser_entityid(ser, &e->next_box);
		ser_entityid(ser, &e->prev_box);
		SER_VAR(&e->compass_rotation);
		SER_VAR(&e->thrust);
		SER_VAR(&e->wanted_thrust);
		SER_VAR(&e->energy_used);
		SER_VAR(&e->sun_amount);
		ser_entityid(ser, &e->player_who_is_inside_of_me);
	}
}

void ser_server_to_client(SerState* ser, ServerToClient* s)
{
	GameState* gs = s->cur_gs;

	int cur_next_entity = 0;
	if (ser->serializing)
		cur_next_entity = gs->cur_next_entity;
	SER_VAR(&cur_next_entity);

	if (!ser->serializing)
	{
		destroy(gs);
		memset((void*)gs->entities, 0, sizeof(*gs->entities) * gs->max_entities);
		initialize(gs, gs->entities, gs->max_entities * sizeof(*gs->entities));
		gs->cur_next_entity = cur_next_entity;
	}

	SER_VAR(&s->your_player);
	SER_VAR(&gs->time);

	ser_V2(ser, &gs->goldpos);

	for (size_t i = 0; i < MAX_PLAYERS; i++)
	{
		ser_player(ser, &gs->players[i]);
	}

	if (ser->serializing)
	{
		bool entities_done = false;
		for (size_t i = 0; i < gs->cur_next_entity; i++)
		{
			Entity* e = &gs->entities[i];
			if (e->exists && !e->is_box) // boxes are serialized after their parent entities, their grid. 
			{
				SER_VAR(&entities_done);
				SER_VAR(&i);
				ser_entity(ser, gs, e);
				if (e->is_grid)
				{
					// serialize boxes always after bodies, so that by the time the boxes
					// are loaded in the parent body is loaded in and can be referenced.
					BOXES_ITER(gs, cur, e)
					{
						EntityID cur_id = get_id(gs, cur);
						assert(cur_id.index < gs->max_entities);
						SER_VAR(&entities_done);
						size_t the_index = (size_t)cur_id.index; // super critical. Type of &i is size_t. @Robust add debug info in serialization for what size the expected type is, maybe string nameof the type
						SER_VAR_NAME(&the_index, "&i");
						ser_entity(ser, gs, cur);
					}
				}
			}
		}
		entities_done = true;
		SER_VAR(&entities_done);
	}
	else
	{
		while (true)
		{
			bool entities_done = false;
			SER_VAR(&entities_done);
			if (entities_done)
				break;
			size_t next_index;
			SER_VAR_NAME(&next_index, "&i");
			assert(next_index < gs->max_entities);
			Entity* e = &gs->entities[next_index];
			e->exists = true;
			ser_entity(ser, gs, e);
			gs->cur_next_entity = (unsigned int)max(gs->cur_next_entity, next_index + 1);
		}
		for (size_t i = 0; i < gs->cur_next_entity; i++)
		{
			Entity* e = &gs->entities[i];
			if (!e->exists)
			{
				e->next_free_entity = gs->free_list;
				gs->free_list = get_id(gs, e);
			}
		}
	}
}

void into_bytes(struct ServerToClient* msg, char* bytes, size_t* out_len, size_t max_len)
{
	assert(msg->cur_gs != NULL);
	assert(msg != NULL);

	SerState ser = (SerState){
		.bytes = bytes,
		.serializing = true,
		.cursor = 0,
		.max_size = max_len,
	};

	ser_server_to_client(&ser, msg);
	*out_len = ser.cursor + 1; // @Robust not sure why I need to add one to cursor, ser.cursor should be the length..
}

void from_bytes(struct ServerToClient* msg, char* bytes, size_t max_len)
{
	assert(msg->cur_gs != NULL);
	assert(msg != NULL);

	SerState ser = (SerState){
		.bytes = bytes,
		.serializing = false,
		.cursor = 0,
		.max_size = max_len,
	};

	ser_server_to_client(&ser, msg);
}

// has to be global var because can only get this information
static cpShape* closest_to_point_in_radius_result = NULL;
static float closest_to_point_in_radius_result_largest_dist = 0.0f;
static void closest_point_callback_func(cpShape* shape, cpContactPointSet* points, void* data)
{
	assert(points->count == 1);
	if (!cp_shape_entity(shape)->is_box)
		return;
	float dist = V2length(cp_to_v2(cpvsub(points->points[0].pointA, points->points[0].pointB)));
	// float dist = -points->points[0].distance;
	if (dist > closest_to_point_in_radius_result_largest_dist)
	{
		closest_to_point_in_radius_result_largest_dist = dist;
		closest_to_point_in_radius_result = shape;
	}
}

Entity* closest_to_point_in_radius(GameState* gs, V2 point, float radius)
{
	closest_to_point_in_radius_result = NULL;
	closest_to_point_in_radius_result_largest_dist = 0.0f;

	cpBody* tmpbody = cpBodyNew(0.0f, 0.0f);
	cpShape* circle = cpCircleShapeNew(tmpbody, radius, v2_to_cp(point));
	cpSpaceShapeQuery(gs->space, circle, closest_point_callback_func, NULL);

	cpShapeFree(circle);
	cpBodyFree(tmpbody);

	if (closest_to_point_in_radius_result != NULL)
	{
		// @Robust query here for only boxes that are part of ships, could get nasty...
		return cp_body_entity(cpShapeGetBody(closest_to_point_in_radius_result));
	}

	return NULL;
}

static float cur_explosion_damage = 0.0f;
static V2 explosion_origin = { 0 };
static void explosion_callback_func(cpShape* shape, cpContactPointSet* points, void* data)
{
	GameState* gs = (GameState*)data;
	cp_shape_entity(shape)->damage += cur_explosion_damage;
	Entity* parent = get_entity(gs, cp_shape_entity(shape)->shape_parent_entity);
	V2 from_pos = entity_pos(cp_shape_entity(shape));
	V2 impulse = V2scale(V2normalize(V2sub(from_pos, explosion_origin)), EXPLOSION_PUSH_STRENGTH);
	assert(parent->body != NULL);
	cpBodyApplyImpulseAtWorldPoint(parent->body, v2_to_cp(impulse), v2_to_cp(from_pos));

}

static void do_explosion(GameState* gs, Entity* explosion, float dt)
{
	cur_explosion_damage = dt * EXPLOSION_DAMAGE_PER_SEC;
	explosion_origin = explosion->explosion_pos;

	cpBody* tmpbody = cpBodyNew(0.0f, 0.0f);
	cpShape* circle = cpCircleShapeNew(tmpbody, EXPLOSION_RADIUS, v2_to_cp(explosion_origin));

	cpSpaceShapeQuery(gs->space, circle, explosion_callback_func, (void*)gs);

	cpShapeFree(circle);
	cpBodyFree(tmpbody);
}

V2 box_facing_vector(Entity* box)
{
	assert(box->is_box);
	V2 to_return = (V2){ .x = 1.0f, .y = 0.0f };

	to_return = V2rotate(to_return, rotangle(box->compass_rotation));
	to_return = V2rotate(to_return, box_rotation(box));

	return to_return;
}

V2 thruster_force(Entity* box)
{
	return V2scale(box_facing_vector(box), -box->thrust * THRUSTER_FORCE);
}

uint64_t tick(GameState* gs)
{
	return (uint64_t)floor(gs->time / ((double)TIMESTEP));
}

V2 get_world_hand_pos(GameState* gs, InputFrame* input, Entity* player)
{
	Entity* potential_grid = get_entity(gs, input->grid_hand_pos_local_to);
	if (potential_grid != NULL)
	{
		return grid_local_to_world(potential_grid, input->hand_pos);
	}
	else {
		return V2add(entity_pos(player), input->hand_pos);
	}
}

// return true if used the energy
bool possibly_use_energy(GameState* gs, Entity* grid, float wanted_energy)
{
	BOXES_ITER(gs, possible_battery, grid)
	{
		if (possible_battery->box_type == BoxBattery && (BATTERY_CAPACITY - possible_battery->energy_used) > wanted_energy)
		{
			possible_battery->energy_used += wanted_energy;
			return true;
		}
	}
	return false;
}

void entity_ensure_in_orbit(Entity* e)
{
	cpVect pos = v2_to_cp(V2sub(entity_pos(e), SUN_POS));
	cpFloat r = cpvlength(pos);
	cpFloat v = cpfsqrt(SUN_GRAVITY_STRENGTH / r) / r;
	cpBodySetVelocity(e->body, cpvmult(cpvperp(pos), v));
}

void process(GameState* gs, float dt)
{
	assert(gs->space != NULL);

	assert(dt == TIMESTEP); // @TODO fix tick being incremented every time
	gs->time += dt;

	// process input
	PLAYERS_ITER(gs->players, player)
	{
		Entity* p = get_entity(gs, player->entity);
		if (p == NULL)
		{
			p = new_entity(gs);
			create_player(gs, p);
			player->entity = get_id(gs, p);
			entity_ensure_in_orbit(p);
		}
		assert(p->is_player);

#ifdef INFINITE_RESOURCES
		p->damage = 0.0f;
#endif
		// update gold win condition
		if (V2length(V2sub(cp_to_v2(cpBodyGetPosition(p->body)), gs->goldpos)) < GOLD_COLLECT_RADIUS)
		{
			p->goldness += 0.1f;
			p->damage = 0.0f;
			gs->goldpos = (V2){ .x = hash11((float)gs->time) * 20.0f, .y = hash11((float)gs->time - 13.6f) * 20.0f };
		}

#if 1
		V2 world_hand_pos = get_world_hand_pos(gs, &player->input, p);
		if (player->input.seat_action)
		{
			player->input.seat_action = false; // "handle" the input
			Entity* the_seat = get_entity(gs, p->currently_inside_of_box);
			if (the_seat == NULL) // not in any seat
			{
				cpPointQueryInfo query_info = { 0 };
				cpShape* result = cpSpacePointQueryNearest(gs->space, v2_to_cp(world_hand_pos), 0.1f, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, BOXES), &query_info);
				if (result != NULL)
				{
					Entity* potential_seat = cp_shape_entity(result);
					assert(potential_seat->is_box);
					if (potential_seat->box_type == BoxCockpit || potential_seat->box_type == BoxMedbay) // @Robust check by feature flag instead of box type
					{
						p->currently_inside_of_box = get_id(gs, potential_seat);
						potential_seat->player_who_is_inside_of_me = get_id(gs, p);
					}
				}
				else
				{
					Log("No seat to get into for a player at point %f %f\n", world_hand_pos.x, world_hand_pos.y);
				}
			}
			else
			{
				V2 pilot_seat_exit_spot = V2add(entity_pos(the_seat), V2scale(box_facing_vector(the_seat), BOX_SIZE));
				cpBodySetPosition(p->body, v2_to_cp(pilot_seat_exit_spot));
				cpBodySetVelocity(p->body, v2_to_cp(player_vel(gs, p)));
				the_seat->player_who_is_inside_of_me = (EntityID){ 0 };
				p->currently_inside_of_box = (EntityID){ 0 };
			}
		}
#endif

		// process movement
		{
			// no cheating by making movement bigger than length 1
			float movement_strength = V2length(player->input.movement);
			if (movement_strength != 0.0f)
			{
				player->input.movement = V2scale(V2normalize(player->input.movement), clamp(V2length(player->input.movement), 0.0f, 1.0f));
			}
			Entity* seat_inside_of = get_entity(gs, p->currently_inside_of_box);

			if (seat_inside_of == NULL)
			{
				cpShapeSetFilter(p->shape, PLAYER_SHAPE_FILTER);
				cpBodyApplyForceAtWorldPoint(p->body, v2_to_cp(V2scale(player->input.movement, PLAYER_JETPACK_FORCE)), cpBodyGetPosition(p->body));
				p->damage += movement_strength * dt * PLAYER_JETPACK_SPICE_PER_SECOND;
			}
			else
			{
				assert(seat_inside_of->is_box);
				cpShapeSetFilter(p->shape, CP_SHAPE_FILTER_NONE); // no collisions while in a seat
				cpBodySetPosition(p->body, v2_to_cp(entity_pos(seat_inside_of)));

				// set thruster thrust from movement
				if (seat_inside_of->box_type == BoxCockpit) {
					Entity* g = get_entity(gs, seat_inside_of->shape_parent_entity);

					V2 target_direction = { 0 };
					if (V2length(player->input.movement) > 0.0f)
					{
						target_direction = V2normalize(player->input.movement);
					}
					BOXES_ITER(gs, cur, g)
					{
						if (cur->box_type != BoxThruster)
							continue;
						float wanted_thrust = -V2dot(target_direction, box_facing_vector(cur));
						wanted_thrust = clamp01(wanted_thrust);
						cur->wanted_thrust = wanted_thrust;
					}
				}
			}
		}

#if 1 // building 
		if (player->input.dobuild)
		{
			player->input.dobuild = false; // handle the input. if didn't do this, after destruction of hovered box, would try to build on its grid with grid_index...

			cpPointQueryInfo info = { 0 };
			V2 world_build = world_hand_pos;

			// @Robust sanitize this input so player can't build on any grid in the world
			Entity* target_grid = get_entity(gs, player->input.grid_hand_pos_local_to);
			cpShape* nearest = cpSpacePointQueryNearest(gs->space, v2_to_cp(world_build), 0.01f, cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, BOXES), &info);
			if (nearest != NULL)
			{
				Entity* cur_box = cp_shape_entity(nearest);
				Entity* cur_grid = cp_body_entity(cpShapeGetBody(nearest));
				p->damage -= DAMAGE_TO_PLAYER_PER_BLOCK * ((BATTERY_CAPACITY - cur_box->energy_used) / BATTERY_CAPACITY);
				grid_remove_box(gs, cur_grid, cur_box);
			}
			else if (target_grid == NULL)
			{
				Entity* new_grid = new_entity(gs);
				grid_create(gs, new_grid);
				p->damage += DAMAGE_TO_PLAYER_PER_BLOCK;
				entity_set_pos(new_grid, world_build);

				Entity* new_box = new_entity(gs);
				box_create(gs, new_box, new_grid, (V2) { 0 });
				new_box->box_type = player->input.build_type;
				new_box->compass_rotation = player->input.build_rotation;
				cpBodySetVelocity(new_grid->body, v2_to_cp(player_vel(gs, p)));
			}
			else
			{
				Entity* new_box = new_entity(gs);
				box_create(gs, new_box, target_grid, grid_world_to_local(target_grid, world_build));
				grid_correct_for_holes(gs, target_grid); // no holey ship for you!
				new_box->box_type = player->input.build_type;
				new_box->compass_rotation = player->input.build_rotation;
				p->damage += DAMAGE_TO_PLAYER_PER_BLOCK;
			}
		}
#endif
		if (p->damage >= 1.0f)
		{
			entity_destroy(gs, p);
			player->entity = (EntityID){ 0 };
		}

		p->damage = clamp01(p->damage);
	}

	// process entities
	for (size_t i = 0; i < gs->cur_next_entity; i++) {
		Entity* e = &gs->entities[i];
		if (!e->exists)
			continue;

		if (e->is_explosion_unlock)
		{
			PLAYERS_ITER(gs->players, player)
			{
				Entity* player_entity = get_entity(gs, player->entity);
				if (player_entity != NULL && V2length(V2sub(entity_pos(player_entity), entity_pos(e))) < GOLD_UNLOCK_RADIUS)
				{
					player->unlocked_bombs = true;
				}
			}
		}

		if (e->body != NULL)
		{
			cpVect p = cpvsub(cpBodyGetPosition(e->body), v2_to_cp(SUN_POS));
			cpFloat sqdist = cpvlengthsq(p);
			if (sqdist > (INSTANT_DEATH_DISTANCE_FROM_SUN * INSTANT_DEATH_DISTANCE_FROM_SUN))
			{
				entity_destroy(gs, e);
				continue;
			}
			if (sqdist < (SUN_RADIUS * SUN_RADIUS))
			{
				entity_destroy(gs, e);
				continue;
			}
			cpVect g = cpvmult(p, -SUN_GRAVITY_STRENGTH / (sqdist * cpfsqrt(sqdist)));

			cpBodyUpdateVelocity(e->body, g, 1.0f, dt);
		}

		if (e->is_explosion)
		{
			e->explosion_progresss += dt;
			e->explosion_pos = V2add(e->explosion_pos, V2scale(e->explosion_vel, dt));
			do_explosion(gs, e, dt);
			if (e->explosion_progresss >= EXPLOSION_TIME)
			{
				entity_destroy(gs, e);
			}
		}

		if (e->is_box)
		{
			if (e->box_type == BoxExplosive && e->damage >= EXPLOSION_DAMAGE_THRESHOLD)
			{
				Entity* explosion = new_entity(gs);
				explosion->is_explosion = true;
				explosion->explosion_pos = entity_pos(e);
				explosion->explosion_vel = grid_vel(box_grid(e));
				grid_remove_box(gs, get_entity(gs, e->shape_parent_entity), e);
			}
			if (e->damage >= 1.0f)
			{
				grid_remove_box(gs, get_entity(gs, e->shape_parent_entity), e);
			}
		}
		if (e->is_grid)
		{
			// calculate how much energy solar panels provide
			float energy_to_add = 0.0f;
			BOXES_ITER(gs, cur, e)
			{
				if (cur->box_type == BoxSolarPanel) {
					cur->sun_amount = clamp01(V2dot(box_facing_vector(cur), V2normalize(V2sub(SUN_POS, entity_pos(cur)))));
					energy_to_add += cur->sun_amount * SOLAR_ENERGY_PER_SECOND * dt;
				}
			}

			// apply all of the energy to all connected batteries
			BOXES_ITER(gs, cur, e)
			{
				if (energy_to_add <= 0.0f)
					break;
				if (cur->box_type == BoxBattery)
				{
					float energy_sucked_up_by_battery = cur->energy_used < energy_to_add ? cur->energy_used : energy_to_add;
					cur->energy_used -= energy_sucked_up_by_battery;
					energy_to_add -= energy_sucked_up_by_battery;
				}
				assert(energy_to_add >= 0.0f);
			}

			// use the energy, stored in the batteries, in various boxes
			BOXES_ITER(gs, cur, e)
			{
				if (cur->box_type == BoxThruster)
				{
					float energy_to_consume = cur->wanted_thrust * THRUSTER_ENERGY_USED_PER_SECOND * dt;
					cur->thrust = 0.0f;
					if (possibly_use_energy(gs, e, energy_to_consume))
					{
						cur->thrust = cur->wanted_thrust;
						cpBodyApplyForceAtWorldPoint(e->body, v2_to_cp(thruster_force(cur)), v2_to_cp(entity_pos(cur)));
					}
				}
				if (cur->box_type == BoxMedbay)
				{
					Entity* potential_meatbag_to_heal = get_entity(gs, cur->player_who_is_inside_of_me);
					if (potential_meatbag_to_heal != NULL)
					{
						float energy_to_recharge = min(potential_meatbag_to_heal->damage, PLAYER_ENERGY_RECHARGE_PER_SECOND * dt);
						if (possibly_use_energy(gs, e, energy_to_recharge))
						{
							potential_meatbag_to_heal->damage -= energy_to_recharge;
						}
					}
				}
			}
		}
	}

	cpSpaceStep(gs->space, dt);
}