#pragma once

#include "skills/skill_types.h"

struct lua_State;

/**
 * Create and push a skill-scoped hw table onto Lua stack.
 * Caller owns the pushed value and should place it in sandbox env.
 */
void skill_hw_api_push_table(struct lua_State *L, int skill_id, const skill_permissions_t *permissions);

