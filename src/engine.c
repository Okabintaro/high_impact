#include "engine.h"
#include "input.h"
#include "map.h"
#include "render.h"
#include "entity.h"
#include "platform.h"
#include "alloc.h"
#include "utils.h"
#include "image.h"
#include "sound.h"
#include <stdio.h>
#include <string.h>

engine_t engine = {
    .time_real = 0,
    .time_scale = 1.0,
    .time = 0,
    .tick = 0,
    .frame = 0,
    .collision_map = NULL,
    .gravity = 1.0,
};


static scene_t *scene = NULL;
static scene_t *scene_next = NULL;

static texture_mark_t init_textures_mark;
static image_mark_t init_images_mark;
static bump_mark_t init_bump_mark;
static sound_mark_t init_sounds_mark;

static bool is_running = false;

extern void main_init(void);
extern void main_cleanup(void);

void engine_init(void) {
	engine.time_real = platform_now();
	render_init(platform_screen_size());
	sound_init(platform_samplerate());
	platform_set_audio_mix_cb(sound_mix_stereo);
	input_init();
	entities_init();
	main_init();

	init_bump_mark = bump_mark();
	init_images_mark = images_mark();
	init_sounds_mark = sound_mark();
	init_textures_mark = textures_mark();
}

void engine_cleanup(void) {
	entities_cleanup();
	main_cleanup();
	input_cleanup();
	sound_cleanup();
	render_cleanup();
}


typedef struct {
	uint16_t first_gid;
	uint16_t tile_count;
	uint16_t tile_width;
	uint16_t tile_height;
	char *image_path;
	char *tsj_path;
	json_t *json;
} tiled_tileset_t;

#define MAX_TILESETS 8
typedef struct {
	uint16_t tile_size;
	uint8_t tilesets_len;
	tiled_tileset_t tilesets[MAX_TILESETS];
} tiled_map_info_t;

static char *parent_dir(const char *path) {
	const size_t path_len = strlen(path);
	char *pdir = bump_alloc(path_len);
	strncpy(pdir, path, path_len);
	char *last_slash = strrchr(pdir, '/');
	if (last_slash) {
		*last_slash = '\0';
		return pdir;
	} else {
		return NULL;
	}
}


static char *norm_path(const char *path) {
	size_t path_len = strlen(path);
	char *path_dup = bump_alloc(path_len + 1);
	char *ret = strncpy(path_dup, path, path_len + 1);
#define MAX_PARTS 32
	char *parts[MAX_PARTS] = {0};
	int parts_len = 0;

	char *token = strtok(path_dup, "/");
	for (int i = 0; i < strlen(path); i++) {
		if (token == NULL) {
			break;
		}
		if (parts_len >= MAX_PARTS) {
			die("Too many parts");
		}
		parts[parts_len++] = token;
		token = strtok(NULL, "/");
	}

	for (int i = 0; i < parts_len; i++) {
		if (str_equals(parts[i], "..")) {
			if (i - 1 < 0) {
				die("Can't resolve parent/upper dir");
			}

			parts[i] = NULL;
			for (int j = i - 1; j > 0; j--) {
				if (parts[j] != NULL) {
					parts[j] = NULL;
					break;
				}
			}
		}
	}

	char *new_path = bump_alloc(path_len);
	memset(new_path, 0, path_len);
	for (int i = 0; i < parts_len; i++) {
		if (parts[i]) {
			strcat(new_path, parts[i]);
			if (i < parts_len - 1) {
				strcat(new_path, "/");
			}
		}
	}
	return new_path;
}


map_t *map_from_tiled_layer_json(json_t *def, tiled_map_info_t *info) {
	error_if(engine_is_running(), "Cannot create map during gameplay");

	char *layer_name = json_string(json_value_for_key(def, "name"));
	char *type = json_string(json_value_for_key(def, "type"));
	if (!str_equals(type, "tilelayer")) {
		fprintf(stderr, "layer %s is not a tilelayer\n", layer_name);
		return NULL;
	}

	map_t *map = bump_alloc(sizeof(map_t));

	map->size.x = json_number(json_value_for_key(def, "width"));
	map->size.y = json_number(json_value_for_key(def, "height"));
	map->tile_size = info->tile_size;

	// map.distance = parallax_x & parallax_y
	json_t *parallax_x_v = json_value_for_key(def, "parallaxx");
	if (parallax_x_v) {
		float parallax_x = json_number(parallax_x_v);
		float parallax_y = json_number(json_value_for_key(def, "parallaxy"));
		error_if(parallax_x != parallax_y, "parallax.x and parallax.y has to be the same(=map.distance)");
		map->distance = parallax_x;
		error_if(map->distance == 0, "invalid distance for map");
	} else {
		map->distance = 1;
	}

	// Try to load other values from custom properties
	json_t *props = json_value_for_key(def, "properties");
	map->foreground = false;
	for (int i = 0; props && i < props->len; i++) {
		json_t *prop = json_value_at(props, i);

		char *prop_name = json_string(json_value_for_key(prop, "name"));
		char *prop_type = json_string(json_value_for_key(prop, "type"));
		json_t *prop_val = json_value_for_key(prop, "value");
		// TODO: Can probably infer that from order between this and entities object layer
		if (str_equals(prop_name, "foreground")) {
			error_if(!str_equals(prop_type, "bool"), "foreground property must be bool");
			map->foreground = json_bool(prop_val);
		}
		if (str_equals(prop_name, "repeat")) {
			error_if(!str_equals(prop_type, "bool"), "repeat property must be bool");
			map->repeat = json_bool(prop_val);
		}
		// TODO: Add more properties when needed
	}

	json_t *name = json_value_for_key(def, "name");
	if (name && name->type == JSON_STRING) {
		error_if(name->len > 15, "Map name exceeds 15 chars: %s", name->string);
		strcpy(map->name, name->string);
	}

	uint16_t min_tile = 0xFFFF;
	uint16_t max_tile = 0;
	json_t *data = json_value_for_key(def, "data");
	error_if(data == NULL, "map has no data");
	map->data = bump_alloc(sizeof(uint16_t) * map->size.x * map->size.y);
	int index = 0;
	for (int i = 0; i < data->len; i++) {
		float raw_tile = json_number(json_value_at(data, i));
		error_if((uint64_t)raw_tile < 0 || (uint64_t)raw_tile > 0xFFFF, "tile is out of range");
		uint16_t tile = raw_tile;
		map->data[index++] = tile;

		if (tile > 0 && tile < min_tile) {
			min_tile = tile;
		} else if (tile > 0 && tile > max_tile) {
			max_tile = tile;
		}
	}
	if (max_tile == 0) {
		fprintf(stderr, "warning: map layer %s has no actual data(every tile is 0)\n", map->name);
		return map;
	}

	// TODO: Try to figure out which tileset got used by data
	int matched_tileset_idx = -1;
	for (int i = 0; i < info->tilesets_len; i++) {
		tiled_tileset_t ts = info->tilesets[i];
		if (min_tile >= ts.first_gid && max_tile <= ts.first_gid + ts.tile_count) {
			matched_tileset_idx = i;
			break;
		}
	}
	// TODO: Clarify message
	error_if(matched_tileset_idx == -1, "No matching tileset found for given layer");
	tiled_tileset_t matched_tileset = info->tilesets[matched_tileset_idx];
	// TODO: Fix this condition, there seems to be something wrong above too
	// error_if(max_tile > (matched_tileset.first_gid + matched_tileset.tile_count), "The layer seems to have more than one tileset(unsupported)");

	char *tileset_image_path = matched_tileset.image_path;
	if (tileset_image_path && tileset_image_path[0]) {
		printf("loaded map %d %d %s\n", map->size.x, map->size.y, tileset_image_path);
	}

	// TODO: Path resolution with name again
	const size_t MAX_PATHLEN = 256;
	char *resolved_image_path = bump_alloc(MAX_PATHLEN);
	// TODO: Double check and test on windows too
	char *tsj_folder = parent_dir(matched_tileset.tsj_path);
	int result = snprintf(resolved_image_path, MAX_PATHLEN, "%s/%s", tsj_folder, tileset_image_path);
	error_if(result < 0 || result >= MAX_PATHLEN, "path is too long or short!");
	char *new_resolved_path = norm_path(resolved_image_path);
	char *png_pos = strstr(new_resolved_path, ".png");
	strcpy(png_pos, ".qoi");

	map->tileset = image(new_resolved_path);

	// Adjust tile data by firstgid which marks the first tile in data
	uint16_t firstgid = info->tilesets[matched_tileset_idx].first_gid;
	for (int i = 0; i < map->size.x * map->size.y; i++) {
		if (map->data[i] > 0) {
			map->data[i] -= firstgid - 1;
		}
	}

	return map;
}

void entities_from_tiled_layer_json(json_t *layer_json) {
	json_t *objects = json_value_for_key(layer_json, "objects");
	error_if(!objects, "No objects in layer");

	// Remember all entities with settings; we want to apply these settings
	// only after all entities have been spawned.
	// FIXME: we do this on the stack. Should maybe use the temp alloc instead.
	struct {
		entity_t *entity;
		json_t *settings;
	} entity_settings[objects->len];
	int entity_settings_len = 0;

	for (int i = 0; objects && i < objects->len; i++) {
		json_t *obj = json_value_at(objects, i);

		char *type_name = json_string(json_value_for_key(obj, "type"));
		error_if(!type_name, "Entity has no type");
		entity_type_t type = entity_type_by_name(type_name);
		error_if(!type, "Unknown entity type %s", type_name);

		float height = json_number(json_value_for_key(obj, "height"));
		vec2_t pos = {
			// Tiled object origins are on bottom left, instead of top left
		    json_number(json_value_for_key(obj, "x")),
		    json_number(json_value_for_key(obj, "y")) - height,
		};
		entity_t *ent = entity_spawn(type, pos);

		// Use id as a name, since it gets used by references too
		json_t *id_v = json_value_for_key(obj, "id");
		error_if(!id_v, "Expected id") int id = (int)id_v->number;
		error_if(id < 0 || id >= UINT16_MAX, "id out of range");
		ent->name = bump_alloc(16);
		snprintf(ent->name, 15, "%d", id);

// Map properties to settings
// TODO Figure out how to build the object/dict
#if 0
		json_t *props = json_value_for_key(obj, "properties");
		json_t *settings = bump_alloc(sizeof(json_t) * props->len);
		char **keys = bump_alloc(sizeof(char*) * props->len);
		if (ent && props && props->type == JSON_ARRAY) {
			for (int i = 0; props && i < props->len; i++) {
				json_t *prop = json_value_at(props, i);

				entity_settings[entity_settings_len].entity = ent;
				entity_settings[entity_settings_len].settings = settings;
				entity_settings_len++;
			}
		}
#endif
	}
}

void engine_load_level_tiled(char *json_path, char *project_dir) {
	json_t *map_json = platform_load_asset_json(json_path);
	error_if(!map_json, "Could not load level json at %s", json_path);

	entities_reset();
	engine.background_maps_len = 0;
	engine.collision_map = NULL;

	uint16_t map_tile_width = json_number(json_value_for_key(map_json, "tilewidth"));
	uint16_t map_tile_height = json_number(json_value_for_key(map_json, "tileheight"));
	error_if(map_tile_height != map_tile_width, "tilewidth and tileheight must be the same(square tiles)");

	tiled_map_info_t info = {
	    .tile_size = map_tile_width,
	};

	// Load tilesets first
	json_t *tilesets = json_value_for_key(map_json, "tilesets");
	for (int i = 0; tilesets && i < tilesets->len; i++) {
		json_t *tileset = json_value_at(tilesets, i);
		char *source = json_string(json_value_for_key(tileset, "source"));
		uint16_t firstgid = json_number(json_value_for_key(tileset, "firstgid"));

		const size_t MAX_PATHLEN = 256;
		char *tsj_path = bump_alloc(MAX_PATHLEN);
		// TODO: Double check and test on windows too
		int result = snprintf(tsj_path, MAX_PATHLEN, "%s/%s", project_dir, source);
		error_if(result < 0 || result >= MAX_PATHLEN, "path is too long or short!");

		json_t *tileset_json = platform_load_asset_json(tsj_path);

		uint16_t tile_count = json_number(json_value_for_key(tileset_json, "tilecount"));
		error_if(tile_count == 0, "tilecount is 0");
		uint16_t ts_tile_width = json_number(json_value_for_key(tileset_json, "tilewidth"));
		uint16_t ts_tile_height = json_number(json_value_for_key(tileset_json, "tileheight"));
		char *image = json_string(json_value_for_key(tileset_json, "image"));
		// TODO: Warn if ts_tile_width and map_tile_width are different?

		info.tilesets[i] = (tiled_tileset_t){
		    .first_gid = firstgid,
		    .tile_count = tile_count,
		    .tile_width = ts_tile_width,
		    .tile_height = ts_tile_height,
		    .image_path = image,
		    .tsj_path = tsj_path,
		    .json = tileset_json,
		};
		info.tilesets_len++;
	}

	// Load layers as maps
	json_t *layers = json_value_for_key(map_json, "layers");
	for (int i = 0; layers && i < layers->len; i++) {
		json_t *layer = json_value_at(layers, i);
		char *name = json_string(json_value_for_key(layer, "name"));
		char *type = json_string(json_value_for_key(layer, "type"));
		error_if(!name || !type, "layer has no name or type");

		if (str_equals(type, "tilelayer")) {
			map_t *map = map_from_tiled_layer_json(layer, &info);
			if (map == NULL) {
				continue;
			}
			if (str_equals(name, "collision")) {
				engine_set_collision_map(map);
			} else {
				engine_add_background_map(map);
			}
		} else if (str_equals(type, "objectgroup")) {
			entities_from_tiled_layer_json(layer);
			// TODO: Load entities
		}
	}

	// Free loaded json stuff
	for (int i = 0; i < info.tilesets_len; i++) {
		temp_free(info.tilesets[i].json);
	}
	temp_free(map_json);
}


void engine_load_level(char *json_path) {
	json_t *json = platform_load_asset_json(json_path);
	error_if(!json, "Could not load level json at %s", json_path);

	entities_reset();
	engine.background_maps_len = 0;
	engine.collision_map = NULL;

	json_t *maps = json_value_for_key(json, "maps");
	for (int i = 0; maps && i < maps->len; i++) {
		json_t *map_def = json_value_at(maps, i);
		char *name = json_string(json_value_for_key(map_def, "name"));
		map_t *map = map_from_json(map_def);

		if (str_equals(name, "collision")) {
			engine_set_collision_map(map);
		} else {
			engine_add_background_map(map);
		}
	}


	json_t *entities = json_value_for_key(json, "entities");

	// Remember all entities with settings; we want to apply these settings
	// only after all entities have been spawned.
	// FIXME: we do this on the stack. Should maybe use the temp alloc instead.
	struct {
		entity_t *entity;
		json_t *settings;
	} entity_settings[entities->len];
	int entity_settings_len = 0;

	for (int i = 0; entities && i < entities->len; i++) {
		json_t *def = json_value_at(entities, i);

		char *type_name = json_string(json_value_for_key(def, "type"));
		error_if(!type_name, "Entity has no type");

		entity_type_t type = entity_type_by_name(type_name);
		error_if(!type, "Unknown entity type %s", type_name);

		vec2_t pos = {json_number(json_value_for_key(def, "x")), json_number(json_value_for_key(def, "y"))};

		entity_t *ent = entity_spawn(type, pos);
		json_t *settings = json_value_for_key(def, "settings");
		if (ent && settings && settings->type == JSON_OBJECT) {
			// Copy name, if we have one
			json_t *name = json_value_for_key(settings, "name");
			if (name && name->type == JSON_STRING) {
				ent->name = bump_alloc(name->len + 1);
				strcpy(ent->name, name->string);
			}

			entity_settings[entity_settings_len].entity = ent;
			entity_settings[entity_settings_len].settings = settings;
			entity_settings_len++;
		}
	}

	for (int i = 0; i < entity_settings_len; i++) {
		entity_settings(entity_settings[i].entity, entity_settings[i].settings);
	}
	temp_free(json);
}

void engine_add_background_map(map_t *map) {
	error_if(engine.background_maps_len >= ENGINE_MAX_BACKGROUND_MAPS, "BACKGROUND_MAPS_MAX reached");
	engine.background_maps[engine.background_maps_len++] = map;
}

void engine_set_collision_map(map_t *map) {
	engine.collision_map = map;
}

void engine_set_scene(scene_t *scene) {
	scene_next = scene;
}

void engine_update(void) {
	double time_frame_start = platform_now();

	// Do we want to switch scenes?
	if (scene_next) {
		is_running = false;
		if (scene && scene->cleanup) {
			scene->cleanup();
		}

		textures_reset(init_textures_mark);
		images_reset(init_images_mark);
		sound_reset(init_sounds_mark);
		bump_reset(init_bump_mark);
		entities_reset();

		engine.background_maps_len = 0;
		engine.collision_map = NULL;
		engine.time = 0;
		engine.frame = 0;
		engine.viewport = vec2(0, 0);

		scene = scene_next;
		if (scene->init) {
			scene->init();
		}
		scene_next = NULL;
	}
	is_running = true;

	error_if(scene == NULL, "No scene set");

	double time_real_now = platform_now();
	double real_delta = time_real_now - engine.time_real;
	engine.time_real = time_real_now;
	engine.tick = min(real_delta * engine.time_scale, ENGINE_MAX_TICK);
	engine.time += engine.tick;
	engine.frame++;

	alloc_pool() {
		if (scene->update) {
			scene->update();
		} else {
			scene_base_update();
		}

		engine.perf.update = platform_now() - time_real_now;

		render_frame_prepare();

		if (scene->draw) {
			scene->draw();
		} else {
			scene_base_draw();
		}

		render_frame_end();
		engine.perf.draw = (platform_now() - time_real_now) - engine.perf.update;
	}

	input_clear();
	temp_alloc_check();

	engine.perf.draw_calls = render_draw_calls();
	engine.perf.total = platform_now() - time_frame_start;
}

bool engine_is_running(void) {
	return is_running;
}

void engine_resize(vec2i_t size) {
	render_resize(size);
}


void scene_base_update(void) {
	entities_update();
}

void scene_base_draw(void) {
	vec2_t px_viewport = render_snap_px(engine.viewport);

	// Background maps
	for (int i = 0; i < engine.background_maps_len; i++) {
		if (!engine.background_maps[i]->foreground) {
			map_draw(engine.background_maps[i], px_viewport);
		}
	}

	entities_draw(px_viewport);

	// Foreground maps
	for (int i = 0; i < engine.background_maps_len; i++) {
		if (engine.background_maps[i]->foreground) {
			map_draw(engine.background_maps[i], px_viewport);
		}
	}
}