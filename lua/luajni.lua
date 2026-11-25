-- luajni: signature driven Lua bindings on top of luajni.jni.
--
--   local jni = require("luajni")
--   local vm = jni.vm({ classpath = "classes" })
--   local System = vm:class("java/lang/System")
--   print(System:callStatic("getProperty", "(Ljava/lang/String;)Ljava/lang/String;", "java.version"))

local core = require("luajni.jni")

local M = { core = core, version = "1.0.0" }
local J = core

local TYPE = {}
for _, t in ipairs({
  { "Z", "Boolean", J.boolean },
  { "B", "Byte", J.byte },
  { "C", "Char", J.char },
  { "S", "Short", J.short },
  { "I", "Int", J.int },
  { "J", "Long", J.long },
  { "F", "Float", J.float },
  { "D", "Double", J.double },
}) do
  TYPE[t[1]] = { name = t[2], box = t[3] }
end

local function err(fmt, ...)
  error("luajni: " .. string.format(fmt, ...), 3)
end

local function parse(sig)
  if type(sig) ~= "string" or sig:sub(1, 1) ~= "(" then
    err("invalid signature %q", tostring(sig))
  end
  local params, i = {}, 2
  while true do
    local c = sig:sub(i, i)
    if c == "" then err("unterminated signature %q", sig) end
    if c == ")" then
      local ret = sig:sub(i + 1)
      if ret == "" then err("missing return type in %q", sig) end
      return params, ret
    end
    if c == "[" then
      local j = i
      while sig:sub(j, j) == "[" do j = j + 1 end
      if sig:sub(j, j) == "L" then
        j = sig:find(";", j, true) or err("invalid signature %q", sig)
      end
      params[#params + 1] = sig:sub(i, j)
      i = j + 1
    elseif c == "L" then
      local j = sig:find(";", i, true) or err("invalid signature %q", sig)
      params[#params + 1] = sig:sub(i, j)
      i = j + 1
    else
      params[#params + 1] = c
      i = i + 1
    end
  end
end

local function isRef(desc)
  local c = desc:sub(1, 1)
  return c == "L" or c == "["
end

local function callName(ret, static)
  local name = TYPE[ret] and TYPE[ret].name or (ret == "V" and "Void" or "Object")
  return (static and "CallStatic" or "Call") .. name .. "Method"
end

local function fieldName(op, desc)
  local t = TYPE[desc]
  return op .. (t and t.name or "Object") .. "Field"
end

local function pending(env)
  local ok, has = pcall(env.ExceptionCheck, env)
  if not ok or not has then return nil end
  local thrown = env:ExceptionOccurred()
  env:ExceptionClear()
  local text
  if thrown then
    pcall(function()
      local tcls = env:FindClass("java/lang/Throwable")
      local tos = env:GetMethodID(tcls, "toString", "()Ljava/lang/String;")
      local s = env:CallObjectMethod(thrown, tos, {})
      if s and not env:ExceptionCheck() then text = env:GetStringUTF8(s) end
      env:DeleteLocalRef(s)
      env:DeleteLocalRef(tcls)
    end)
    env:ExceptionClear()
    env:DeleteLocalRef(thrown)
  end
  return text or "java exception"
end

local Obj, Class, Arr = {}, {}, {}
Obj.__index, Class.__index, Arr.__index = Obj, Class, Arr

local function wrap(vm, env, desc, ref)
  local mt = desc:sub(1, 1) == "[" and Arr or Obj
  return setmetatable({
    vm = vm, env = env, ref = ref, desc = desc,
    _methods = {}, _fields = {},
  }, mt)
end

local function guarded(vm, ret, fn, cap)
  local env = vm:env()
  local rc = env:PushLocalFrame(cap or 16)
  if rc < 0 then
    local ex = pending(env)
    error(ex or "luajni: cannot push a local frame", 0)
  end
  local ok, r = pcall(fn)
  if not ok then
    local ex = pending(env)
    env:PopLocalFrame(nil)
    error(ex or r, 0)
  end
  local ex = pending(env)
  if ex then
    env:PopLocalFrame(nil)
    error(ex, 0)
  end
  if ret == "V" then
    env:PopLocalFrame(nil)
    return nil
  end
  if ret == "Ljava/lang/String;" then
    local s
    if r ~= nil then
      s = env:GetStringUTF8(r)
      if s == nil then
        local err2 = pending(env)
        env:PopLocalFrame(nil)
        error(err2 or "luajni: cannot convert the returned string", 0)
      end
    end
    env:PopLocalFrame(nil)
    return s
  end
  if not isRef(ret) then
    env:PopLocalFrame(nil)
    return r
  end
  local keep
  if r ~= nil then keep = env:NewLocalRef(r) end
  local out = env:PopLocalFrame(keep)
  if not out then return nil end
  return wrap(vm, env, ret, out)
end

local newArrayRef, boxArg

newArrayRef = function(env, desc, values, n)
  local elem = desc:sub(2)
  local t = TYPE[elem]
  local len = n or (values and #values) or 0
  if t then
    local ref = env["New" .. t.name .. "Array"](env, len)
    if values and len > 0 then
      env["Set" .. t.name .. "ArrayRegion"](env, ref, 0, len, values)
    end
    return ref
  end
  local clsname = elem:sub(1, 1) == "L" and elem:sub(2, -2) or elem
  local cls = env:FindClass(clsname)
  local ex = pending(env)
  if ex then error(ex, 3) end
  local ref = env:NewObjectArray(len, cls, nil)
  env:DeleteLocalRef(cls)
  for i = 1, len do
    local v = values and values[i] or nil
    local o = boxArg(env, elem, v)
    env:SetObjectArrayElement(ref, i - 1, o)
    if type(v) == "string" or (type(v) == "table" and not v.ref) then
      env:DeleteLocalRef(o)
    end
  end
  return ref
end

boxArg = function(env, desc, v)
  local t = TYPE[desc]
  if t then
    if v == nil then err("argument of type %s cannot be nil", desc) end
    return t.box(v)
  end
  local c = desc:sub(1, 1)
  if c == "L" then
    if v == nil or v == J.null then return J.null end
    if type(v) == "string" then
      if desc == "Ljava/lang/String;" then return env:NewStringUTF8(v) end
      err("cannot pass a Lua string as %s", desc)
    end
    if type(v) == "table" then
      if v.ref then return v.ref end
      err("cannot pass a table as %s", desc)
    end
    if J.ispointer(v) then return v end
    err("cannot pass %s as %s", type(v), desc)
  end
  if c == "[" then
    if v == nil or v == J.null then return J.null end
    if type(v) == "table" then
      if v.ref then return v.ref end
      return newArrayRef(env, desc, v)
    end
    if J.ispointer(v) then return v end
    err("cannot pass %s as %s", type(v), desc)
  end
  err("invalid descriptor %q", desc)
end

local function rawArg(env, desc, v)
  if TYPE[desc] then
    if desc == "Z" then return v and true or false end
    return v
  end
  return boxArg(env, desc, v)
end

local function checkArity(name, sig, params, n)
  if n ~= #params then
    err("%s%s expects %d arguments, got %d", name, sig, #params, n)
  end
end

local function callMethod(vm, env, owner, mid, fname, params, ret, ...)
  local packed = table.pack(...)
  return guarded(vm, ret, function()
    local boxed = {}
    for i = 1, #params do boxed[i] = boxArg(env, params[i], packed[i]) end
    return env[fname](env, owner.ref, mid, boxed)
  end, 16 + 2 * #params)
end

function Obj:methodID(name, sig)
  local key = name .. sig
  local id = self._methods[key]
  if id then return id end
  local env = self.vm:env()
  local cls = env:GetObjectClass(self.ref)
  id = env:GetMethodID(cls, name, sig)
  env:DeleteLocalRef(cls)
  local ex = pending(env)
  if ex then error(ex, 3) end
  if not id then err("method %s%s not found", name, sig) end
  self._methods[key] = id
  return id
end

function Obj:method(name, sig)
  local params, ret = parse(sig)
  local mid = self:methodID(name, sig)
  local fname = callName(ret, false)
  local owner, env = self, self.env
  return function(...)
    local n = select("#", ...)
    checkArity(name, sig, params, n)
    return callMethod(owner.vm, env, owner, mid, fname, params, ret, ...)
  end
end

function Obj:call(name, sig, ...)
  local params, ret = parse(sig)
  local mid = self:methodID(name, sig)
  local n = select("#", ...)
  checkArity(name, sig, params, n)
  return callMethod(self.vm, self.env, self, mid, callName(ret, false), params, ret, ...)
end

function Obj:fieldID(name, desc)
  local key = name .. desc
  local id = self._fields[key]
  if id then return id end
  local env = self.vm:env()
  local cls = env:GetObjectClass(self.ref)
  id = env:GetFieldID(cls, name, desc)
  env:DeleteLocalRef(cls)
  local ex = pending(env)
  if ex then error(ex, 3) end
  if not id then err("field %s of type %s not found", name, desc) end
  self._fields[key] = id
  return id
end

function Obj:get(name, desc)
  local fid = self:fieldID(name, desc)
  local env, fname = self.env, fieldName("Get", desc)
  return guarded(self.vm, desc, function() return env[fname](env, self.ref, fid) end)
end

function Obj:set(name, desc, value)
  local fid = self:fieldID(name, desc)
  local env, fname = self.env, fieldName("Set", desc)
  guarded(self.vm, "V", function()
    env[fname](env, self.ref, fid, rawArg(env, desc, value))
  end)
end

function Obj:class()
  local env = self.vm:env()
  local local_cls = env:GetObjectClass(self.ref)
  local ref = env:NewGlobalRef(local_cls)
  env:DeleteLocalRef(local_cls)
  local ex = pending(env)
  if ex then error(ex, 3) end
  if not ref then err("cannot create a global reference for the class") end
  return setmetatable({
    vm = self.vm, env = env, ref = ref,
    _methods = {}, _smethods = {}, _fields = {}, _sfields = {},
  }, Class)
end

function Obj:ref_()
  return self.ref
end

function Obj:__tostring()
  local ok, s = pcall(function() return self:call("toString", "()Ljava/lang/String;") end)
  if ok and type(s) == "string" then return s end
  return "jni.object"
end

function Obj:__gc()
  if self.ref and self.vm and not self.vm._closed then
    pcall(self.env.DeleteLocalRef, self.env, self.ref)
  end
  self.ref = nil
end

local function newClass(vm, env, ref, name)
  return setmetatable({
    vm = vm, env = env, ref = ref, name = name,
    _methods = {}, _smethods = {}, _fields = {}, _sfields = {},
  }, Class)
end

function Class:methodID(name, sig)
  local key = name .. sig
  local id = self._methods[key]
  if id then return id end
  local env = self.vm:env()
  id = env:GetMethodID(self.ref, name, sig)
  local ex = pending(env)
  if ex then error(ex, 3) end
  if not id then err("method %s%s not found on %s", name, sig, self.name or "class") end
  self._methods[key] = id
  return id
end

function Class:staticID(name, sig)
  local key = name .. sig
  local id = self._smethods[key]
  if id then return id end
  local env = self.vm:env()
  id = env:GetStaticMethodID(self.ref, name, sig)
  local ex = pending(env)
  if ex then error(ex, 3) end
  if not id then err("static method %s%s not found on %s", name, sig, self.name or "class") end
  self._smethods[key] = id
  return id
end

function Class:staticFieldID(name, desc)
  local key = name .. desc
  local id = self._sfields[key]
  if id then return id end
  local env = self.vm:env()
  id = env:GetStaticFieldID(self.ref, name, desc)
  local ex = pending(env)
  if ex then error(ex, 3) end
  if not id then err("static field %s of type %s not found", name, desc) end
  self._sfields[key] = id
  return id
end

function Class:new(sig, ...)
  if sig == nil then sig = "()V" end
  local params = parse(sig)
  local n = select("#", ...)
  checkArity("<init>", sig, params, n)
  local mid = self:methodID("<init>", sig)
  local ret = self.name and ("L" .. self.name .. ";") or "Ljava/lang/Object;"
  local env = self.env
  local packed = table.pack(...)
  return guarded(self.vm, ret, function()
    local boxed = {}
    for i = 1, #params do boxed[i] = boxArg(env, params[i], packed[i]) end
    return env:NewObject(self.ref, mid, boxed)
  end, 16 + 2 * #params)
end

function Class:callStatic(name, sig, ...)
  local params, ret = parse(sig)
  local n = select("#", ...)
  checkArity(name, sig, params, n)
  local mid = self:staticID(name, sig)
  return callMethod(self.vm, self.env, self, mid, callName(ret, true), params, ret, ...)
end

function Class:static(name, sig)
  local params, ret = parse(sig)
  local mid = self:staticID(name, sig)
  local fname = callName(ret, true)
  local owner, env = self, self.env
  return function(...)
    local n = select("#", ...)
    checkArity(name, sig, params, n)
    return callMethod(owner.vm, env, owner, mid, fname, params, ret, ...)
  end
end

function Class:getStatic(name, desc)
  local fid = self:staticFieldID(name, desc)
  local env, fname = self.env, fieldName("GetStatic", desc)
  return guarded(self.vm, desc, function() return env[fname](env, self.ref, fid) end)
end

function Class:setStatic(name, desc, value)
  local fid = self:staticFieldID(name, desc)
  local env, fname = self.env, fieldName("SetStatic", desc)
  guarded(self.vm, "V", function()
    env[fname](env, self.ref, fid, rawArg(env, desc, value))
  end)
end

function Class:__tostring()
  return "class " .. (self.name or "?")
end

function Class:__gc()
  if self.ref and self.vm and not self.vm._closed then
    pcall(self.env.DeleteGlobalRef, self.env, self.ref)
  end
  self.ref = nil
end

local function arrayIndex(i)
  local n = math.tointeger(i)
  if not n then err("array index must be an integer") end
  return n
end

function Arr:len()
  return self.env:GetArrayLength(self.ref)
end

function Arr:get(i)
  i = arrayIndex(i)
  local env = self.env
  local elem = self.desc:sub(2)
  local t = TYPE[elem]
  if t then
    return guarded(self.vm, elem, function()
      local v = env["Get" .. t.name .. "ArrayRegion"](env, self.ref, i - 1, 1)
      return v[1]
    end)
  end
  return guarded(self.vm, elem, function()
    return env:GetObjectArrayElement(self.ref, i - 1)
  end)
end

function Arr:set(i, v)
  i = arrayIndex(i)
  local env = self.env
  local elem = self.desc:sub(2)
  local t = TYPE[elem]
  if t then
    local fname = "Set" .. t.name .. "ArrayRegion"
    guarded(self.vm, "V", function()
      env[fname](env, self.ref, i - 1, 1, { rawArg(env, elem, v) })
    end)
  else
    guarded(self.vm, "V", function()
      env:SetObjectArrayElement(self.ref, i - 1, boxArg(env, elem, v))
    end)
  end
end

function Arr:values()
  local env = self.env
  local elem = self.desc:sub(2)
  local n = env:GetArrayLength(self.ref)
  local t = TYPE[elem]
  if t then
    return guarded(self.vm, elem, function()
      return env["Get" .. t.name .. "ArrayRegion"](env, self.ref, 0, n)
    end)
  end
  local out = {}
  for i = 1, n do out[i] = self:get(i) end
  return out
end

function Arr:__tostring()
  return "jni.array<" .. self.desc .. ">"
end

function Arr:__gc()
  if self.ref and self.vm and not self.vm._closed then
    pcall(self.env.DeleteLocalRef, self.env, self.ref)
  end
  self.ref = nil
end

local VM = {}
VM.__index = VM

function VM:env()
  if self._closed then error("luajni: JVM is closed", 2) end
  local env = self._env
  if not env then
    env = self.handle:env()
    self._env = env
  end
  return env
end

function VM:class(name)
  local env = self:env()
  local ref = env:FindClass(name)
  local ex = pending(env)
  if ex then error(ex, 2) end
  if not ref then err("class not found: %s", name) end
  local global = env:NewGlobalRef(ref)
  env:DeleteLocalRef(ref)
  if not global then
    local ex2 = pending(env)
    error(ex2 or ("luajni: cannot create a global reference for " .. name), 2)
  end
  return newClass(self, env, global, name)
end

function VM:array(desc, values)
  if type(desc) ~= "string" or desc:sub(1, 1) ~= "[" then
    err("array descriptor expected, got %q", tostring(desc))
  end
  local n, vals
  if type(values) == "number" then
    n = math.tointeger(values) or err("array length must be an integer")
    if n < 0 then err("array length must not be negative") end
  elseif type(values) == "table" then
    vals = values
  elseif values ~= nil then
    err("array values must be a table or a length")
  end
  local env = self:env()
  local count = n or (vals and #vals) or 0
  return guarded(self, desc, function() return newArrayRef(env, desc, vals, n) end, 16 + 2 * count)
end

function VM:attach()
  self._env = self.handle:attach()
  return self._env
end

function VM:detach()
  self._env = nil
  return self.handle:detach()
end

function VM:ptr()
  return self.handle:ptr()
end

function VM:close()
  if self._closed then return end
  self._closed = true
  self._env = nil
  local handle = self.handle
  self.handle = nil
  if handle then handle:close() end
end

function M.vm(opts)
  return setmetatable({ handle = J.vm(opts), _env = nil, _closed = false }, VM)
end

function M.check(env)
  local ex = pending(env)
  if ex then error(ex, 2) end
end

return M
