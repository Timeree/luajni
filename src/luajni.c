/*
 * luajni.jni -- complete Lua binding for the JNI C interface.
 *
 * Every JNIEnv function is exposed under its jni.h name. The variadic call
 * families (Call<Type>Method, NewObject, ...) accept a positional table of
 * arguments; the "A" aliases accept the same table, the "V" aliases are
 * aliases of "A" because a va_list cannot cross the Lua boundary.
 *
 * Arguments may be plain Lua values or typed values created by jni.int(),
 * jni.long(), jni.float(), ... . Typed values are required whenever the Java
 * signature is not obvious from the Lua type (int vs long, float vs double).
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jni.h>
#include <lua.h>
#include <lauxlib.h>

#define ENV_MT "luajni.env"
#define VM_MT "luajni.vm"
#define VAL_MT "luajni.value"

typedef struct {
  JavaVM *jvm;
  int owned;
  int attached;
} VM;

typedef struct {
  JNIEnv *env;
  VM *vm;
} Env;

typedef struct {
  jvalue v;
  int type;
} Val;

typedef struct {
  jvalue *v;
  jobject *refs;
  int n;
  int nrefs;
} Args;

static Env *check_env(lua_State *L) { return (Env *)luaL_checkudata(L, 1, ENV_MT); }

static JNIEnv *env_of(lua_State *L) {
  Env *e = check_env(L);
  if (!e->vm->jvm) luaL_error(L, "luajni: JVM is closed");
  return e->env;
}

#define E(L) env_of(L)
#define ENVV(L) check_env(L)

static VM *check_vm(lua_State *L) { return (VM *)luaL_checkudata(L, 1, VM_MT); }

#define A_PTR(i) (lua_touserdata(L, (i)))
#define A_CLS(i) ((jclass)lua_touserdata(L, (i)))
#define A_STR(i) luaL_checkstring(L, (i))
#define A_INT(i) ((jint)luaL_checkinteger(L, (i)))
#define A_SIZE(i) ((jsize)luaL_checkinteger(L, (i)))
#define A_MODE(i) ((jint)luaL_optinteger(L, (i), 0))

#define A_BOOL(i) ((jboolean)(lua_toboolean(L, (i)) ? JNI_TRUE : JNI_FALSE))
#define A_BYTE(i) ((jbyte)luaL_checkinteger(L, (i)))
#define A_CHAR(i) arg_char(L, (i))
#define A_SHORT(i) ((jshort)luaL_checkinteger(L, (i)))
#define A_LONG(i) ((jlong)luaL_checkinteger(L, (i)))
#define A_FLOAT(i) ((jfloat)luaL_checknumber(L, (i)))
#define A_DOUBLE(i) ((jdouble)luaL_checknumber(L, (i)))

#define PUSH_REF(v) return ((v) ? (lua_pushlightuserdata(L, (void *)(v)), 1) : (lua_pushnil(L), 1))
#define PUSH_INT(v) return (lua_pushinteger(L, (lua_Integer)(v)), 1)
#define PUSH_NUM(v) return (lua_pushnumber(L, (lua_Number)(v)), 1)
#define PUSH_BOOL(v) return (lua_pushboolean(L, (v) != JNI_FALSE), 1)
#define PUSH_VOID(v) return 0

static jchar arg_char(lua_State *L, int i) {
  if (lua_type(L, i) == LUA_TSTRING) {
    size_t n;
    const char *s = lua_tolstring(L, i, &n);
    return n ? (jchar)(unsigned char)s[0] : 0;
  }
  return (jchar)luaL_checkinteger(L, i);
}

static void args_init(Args *a) { memset(a, 0, sizeof(*a)); }

static void args_free(JNIEnv *e, Args *a) {
  for (int i = 0; i < a->nrefs; i++)
    if (a->refs[i]) (*e)->DeleteLocalRef(e, a->refs[i]);
  free(a->v);
  free(a->refs);
}

static void fill_value(lua_State *L, JNIEnv *e, int idx, jvalue *out, Args *a) {
  memset(out, 0, sizeof(*out));
  switch (lua_type(L, idx)) {
    case LUA_TNIL:
      break;
    case LUA_TBOOLEAN:
      out->z = lua_toboolean(L, idx) ? JNI_TRUE : JNI_FALSE;
      break;
    case LUA_TNUMBER:
      if (lua_isinteger(L, idx))
        out->j = (jlong)lua_tointeger(L, idx);
      else
        out->d = (jdouble)lua_tonumber(L, idx);
      break;
    case LUA_TSTRING: {
      jstring s = (*e)->NewStringUTF(e, lua_tostring(L, idx));
      out->l = s;
      if (s) a->refs[a->nrefs++] = s;
      break;
    }
    case LUA_TLIGHTUSERDATA:
      out->l = lua_touserdata(L, idx);
      break;
    case LUA_TUSERDATA:
      if (luaL_testudata(L, idx, VAL_MT)) {
        Val *v = (Val *)lua_touserdata(L, idx);
        *out = v->v;
      } else {
        out->l = lua_touserdata(L, idx);
      }
      break;
    default:
      luaL_error(L, "luajni: unsupported argument type %s", luaL_typename(L, idx));
  }
}

static void build_args(lua_State *L, Env *en, int idx, Args *a) {
  args_init(a);
  if (lua_isnoneornil(L, idx)) return;
  luaL_checktype(L, idx, LUA_TTABLE);
  int n = (int)lua_rawlen(L, idx);
  if (n <= 0) return;
  a->v = (jvalue *)calloc((size_t)n, sizeof(jvalue));
  a->refs = (jobject *)calloc((size_t)n, sizeof(jobject));
  if (!a->v || !a->refs) {
    free(a->v);
    free(a->refs);
    luaL_error(L, "luajni: out of memory");
  }
  a->n = n;
  for (int i = 0; i < n; i++) {
    lua_rawgeti(L, idx, i + 1);
    fill_value(L, en->env, -1, &a->v[i], a);
    lua_pop(L, 1);
  }
}

static void push_env(lua_State *L, JNIEnv *e, int vmidx) {
  Env *en = (Env *)lua_newuserdatauv(L, sizeof(Env), 1);
  en->env = e;
  en->vm = (VM *)lua_touserdata(L, vmidx);
  luaL_setmetatable(L, ENV_MT);
  lua_pushvalue(L, vmidx);
  lua_setiuservalue(L, -2, 1);
}

static int push_vm(lua_State *L, JavaVM *jvm, int owned) {
  VM *vm = (VM *)lua_newuserdatauv(L, sizeof(VM), 0);
  memset(vm, 0, sizeof(*vm));
  vm->jvm = jvm;
  vm->owned = owned;
  luaL_setmetatable(L, VM_MT);
  return 1;
}

/* ------------------------------------------------------------------ */
/* JNIEnv: basic                                                       */
/* ------------------------------------------------------------------ */

static int l_GetVersion(lua_State *L) { PUSH_INT((*E(L))->GetVersion(E(L))); }

static int l_DefineClass(lua_State *L) {
  size_t len;
  const char *buf = luaL_checklstring(L, 4, &len);
  PUSH_REF((*E(L))->DefineClass(E(L), A_STR(2), A_PTR(3), (const jbyte *)buf, (jsize)len));
}

static int l_FindClass(lua_State *L) { PUSH_REF((*E(L))->FindClass(E(L), A_STR(2))); }

static int l_FromReflectedMethod(lua_State *L) {
  PUSH_REF((*E(L))->FromReflectedMethod(E(L), A_PTR(2)));
}
static int l_FromReflectedField(lua_State *L) {
  PUSH_REF((*E(L))->FromReflectedField(E(L), A_PTR(2)));
}
static int l_ToReflectedMethod(lua_State *L) {
  PUSH_REF((*E(L))->ToReflectedMethod(E(L), A_CLS(2), (jmethodID)A_PTR(3), A_BOOL(4)));
}
static int l_ToReflectedField(lua_State *L) {
  PUSH_REF((*E(L))->ToReflectedField(E(L), A_CLS(2), (jfieldID)A_PTR(3), A_BOOL(4)));
}

static int l_GetSuperclass(lua_State *L) { PUSH_REF((*E(L))->GetSuperclass(E(L), A_CLS(2))); }
static int l_IsAssignableFrom(lua_State *L) {
  PUSH_BOOL((*E(L))->IsAssignableFrom(E(L), A_CLS(2), A_CLS(3)));
}

static int l_Throw(lua_State *L) { PUSH_INT((*E(L))->Throw(E(L), (jthrowable)A_PTR(2))); }
static int l_ThrowNew(lua_State *L) {
  PUSH_INT((*E(L))->ThrowNew(E(L), A_CLS(2), A_STR(3)));
}

static int l_ExceptionOccurred(lua_State *L) {
  PUSH_REF((*E(L))->ExceptionOccurred(E(L)));
}
static int l_ExceptionDescribe(lua_State *L) {
  (*E(L))->ExceptionDescribe(E(L));
  return 0;
}
static int l_ExceptionClear(lua_State *L) {
  (*E(L))->ExceptionClear(E(L));
  return 0;
}
static int l_ExceptionCheck(lua_State *L) { PUSH_BOOL((*E(L))->ExceptionCheck(E(L))); }
static int l_FatalError(lua_State *L) {
  (*E(L))->FatalError(E(L), A_STR(2));
  return 0;
}

static int l_PushLocalFrame(lua_State *L) {
  PUSH_INT((*E(L))->PushLocalFrame(E(L), A_INT(2)));
}
static int l_PopLocalFrame(lua_State *L) {
  PUSH_REF((*E(L))->PopLocalFrame(E(L), (jobject)A_PTR(2)));
}

static int l_NewGlobalRef(lua_State *L) {
  PUSH_REF((*E(L))->NewGlobalRef(E(L), (jobject)A_PTR(2)));
}
static int l_DeleteGlobalRef(lua_State *L) {
  (*E(L))->DeleteGlobalRef(E(L), (jobject)A_PTR(2));
  return 0;
}
static int l_DeleteLocalRef(lua_State *L) {
  (*E(L))->DeleteLocalRef(E(L), (jobject)A_PTR(2));
  return 0;
}
static int l_IsSameObject(lua_State *L) {
  PUSH_BOOL((*E(L))->IsSameObject(E(L), (jobject)A_PTR(2), (jobject)A_PTR(3)));
}
static int l_NewLocalRef(lua_State *L) {
  PUSH_REF((*E(L))->NewLocalRef(E(L), (jobject)A_PTR(2)));
}
static int l_EnsureLocalCapacity(lua_State *L) {
  PUSH_INT((*E(L))->EnsureLocalCapacity(E(L), A_INT(2)));
}

static int l_AllocObject(lua_State *L) { PUSH_REF((*E(L))->AllocObject(E(L), A_CLS(2))); }
static int l_GetObjectClass(lua_State *L) {
  PUSH_REF((*E(L))->GetObjectClass(E(L), (jobject)A_PTR(2)));
}
static int l_IsInstanceOf(lua_State *L) {
  PUSH_BOOL((*E(L))->IsInstanceOf(E(L), (jobject)A_PTR(2), A_CLS(3)));
}

static int l_GetMethodID(lua_State *L) {
  PUSH_REF((*E(L))->GetMethodID(E(L), A_CLS(2), A_STR(3), A_STR(4)));
}
static int l_GetFieldID(lua_State *L) {
  PUSH_REF((*E(L))->GetFieldID(E(L), A_CLS(2), A_STR(3), A_STR(4)));
}
static int l_GetStaticMethodID(lua_State *L) {
  PUSH_REF((*E(L))->GetStaticMethodID(E(L), A_CLS(2), A_STR(3), A_STR(4)));
}
static int l_GetStaticFieldID(lua_State *L) {
  PUSH_REF((*E(L))->GetStaticFieldID(E(L), A_CLS(2), A_STR(3), A_STR(4)));
}

/* ------------------------------------------------------------------ */
/* JNIEnv: calls                                                       */
/* ------------------------------------------------------------------ */

#define DEF_CALL_BODY(NAME, CTYPE, PUSH, ARGEXPR, AIDX) \
  static int l_##NAME(lua_State *L) {                   \
    JNIEnv *e = E(L);                                   \
    Args a;                                             \
    build_args(L, ENVV(L), AIDX, &a);                   \
    CTYPE r = (*e)->NAME##A ARGEXPR;                    \
    args_free(e, &a);                                   \
    PUSH(r);                                            \
  }

#define DEF_CALL_INST(NAME, CTYPE, PUSH) \
  DEF_CALL_BODY(NAME, CTYPE, PUSH, (e, (jobject)A_PTR(2), (jmethodID)A_PTR(3), a.v), 4)
#define DEF_CALL_STATIC(NAME, CTYPE, PUSH) \
  DEF_CALL_BODY(NAME, CTYPE, PUSH, (e, (jclass)A_PTR(2), (jmethodID)A_PTR(3), a.v), 4)
#define DEF_CALL_NONVIRT(NAME, CTYPE, PUSH)                                      \
  DEF_CALL_BODY(NAME, CTYPE, PUSH,                                                \
                (e, (jobject)A_PTR(2), (jclass)A_PTR(3), (jmethodID)A_PTR(4), a.v), 5)

#define PRIM_LIST(X)                     \
  X(Boolean, jboolean, PUSH_BOOL, A_BOOL) \
  X(Byte, jbyte, PUSH_INT, A_BYTE)       \
  X(Char, jchar, PUSH_INT, A_CHAR)       \
  X(Short, jshort, PUSH_INT, A_SHORT)    \
  X(Int, jint, PUSH_INT, A_INT)          \
  X(Long, jlong, PUSH_INT, A_LONG)       \
  X(Float, jfloat, PUSH_NUM, A_FLOAT)    \
  X(Double, jdouble, PUSH_NUM, A_DOUBLE)

#define EXPAND_CALLS(N, T, P, G)                 \
  DEF_CALL_INST(Call##N##Method, T, P)           \
  DEF_CALL_STATIC(CallStatic##N##Method, T, P)   \
  DEF_CALL_NONVIRT(CallNonvirtual##N##Method, T, P)

PRIM_LIST(EXPAND_CALLS)
EXPAND_CALLS(Object, jobject, PUSH_REF, A_PTR)

static int l_CallVoidMethod(lua_State *L) {
  JNIEnv *e = E(L);
  Args a;
  build_args(L, ENVV(L), 4, &a);
  (*e)->CallVoidMethodA(e, (jobject)A_PTR(2), (jmethodID)A_PTR(3), a.v);
  args_free(e, &a);
  return 0;
}
static int l_CallStaticVoidMethod(lua_State *L) {
  JNIEnv *e = E(L);
  Args a;
  build_args(L, ENVV(L), 4, &a);
  (*e)->CallStaticVoidMethodA(e, (jclass)A_PTR(2), (jmethodID)A_PTR(3), a.v);
  args_free(e, &a);
  return 0;
}
static int l_CallNonvirtualVoidMethod(lua_State *L) {
  JNIEnv *e = E(L);
  Args a;
  build_args(L, ENVV(L), 5, &a);
  (*e)->CallNonvirtualVoidMethodA(e, (jobject)A_PTR(2), (jclass)A_PTR(3),
                                  (jmethodID)A_PTR(4), a.v);
  args_free(e, &a);
  return 0;
}

static int l_NewObject(lua_State *L) {
  JNIEnv *e = E(L);
  Args a;
  build_args(L, ENVV(L), 4, &a);
  jobject r = (*e)->NewObjectA(e, A_CLS(2), (jmethodID)A_PTR(3), a.v);
  args_free(e, &a);
  PUSH_REF(r);
}

/* ------------------------------------------------------------------ */
/* JNIEnv: fields                                                      */
/* ------------------------------------------------------------------ */

#define DEF_FIELDS(NAME, CTYPE, PUSH, GET)                                     \
  static int l_Get##NAME##Field(lua_State *L) {                                \
    PUSH((*E(L))->Get##NAME##Field(E(L), (jobject)A_PTR(2), (jfieldID)A_PTR(3))); \
  }                                                                            \
  static int l_Set##NAME##Field(lua_State *L) {                                \
    (*E(L))->Set##NAME##Field(E(L), (jobject)A_PTR(2), (jfieldID)A_PTR(3),     \
                              (CTYPE)GET(4));                                  \
    return 0;                                                                  \
  }                                                                            \
  static int l_GetStatic##NAME##Field(lua_State *L) {                          \
    PUSH((*E(L))->GetStatic##NAME##Field(E(L), A_CLS(2), (jfieldID)A_PTR(3))); \
  }                                                                            \
  static int l_SetStatic##NAME##Field(lua_State *L) {                          \
    (*E(L))->SetStatic##NAME##Field(E(L), A_CLS(2), (jfieldID)A_PTR(3),        \
                                    (CTYPE)GET(4));                            \
    return 0;                                                                  \
  }

PRIM_LIST(DEF_FIELDS)
DEF_FIELDS(Object, jobject, PUSH_REF, A_PTR)

/* ------------------------------------------------------------------ */
/* JNIEnv: strings                                                     */
/* ------------------------------------------------------------------ */

static int l_NewString(lua_State *L) {
  PUSH_REF((*E(L))->NewString(E(L), (const jchar *)A_PTR(2), A_SIZE(3)));
}
static int l_GetStringLength(lua_State *L) {
  PUSH_INT((*E(L))->GetStringLength(E(L), (jstring)A_PTR(2)));
}
static int l_GetStringChars(lua_State *L) {
  jboolean copy = JNI_FALSE;
  const jchar *p = (*E(L))->GetStringChars(E(L), (jstring)A_PTR(2), &copy);
  lua_pushlightuserdata(L, (void *)p);
  lua_pushboolean(L, copy != JNI_FALSE);
  return 2;
}
static int l_ReleaseStringChars(lua_State *L) {
  (*E(L))->ReleaseStringChars(E(L), (jstring)A_PTR(2), (const jchar *)A_PTR(3));
  return 0;
}
static int l_NewStringUTF(lua_State *L) {
  if (lua_isnil(L, 2)) return (lua_pushnil(L), 1);
  PUSH_REF((*E(L))->NewStringUTF(E(L), A_STR(2)));
}
static int l_GetStringUTFLength(lua_State *L) {
  PUSH_INT((*E(L))->GetStringUTFLength(E(L), (jstring)A_PTR(2)));
}
static int l_GetStringUTFChars(lua_State *L) {
  jboolean copy = JNI_FALSE;
  const char *p = (*E(L))->GetStringUTFChars(E(L), (jstring)A_PTR(2), &copy);
  lua_pushlightuserdata(L, (void *)p);
  lua_pushboolean(L, copy != JNI_FALSE);
  return 2;
}
static int l_ReleaseStringUTFChars(lua_State *L) {
  (*E(L))->ReleaseStringUTFChars(E(L), (jstring)A_PTR(2), (const char *)A_PTR(3));
  return 0;
}

static int l_GetStringRegion(lua_State *L) {
  JNIEnv *e = E(L);
  jsize start = A_SIZE(3), len = A_SIZE(4);
  if (!lua_isnoneornil(L, 5)) {
    (*e)->GetStringRegion(e, (jstring)A_PTR(2), start, len, (jchar *)A_PTR(5));
    return 0;
  }
  jchar *buf = (jchar *)malloc(sizeof(jchar) * (size_t)(len > 0 ? len : 1));
  if (!buf) return luaL_error(L, "luajni: out of memory");
  (*e)->GetStringRegion(e, (jstring)A_PTR(2), start, len, buf);
  lua_createtable(L, len > 0 ? len : 0, 0);
  for (jsize i = 0; i < len; i++) {
    lua_pushinteger(L, (lua_Integer)buf[i]);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  free(buf);
  return 1;
}

static int l_GetStringUTFRegion(lua_State *L) {
  JNIEnv *e = E(L);
  jsize start = A_SIZE(3), len = A_SIZE(4);
  if (!lua_isnoneornil(L, 5)) {
    (*e)->GetStringUTFRegion(e, (jstring)A_PTR(2), start, len, (char *)A_PTR(5));
    return 0;
  }
  char *buf = (char *)malloc((size_t)(len > 0 ? len : 1) + 1);
  if (!buf) return luaL_error(L, "luajni: out of memory");
  (*e)->GetStringUTFRegion(e, (jstring)A_PTR(2), start, len, buf);
  lua_pushlstring(L, buf, (size_t)len);
  free(buf);
  return 1;
}

/* convenience: read a jstring as a Lua UTF-8 string */
static int l_GetStringUTF(lua_State *L) {
  JNIEnv *e = E(L);
  jstring s = (jstring)A_PTR(2);
  if (!s) return (lua_pushnil(L), 1);
  const char *c = (*e)->GetStringUTFChars(e, s, NULL);
  if (!c) return (lua_pushnil(L), 1);
  lua_pushstring(L, c);
  (*e)->ReleaseStringUTFChars(e, s, c);
  return 1;
}

/* ------------------------------------------------------------------ */
/* JNIEnv: arrays                                                      */
/* ------------------------------------------------------------------ */

#define ARRAY_LIST(X)                                                                          \
  X(Boolean, jboolean, jbooleanArray, lua_pushboolean(L, buf[i] != JNI_FALSE), (jboolean)lua_toboolean(L, -1)) \
  X(Byte, jbyte, jbyteArray, lua_pushinteger(L, (lua_Integer)buf[i]), (jbyte)luaL_checkinteger(L, -1)) \
  X(Char, jchar, jcharArray, lua_pushinteger(L, (lua_Integer)buf[i]), (jchar)luaL_checkinteger(L, -1)) \
  X(Short, jshort, jshortArray, lua_pushinteger(L, (lua_Integer)buf[i]), (jshort)luaL_checkinteger(L, -1)) \
  X(Int, jint, jintArray, lua_pushinteger(L, (lua_Integer)buf[i]), (jint)luaL_checkinteger(L, -1)) \
  X(Long, jlong, jlongArray, lua_pushinteger(L, (lua_Integer)buf[i]), (jlong)luaL_checkinteger(L, -1)) \
  X(Float, jfloat, jfloatArray, lua_pushnumber(L, (lua_Number)buf[i]), (jfloat)luaL_checknumber(L, -1)) \
  X(Double, jdouble, jdoubleArray, lua_pushnumber(L, (lua_Number)buf[i]), (jdouble)luaL_checknumber(L, -1))

#define DEF_ARRAY(NAME, CTYPE, ATYPE, PUSH_ELEM, READ_ELEM)                                        \
  static int l_New##NAME##Array(lua_State *L) {                                             \
    PUSH_REF((*E(L))->New##NAME##Array(E(L), A_SIZE(2)));                                   \
  }                                                                                         \
  static int l_Get##NAME##ArrayElements(lua_State *L) {                                     \
    jboolean copy = JNI_FALSE;                                                              \
    CTYPE *p = (*E(L))->Get##NAME##ArrayElements(E(L), (ATYPE)A_PTR(2), &copy);     \
    lua_pushlightuserdata(L, (void *)p);                                                    \
    lua_pushboolean(L, copy != JNI_FALSE);                                                  \
    return 2;                                                                               \
  }                                                                                         \
  static int l_Release##NAME##ArrayElements(lua_State *L) {                                 \
    (*E(L))->Release##NAME##ArrayElements(E(L), (ATYPE)A_PTR(2), (CTYPE *)A_PTR(3), \
                                          A_MODE(4));                                       \
    return 0;                                                                               \
  }                                                                                         \
  static int l_Get##NAME##ArrayRegion(lua_State *L) {                                       \
    JNIEnv *e = E(L);                                                                       \
    jsize start = A_SIZE(3), len = A_SIZE(4);                                               \
    if (!lua_isnoneornil(L, 5)) {                                                           \
      (*e)->Get##NAME##ArrayRegion(e, (ATYPE)A_PTR(2), start, len, (CTYPE *)A_PTR(5)); \
      return 0;                                                                             \
    }                                                                                       \
    CTYPE *buf = (CTYPE *)malloc(sizeof(CTYPE) * (size_t)(len > 0 ? len : 1));              \
    if (!buf) return luaL_error(L, "luajni: out of memory");                                 \
    (*e)->Get##NAME##ArrayRegion(e, (ATYPE)A_PTR(2), start, len, buf);             \
    lua_createtable(L, len > 0 ? len : 0, 0);                                               \
    for (jsize i = 0; i < len; i++) {                                                       \
      PUSH_ELEM;                                                                            \
      lua_rawseti(L, -2, (lua_Integer)i + 1);                                               \
    }                                                                                       \
    free(buf);                                                                              \
    return 1;                                                                               \
  }                                                                                         \
  static int l_Set##NAME##ArrayRegion(lua_State *L) {                                       \
    JNIEnv *e = E(L);                                                                       \
    jsize start = A_SIZE(3), len = A_SIZE(4);                                               \
    if (!lua_istable(L, 5)) {                                                               \
      (*e)->Set##NAME##ArrayRegion(e, (ATYPE)A_PTR(2), start, len,                 \
                                   (const CTYPE *)A_PTR(5));                                \
      return 0;                                                                             \
    }                                                                                       \
    CTYPE *buf = (CTYPE *)malloc(sizeof(CTYPE) * (size_t)(len > 0 ? len : 1));              \
    if (!buf) return luaL_error(L, "luajni: out of memory");                                 \
    for (jsize i = 0; i < len; i++) {                                                       \
      lua_rawgeti(L, 5, (lua_Integer)i + 1);                                                \
      buf[i] = READ_ELEM;                                                                   \
      lua_pop(L, 1);                                                                        \
    }                                                                                       \
    (*e)->Set##NAME##ArrayRegion(e, (ATYPE)A_PTR(2), start, len, buf);             \
    free(buf);                                                                              \
    return 0;                                                                               \
  }

ARRAY_LIST(DEF_ARRAY)

static int l_GetArrayLength(lua_State *L) {
  PUSH_INT((*E(L))->GetArrayLength(E(L), (jarray)A_PTR(2)));
}
static int l_NewObjectArray(lua_State *L) {
  PUSH_REF((*E(L))->NewObjectArray(E(L), A_SIZE(2), A_CLS(3), (jobject)A_PTR(4)));
}
static int l_GetObjectArrayElement(lua_State *L) {
  PUSH_REF((*E(L))->GetObjectArrayElement(E(L), (jobjectArray)A_PTR(2), A_SIZE(3)));
}
static int l_SetObjectArrayElement(lua_State *L) {
  (*E(L))->SetObjectArrayElement(E(L), (jobjectArray)A_PTR(2), A_SIZE(3),
                                  (jobject)A_PTR(4));
  return 0;
}

/* ------------------------------------------------------------------ */
/* JNIEnv: references, monitors, natives, buffers, misc                */
/* ------------------------------------------------------------------ */

static int l_NewWeakGlobalRef(lua_State *L) {
  PUSH_REF((*E(L))->NewWeakGlobalRef(E(L), (jobject)A_PTR(2)));
}
static int l_DeleteWeakGlobalRef(lua_State *L) {
  (*E(L))->DeleteWeakGlobalRef(E(L), (jweak)A_PTR(2));
  return 0;
}

static int l_MonitorEnter(lua_State *L) {
  PUSH_INT((*E(L))->MonitorEnter(E(L), (jobject)A_PTR(2)));
}
static int l_MonitorExit(lua_State *L) {
  PUSH_INT((*E(L))->MonitorExit(E(L), (jobject)A_PTR(2)));
}

static int l_GetJavaVM(lua_State *L) {
  JavaVM *jvm = NULL;
  (*E(L))->GetJavaVM(E(L), &jvm);
  PUSH_REF(jvm);
}

static int l_RegisterNatives(lua_State *L) {
  JNIEnv *e = E(L);
  luaL_checktype(L, 3, LUA_TTABLE);
  int n = (int)lua_rawlen(L, 3);
  JNINativeMethod *m = (JNINativeMethod *)calloc((size_t)(n > 0 ? n : 1), sizeof(*m));
  if (!m) return luaL_error(L, "luajni: out of memory");
  for (int i = 0; i < n; i++) {
    lua_rawgeti(L, 3, i + 1);
    lua_rawgeti(L, -1, 1);
    m[i].name = (char *)lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_rawgeti(L, -1, 2);
    m[i].signature = (char *)lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_rawgeti(L, -1, 3);
    m[i].fnPtr = lua_touserdata(L, -1);
    lua_pop(L, 2);
  }
  jint rc = (*e)->RegisterNatives(e, A_CLS(2), m, n);
  free(m);
  PUSH_INT(rc);
}

static int l_UnregisterNatives(lua_State *L) {
  PUSH_INT((*E(L))->UnregisterNatives(E(L), A_CLS(2)));
}

static int l_NewDirectByteBuffer(lua_State *L) {
  PUSH_REF((*E(L))->NewDirectByteBuffer(E(L), A_PTR(2), (jlong)luaL_checkinteger(L, 3)));
}
static int l_GetDirectBufferAddress(lua_State *L) {
  PUSH_REF((*E(L))->GetDirectBufferAddress(E(L), (jobject)A_PTR(2)));
}
static int l_GetDirectBufferCapacity(lua_State *L) {
  PUSH_INT((*E(L))->GetDirectBufferCapacity(E(L), (jobject)A_PTR(2)));
}

static int l_GetObjectRefType(lua_State *L) {
  PUSH_INT((*E(L))->GetObjectRefType(E(L), (jobject)A_PTR(2)));
}

#ifdef JNI_VERSION_9
static int l_GetModule(lua_State *L) {
  PUSH_REF((*E(L))->GetModule(E(L), A_CLS(2)));
}
#endif

#ifdef JNI_VERSION_21
static int l_IsVirtualThread(lua_State *L) {
  PUSH_BOOL((*E(L))->IsVirtualThread(E(L), (jobject)A_PTR(2)));
}
#endif

/* ------------------------------------------------------------------ */
/* JavaVM                                                              */
/* ------------------------------------------------------------------ */

static int vm_getenv(lua_State *L) {
  VM *vm = check_vm(L);
  JNIEnv *e = NULL;
  jint rc = (*vm->jvm)->GetEnv(vm->jvm, (void **)&e, JNI_VERSION_1_8);
  if (rc != JNI_OK) return (lua_pushnil(L), 1);
  push_env(L, e, 1);
  return 1;
}

static int vm_env(lua_State *L) {
  VM *vm = check_vm(L);
  if (!vm->jvm) return luaL_error(L, "luajni: JVM is closed");
  JNIEnv *e = NULL;
  jint rc = (*vm->jvm)->GetEnv(vm->jvm, (void **)&e, JNI_VERSION_1_8);
  if (rc == JNI_EDETACHED) {
    rc = (*vm->jvm)->AttachCurrentThread(vm->jvm, (void **)&e, NULL);
    if (rc == JNI_OK) vm->attached = 1;
  }
  if (rc != JNI_OK) return luaL_error(L, "luajni: cannot get JNIEnv (%d)", rc);
  push_env(L, e, 1);
  return 1;
}

static int vm_attach(lua_State *L) {
  VM *vm = check_vm(L);
  if (!vm->jvm) return luaL_error(L, "luajni: JVM is closed");
  int daemon = lua_toboolean(L, 2);
  JNIEnv *e = NULL;
  jint rc;
  if (daemon)
    rc = (*vm->jvm)->AttachCurrentThreadAsDaemon(vm->jvm, (void **)&e, NULL);
  else
    rc = (*vm->jvm)->AttachCurrentThread(vm->jvm, (void **)&e, NULL);
  if (rc != JNI_OK) return luaL_error(L, "luajni: attach failed (%d)", rc);
  vm->attached = 1;
  lua_pushvalue(L, 1);
  push_env(L, e, lua_gettop(L) - 1);
  return 1;
}

static int vm_detach(lua_State *L) {
  VM *vm = check_vm(L);
  if (!vm->jvm) return luaL_error(L, "luajni: JVM is closed");
  jint rc = (*vm->jvm)->DetachCurrentThread(vm->jvm);
  if (rc == JNI_OK) vm->attached = 0;
  PUSH_INT(rc);
}

static int vm_destroy(lua_State *L) {
  VM *vm = check_vm(L);
  if (!vm->jvm) return 0;
  if (!vm->owned) return luaL_error(L, "luajni: JVM was not created by luajni");
  jint rc = (*vm->jvm)->DestroyJavaVM(vm->jvm);
  if (rc == JNI_OK) {
    vm->jvm = NULL;
    vm->owned = 0;
    vm->attached = 0;
  }
  PUSH_INT(rc);
}

static int vm_close(lua_State *L) {
  VM *vm = check_vm(L);
  if (!vm->jvm) return 0;
  jint rc;
  if (vm->owned) {
    rc = (*vm->jvm)->DestroyJavaVM(vm->jvm);
    if (rc != JNI_OK) return luaL_error(L, "luajni: DestroyJavaVM failed (%d)", rc);
    vm->jvm = NULL;
    vm->owned = 0;
    vm->attached = 0;
    return 0;
  }
  if (vm->attached) (*vm->jvm)->DetachCurrentThread(vm->jvm);
  vm->jvm = NULL;
  vm->attached = 0;
  return 0;
}

static int vm_ptr(lua_State *L) { PUSH_REF(check_vm(L)->jvm); }

static int vm_gc(lua_State *L) {
  VM *vm = check_vm(L);
  if (vm->jvm && vm->attached) (*vm->jvm)->DetachCurrentThread(vm->jvm);
  return 0;
}

/* ------------------------------------------------------------------ */
/* VM creation                                                         */
/* ------------------------------------------------------------------ */

typedef jint(JNICALL *fn_CreateJavaVM)(JavaVM **, void **, void *);
typedef jint(JNICALL *fn_GetCreatedJavaVMs)(JavaVM **, jsize, jsize *);
typedef jint(JNICALL *fn_GetDefaultJavaVMInitArgs)(void *);

static void *open_jvm(const char *path) {
  if (!path || !*path) return NULL;
  return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}

static void *load_libjvm(lua_State *L, const char *explicit_path) {
  void *h;
  if (explicit_path && *explicit_path) {
    h = open_jvm(explicit_path);
    if (!h) luaL_error(L, "luajni: cannot load %s: %s", explicit_path, dlerror());
    return h;
  }

  const char *home = getenv("JAVA_HOME");
  if (home && *home) {
    static const char *rels[] = {
        "/lib/server/libjvm.so", "/jre/lib/server/libjvm.so",
        "/lib/amd64/server/libjvm.so", "/lib/aarch64/server/libjvm.so", NULL};
    char buf[4096];
    for (int i = 0; rels[i]; i++) {
      snprintf(buf, sizeof(buf), "%s%s", home, rels[i]);
      if ((h = open_jvm(buf))) return h;
    }
  }

  static const char *patterns[] = {
      "/usr/lib/jvm/*/lib/server/libjvm.so",
      "/usr/lib/jvm/*/jre/lib/*/server/libjvm.so",
      "/usr/lib64/jvm/*/lib/server/libjvm.so",
      "/usr/local/lib/jvm/*/lib/server/libjvm.so", NULL};
  glob_t g;
  for (int i = 0; patterns[i]; i++) {
    if (glob(patterns[i], 0, NULL, &g) != 0) continue;
    for (size_t k = 0; k < g.gl_pathc; k++)
      if ((h = open_jvm(g.gl_pathv[k]))) {
        globfree(&g);
        return h;
      }
    globfree(&g);
  }

  h = open_jvm("libjvm.so");
  if (!h)
    luaL_error(L, "luajni: libjvm.so not found; set JAVA_HOME or pass { jvm = path }");
  return h;
}

static int l_get_created_vms(lua_State *L) {
  void *lib = load_libjvm(L, NULL);
  fn_GetCreatedJavaVMs created = NULL;
  *(void **)(&created) = dlsym(lib, "JNI_GetCreatedJavaVMs");
  if (!created) return luaL_error(L, "luajni: JNI_GetCreatedJavaVMs not found");
  JavaVM *vms[64];
  jsize n = 0;
  if (created(vms, 64, &n) != JNI_OK) return luaL_error(L, "luajni: JNI_GetCreatedJavaVMs failed");
  lua_createtable(L, n, 0);
  for (jsize i = 0; i < n; i++) {
    lua_pushlightuserdata(L, vms[i]);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int l_default_init_args(lua_State *L) {
  void *lib = load_libjvm(L, NULL);
  fn_GetDefaultJavaVMInitArgs fn = NULL;
  *(void **)(&fn) = dlsym(lib, "JNI_GetDefaultJavaVMInitArgs");
  if (!fn) return luaL_error(L, "luajni: JNI_GetDefaultJavaVMInitArgs not found");
  JavaVMInitArgs args;
  memset(&args, 0, sizeof(args));
  args.version = (jint)luaL_optinteger(L, 1, JNI_VERSION_1_8);
  args.ignoreUnrecognized = JNI_FALSE;
  jint rc = fn(&args);
  if (rc != JNI_OK) return (lua_pushnil(L), 1);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, (lua_Integer)args.version);
  lua_setfield(L, -2, "version");
  lua_pushinteger(L, (lua_Integer)args.nOptions);
  lua_setfield(L, -2, "nOptions");
  lua_pushboolean(L, args.ignoreUnrecognized != JNI_FALSE);
  lua_setfield(L, -2, "ignoreUnrecognized");
  return 1;
}

static int l_vm(lua_State *L) {
  lua_settop(L, 1);

  const char *jvm_path = NULL;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "jvm");
    if (lua_isstring(L, -1)) jvm_path = lua_tostring(L, -1);
    lua_pop(L, 1);
  }

  void *lib = load_libjvm(L, jvm_path);
  fn_CreateJavaVM create = NULL;
  fn_GetCreatedJavaVMs created = NULL;
  *(void **)(&create) = dlsym(lib, "JNI_CreateJavaVM");
  *(void **)(&created) = dlsym(lib, "JNI_GetCreatedJavaVMs");
  if (!create || !created) luaL_error(L, "luajni: libjvm is missing JNI entry points");

  JNIEnv *env = NULL;
  JavaVM *jvm = NULL;
  JavaVM *existing[1];
  jsize count = 0;
  if (created(existing, 1, &count) == JNI_OK && count > 0)
    return push_vm(L, existing[0], 0);

  const char *classpath = NULL;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "classpath");
    if (lua_isstring(L, -1)) classpath = lua_tostring(L, -1);
    lua_pop(L, 1);
  }

  char *cp_opt = NULL;
  if (classpath) {
    size_t n = strlen(classpath) + 24;
    cp_opt = (char *)malloc(n);
    if (!cp_opt) return luaL_error(L, "luajni: out of memory");
    snprintf(cp_opt, n, "-Djava.class.path=%s", classpath);
  }

  int extra = 0;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "options");
    if (lua_istable(L, -1)) extra = (int)lua_rawlen(L, -1);
    lua_pop(L, 1);
  }

  JavaVMOption *options = (JavaVMOption *)calloc((size_t)(extra + 1), sizeof(*options));
  if (!options) {
    free(cp_opt);
    return luaL_error(L, "luajni: out of memory");
  }

  int nopts = 0;
  if (cp_opt) options[nopts++].optionString = cp_opt;
  if (extra) {
    lua_getfield(L, 1, "options");
    for (int i = 0; i < extra; i++) {
      lua_rawgeti(L, -1, i + 1);
      if (lua_isstring(L, -1)) options[nopts++].optionString = (char *)lua_tostring(L, -1);
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }

  JavaVMInitArgs args;
  memset(&args, 0, sizeof(args));
  args.version = JNI_VERSION_1_8;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "version");
    if (lua_isinteger(L, -1)) args.version = (jint)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "ignoreUnrecognized");
    args.ignoreUnrecognized = lua_toboolean(L, -1);
    lua_pop(L, 1);
  }
  args.nOptions = nopts;
  args.options = options;

  jint rc = create(&jvm, (void **)&env, &args);
  free(cp_opt);
  free(options);
  if (rc != JNI_OK)
    return luaL_error(L, "luajni: JNI_CreateJavaVM failed (%d)", rc);

  return push_vm(L, jvm, 1);
}

/* ------------------------------------------------------------------ */
/* Typed argument values                                               */
/* ------------------------------------------------------------------ */

static Val *val_new(lua_State *L, int type) {
  Val *v = (Val *)lua_newuserdatauv(L, sizeof(Val), 0);
  memset(v, 0, sizeof(*v));
  v->type = type;
  luaL_setmetatable(L, VAL_MT);
  return v;
}

static int l_val_boolean(lua_State *L) {
  Val *v = val_new(L, 0);
  v->v.z = lua_toboolean(L, 1) ? JNI_TRUE : JNI_FALSE;
  return 1;
}
static int l_val_byte(lua_State *L) {
  Val *v = val_new(L, 1);
  v->v.b = (jbyte)luaL_checkinteger(L, 1);
  return 1;
}
static int l_val_char(lua_State *L) {
  Val *v = val_new(L, 2);
  v->v.c = arg_char(L, 1);
  return 1;
}
static int l_val_short(lua_State *L) {
  Val *v = val_new(L, 3);
  v->v.s = (jshort)luaL_checkinteger(L, 1);
  return 1;
}
static int l_val_int(lua_State *L) {
  Val *v = val_new(L, 4);
  v->v.i = (jint)luaL_checkinteger(L, 1);
  return 1;
}
static int l_val_long(lua_State *L) {
  Val *v = val_new(L, 5);
  v->v.j = (jlong)luaL_checkinteger(L, 1);
  return 1;
}
static int l_val_float(lua_State *L) {
  Val *v = val_new(L, 6);
  v->v.f = (jfloat)luaL_checknumber(L, 1);
  return 1;
}
static int l_val_double(lua_State *L) {
  Val *v = val_new(L, 7);
  v->v.d = (jdouble)luaL_checknumber(L, 1);
  return 1;
}
static int l_val_object(lua_State *L) {
  lua_pushlightuserdata(L, lua_touserdata(L, 1));
  return 1;
}

/* ------------------------------------------------------------------ */
/* Raw memory helpers                                                  */
/* ------------------------------------------------------------------ */

enum {
  C_I8, C_U8, C_I16, C_U16, C_I32, C_U32, C_I64, C_U64,
  C_F32, C_F64, C_PTR
};

static int ctype_of(const char *s) {
  static const char *names[] = {"int8", "uint8", "int16", "uint16", "int32",
                                "uint32", "int64", "uint64", "float", "double", "pointer"};
  for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
    if (!strcmp(s, names[i])) return i;
  return -1;
}

static int l_buffer(lua_State *L) {
  size_t n;
  const char *init = NULL;
  if (lua_type(L, 1) == LUA_TSTRING)
    init = lua_tolstring(L, 1, &n);
  else
    n = (size_t)luaL_checkinteger(L, 1);
  void *p = lua_newuserdatauv(L, n > 0 ? n : 1, 0);
  if (init)
    memcpy(p, init, n);
  else
    memset(p, 0, n > 0 ? n : 1);
  return 1;
}

static int l_read(lua_State *L) {
  unsigned char *p = (unsigned char *)lua_touserdata(L, 1);
  if (!p) return luaL_error(L, "luajni: null pointer");
  const char *t = luaL_optstring(L, 2, "int32");
  if (!strcmp(t, "utf8") || !strcmp(t, "string")) {
    size_t n = lua_isnoneornil(L, 3) ? strlen((const char *)p)
                                     : (size_t)luaL_checkinteger(L, 3);
    lua_pushlstring(L, (const char *)p, n);
    return 1;
  }
  int c = ctype_of(t);
  if (c < 0) return luaL_error(L, "luajni: unknown type %s", t);
  int n = (int)luaL_optinteger(L, 3, 1);
  if (n < 0) n = 0;
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    switch (c) {
      case C_I8: lua_pushinteger(L, (lua_Integer)(int8_t)p[i]); break;
      case C_U8: lua_pushinteger(L, (lua_Integer)p[i]); break;
      case C_I16: lua_pushinteger(L, (lua_Integer)*(int16_t *)(p + i * 2)); break;
      case C_U16: lua_pushinteger(L, (lua_Integer)*(uint16_t *)(p + i * 2)); break;
      case C_I32: lua_pushinteger(L, (lua_Integer)*(int32_t *)(p + i * 4)); break;
      case C_U32: lua_pushinteger(L, (lua_Integer)*(uint32_t *)(p + i * 4)); break;
      case C_I64: lua_pushinteger(L, (lua_Integer)*(int64_t *)(p + i * 8)); break;
      case C_U64: lua_pushinteger(L, (lua_Integer)*(uint64_t *)(p + i * 8)); break;
      case C_F32: lua_pushnumber(L, (lua_Number)*(float *)(p + i * 4)); break;
      case C_F64: lua_pushnumber(L, (lua_Number)*(double *)(p + i * 8)); break;
      case C_PTR: lua_pushlightuserdata(L, *(void **)(p + i * (int)sizeof(void *))); break;
    }
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int l_write(lua_State *L) {
  unsigned char *p = (unsigned char *)lua_touserdata(L, 1);
  if (!p) return luaL_error(L, "luajni: null pointer");
  const char *t = luaL_optstring(L, 2, "int32");
  if (!strcmp(t, "utf8") || !strcmp(t, "string")) {
    size_t n;
    const char *s = luaL_checklstring(L, 3, &n);
    memcpy(p, s, n);
    return 0;
  }
  int c = ctype_of(t);
  if (c < 0) return luaL_error(L, "luajni: unknown type %s", t);
  luaL_checktype(L, 3, LUA_TTABLE);
  int n = (int)lua_rawlen(L, 3);
  for (int i = 0; i < n; i++) {
    lua_rawgeti(L, 3, i + 1);
    switch (c) {
      case C_I8: *(int8_t *)(p + i) = (int8_t)luaL_checkinteger(L, -1); break;
      case C_U8: p[i] = (unsigned char)luaL_checkinteger(L, -1); break;
      case C_I16: *(int16_t *)(p + i * 2) = (int16_t)luaL_checkinteger(L, -1); break;
      case C_U16: *(uint16_t *)(p + i * 2) = (uint16_t)luaL_checkinteger(L, -1); break;
      case C_I32: *(int32_t *)(p + i * 4) = (int32_t)luaL_checkinteger(L, -1); break;
      case C_U32: *(uint32_t *)(p + i * 4) = (uint32_t)luaL_checkinteger(L, -1); break;
      case C_I64: *(int64_t *)(p + i * 8) = (int64_t)luaL_checkinteger(L, -1); break;
      case C_U64: *(uint64_t *)(p + i * 8) = (uint64_t)luaL_checkinteger(L, -1); break;
      case C_F32: *(float *)(p + i * 4) = (float)luaL_checknumber(L, -1); break;
      case C_F64: *(double *)(p + i * 8) = (double)luaL_checknumber(L, -1); break;
      case C_PTR: *(void **)(p + i * (int)sizeof(void *)) = lua_touserdata(L, -1); break;
    }
    lua_pop(L, 1);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

#define REG_CALLS(N, T, P, G)                          \
  {"Call" #N "Method", l_Call##N##Method},             \
      {"Call" #N "MethodA", l_Call##N##Method},        \
      {"Call" #N "MethodV", l_Call##N##Method},        \
      {"CallStatic" #N "Method", l_CallStatic##N##Method},   \
      {"CallStatic" #N "MethodA", l_CallStatic##N##Method},  \
      {"CallStatic" #N "MethodV", l_CallStatic##N##Method},  \
      {"CallNonvirtual" #N "Method", l_CallNonvirtual##N##Method},  \
      {"CallNonvirtual" #N "MethodA", l_CallNonvirtual##N##Method}, \
      {"CallNonvirtual" #N "MethodV", l_CallNonvirtual##N##Method},

#define REG_FIELDS(N, T, P, G)                              \
  {"Get" #N "Field", l_Get##N##Field},                      \
      {"Set" #N "Field", l_Set##N##Field},                  \
      {"GetStatic" #N "Field", l_GetStatic##N##Field},      \
      {"SetStatic" #N "Field", l_SetStatic##N##Field},

#define REG_ARRAYS(N, T, A, P, R)                                 \
  {"New" #N "Array", l_New##N##Array},                         \
      {"Get" #N "ArrayElements", l_Get##N##ArrayElements},     \
      {"Release" #N "ArrayElements", l_Release##N##ArrayElements}, \
      {"Get" #N "ArrayRegion", l_Get##N##ArrayRegion},         \
      {"Set" #N "ArrayRegion", l_Set##N##ArrayRegion},

static const luaL_Reg env_methods[] = {
    {"GetVersion", l_GetVersion},
    {"DefineClass", l_DefineClass},
    {"FindClass", l_FindClass},
    {"FromReflectedMethod", l_FromReflectedMethod},
    {"FromReflectedField", l_FromReflectedField},
    {"ToReflectedMethod", l_ToReflectedMethod},
    {"ToReflectedField", l_ToReflectedField},
    {"GetSuperclass", l_GetSuperclass},
    {"IsAssignableFrom", l_IsAssignableFrom},
    {"Throw", l_Throw},
    {"ThrowNew", l_ThrowNew},
    {"ExceptionOccurred", l_ExceptionOccurred},
    {"ExceptionDescribe", l_ExceptionDescribe},
    {"ExceptionClear", l_ExceptionClear},
    {"ExceptionCheck", l_ExceptionCheck},
    {"FatalError", l_FatalError},
    {"PushLocalFrame", l_PushLocalFrame},
    {"PopLocalFrame", l_PopLocalFrame},
    {"NewGlobalRef", l_NewGlobalRef},
    {"DeleteGlobalRef", l_DeleteGlobalRef},
    {"DeleteLocalRef", l_DeleteLocalRef},
    {"IsSameObject", l_IsSameObject},
    {"NewLocalRef", l_NewLocalRef},
    {"EnsureLocalCapacity", l_EnsureLocalCapacity},
    {"AllocObject", l_AllocObject},
    {"GetObjectClass", l_GetObjectClass},
    {"IsInstanceOf", l_IsInstanceOf},
    {"GetMethodID", l_GetMethodID},
    {"GetFieldID", l_GetFieldID},
    {"GetStaticMethodID", l_GetStaticMethodID},
    {"GetStaticFieldID", l_GetStaticFieldID},
    {"NewObject", l_NewObject},
    {"NewObjectA", l_NewObject},
    {"NewObjectV", l_NewObject},
    {"CallVoidMethod", l_CallVoidMethod},
    {"CallVoidMethodA", l_CallVoidMethod},
    {"CallVoidMethodV", l_CallVoidMethod},
    {"CallStaticVoidMethod", l_CallStaticVoidMethod},
    {"CallStaticVoidMethodA", l_CallStaticVoidMethod},
    {"CallStaticVoidMethodV", l_CallStaticVoidMethod},
    {"CallNonvirtualVoidMethod", l_CallNonvirtualVoidMethod},
    {"CallNonvirtualVoidMethodA", l_CallNonvirtualVoidMethod},
    {"CallNonvirtualVoidMethodV", l_CallNonvirtualVoidMethod},
    PRIM_LIST(REG_CALLS)
    REG_CALLS(Object, jobject, PUSH_REF, A_PTR)
    PRIM_LIST(REG_FIELDS)
    REG_FIELDS(Object, jobject, PUSH_REF, A_PTR)
    {"NewString", l_NewString},
    {"GetStringLength", l_GetStringLength},
    {"GetStringChars", l_GetStringChars},
    {"ReleaseStringChars", l_ReleaseStringChars},
    {"NewStringUTF", l_NewStringUTF},
    {"GetStringUTFLength", l_GetStringUTFLength},
    {"GetStringUTFChars", l_GetStringUTFChars},
    {"ReleaseStringUTFChars", l_ReleaseStringUTFChars},
    {"GetStringRegion", l_GetStringRegion},
    {"GetStringUTFRegion", l_GetStringUTFRegion},
    {"GetStringUTF", l_GetStringUTF},
    {"GetArrayLength", l_GetArrayLength},
    ARRAY_LIST(REG_ARRAYS)
    {"NewObjectArray", l_NewObjectArray},
    {"GetObjectArrayElement", l_GetObjectArrayElement},
    {"SetObjectArrayElement", l_SetObjectArrayElement},
    {"NewWeakGlobalRef", l_NewWeakGlobalRef},
    {"DeleteWeakGlobalRef", l_DeleteWeakGlobalRef},
    {"MonitorEnter", l_MonitorEnter},
    {"MonitorExit", l_MonitorExit},
    {"GetJavaVM", l_GetJavaVM},
    {"RegisterNatives", l_RegisterNatives},
    {"UnregisterNatives", l_UnregisterNatives},
    {"NewDirectByteBuffer", l_NewDirectByteBuffer},
    {"GetDirectBufferAddress", l_GetDirectBufferAddress},
    {"GetDirectBufferCapacity", l_GetDirectBufferCapacity},
    {"GetObjectRefType", l_GetObjectRefType},
#ifdef JNI_VERSION_9
    {"GetModule", l_GetModule},
#endif
#ifdef JNI_VERSION_21
    {"IsVirtualThread", l_IsVirtualThread},
#endif
    {NULL, NULL}};

static const luaL_Reg vm_methods[] = {
    {"env", vm_env},
    {"attach", vm_attach},
    {"detach", vm_detach},
    {"close", vm_close},
    {"ptr", vm_ptr},
    {"GetEnv", vm_getenv},
    {"AttachCurrentThread", vm_attach},
    {"AttachCurrentThreadAsDaemon", vm_attach},
    {"DetachCurrentThread", vm_detach},
    {"DestroyJavaVM", vm_destroy},
    {NULL, NULL}};

static const luaL_Reg module_methods[] = {
    {"vm", l_vm},
    {"JNI_GetCreatedJavaVMs", l_get_created_vms},
    {"JNI_GetDefaultJavaVMInitArgs", l_default_init_args},
    {"buffer", l_buffer},
    {"read", l_read},
    {"write", l_write},
    {"boolean", l_val_boolean},
    {"byte", l_val_byte},
    {"char", l_val_char},
    {"short", l_val_short},
    {"int", l_val_int},
    {"long", l_val_long},
    {"float", l_val_float},
    {"double", l_val_double},
    {"object", l_val_object},
    {NULL, NULL}};

static void push_int_field(lua_State *L, const char *name, lua_Integer v) {
  lua_pushinteger(L, v);
  lua_setfield(L, -2, name);
}

int luaopen_luajni_jni(lua_State *L) {
  luaL_newmetatable(L, ENV_MT);
  lua_newtable(L);
  luaL_setfuncs(L, env_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newmetatable(L, VM_MT);
  lua_newtable(L);
  luaL_setfuncs(L, vm_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, vm_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, VAL_MT);
  lua_pop(L, 1);

  luaL_newlib(L, module_methods);

  lua_pushlightuserdata(L, NULL);
  lua_setfield(L, -2, "null");

  lua_newtable(L);
  push_int_field(L, "JNI_OK", JNI_OK);
  push_int_field(L, "JNI_ERR", JNI_ERR);
  push_int_field(L, "JNI_EDETACHED", JNI_EDETACHED);
  push_int_field(L, "JNI_EVERSION", JNI_EVERSION);
  push_int_field(L, "JNI_ENOMEM", JNI_ENOMEM);
  push_int_field(L, "JNI_EEXIST", JNI_EEXIST);
  push_int_field(L, "JNI_EINVAL", JNI_EINVAL);
  push_int_field(L, "JNI_COMMIT", JNI_COMMIT);
  push_int_field(L, "JNI_ABORT", JNI_ABORT);
  push_int_field(L, "JNI_VERSION_1_1", JNI_VERSION_1_1);
  push_int_field(L, "JNI_VERSION_1_2", JNI_VERSION_1_2);
  push_int_field(L, "JNI_VERSION_1_4", JNI_VERSION_1_4);
  push_int_field(L, "JNI_VERSION_1_6", JNI_VERSION_1_6);
  push_int_field(L, "JNI_VERSION_1_8", JNI_VERSION_1_8);
#ifdef JNI_VERSION_9
  push_int_field(L, "JNI_VERSION_9", JNI_VERSION_9);
#endif
#ifdef JNI_VERSION_10
  push_int_field(L, "JNI_VERSION_10", JNI_VERSION_10);
#endif
#ifdef JNI_VERSION_19
  push_int_field(L, "JNI_VERSION_19", JNI_VERSION_19);
#endif
#ifdef JNI_VERSION_21
  push_int_field(L, "JNI_VERSION_21", JNI_VERSION_21);
#endif
#ifdef JNI_VERSION_24
  push_int_field(L, "JNI_VERSION_24", JNI_VERSION_24);
#endif
  push_int_field(L, "JNI_INVALID_OBJECT", 0);
  push_int_field(L, "JNI_LOCAL_REF", 1);
  push_int_field(L, "JNI_GLOBAL_REF", 2);
  push_int_field(L, "JNI_WEAK_GLOBAL_REF", 3);
  lua_setfield(L, -2, "constants");

  return 1;
}
