#ifndef ACTIVATION_SCENE_H
#define ACTIVATION_SCENE_H

#include "boards.h"
#include "mui_scene_dispatcher.h"

#define ADD_SCENE(prefix, name, id) ACTIVATION_SCENE_##id,
typedef enum {
#include "activation_scene_config.h"
    ACTIVATION_SCENE_MAX,
} activation_scene_id_t;
#undef ADD_SCENE

extern const mui_scene_t activation_scene_defines[];

#endif /* ACTIVATION_SCENE_H */
