/*
 * simplehttp - Android implementation using JNI + HttpURLConnection
 *
 * On Android, curl cannot be used directly. Instead, we delegate to
 * Java's HttpURLConnection via JNI. TLS and certificate validation are
 * handled automatically by Android's Conscrypt/BoringSSL stack — no
 * certificate bundle needed.
 *
 * Lua symbols (lua_State, luaL_checkstring, etc.) are provided at
 * runtime by librime.so, which statically links Lua 5.4. This file is
 * linked at build time against a Lua stub named librime.so; at runtime
 * the real librime.so from the fcitx5-android RIME plugin is used.
 *
 * API compatible with simplehttp (main.c):
 *   http.request(url)                    → body, code
 *   http.request(url, post_body)         → body, code
 *   http.request({url=, method=, data=}) → body, code
 *   http.TIMEOUT = seconds               (default: no timeout)
 */

#include <jni.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

/* Obtain the JVM running in this process.
   On Android, the JVM is always present (started by the app framework).
   JNI_GetCreatedJavaVMs lives in libart.so, which is globally visible. */
static JavaVM *get_jvm(void) {
    typedef jint (*fn_t)(JavaVM **, jsize, jsize *);
    fn_t fn = (fn_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (!fn) return NULL;
    JavaVM *vms[1];
    jsize count = 0;
    if (fn(vms, 1, &count) != JNI_OK) return NULL;
    return count > 0 ? vms[0] : NULL;
}

/*
 * Perform an HTTP(S) request via java.net.HttpURLConnection.
 *
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
    JavaVM *jvm = get_jvm();
    if (!jvm) {
        lua_pushnil(L);
        lua_pushstring(L, "simplehttp: JVM not available");
        return 2;
    }

    JNIEnv *env = NULL;
    jboolean attached = JNI_FALSE;
    jint rc = (*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK) {
            lua_pushnil(L);
            lua_pushstring(L, "simplehttp: cannot attach thread to JVM");
            return 2;
        }
        attached = JNI_TRUE;
    } else if (rc != JNI_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "simplehttp: cannot get JNI env");
        return 2;
    }

/* Clear pending exception and jump to fail */
#define CHECK(expr) \
    do { (expr); if ((*env)->ExceptionCheck(env)) \
        { (*env)->ExceptionClear(env); goto fail; } } while (0)
#define CHECKV(var, expr) \
    do { (var) = (expr); if (!(var) || (*env)->ExceptionCheck(env)) \
        { (*env)->ExceptionClear(env); goto fail; } } while (0)

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
    CHECKV(URL_cls,  (*env)->FindClass(env, "java/net/URL"));
    CHECKV(HTTP_cls, (*env)->FindClass(env, "java/net/HttpURLConnection"));
    CHECKV(OS_cls,   (*env)->FindClass(env, "java/io/OutputStream"));
    CHECKV(IS_cls,   (*env)->FindClass(env, "java/io/InputStream"));

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
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); goto fail; }

    /* Create URL and open connection */
    CHECKV(jurl,    (*env)->NewStringUTF(env, url));
    CHECKV(url_obj, (*env)->NewObject(env, URL_cls, URL_init, jurl));
    CHECKV(conn,    (*env)->CallObjectMethod(env, url_obj, URL_open));

    /* Set request method */
    CHECKV(jmethod, (*env)->NewStringUTF(env, method));
    CHECK((*env)->CallVoidMethod(env, conn, H_setmeth, jmethod));

    /* Set timeouts */
    if (timeout_ms > 0) {
        (*env)->CallVoidMethod(env, conn, H_ctout, timeout_ms);
        (*env)->CallVoidMethod(env, conn, H_rtout, timeout_ms);
    }

    /* POST body */
    if (post_data && post_len > 0) {
        jbyteArray jbody;
        CHECKV(jbody, (*env)->NewByteArray(env, post_len));
        (*env)->SetByteArrayRegion(env, jbody, 0, post_len, (const jbyte *)post_data);
        CHECK((*env)->CallVoidMethod(env, conn, H_doout, JNI_TRUE));
        jobject os;
        CHECKV(os, (*env)->CallObjectMethod(env, conn, H_getos));
        (*env)->CallVoidMethod(env, os, OS_write, jbody);
        (*env)->CallVoidMethod(env, os, OS_flush);
        (*env)->CallVoidMethod(env, os, OS_close);
        (*env)->DeleteLocalRef(env, jbody);
        (*env)->DeleteLocalRef(env, os);
    }

    /* Trigger connection and get status code */
    code = (*env)->CallIntMethod(env, conn, H_code);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); goto fail; }

    /* Read response body */
    is_obj = (*env)->CallObjectMethod(env, conn, H_getis);
    if ((*env)->ExceptionCheck(env)) {
        /* Non-2xx: getInputStream() throws IOException.
           Return empty body with the error code so the Lua caller
           can check the status and discard gracefully. */
        (*env)->ExceptionClear(env);
        lua_pushlstring(L, "", 0);
        lua_pushinteger(L, (lua_Integer)code);
        goto cleanup;
    }

    CHECKV(chunk, (*env)->NewByteArray(env, 8192));
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

    lua_pushlstring(L, body ? body : "", body_len);
    lua_pushinteger(L, (lua_Integer)code);
    goto cleanup;

fail:
    lua_pushnil(L);
    lua_pushstring(L, "simplehttp: request failed");

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

#undef CHECK
#undef CHECKV

    return 2;
}

/* Lua-facing entry point.
   Upvalue 1 = the module table (for reading TIMEOUT). */
static int w_request(lua_State *L) {
    const char *url      = NULL;
    const char *method   = "GET";
    const char *post_data = NULL;
    size_t      post_len  = 0;
    jint        timeout_ms = 0;

    /* Read TIMEOUT from module table */
    lua_getfield(L, lua_upvalueindex(1), "TIMEOUT");
    if (!lua_isnoneornil(L, -1))
        timeout_ms = (jint)(lua_tonumber(L, -1) * 1000.0);
    lua_pop(L, 1);

    if (lua_isstring(L, 1)) {
        /* http.request(url [, post_body]) */
        size_t url_len;
        url = luaL_checklstring(L, 1, &url_len);
        if (lua_isstring(L, 2)) {
            post_data = luaL_checklstring(L, 2, &post_len);
            method = "POST";
        }
    } else if (lua_istable(L, 1)) {
        /* http.request({url=, method=, data=}) */
        lua_getfield(L, 1, "url");
        size_t url_len;
        url = luaL_checklstring(L, -1, &url_len);
        /* Leave url on stack to keep the pointer valid */

        lua_getfield(L, 1, "method");
        if (!lua_isnoneornil(L, -1))
            method = lua_tostring(L, -1);
        /* Leave method on stack */

        lua_getfield(L, 1, "data");
        if (!lua_isnoneornil(L, -1)) {
            post_data = luaL_checklstring(L, -1, &post_len);
            if (strcmp(method, "GET") == 0) method = "POST";
        }
        /* Leave data on stack */
    } else {
        lua_pushnil(L);
        lua_pushstring(L, "simplehttp: expected url string or request table");
        return 2;
    }

    return do_request(L, url, method, post_data, (jsize)post_len, timeout_ms);
}

__attribute__((visibility("default")))
int luaopen_simplehttp(lua_State *L) {
    lua_newtable(L);
    lua_pushvalue(L, -1);           /* upvalue 1: the module table itself */
    lua_pushcclosure(L, w_request, 1);
    lua_setfield(L, -2, "request");
    return 1;
}
