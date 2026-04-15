/*
 * simplehttp - Android implementation using JNI + HttpURLConnection
 *
 * On Android, curl cannot be used directly. Instead, we delegate to
 * Java's HttpURLConnection via JNI. TLS and certificate validation are
 * handled automatically by Android's Conscrypt/BoringSSL stack — no
 * certificate bundle needed.
 *
 * Lua symbols are resolved at runtime via dladdr + dlopen(RTLD_NOLOAD)
 * + dlsym. This means simplehttp.so has ZERO undefined Lua symbols at
 * link time and needs no NEEDED entry for librime.so. At runtime,
 * luaopen_simplehttp() finds the calling library (librime.so) via the
 * return address and resolves every Lua API function by name.
 *
 * API compatible with simplehttp (main.c):
 *   http.request(url)                    → body, code
 *   http.request(url, post_body)         → body, code
 *   http.request({url=, method=, data=}) → body, code
 *   http.TIMEOUT = seconds               (default: no timeout)
 */

#include <jni.h>
#include <dlfcn.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

/* ------------------------------------------------------------------ */
/* Lua types and constants — no linking required                       */
/* ------------------------------------------------------------------ */

typedef struct lua_State lua_State;
typedef double           lua_Number;
typedef long long        lua_Integer;
typedef int (*lua_CFunction)(lua_State *L);

#define LUA_REGISTRYINDEX   (-1000000 - 1000)
#define lua_upvalueindex(i) (LUA_REGISTRYINDEX - (i))

/* lua_type return values */
#define LUA_TNONE          (-1)
#define LUA_TNIL            0
#define LUA_TBOOLEAN        1
#define LUA_TLIGHTUSERDATA  2
#define LUA_TNUMBER         3
#define LUA_TSTRING         4
#define LUA_TTABLE          5
#define LUA_TFUNCTION       6
#define LUA_TUSERDATA       7
#define LUA_TTHREAD         8

/* ------------------------------------------------------------------ */
/* Lua API function pointer table                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    /* stack manipulation */
    void        (*lua_settop)      (lua_State *L, int idx);
    void        (*lua_pushnil)     (lua_State *L);
    const char *(*lua_pushstring)  (lua_State *L, const char *s);
    const char *(*lua_pushlstring) (lua_State *L, const char *s, size_t len);
    void        (*lua_pushinteger) (lua_State *L, lua_Integer n);
    void        (*lua_pushvalue)   (lua_State *L, int idx);
    void        (*lua_pushcclosure)(lua_State *L, lua_CFunction fn, int n);
    /* table */
    void        (*lua_createtable) (lua_State *L, int narr, int nrec);
    int         (*lua_getfield)    (lua_State *L, int idx, const char *k);
    void        (*lua_setfield)    (lua_State *L, int idx, const char *k);
    /* type / value query */
    int         (*lua_type)        (lua_State *L, int idx);
    int         (*lua_isstring)    (lua_State *L, int idx);
    lua_Number  (*lua_tonumberx)   (lua_State *L, int idx, int *isnum);
    const char *(*lua_tolstring)   (lua_State *L, int idx, size_t *len);
    /* aux lib */
    const char *(*luaL_checklstring)(lua_State *L, int arg, size_t *l);
} LuaAPI;

static LuaAPI lapi;          /* zero-initialised; populated in luaopen_ */
static int    lapi_ready = 0;

/* Convenience wrappers matching standard Lua macros */
#define lua_pop(L,n)           lapi.lua_settop((L), -(n)-1)
#define lua_newtable(L)        lapi.lua_createtable((L), 0, 0)
#define lua_isnoneornil(L,n)   (lapi.lua_type((L),(n)) <= 0)
#define lua_istable(L,n)       (lapi.lua_type((L),(n)) == LUA_TTABLE)
#define lua_tostring(L,i)      lapi.lua_tolstring((L),(i),NULL)

/* ------------------------------------------------------------------ */
/* JVM helpers                                                         */
/* ------------------------------------------------------------------ */

typedef jint (*get_vms_fn_t)(JavaVM **, jsize, jsize *);

/* dl_iterate_phdr callback: search libart.so / libdvm.so for
   JNI_GetCreatedJavaVMs and return the first one found. */
static int find_jni_get_vms(struct dl_phdr_info *info, size_t size, void *data) {
    if (!info->dlpi_name || !*info->dlpi_name) return 0;
    if (!strstr(info->dlpi_name, "libart.so") &&
        !strstr(info->dlpi_name, "libdvm.so"))
        return 0;
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp",
        "searching JNI_GetCreatedJavaVMs in: %s", info->dlpi_name);
    void *h = dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD);
    if (!h) return 0;
    get_vms_fn_t fn = (get_vms_fn_t)dlsym(h, "JNI_GetCreatedJavaVMs");
    dlclose(h);
    if (fn) {
        *(get_vms_fn_t *)data = fn;
        return 1; /* stop iteration */
    }
    return 0;
}

/* Obtain the JVM running in this process.
   Strategy:
   1. Try RTLD_DEFAULT (works on older Android / some configurations).
   2. Walk all loaded libraries via dl_iterate_phdr to find libart.so
      or libdvm.so directly — required on Android 7+ with namespace
      isolation, and on Android 10+ where ART lives under /apex/. */
static JavaVM *g_jvm = NULL;

static JavaVM *get_jvm(void) {
    if (g_jvm) return g_jvm;

    get_vms_fn_t fn = (get_vms_fn_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (!fn) {
        __android_log_print(ANDROID_LOG_DEBUG, "simplehttp",
            "RTLD_DEFAULT dlsym failed, trying dl_iterate_phdr...");
        dl_iterate_phdr(find_jni_get_vms, &fn);
    }
    if (!fn) {
        __android_log_print(ANDROID_LOG_ERROR, "simplehttp",
            "JNI_GetCreatedJavaVMs not found in any loaded library");
        return NULL;
    }

    JavaVM *vms[1] = {NULL};
    jsize count = 0;
    if (fn(vms, 1, &count) != JNI_OK || count == 0) {
        __android_log_print(ANDROID_LOG_ERROR, "simplehttp",
            "JNI_GetCreatedJavaVMs returned rc!=OK or count=0");
        return NULL;
    }
    g_jvm = vms[0];
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp", "JVM acquired: %p", (void *)g_jvm);
    return g_jvm;
}

/* ------------------------------------------------------------------ */
/* HTTP request via java.net.HttpURLConnection                         */
/* ------------------------------------------------------------------ */

/* Log the pending Java exception class name, then clear it.
   step: short label identifying where the failure happened. */
static void log_exception(JNIEnv *env, const char *step) {
    jthrowable ex = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    if (!ex) {
        __android_log_print(ANDROID_LOG_ERROR, "simplehttp",
            "FAIL at [%s]: no exception (null/0 return)", step);
        return;
    }
    jclass cls = (*env)->GetObjectClass(env, ex);
    jmethodID getName = (*env)->GetMethodID(env, cls, "getName", "()Ljava/lang/String;");
    if (getName) {
        jstring name = (jstring)(*env)->CallObjectMethod(env, cls, getName);
        if (name) {
            const char *cname = (*env)->GetStringUTFChars(env, name, NULL);
            __android_log_print(ANDROID_LOG_ERROR, "simplehttp",
                "FAIL at [%s]: exception = %s", step, cname ? cname : "(null)");
            if (cname) (*env)->ReleaseStringUTFChars(env, name, cname);
            (*env)->DeleteLocalRef(env, name);
        }
    } else {
        (*env)->ExceptionClear(env); /* clear from failed GetMethodID */
        __android_log_print(ANDROID_LOG_ERROR, "simplehttp",
            "FAIL at [%s]: exception (class name unavailable)", step);
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, ex);
}

/*
 * Pushes 2 values onto the Lua stack:
 *   success: body_string, status_code
 *   failure: nil, error_string
 */
static int do_request(lua_State *L,
                      const char *url,
                      const char *method,
                      const char *post_data,
                      jsize      post_len,
                      jint       timeout_ms) {
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp",
        "do_request: %s %s", method, url);

    JavaVM *jvm = get_jvm();
    if (!jvm) {
        __android_log_print(ANDROID_LOG_ERROR, "simplehttp", "JVM not available");
        lapi.lua_pushnil(L);
        lapi.lua_pushstring(L, "simplehttp: JVM not available");
        return 2;
    }

    JNIEnv *env = NULL;
    jboolean attached = JNI_FALSE;
    jint rc = (*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, "simplehttp", "AttachCurrentThread failed");
            lapi.lua_pushnil(L);
            lapi.lua_pushstring(L, "simplehttp: cannot attach thread to JVM");
            return 2;
        }
        attached = JNI_TRUE;
    } else if (rc != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "simplehttp", "GetEnv failed: %d", rc);
        lapi.lua_pushnil(L);
        lapi.lua_pushstring(L, "simplehttp: cannot get JNI env");
        return 2;
    }

/* Log exception class name, clear it, and jump to fail_labeled */
#define CHKLOG(step, expr) \
    do { (expr); if ((*env)->ExceptionCheck(env)) \
        { log_exception(env, step); goto fail; } } while (0)
#define CHKVLOG(var, step, expr) \
    do { (var) = (expr); if (!(var) || (*env)->ExceptionCheck(env)) \
        { log_exception(env, step); goto fail; } } while (0)

    jstring  jurl    = NULL;
    jstring  jmethod = NULL;
    jobject  url_obj = NULL;
    jobject  conn    = NULL;
    jobject  is_obj  = NULL;
    jbyteArray chunk = NULL;
    char    *body    = NULL;
    size_t   body_len = 0;
    jint     code    = 0;

    /* Resolve classes */
    jclass URL_cls, HTTP_cls, OS_cls, IS_cls;
    CHKVLOG(URL_cls,  "FindClass(URL)",              (*env)->FindClass(env, "java/net/URL"));
    CHKVLOG(HTTP_cls, "FindClass(HttpURLConnection)",(*env)->FindClass(env, "java/net/HttpURLConnection"));
    CHKVLOG(OS_cls,   "FindClass(OutputStream)",     (*env)->FindClass(env, "java/io/OutputStream"));
    CHKVLOG(IS_cls,   "FindClass(InputStream)",      (*env)->FindClass(env, "java/io/InputStream"));
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp", "FindClass: OK");

    /* Resolve methods */
    jmethodID URL_init  = (*env)->GetMethodID(env, URL_cls,  "<init>",            "(Ljava/lang/String;)V");
    jmethodID URL_open  = (*env)->GetMethodID(env, URL_cls,  "openConnection",    "()Ljava/net/URLConnection;");
    jmethodID H_setmeth = (*env)->GetMethodID(env, HTTP_cls, "setRequestMethod",  "(Ljava/lang/String;)V");
    jmethodID H_ctout   = (*env)->GetMethodID(env, HTTP_cls, "setConnectTimeout", "(I)V");
    jmethodID H_rtout   = (*env)->GetMethodID(env, HTTP_cls, "setReadTimeout",    "(I)V");
    jmethodID H_doout   = (*env)->GetMethodID(env, HTTP_cls, "setDoOutput",       "(Z)V");
    jmethodID H_getos   = (*env)->GetMethodID(env, HTTP_cls, "getOutputStream",   "()Ljava/io/OutputStream;");
    jmethodID H_code    = (*env)->GetMethodID(env, HTTP_cls, "getResponseCode",   "()I");
    jmethodID H_getis   = (*env)->GetMethodID(env, HTTP_cls, "getInputStream",    "()Ljava/io/InputStream;");
    jmethodID H_discon  = (*env)->GetMethodID(env, HTTP_cls, "disconnect",        "()V");
    jmethodID OS_write  = (*env)->GetMethodID(env, OS_cls,   "write",  "([B)V");
    jmethodID OS_flush  = (*env)->GetMethodID(env, OS_cls,   "flush",  "()V");
    jmethodID OS_close  = (*env)->GetMethodID(env, OS_cls,   "close",  "()V");
    jmethodID IS_read   = (*env)->GetMethodID(env, IS_cls,   "read",   "([B)I");
    jmethodID IS_close  = (*env)->GetMethodID(env, IS_cls,   "close",  "()V");
    if ((*env)->ExceptionCheck(env)) { log_exception(env, "GetMethodID"); goto fail; }
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp", "GetMethodID: OK");

    /* Create URL object */
    CHKVLOG(jurl,    "NewStringUTF(url)",  (*env)->NewStringUTF(env, url));
    CHKVLOG(url_obj, "URL.<init>",         (*env)->NewObject(env, URL_cls, URL_init, jurl));
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp", "URL object created");

    /* Open connection */
    CHKVLOG(conn, "openConnection", (*env)->CallObjectMethod(env, url_obj, URL_open));
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp", "openConnection: OK");

    /* Set request method */
    CHKVLOG(jmethod, "NewStringUTF(method)", (*env)->NewStringUTF(env, method));
    CHKLOG("setRequestMethod", (*env)->CallVoidMethod(env, conn, H_setmeth, jmethod));

    /* Set timeouts */
    if (timeout_ms > 0) {
        (*env)->CallVoidMethod(env, conn, H_ctout, timeout_ms);
        (*env)->CallVoidMethod(env, conn, H_rtout, timeout_ms);
        if ((*env)->ExceptionCheck(env)) { log_exception(env, "setTimeout"); goto fail; }
    }

    /* POST body */
    if (post_data && post_len > 0) {
        jbyteArray jbody;
        CHKVLOG(jbody, "NewByteArray", (*env)->NewByteArray(env, post_len));
        (*env)->SetByteArrayRegion(env, jbody, 0, post_len, (const jbyte *)post_data);
        CHKLOG("setDoOutput", (*env)->CallVoidMethod(env, conn, H_doout, JNI_TRUE));
        jobject os;
        CHKVLOG(os, "getOutputStream", (*env)->CallObjectMethod(env, conn, H_getos));
        (*env)->CallVoidMethod(env, os, OS_write, jbody);
        (*env)->CallVoidMethod(env, os, OS_flush);
        (*env)->CallVoidMethod(env, os, OS_close);
        (*env)->DeleteLocalRef(env, jbody);
        (*env)->DeleteLocalRef(env, os);
        if ((*env)->ExceptionCheck(env)) { log_exception(env, "writePostBody"); goto fail; }
    }

    /* Trigger connection and get status code */
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp", "calling getResponseCode...");
    code = (*env)->CallIntMethod(env, conn, H_code);
    if ((*env)->ExceptionCheck(env)) { log_exception(env, "getResponseCode"); goto fail; }
    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp", "HTTP status: %d", (int)code);

    /* Read response body */
    is_obj = (*env)->CallObjectMethod(env, conn, H_getis);
    if ((*env)->ExceptionCheck(env)) {
        /* Non-2xx: getInputStream() throws IOException.
           Return empty body with the error code so the Lua caller
           can check the status and discard gracefully. */
        (*env)->ExceptionClear(env);
        __android_log_print(ANDROID_LOG_DEBUG, "simplehttp",
            "getInputStream threw (non-2xx?), returning empty body, code=%d", (int)code);
        lapi.lua_pushlstring(L, "", 0);
        lapi.lua_pushinteger(L, (lua_Integer)code);
        goto cleanup;
    }

    CHKVLOG(chunk, "NewByteArray(8192)", (*env)->NewByteArray(env, 8192));
    for (;;) {
        jint n = (*env)->CallIntMethod(env, is_obj, IS_read, chunk);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); break; }
        if (n <= 0) break;
        char *tmp = (char *)realloc(body, body_len + (size_t)n);
        if (!tmp) goto fail;
        body = tmp;
        (*env)->GetByteArrayRegion(env, chunk, 0, n, (jbyte *)(body + body_len));
        body_len += (size_t)n;
    }
    (*env)->CallVoidMethod(env, is_obj, IS_close);

    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp",
        "response body: %zu bytes, code=%d", body_len, (int)code);
    lapi.lua_pushlstring(L, body ? body : "", body_len);
    lapi.lua_pushinteger(L, (lua_Integer)code);
    goto cleanup;

fail:
    lapi.lua_pushnil(L);
    lapi.lua_pushstring(L, "simplehttp: request failed");

cleanup:
    free(body);
    if (chunk)   (*env)->DeleteLocalRef(env, chunk);
    if (is_obj)  (*env)->DeleteLocalRef(env, is_obj);
    if (conn) {
        (*env)->CallVoidMethod(env, conn, H_discon);
        (*env)->DeleteLocalRef(env, conn);
    }
    if (url_obj) (*env)->DeleteLocalRef(env, url_obj);
    if (jmethod) (*env)->DeleteLocalRef(env, jmethod);
    if (jurl)    (*env)->DeleteLocalRef(env, jurl);

    if (attached) (*jvm)->DetachCurrentThread(jvm);

#undef CHKLOG
#undef CHKVLOG

    return 2;
}

/* ------------------------------------------------------------------ */
/* Lua-facing entry point                                              */
/* ------------------------------------------------------------------ */

/* Upvalue 1 = the module table (for reading TIMEOUT). */
static int w_request(lua_State *L) {
    const char *url      = NULL;
    const char *method   = "GET";
    const char *post_data = NULL;
    size_t      post_len  = 0;
    jint        timeout_ms = 0;

    /* Read TIMEOUT from module table */
    lapi.lua_getfield(L, lua_upvalueindex(1), "TIMEOUT");
    if (!lua_isnoneornil(L, -1))
        timeout_ms = (jint)(lapi.lua_tonumberx(L, -1, NULL) * 1000.0);
    lua_pop(L, 1);

    if (lapi.lua_isstring(L, 1)) {
        /* http.request(url [, post_body]) */
        size_t url_len;
        url = lapi.luaL_checklstring(L, 1, &url_len);
        if (lapi.lua_isstring(L, 2)) {
            post_data = lapi.luaL_checklstring(L, 2, &post_len);
            method = "POST";
        }
    } else if (lua_istable(L, 1)) {
        /* http.request({url=, method=, data=}) */
        lapi.lua_getfield(L, 1, "url");
        size_t url_len;
        url = lapi.luaL_checklstring(L, -1, &url_len);
        /* Leave url on stack to keep the pointer valid */

        lapi.lua_getfield(L, 1, "method");
        if (!lua_isnoneornil(L, -1))
            method = lua_tostring(L, -1);
        /* Leave method on stack */

        lapi.lua_getfield(L, 1, "data");
        if (!lua_isnoneornil(L, -1)) {
            post_data = lapi.luaL_checklstring(L, -1, &post_len);
            if (strcmp(method, "GET") == 0) method = "POST";
        }
        /* Leave data on stack */
    } else {
        lapi.lua_pushnil(L);
        lapi.lua_pushstring(L, "simplehttp: expected url string or request table");
        return 2;
    }

    return do_request(L, url, method, post_data, (jsize)post_len, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* Resolve Lua API from the calling library at runtime                 */
/* ------------------------------------------------------------------ */

#define RESOLVE(name) \
    do { \
        lapi.name = (typeof(lapi.name))dlsym(h, #name); \
        if (!lapi.name) { \
            __android_log_print(ANDROID_LOG_ERROR, "simplehttp", \
                "dlsym(" #name ") failed: %s", dlerror()); \
            dlclose(h); \
            return -1; \
        } \
    } while (0)

static int resolve_lua_api(void) {
    /* Find the library that called luaopen_simplehttp.
       One level up: luaopen_simplehttp <- Lua loader in librime.so */
    void *ret = __builtin_return_address(0);
    Dl_info info;
    if (!dladdr(ret, &info) || !info.dli_fname) {
        __android_log_print(ANDROID_LOG_ERROR, "simplehttp",
            "dladdr failed to identify caller library");
        return -1;
    }

    __android_log_print(ANDROID_LOG_DEBUG, "simplehttp",
        "caller library: %s", info.dli_fname);

    /* RTLD_NOLOAD: don't load, just get handle to the already-loaded lib */
    void *h = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        __android_log_print(ANDROID_LOG_ERROR, "simplehttp",
            "dlopen(RTLD_NOLOAD) failed: %s", dlerror());
        return -1;
    }

    RESOLVE(lua_settop);
    RESOLVE(lua_pushnil);
    RESOLVE(lua_pushstring);
    RESOLVE(lua_pushlstring);
    RESOLVE(lua_pushinteger);
    RESOLVE(lua_pushvalue);
    RESOLVE(lua_pushcclosure);
    RESOLVE(lua_createtable);
    RESOLVE(lua_getfield);
    RESOLVE(lua_setfield);
    RESOLVE(lua_type);
    RESOLVE(lua_isstring);
    RESOLVE(lua_tonumberx);
    RESOLVE(lua_tolstring);
    RESOLVE(luaL_checklstring);

    dlclose(h);  /* release the extra ref; the library stays loaded */
    return 0;
}

#undef RESOLVE

/* ------------------------------------------------------------------ */
/* Module entry point                                                  */
/* ------------------------------------------------------------------ */

__attribute__((visibility("default")))
int luaopen_simplehttp(lua_State *L) {
    if (!lapi_ready) {
        if (resolve_lua_api() != 0) {
            /* Can't push error via Lua API if it's not resolved yet.
               Return 0 so Lua gets nil from require(). */
            return 0;
        }
        lapi_ready = 1;
    }

    lua_newtable(L);
    lapi.lua_pushvalue(L, -1);           /* upvalue 1: the module table itself */
    lapi.lua_pushcclosure(L, w_request, 1);
    lapi.lua_setfield(L, -2, "request");
    return 1;
}
