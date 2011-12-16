#include "inter_stack_tools.h"
#include <lauxlib.h>
#include <string.h>
#include <assert.h>

#include "reference.h"

static inline void copy_type(lua_State* from, lua_State* to, int i)
{
	int type = lua_type(from, i);
	switch (type) {
		case LUA_TNIL:
			l_copy_nil(from, to, i); break;
		case LUA_TBOOLEAN:
			l_copy_boolean(from, to, i); break;
		case LUA_TNUMBER:
			l_copy_number(from, to, i); break;
		case LUA_TSTRING:
			l_copy_string(from, to, i); break;
		case LUA_TTABLE:
			l_copy_table(from, to, i); break;
		case LUA_TFUNCTION:
			l_copy_function(from, to, i); break;
		case LUA_TUSERDATA:
			l_copy_udata(from, to, i); break;
		case LUA_TLIGHTUSERDATA:
			l_copy_lightudata(from, to, i); break;
		default:
			fprintf(stderr, "Cannot copy `%s' at index %d. Putting nil-value.\n", lua_typename(from, type), i);
			lua_pushnil(to);
	}
}

int string_writer(lua_State* L, const void* b, size_t size, void* ud)
{
	(void)L;
	StringBuilder* sb = (StringBuilder*)ud;
	sb->str = (char*)realloc(sb->str, sb->size + size);
	memcpy(sb->str + sb->size, b, size);
	sb->size += size;
	return 0;
}

static int function_type_check_writer(lua_State* L, const void* b, size_t s, void* u)
{
	(void)L; (void)b; (void)s; (void)u;
	return 0xFEED; // me
}

FunctionType l_functiontype(lua_State* L, int idx)
{
	if (NULL != lua_tocfunction(L, idx))
		return LUA_FT_C;

	/* with luajit, lua_dump will only attempt to dump the function if it
	 * is not inlined (or something, see lj_api.c) and otherwise return 1. */
	lua_pushvalue(L, idx);
	int res = lua_dump(L, function_type_check_writer, NULL);
	lua_pop(L, 1);
	if (0xFEED != res)
		return LUA_FT_FASTJIT;

	return LUA_FT_VM;
}

const char* l_functypename(lua_State* L, int idx)
{
	switch (l_functiontype(L, idx)) {
		case LUA_FT_C:       return "C Function";
		case LUA_FT_VM:      return "Lua Function";
		case LUA_FT_FASTJIT: return "JITed Function";
		default: return "unknown";
	}
}

void l_copy_string(lua_State* from, lua_State* to, int i)
{
	size_t len;
	const char* str = lua_tolstring(from, i, &len);
	lua_pushlstring(to, str, len);
}

void l_copy_table(lua_State* from, lua_State* to, int i)
{
#ifdef DEBUG
	int top_from = lua_gettop(from);
	int top_to = lua_gettop(to);
#endif
	lua_getfield(to, LUA_REGISTRYINDEX, "l-copy-values-visited");
	lua_pushlightuserdata(to, (void*)lua_topointer(from, i));
	lua_rawget(to, -2);

	/* value already there, remove lookup table, and leave table on top */
	if (!lua_isnil(to, -1)) {
		lua_remove(to, -2);
		return;
	}
	lua_pop(to, 1); /* remove nil */

	/* new value -> deep copy*/
	lua_createtable(to, lua_objlen(from, i), 0);
	lua_pushnil(from);
	while (lua_next(from, i) != 0) {
		/* copy key, then value */
		for (int i = -2; i <= -1; ++i)
			copy_type(from, to, i);

		lua_rawset(to, -3);
		lua_pop(from, 1);
	}

	/* copy the metatable (if any) */
	if (lua_getmetatable(from, i)) {
		l_copy_table(from, to, lua_gettop(from));
		lua_pop(from, 1);
		lua_setmetatable(to, -2);
	}

	/* put new table in lookup table and remove it from the stack leaving
	 * only the new table on top */
	lua_pushlightuserdata(to, (void*)lua_topointer(from, i));
	lua_pushvalue(to, -2);
	lua_rawset(to, -4);
	lua_remove(to, -2);
#ifdef DEBUG
	assert(lua_gettop(from) == top_from);
	assert(lua_gettop(to) == top_to + 1);
#endif
}

void l_copy_function(lua_State* from, lua_State* to, int i)
{
#ifdef DEBUG
	int top_from = lua_gettop(from);
	int top_to = lua_gettop(to);
#endif
	lua_getfield(to, LUA_REGISTRYINDEX, "l-copy-values-visited");
	lua_pushlightuserdata(to, (void*)lua_topointer(from, i));
	lua_rawget(to, -2);

	if (!lua_isnil(to, -1)) {
		lua_remove(to, -2);
		return;
	}
	lua_pop(to, 1);

	/* copy function if serializeable or c function */
	FunctionType ftype = l_functiontype(from, i);
	if (ftype == LUA_FT_VM) {
		StringBuilder b;
		string_builder_init(b);
		lua_pushvalue(from, i);
		lua_dump(from, string_writer, &b);
		lua_pop(from, 1);

		int result = luaL_loadbuffer(to, b.str, b.size, NULL);
		string_builder_cleanup(b);
		if (0 != result) {
			fprintf(stderr, "Cannot copy function at index %d: %s\n", i, lua_tostring(to, -1));
			return;
		}
	} else if (ftype == LUA_FT_C) {
		lua_pushcfunction(to, lua_tocfunction(from, i));
	} else {
		fprintf(stderr, "Cannot copy function at index %d (%s)\n", i, l_functypename(from, i));
		return;
	}

	/* copy upvalues */
	const char* name = NULL;
	for (int upidx = 1; NULL != (name = lua_getupvalue(from, i, upidx)); ++upidx) {
		if (lua_equal(from, i, -1)) /* upvalue to itself */
			lua_pushvalue(to, -1);
		else
			copy_type(from, to, lua_gettop(from));
		lua_pop(from, 1);
		if (NULL == lua_setupvalue(to, -2, upidx)) { /* shouldn't really happen */
			luaL_error(from, "Cannot copy upvalue (%s)", name);
		}
	}

	/* save function in lookup table */
	lua_pushlightuserdata(to, (void*)lua_topointer(from, i));
	lua_pushvalue(to, -2);
	lua_rawset(to, -4);
	lua_remove(to, -2);
#ifdef DEBUG
	assert(lua_gettop(from) == top_from);
	assert(lua_gettop(to) == top_to + 1);
#endif

	return;
}

void l_copy_udata(lua_State* from, lua_State* to, int i)
{
	/* check if already copied */
	lua_getfield(to, LUA_REGISTRYINDEX, "l-copy-values-visited");
	lua_pushlightuserdata(to, (void*)lua_topointer(from, i));
	lua_rawget(to, -2);

	if (!lua_isnil(to, -1)) {
		lua_remove(to, -2);
		return;
	}
	lua_pop(to, 1);

	/* check if copyable */
	lua_getmetatable(from, i);
	lua_getfield(from, LUA_REGISTRYINDEX, reference_names[REF_PLAYER]);
	lua_getfield(from, LUA_REGISTRYINDEX, reference_names[REF_SOUNDFILE]);
	int known_udata = lua_rawequal(from, -3, -2) || lua_rawequal(from, -3, -1);
	lua_pop(from, 3);

	if (!known_udata) {
		fprintf(stderr, "Cannot copy userdata at index %d: Don't know how.\n", i);
		return;
	}

	/* copy reference */
	Reference* ref_from = (Reference*)lua_touserdata(from, i);
	Reference* ref_to   = (Reference*)lua_newuserdata(to, sizeof(Reference));
	ref_to->type        = ref_from->type;
	ref_to->ref         = ref_from->ref;
	/* horrible hack that assumes refcount is the first item in the referenced struct */
	RefHeader* header = (RefHeader*)ref_from->ref;
	header->refcount++;

	/* copy metatable if needed */
	lua_getfield(to, LUA_REGISTRYINDEX, reference_names[ref_from->type]);
	if (lua_isnil(to, -1)) {
		lua_pop(to, 1);
		lua_getmetatable(from, i);
		l_copy_table(from, to, lua_gettop(from));
		lua_pop(from, 1);

		lua_pushvalue(to, -1);
		lua_setfield(to, LUA_REGISTRYINDEX, reference_names[ref_from->type]);
	}
	lua_setmetatable(to, -2);

	/* save udata in lookup table */
	lua_pushlightuserdata(to, (void*)lua_topointer(from, i));
	lua_pushvalue(to, -2);
	lua_rawset(to, -4);
	lua_remove(to, -2);
}

void l_copy_values(lua_State* from, lua_State* to, int n)
{
	int top_from = lua_gettop(from);
#ifdef DEBUG
	int top_to = lua_gettop(to);
	assert(top_from >= n);
#endif

	/* create lookup-table for visited values:
	 * lookup[pointer] == value  => value already visited
	 * lookup[pointer] == nil    => new value
	 */
	lua_newtable(to);
	lua_setfield(to, LUA_REGISTRYINDEX, "l-copy-values-visited");

	for (int i = top_from - n + 1; i <= top_from; ++i)
		copy_type(from, to, i);

	/* clear lookup-table  */
	lua_pushnil(to);
	lua_setfield(to, LUA_REGISTRYINDEX, "l-copy-values-visited");

#ifdef DEBUG
	assert(lua_gettop(from) == top_from);
	assert(lua_gettop(to) == top_to + n);
#endif
}
