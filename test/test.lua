local jni = require("luajni")
local J = jni.core

local function eq(got, want, what)
  if got ~= want then
    error(("%s: expected %s, got %s"):format(what or "value", tostring(want), tostring(got)), 2)
  end
end

local function near(got, want, what)
  if math.abs(got - want) > 1e-6 then
    error(("%s: expected %s, got %s"):format(what or "value", tostring(want), tostring(got)), 2)
  end
end

local classpath = os.getenv("LUAJNI_CLASSPATH") or "build/test-classes"
local options = { "-Xmx64m" }
if os.getenv("LUAJNI_CHECK_JNI") == "1" then options[#options + 1] = "-Xcheck:jni" end
local vm = jni.vm({ classpath = classpath, options = options })
local env = vm:env()

-- Complete JNIEnv surface ----------------------------------------------------

local prim = { "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double" }
local function has(name)
  assert(type(env[name]) == "function", "missing JNIEnv function " .. name)
end

for _, t in ipairs(prim) do
  for _, p in ipairs({ "Call", "CallStatic", "CallNonvirtual" }) do
    for _, s in ipairs({ "", "A", "V" }) do has(p .. t .. "Method" .. s) end
  end
  for _, p in ipairs({ "Get", "Set", "GetStatic", "SetStatic" }) do has(p .. t .. "Field") end
  has("New" .. t .. "Array")
  has("Get" .. t .. "ArrayElements")
  has("Release" .. t .. "ArrayElements")
  has("Get" .. t .. "ArrayRegion")
  has("Set" .. t .. "ArrayRegion")
end
for _, p in ipairs({ "Call", "CallStatic", "CallNonvirtual" }) do
  for _, s in ipairs({ "", "A", "V" }) do
    has(p .. "ObjectMethod" .. s)
    has(p .. "VoidMethod" .. s)
  end
end
for _, p in ipairs({ "Get", "Set", "GetStatic", "SetStatic" }) do has(p .. "ObjectField") end
for _, name in ipairs({
  "GetVersion", "DefineClass", "FindClass", "FromReflectedMethod", "FromReflectedField",
  "ToReflectedMethod", "ToReflectedField", "GetSuperclass", "IsAssignableFrom", "Throw",
  "ThrowNew", "ExceptionOccurred", "ExceptionDescribe", "ExceptionClear", "FatalError",
  "PushLocalFrame", "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef",
  "IsSameObject", "NewLocalRef", "EnsureLocalCapacity", "AllocObject", "GetObjectClass",
  "IsInstanceOf", "GetMethodID", "GetFieldID", "GetStaticMethodID", "GetStaticFieldID",
  "NewObject", "NewObjectA", "NewObjectV", "NewString", "GetStringLength", "GetStringChars",
  "ReleaseStringChars", "NewStringUTF", "GetStringUTFLength", "GetStringUTFChars",
  "ReleaseStringUTFChars", "GetStringRegion", "GetStringUTFRegion", "GetStringUTF8", "NewStringUTF8",
  "GetPrimitiveArrayCritical", "ReleasePrimitiveArrayCritical", "GetStringCritical", "ReleaseStringCritical",
  "GetArrayLength", "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement",
  "NewWeakGlobalRef", "DeleteWeakGlobalRef", "MonitorEnter", "MonitorExit", "GetJavaVM",
  "RegisterNatives", "UnregisterNatives", "NewDirectByteBuffer", "GetDirectBufferAddress",
  "GetDirectBufferCapacity", "GetObjectRefType",
}) do
  has(name)
end

assert(type(env.GetModule) == "function" or type(env.GetModule) == "nil", "GetModule")
assert(type(env.GetStringUTFLengthAsLong) == "function" or type(env.GetStringUTFLengthAsLong) == "nil",
  "GetStringUTFLengthAsLong")
assert(type(env.IsVirtualThread) == "function" or type(env.IsVirtualThread) == "nil", "IsVirtualThread")
assert(env:GetVersion() >= J.constants.JNI_VERSION_1_8, "GetVersion")

-- Invocation API -------------------------------------------------------------

assert(type(J.JNI_GetCreatedJavaVMs()) == "table", "JNI_GetCreatedJavaVMs")
assert(#J.JNI_GetCreatedJavaVMs() >= 1, "created VM count")
assert(J.JNI_GetDefaultJavaVMInitArgs(J.constants.JNI_VERSION_1_8).version == J.constants.JNI_VERSION_1_8,
  "JNI_GetDefaultJavaVMInitArgs")

-- JavaVM surface -------------------------------------------------------------

for _, name in ipairs({ "env", "attach", "detach", "close", "ptr" }) do
  assert(type(vm[name]) == "function", "missing VM function " .. name)
end
for _, name in ipairs({
  "GetEnv", "AttachCurrentThread", "AttachCurrentThreadAsDaemon",
  "DetachCurrentThread", "DestroyJavaVM",
}) do
  assert(type(vm.handle[name]) == "function", "missing JavaVM function " .. name)
end

-- Static methods -------------------------------------------------------------

local Demo = vm:class("demo/Demo")
local add = Demo:static("add", "(II)I")
eq(add(2, 3), 5, "add")
eq(Demo:callStatic("add", "(II)I", -5, 8), 3, "add negative")
eq(Demo:callStatic("twice", "(J)J", 1099511627776), 2199023255552, "twice long")
near(Demo:callStatic("half", "(D)D", 5), 2.5, "half double")
near(Demo:callStatic("third", "(F)F", 1.5), 0.5, "third float")
eq(Demo:callStatic("not", "(Z)Z", true), false, "not")
eq(Demo:callStatic("firstChar", "(Ljava/lang/String;)C", "Hello"), 72, "firstChar")
eq(Demo:callStatic("greet", "(Ljava/lang/String;)Ljava/lang/String;", "Lua"), "Hello, Lua!", "greet")
eq(Demo:callStatic("greet", "(Ljava/lang/String;)Ljava/lang/String;", "世界"), "Hello, 世界!", "greet utf8")
eq(Demo:callStatic("greet", "(Ljava/lang/String;)Ljava/lang/String;", "😀"), "Hello, 😀!", "greet emoji")
eq(Demo:callStatic("echo", "(Ljava/lang/String;)Ljava/lang/String;", "a\0b"), "a\0b", "embedded NUL")
eq(Demo:callStatic("nothing", "()Ljava/lang/Object;"), nil, "null object")

-- Fields ---------------------------------------------------------------------

eq(Demo:getStatic("CONST", "I"), 42, "CONST")
eq(Demo:getStatic("BIG", "J"), 1099511627776, "BIG")
Demo:setStatic("BIG", "J", 5)
eq(Demo:getStatic("BIG", "J"), 5, "BIG after set")

-- Objects --------------------------------------------------------------------

local d = Demo:new("(I)V", 10)
eq(d:call("get", "()I"), 10, "get")
local inc = d:method("inc", "(I)I")
eq(inc(5), 15, "inc")
eq(d:call("get", "()I"), 15, "get after inc")
eq(tostring(d), "Demo(15)", "toString")
d:set("value", "I", 7)
eq(d:get("value", "I"), 7, "public field")
eq(Demo:new():call("get", "()I"), 0, "default constructor")
eq(d:class():callStatic("add", "(II)I", 1, 1), 2, "obj:class()")

for i = 1, 200 do
  collectgarbage()
  assert(Demo:new("(I)V", i):call("get", "()I") == i, "temporary object call")
  assert(vm:class("demo/Demo"):callStatic("add", "(II)I", 1, 2) == 3, "temporary class call")
end

local keep = Demo:new("(I)V", 1)
local keepInc = keep:method("inc", "(I)I")
keep = nil
collectgarbage()
collectgarbage()
eq(keepInc(1), 2, "method closure keeps the object alive")

local add2 = vm:class("demo/Demo"):static("add", "(II)I")
collectgarbage()
eq(add2(4, 5), 9, "static closure keeps the class alive")

-- Arrays ---------------------------------------------------------------------

local numbers = Demo:callStatic("numbers", "(I)[I", 5)
eq(numbers:len(), 5, "array length")
eq(numbers:get(3), 4, "array get")
numbers:set(1, 100)
eq(numbers:values()[1], 100, "array values")
eq(Demo:callStatic("sum", "([I)I", numbers), 130, "sum array object")
eq(Demo:callStatic("sum", "([I)I", { 1, 2, 3 }), 6, "sum array table")

local floats = Demo:callStatic("floats", "()[F")
near(floats:get(2), 2.5, "float array")
near(floats:values()[1], 1.5, "float array values")
local flags = Demo:callStatic("flags", "()[Z")
eq(flags:get(1), true, "boolean array")
eq(flags:values()[2], false, "boolean array values")
eq(Demo:callStatic("chars", "()[C"):get(1), 97, "char array")
eq(Demo:callStatic("join", "([Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
  { "a", "b" }, "-"), "a-b", "string array argument")

local many = {}
for i = 1, 200 do many[i] = tostring(i) end
eq(Demo:callStatic("join", "([Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
  many, ","):sub(1, 4), "1,2,", "large string array")
eq(vm:array("[Ljava/lang/String;", many):get(200), "200", "large object array")

local oa = Demo:new("(I)V", 1)
local ob = Demo:new("(I)V", 2)
eq(tostring(oa), "Demo(1)", "distinct object a")
eq(tostring(ob), "Demo(2)", "distinct object b")

assert(not pcall(function()
  Demo:callStatic("greet", "(Ljava/lang/String;)Ljava/lang/String;", 42)
end), "non-string object argument must be rejected")
assert(not pcall(function()
  vm:array("[Ljava/lang/String;", { 1 })
end), "non-string array element must be rejected")

local own = vm:array("[I", { 7, 8 })
eq(own:len(), 2, "vm:array length")
eq(own:get(2), 8, "vm:array get")
own:set(2, 9)
eq(own:get(2), 9, "vm:array set")
eq(vm:array("[I", 3):len(), 3, "vm:array sized")

local names = vm:array("[Ljava/lang/String;", { "x", "y" })
eq(names:len(), 2, "object array length")
eq(names:get(2), "y", "object array get")
names:set(1, "z")
eq(names:get(1), "z", "object array set")

-- Exceptions -----------------------------------------------------------------

local ok, ex = pcall(function()
  Demo:callStatic("fail", "(Ljava/lang/String;)V", "boom")
end)
assert(not ok, "exception must propagate")
assert(tostring(ex):find("IllegalStateException") and tostring(ex):find("boom"),
  "exception text: " .. tostring(ex))

ok = pcall(function()
  Demo:callStatic("missing", "()V")
end)
assert(not ok, "missing method must raise")

-- Raw JNI API ----------------------------------------------------------------

local DemoCls = env:FindClass("demo/Demo")
local addID = env:GetStaticMethodID(DemoCls, "add", "(II)I")
eq(env:CallStaticIntMethod(DemoCls, addID, { J.int(20), J.int(22) }), 42, "typed int args")
assert(not env:ExceptionCheck(), "no pending exception after raw call")
eq(env:CallStaticIntMethod(DemoCls, addID, { 20, 22 }), 42, "plain number args")
assert(not env:ExceptionCheck(), "no pending exception after raw call")
env:DeleteLocalRef(DemoCls)

local Integer = env:FindClass("java/lang/Integer")
local parseInt = env:GetStaticMethodID(Integer, "parseInt", "(Ljava/lang/String;)I")
local arg = env:NewStringUTF("-123")
eq(env:CallStaticIntMethod(Integer, parseInt, { arg }), -123, "raw call")
assert(not env:ExceptionCheck(), "no pending exception after raw call")
env:DeleteLocalRef(arg)

local bad = env:GetStaticMethodID(Integer, "missing", "()V")
assert(bad == nil, "missing method id")
assert(not pcall(jni.check, env), "jni.check raises pending exception")

env:DeleteLocalRef(Integer)

local buf = J.buffer(16)
J.write(buf, "int64", { 1099511627776 })
eq(J.read(buf, "int64")[1], 1099511627776, "read int64")
J.write(buf, "utf8", "lua")
eq(J.read(buf, "utf8", 3), "lua", "read utf8")

assert(J.null ~= nil, "null sentinel")

local carr = env:NewIntArray(3)
env:SetIntArrayRegion(carr, 0, 3, { 4, 5, 6 })
local cptr = env:GetPrimitiveArrayCritical(carr, false)
local cvals = J.read(cptr, "int32", 3)
env:ReleasePrimitiveArrayCritical(carr, cptr, 0)
eq(cvals[2], 5, "primitive array critical")
env:DeleteLocalRef(carr)

local cstr = env:NewStringUTF8("hi")
local sptr = env:GetStringCritical(cstr, false)
local units = J.read(sptr, "uint16", 2)
env:ReleaseStringCritical(cstr, sptr)
eq(units[1], 104, "string critical")
if env.GetStringUTFLengthAsLong then
  eq(env:GetStringUTFLengthAsLong(cstr), 2, "large UTF-8 length")
end
env:DeleteLocalRef(cstr)

local raw = env:NewStringUTF8("😀\0x")
eq(env:GetStringUTF8(raw), "😀\0x", "raw UTF-8 conversion")
env:DeleteLocalRef(raw)

-- Thread attachment ----------------------------------------------------------

local tmp = Demo:new("()V")
tmp = nil
vm:detach()
collectgarbage()
collectgarbage()
eq(Demo:callStatic("add", "(II)I", 2, 2), 4, "reattach after detach")
eq(Demo:callStatic("square", "(I)I", 5), 25, "resolve an uncached method after detach")

-- Shutdown -------------------------------------------------------------------

vm:close()
assert(not pcall(function() return vm:env() end), "env after close must fail")

print("ok: all luajni tests passed")
