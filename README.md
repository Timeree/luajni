# luajni

通过 Lua 直接调用 JVM：把完整的 JNI C 接口封装成 Lua 模块，并在其上提供按签名自动编组的高层 API。

```
luajni/
├── src/luajni.c          # C 模块：完整 JNIEnv / JavaVM 接口
├── lua/luajni.lua        # 高层 API：vm / class / object / array
├── test/
│   ├── java/demo/Demo.java
│   └── test.lua
├── Makefile
└── .github/workflows/ci.yml
```

## 环境

- Linux，C 编译器，GNU make，curl
- JDK 8+（需要 `jni.h`，默认由 `javac` 位置推导）
- Lua 5.4；`make` 会从 lua.org 下载并编译 `LUA_VERSION`（默认 5.4.9）

## 构建与测试

```sh
make                  # 生成 build/luajni/jni.so 与 build/luajni.lua
make test             # 编译 Java 夹具并运行完整测试
make install          # PREFIX=/usr/local
```

可覆盖变量：

```sh
make LUA_VERSION=5.4.9          # 指定 Lua 源码版本
make JDK=/path/to/jdk           # 指定 JDK
make LUA_INC=/usr/include/lua5.4 LUA=lua5.4 test   # 使用系统 Lua
```

## 快速开始

```lua
local jni = require("luajni")

local vm = jni.vm({ classpath = "/opt/app/classes", options = { "-Xmx256m" } })

local System = vm:class("java/lang/System")
print(System:callStatic("getProperty", "(Ljava/lang/String;)Ljava/lang/String;", "java.version"))

local File = vm:class("java/io/File")
local f = File:new("(Ljava/lang/String;)V", "/tmp")
print(f:call("exists", "()Z"))
print(f:call("getAbsolutePath", "()Ljava/lang/String;"))
```

## 高层 API

### jni.vm(opts) -> VM

创建或附着当前进程已有的 JVM。参数均可省略：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `classpath` | string | `-Djava.class.path=...` |
| `options` | table | 追加 `JavaVMOption`，如 `{"-Xmx256m"}` |
| `jvm` | string | `libjvm.so` 绝对路径；默认查找 `JAVA_HOME` 与 `/usr/lib/jvm/*` |
| `version` | integer | JNI 版本，默认 `JNI_VERSION_1_8` |
| `ignoreUnrecognized` | boolean | 传给 `JavaVMInitArgs` |

### VM

| 方法 | 说明 |
| --- | --- |
| `vm:env()` | 当前线程的 `JNIEnv`，未附着时自动附着并缓存 |
| `vm:attach()` / `vm:detach()` | 显式附着 / 解除当前线程 |
| `vm:class(name)` | 查找类，返回 `Class`（全局引用） |
| `vm:array(desc, values)` | 由 Lua 表或长度创建数组，返回 `Array` |
| `vm:ptr()` | 原始 `JavaVM*` |
| `vm:close()` | 销毁自己创建的 JVM；附着模式仅 detach |
| `vm.handle` | 底层 `luajni.jni` 的 VM 对象（JavaVM 全部函数） |

### Class

```lua
local C = vm:class("java/util/ArrayList")

local o = C:new("(I)V", 16)                    -- 构造
local add = C:static("valueOf", "(I)Ljava/lang/Integer;")  -- 静态方法绑定
local n = C:callStatic("parseInt", "(Ljava/lang/String;)I", "42")
local f = C:getStatic("MAX_VALUE", "I")
C:setStatic("SOME_FIELD", "I", 1)
```

| 方法 | 说明 |
| --- | --- |
| `C:new([sig, ...])` | 调用构造函数，`sig` 省略时为 `()V` |
| `C:callStatic(name, sig, ...)` | 调用静态方法 |
| `C:static(name, sig)` | 返回预先解析 methodID 的函数 |
| `C:getStatic(name, desc)` / `C:setStatic(name, desc, v)` | 静态字段 |
| `C:methodID(name, sig)` / `C:staticID` / `C:staticFieldID` | 手工解析并缓存 ID |

### Object

```lua
local o = C:new("()V")
local size = o:method("size", "()I")
print(size())
print(o:call("toString", "()Ljava/lang/String;"))
print(o:get("value", "I"))
o:set("value", "I", 9)
```

| 方法 | 说明 |
| --- | --- |
| `o:call(name, sig, ...)` | 调用实例方法 |
| `o:method(name, sig)` | 返回绑定函数 |
| `o:get(name, desc)` / `o:set(name, desc, v)` | 实例字段 |
| `o:class()` | 对象的类（Class） |
| `o:ref_()` | 原始 `jobject`（局部引用） |
| `tostring(o)` | 调用 `toString()` |

### Array

```lua
local a = vm:array("[I", { 1, 2, 3 })
print(a:len(), a:get(1), a:values()[3])
a:set(2, 20)
```

| 方法 | 说明 |
| --- | --- |
| `a:len()` | 长度 |
| `a:get(i)` / `a:set(i, v)` | 1 起始索引 |
| `a:values()` | 转为 Lua 表（基本类型数组） |

### 签名与类型映射

方法必须给出 JNI 描述符，例如 `"(ILjava/lang/String;)V"`。

| Java | 传入 | 返回 |
| --- | --- | --- |
| `Z` | boolean | boolean |
| `B` `S` `I` `J` | integer | integer |
| `F` `D` | number | number |
| `C` | integer 或单字节字符串 | integer（UTF-16 码元） |
| `Ljava/lang/String;` | Lua 字符串 | Lua 字符串 |
| 其它 `L...;` | Object/Class 包装、原始引用 | Object 包装 |
| `[...` | Lua 表、Array 包装、原始引用 | Array 包装 |
| `V` | — | nil |

Java 异常会转成 Lua error，消息为 `Throwable.toString()`，例如
`java.lang.IllegalStateException: boom`。每次高层调用都在独立局部引用帧内执行，返回对象会提升为调用者帧的局部引用。

## 底层 API

`require("luajni.jni")` 暴露与 `jni.h` 同名的全部 `JNIEnv` 函数，以及 JavaVM 函数。

```lua
local jni = require("luajni.jni")

local vm = jni.vm({ classpath = "build/test-classes" })
local env = vm:env()

local cls = env:FindClass("java/lang/Integer")
local mid = env:GetStaticMethodID(cls, "parseInt", "(Ljava/lang/String;)I")
print(env:CallStaticIntMethod(cls, mid, { jni.int(42) }))

env:DeleteLocalRef(cls)
```

### 函数范围

- 基础：`GetVersion`、`DefineClass`、`FindClass`、反射方法/字段转换、`GetSuperclass`、`IsAssignableFrom`
- 异常：`Throw`、`ThrowNew`、`ExceptionOccurred/Describe/Clear/Check`、`FatalError`
- 引用：`PushLocalFrame`、`PopLocalFrame`、`NewGlobalRef`、`DeleteGlobalRef`、`DeleteLocalRef`、`NewLocalRef`、`IsSameObject`、`EnsureLocalCapacity`、弱全局引用
- 对象与方法：`AllocObject`、`GetObjectClass`、`IsInstanceOf`、`GetMethodID`、`GetFieldID`、`GetStaticMethodID`、`GetStaticFieldID`
- 调用：`Call<Type>Method`、`CallStatic<Type>Method`、`CallNonvirtual<Type>Method`（`Type` = Boolean/Byte/Char/Short/Int/Long/Float/Double/Object/Void），每个都有 `A`、`V` 别名
- 字段：`Get/Set<Type>Field`、`Get/SetStatic<Type>Field`
- 字符串：`NewString`、`GetStringLength`、`GetStringChars`、`ReleaseStringChars`、`NewStringUTF`、`GetStringUTFLength`、`GetStringUTFChars`、`ReleaseStringUTFChars`、`GetStringRegion`、`GetStringUTFRegion`
- 数组：`New<Type>Array`、`Get/Release<Type>ArrayElements`、`Get/Set<Type>ArrayRegion`、`NewObjectArray`、`Get/SetObjectArrayElement`、`GetArrayLength`
- 其它：`MonitorEnter/Exit`、`RegisterNatives`、`UnregisterNatives`、`GetJavaVM`、`GetModule`（JNI 9+）、`IsVirtualThread`（JNI 21+）、`NewDirectByteBuffer`、`GetDirectBufferAddress`、`GetDirectBufferCapacity`、`GetObjectRefType`

`A` 与 `V` 别名与普通形式相同：参数位置接受 Lua 表（内部构造 `jvalue[]`），因为 `va_list` 无法跨 Lua 边界。

### 参数与类型化值

调用族的第三个参数是位置参数表：

```lua
env:CallStaticLongMethod(cls, mid, { jni.long(1099511627776) })
```

普通值按以下规则填入 `jvalue`：boolean → `z`，整数 → `j`，浮点 → `d`，字符串 → `NewStringUTF` 得到的 `jstring`，lightuserdata → `l`。当 Java 参数是 `int`、`float` 等窄类型时，应显式使用：

`jni.boolean`、`jni.byte`、`jni.char`、`jni.short`、`jni.int`、`jni.long`、`jni.float`、`jni.double`、`jni.object(ptr)`。

`jni.null` 是空对象哨兵（`lightuserdata NULL`）。传入 `nil` 也可表示 null，但表中间的 `nil` 会破坏数组长度，建议使用 `jni.null`。

### 内存

```lua
local buf = jni.buffer(64)          -- GC 管理的缓冲区
jni.write(buf, "int32", { 1, 2 })
print(jni.read(buf, "int32", 2)[1])
jni.write(buf, "utf8", "hello")
print(jni.read(buf, "utf8", 5))
```

类型：`int8` `uint8` `int16` `uint16` `int32` `uint32` `int64` `uint64` `float` `double` `pointer` `utf8`。
`Get<Type>ArrayRegion` / `Set<Type>ArrayRegion` / `GetStringRegion` / `GetStringUTFRegion` 在省略缓冲区参数时直接收发 Lua 表或字符串。

### JavaVM

底层 VM 对象提供 `env`、`attach`、`detach`、`GetEnv`、`AttachCurrentThread`、`AttachCurrentThreadAsDaemon`、`DetachCurrentThread`、`DestroyJavaVM`、`close`、`ptr`。

## 注意

- 每个进程只能创建一个 JVM；`vm:close()` 销毁后无法在同一进程重建。
- 默认单线程使用。其它线程先 `vm:attach()`，结束前 `vm:detach()`。
- 局部引用只在当前线程与调用帧内有效；循环中处理大量对象时用 `env:PushLocalFrame` / `env:PopLocalFrame`。
- `RegisterNatives` 的第三项是 C 函数指针，需要由宿主程序提供。
- JNI 调用失败会留下 pending exception；底层 API 不自动清除，使用 `luajni.check(env)` 或 `env:ExceptionCheck()`。

## 许可证

MIT
