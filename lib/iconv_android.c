/*
 * iconv - Android implementation wrapping Bionic libc's iconv
 *
 * Android Bionic libc provides iconv_open / iconv / iconv_close natively
 * since API 21. No external library is needed — just link against libc.
 *
 * Lua API (compatible with lua-iconv 7.x):
 *   cd, err  = iconv.new("utf-8", "utf-16le")
 *   out, err = cd:iconv(str)
 *
 * Lua symbols are resolved at runtime using the same function-pointer
 * approach as simplehttp_android.c — zero undefined Lua symbols at
 * link time. See that file for a detailed explanation of the mechanism.
 *
 * Implementation note: This module avoids lua_newuserdata / luaL_newmetatable
 * / lua_setmetatable / luaL_checkudata because those auxlib / advanced-core
 * symbols may not be exported by the host librime.so.  Instead, iconv.new()
 * returns a plain table whose "iconv" field is a C closure that captures the
 * iconv_t handle as a lua_Integer upvalue.  The object protocol cd:iconv(str)
 * is fully preserved; GC of the underlying C handle is not performed (the
 * handle leaks when the table is collected), which is acceptable for the
 * one-or-few handles opened by a cloud-pinyin script.
 */

/* iconv functions are declared manually rather than via <iconv.h>.
   The header was only added to NDK public API at API level 28, but the
   functions have been present in Bionic libc since API 21 at runtime.
   We declare them ourselves to avoid the header restriction — the same
   approach used for Lua symbols throughout this project. */
#include <stddef.h>   /* size_t */
typedef void *iconv_t;
extern iconv_t iconv_open(const char *tocode, const char *fromcode);
extern size_t  iconv(iconv_t cd, char **inbuf,  size_t *inbytesleft,
                                 char **outbuf, size_t *outbytesleft);
extern int     iconv_close(iconv_t cd);

#include <dlfcn.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>   /* uintptr_t */
#include <android/log.h>

/* ------------------------------------------------------------------ */
/* Lua types and constants — no linking required                       */
/* ------------------------------------------------------------------ */

typedef struct lua_State lua_State;
typedef long long        lua_Integer;
typedef int (*lua_CFunction)(lua_State *L);

#define LUA_REGISTRYINDEX   (-1000000 - 1000)
/* upvalue pseudo-index: upvalue i of the running C closure */
#define lua_upvalueindex(i) (LUA_REGISTRYINDEX - (i))

/* ------------------------------------------------------------------ */
/* Lua API function pointer table                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    void        (*lua_settop)        (lua_State *L, int idx);
    void        (*lua_pushnil)       (lua_State *L);
    const char *(*lua_pushstring)    (lua_State *L, const char *s);
    const char *(*lua_pushlstring)   (lua_State *L, const char *s, size_t len);
    void        (*lua_pushvalue)     (lua_State *L, int idx);
    void        (*lua_pushcclosure)  (lua_State *L, lua_CFunction fn, int n);
    void        (*lua_createtable)   (lua_State *L, int narr, int nrec);
    void        (*lua_setfield)      (lua_State *L, int idx, const char *k);
    void        (*lua_pushinteger)   (lua_State *L, lua_Integer n);
    lua_Integer (*lua_tointegerx)    (lua_State *L, int idx, int *isnum);
    const char *(*luaL_checklstring) (lua_State *L, int arg, size_t *l);
} LuaAPI;

static LuaAPI lapi;
static int    lapi_ready = 0;

#define lua_pop(L, n)   lapi.lua_settop((L), -(n)-1)
#define lua_newtable(L) lapi.lua_createtable((L), 0, 0)

/* ------------------------------------------------------------------ */
/* cd:iconv(str) — the iconv_t handle is captured as upvalue 1        */
/* ------------------------------------------------------------------ */

/* cd:iconv(str)  →  result  or  nil, errmsg
 * self (the table) is at index 1; str is at index 2.
 * iconv_t handle is stored as a lua_Integer in upvalue 1. */
static int iconv_convert(lua_State *L) {
    int isnum = 0;
    lua_Integer h_int = lapi.lua_tointegerx(L, lua_upvalueindex(1), &isnum);
    if (!isnum) {
        lapi.lua_pushnil(L);
        lapi.lua_pushstring(L, "invalid iconv handle");
        return 2;
    }
    iconv_t cd = (iconv_t)(uintptr_t)h_int;

    size_t inlen;
    const char *inbuf = lapi.luaL_checklstring(L, 2, &inlen);

    /* Reset shift state before each conversion */
    iconv(cd, NULL, NULL, NULL, NULL);

    /* Worst-case output: UTF-8 is at most 4 bytes per code point,
       UTF-16LE input is 2 bytes per code point → factor of 2 is enough,
       but we use 4 to be safe for any encoding pair.  Add 4 for BOM/NUL. */
    size_t outsize = inlen * 4 + 4;
    char  *outbuf  = (char *)malloc(outsize);
    if (!outbuf) {
        lapi.lua_pushnil(L);
        lapi.lua_pushstring(L, "out of memory");
        return 2;
    }

    char  *inp     = (char *)inbuf;
    char  *outp    = outbuf;
    size_t inleft  = inlen;
    size_t outleft = outsize;

    size_t rc = iconv(cd, &inp, &inleft, &outp, &outleft);
    if (rc == (size_t)-1) {
        const char *err = strerror(errno);
        free(outbuf);
        lapi.lua_pushnil(L);
        lapi.lua_pushstring(L, err);
        return 2;
    }

    lapi.lua_pushlstring(L, outbuf, outsize - outleft);
    free(outbuf);
    return 1;   /* success: only the converted string, no error value */
}

/* ------------------------------------------------------------------ */
/* iconv.new(to, from)                                                 */
/* Returns a table { iconv = <C closure capturing iconv_t as integer> */
/* ------------------------------------------------------------------ */

/* iconv.new(to, from)  →  cd  or  nil, errmsg */
static int iconv_new(lua_State *L) {
    size_t tolen, fromlen;
    const char *to   = lapi.luaL_checklstring(L, 1, &tolen);
    const char *from = lapi.luaL_checklstring(L, 2, &fromlen);

    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        lapi.lua_pushnil(L);
        lapi.lua_pushstring(L, strerror(errno));
        return 2;
    }

    /* Build: table = { iconv = closure(cd) }
     * Stack layout after each operation:
     *   [1]=to  [2]=from  ... → we build on top */

    lua_newtable(L);                                      /* [.. table] */
    lapi.lua_pushinteger(L, (lua_Integer)(uintptr_t)cd);  /* [.. table int] */
    lapi.lua_pushcclosure(L, iconv_convert, 1);           /* [.. table closure] */
    lapi.lua_setfield(L, -2, "iconv");                    /* [.. table] */
    return 1;
}

/* ------------------------------------------------------------------ */
/* Resolve Lua API from the calling library at runtime                 */
/* ------------------------------------------------------------------ */

#define RESOLVE(name) \
    do { \
        lapi.name = (typeof(lapi.name))dlsym(h, #name); \
        if (!lapi.name) { \
            __android_log_print(ANDROID_LOG_ERROR, "iconv", \
                "dlsym(" #name ") failed: %s", dlerror()); \
            dlclose(h); \
            return -1; \
        } \
    } while (0)

static int resolve_lua_api(void) {
    void *ret = __builtin_return_address(0);
    Dl_info info;
    if (!dladdr(ret, &info) || !info.dli_fname) {
        __android_log_print(ANDROID_LOG_ERROR, "iconv",
            "dladdr failed to identify caller library");
        return -1;
    }
    __android_log_print(ANDROID_LOG_DEBUG, "iconv",
        "caller library: %s", info.dli_fname);

    void *h = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        __android_log_print(ANDROID_LOG_ERROR, "iconv",
            "dlopen(RTLD_NOLOAD) failed: %s", dlerror());
        return -1;
    }

    RESOLVE(lua_settop);
    RESOLVE(lua_pushnil);
    RESOLVE(lua_pushstring);
    RESOLVE(lua_pushlstring);
    RESOLVE(lua_pushvalue);
    RESOLVE(lua_pushcclosure);
    RESOLVE(lua_createtable);
    RESOLVE(lua_setfield);
    RESOLVE(lua_pushinteger);
    RESOLVE(lua_tointegerx);
    RESOLVE(luaL_checklstring);

    dlclose(h);
    return 0;
}

#undef RESOLVE

/* ------------------------------------------------------------------ */
/* Module entry point                                                  */
/* ------------------------------------------------------------------ */

__attribute__((visibility("default")))
int luaopen_iconv(lua_State *L) {
    if (!lapi_ready) {
        if (resolve_lua_api() != 0)
            return 0;   /* require("iconv") → nil */
        lapi_ready = 1;
    }

    lua_newtable(L);
    lapi.lua_pushcclosure(L, iconv_new, 0);
    lapi.lua_setfield(L, -2, "new");
    return 1;
}
