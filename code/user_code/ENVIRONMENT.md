# Java 环境补充指南

> 这份文档规定 `code/user_code/` 下目标项目的**写法**。平台级环境在
> `src/runtime/jni/envs/`，不在这里。
>
> 核心原则：**声明即存在**。没写的东西一律不存在；查询不到返回 null 并产生
> `JniEvent`，不会自动造类/方法/字段，也不会内置任何设备值。
>
> **Java 平台方法不是项目代码。** `java.lang.*`、`java.util.*`、`java.security.*`、
> `javax.*` 这类 JDK 自带的方法（`String`、`HashMap`、`Integer`、证书链……）
> 一律补在 `src/runtime/jni/envs/`。项目 env 里出现 `java.*`/`javax.*` 的
> `declareClass`/`declareMethod` 就是放错了位置；下次遇到缺失先补平台包，
> 不在目标项目里手写。

---

## 1. 先决定放哪

| 内容 | 位置 | 规则 |
|---|---|---|
| Java 平台方法（`java.*` / `javax.*`：`java.lang.String`、`java.util.HashMap`、`java.lang.Integer`、`java.security.*` 证书链），以及以后的 `android.os.Build`、`Context` | `src/runtime/jni/envs/` | 跨目标复用；缺什么补平台包，项目里不手写；runtime 不出现目标名字 |
| 目标类、目标专属方法/字段/常量、目标 so 与 runner | `code/user_code/<case>/` | 每个目标一个项目 |
| 导入符号（libc/libandroid 的 `malloc`/`fopen` 等） | `src/runtime/loader/` | 走 thunk/虚拟模块，**不写进 env** |
| 测试专用类（如 `env/Fixture`） | `tests/fixtures/envs/` | 测试 main 里显式安装 |

判断标准：**这段行为换一个目标还成立吗？** 成立 → 平台；只对当前 so/APK 成立
→ 项目。拿不准就先放项目，等第二个目标也需要时再上提。

---

## 2. 每个文件干什么

```text
<case>/
├── <case>_env.hpp       # 只声明安装函数；项目对外的唯一入口
├── <case>_env.cpp       # 目标专属的类/方法/字段声明与 shim；项目 Java 侧的全部内容
├── main.cpp             # 唯一的接线点：create → 装环境 → 加载 libc/so → 调用
└── assets/              # 目标 so、properties、证书、配置
```

| 文件 | 放什么 | 不放什么 |
|---|---|---|
| `<case>_env.hpp` | `installEnvironment(JavaWorld&)` 的声明、必要的类型前置声明 | 实现、常量表、业务逻辑 |
| `<case>_env.cpp` | `declareClass` / `declareMethod` / `declareField`、shim lambda、目标常量 | runner、文件加载、通用框架代码 |
| `main.cpp` | `RuntimeConfig`、`installJavaEnvironment`、`loadLibrary`、`callRegisteredNative`、输出 | 大段环境声明（那些在 env.cpp） |
| `assets/` | 目标二进制与数据 | 与目标无关的测试产物 |

**没有静态注册。** `main.cpp` 必须显式调用安装函数；不写 `REGISTER_*`、
不建名字表、不改 `RuntimeConfig`。

---

## 3. 格式与模板

### 3.1 `<case>_env.hpp`

```cpp
#pragma once

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::user::<case>
{

Result<void> installEnvironment(runtime::jni::JavaWorld &world);

} // namespace duckemu::user::<case>
```

### 3.2 `<case>_env.cpp`

```cpp
#include "<case>_env.hpp"

#include "runtime/jni/world/java_world.hpp"

namespace duckemu::user::<case>
{

using runtime::jni::JavaWorld;

Result<void> installEnvironment(JavaWorld &world)
{
	// 1) 类：只声明 so 会 FindClass 的类，以及解析方法真正需要的父类。
	//    父类不知道就传 ""，不要编一个。
	auto klass = world.declareClass("com/example/Target", "");
	if (!klass.ok())
	{
		return klass.error();
	}

	// 2) native 方法：只声明签名与静态性，shim 传 {}；方法体由 so 提供，
	//    JNI_OnLoad 的 RegisterNatives 会把地址绑进来。
	auto native = world.declareMethod(klass.value(), "data", "(I)Ljava/lang/String;",
		/*is_static=*/true, {});
	if (!native.ok())
	{
		return native.error();
	}

	// 3) host 方法：最小行为。常量、字段、宿主时钟优先；复杂逻辑再写 shim。
	auto version = world.declareMethod(klass.value(), "getVersion", "()Ljava/lang/String;",
		/*is_static=*/false,
		[](JavaWorld &java, runtime::jni::InvokeContext &) -> Result<uint64_t> {
			return java.newString("16.1.50");
		});
	if (!version.ok())
	{
		return version.error();
	}

	// 4) 字段：静态/实例都要给出签名；读写走 JavaWorld。
	auto counter = world.declareField(klass.value(), "counter", "I", /*is_static=*/true);
	if (!counter.ok())
	{
		return counter.error();
	}
	return {};
}

} // namespace duckemu::user::<case>
```

### 3.3 `main.cpp`（接线）

```cpp
#include "<case>_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"

RuntimeConfig config;
config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };

auto created = RuntimeInstance::create(config);
if (!created.ok()) { /* 报错退出 */ }
RuntimeInstance &instance = *created.value();

// 唯一的接线点：create 之后、任何 loadLibrary 之前。
auto installed = instance.installJavaEnvironment(&user::<case>::installEnvironment);
if (!installed.ok()) { /* 报错退出 */ }

// 严格模式的失败要看得见。
instance.addJniListener([](const observer::JniEvent &event) {
	std::fprintf(stderr, "jni %s class=%s name=%s sig=%s slot=%u tid=%u\n",
		std::string(observer::jniEventKindName(event.kind)).c_str(),
		event.class_descriptor.c_str(), event.name.c_str(),
		event.signature.c_str(), event.slot, event.tid);
});

instance.loadLibrary(romLibc());
instance.loadLibrary(SO_PATH);

auto result = instance.callRegisteredNative(CLASS, "data", "(I)Ljava/lang/String;", { n });
if (result.ok() && result.value() != 0) {
	auto text = instance.jniStringUtf8(result.value());
	// 打印 text.value()
}
```

`installJavaEnvironment` 在**第一个模块加载之后**调用会返回
`InvalidArgument`：`JNI_OnLoad` 在 `loadLibrary` 里执行，晚装等于没装。

---

## 4. 常用 API 与行为

### 4.1 声明

| 调用 | 用途 | 注意 |
|---|---|---|
| `world.declareClass(descriptor, super)` | 声明类 | descriptor 用 `/`；`super` 未知时传 `""`；重复声明且父类一致时幂等 |
| `world.declareMethod(class, name, signature, is_static, shim)` | 声明方法 | native 方法传 `{}`；host 方法给 shim |
| `world.declareField(class, name, signature, is_static)` | 声明字段 | signature 是 `I`/`J` 这样的基本类型，或 `Ljava/lang/String;`、`[L...;` 这样的完整描述符 |

`is_static` **必须和 Java 侧一致**，因为 `RegisterNatives` 不携带静态性：

- runner 用 `callRegisteredNative`（传 jclass）→ native 声明为 `true`；
- so 里用 `CallObjectMethod` 调它 → 声明为 `false`；
- 不确定时看 Java 声明或 dex，不要猜。

### 4.2 shim 的输入与返回

```cpp
[](JavaWorld &world, InvokeContext &call) -> Result<uint64_t>
```

- `call.receiver`：实例方法是对象地址，静态方法是类对象地址。
- `call.args`：**对象参数是对象地址，不是 JNI 句柄**；基本类型是原始值。
- 返回值：基本类型直接返回；对象结果返回对象地址（`world.newString(...)`、
  `world.findClass(...)`、`call.receiver`）；void 返回 0。
- 读取字符串参数：`world.stringUtf8(call.args[0])`。
- 读取/写入字段：`world.readInstanceField(obj, field_id)`、
  `world.readStaticField(class_object, field_id)`，字段 id 来自 `declareField`。

### 4.3 结果与错误

- 声明失败（父类不存在、签名非法、重复声明）直接 `return xxx.error()`，
  让实例创建失败；不要吞掉。
- shim 里真的无法回答时返回 `Error{ ErrorCode::Unsupported, "..." }`，
  由 JNI 层记录并返回 null/0；不要编一个默认值。
- 正常路径不应该出现 `JniEvent`；出现了就是环境没补完。

### 4.4 host → guest

`RuntimeInstance::callRegisteredNative` 由 runtime 填 `JNIEnv*`（x0）和
`jclass`（x1），参数从 x2 开始，最多 6 个。因此它调用的是**静态** native。
返回值是 x0；对象结果是 JNI 句柄，用 `jniStringUtf8` 等辅助函数解析。

- **对象参数**按签名判断：`L...;` / `[...` 位置的实参是**对象地址**，
  runtime 会把它包成本地引用；基本类型按原始值传。
- **对象结果**是句柄：`jniResolveHandle(handle)` 得到对象地址，再由项目环境
  用 `JavaWorld`（`classOf` / `arrayView` / `readInstanceField` 等）读取。
  `callRegisteredNative` 为每次调用建立本地帧，只把对象结果提升回调用者帧。
- 项目需要在 install 时保存 `JavaWorld*` 才能构建参数对象（字符串、数组）。
  一个进程一个实例时这是安全的；worker 的实例进程正好满足。

---

## 5. 怎么补：严格模式驱动

1. **先什么都不加**，构建并跑目标。
2. 看 stderr：
   - `missing-class class=...` → 补类；
   - `missing-method class=... name=... sig=...` → 补方法；
   - `missing-field ...` → 补字段；
   - `unsupported-function slot=...` → 该 JNI 表项还没实现，属于 runtime 的活。
3. **只补事件里的那一个**，不要顺手声明“以后可能用到”的成员。
4. 父类/接口不知道就留空；等真实调用需要继承关系时再补。
5. `FindClass` 找不到的类不要声明；先确认 so 真的调了它。
6. 重新跑到**正常路径 `JniEvent` 计数为 0**。
7. 缺失场景单独写测试，断言事件类型与内容（不能只看返回值）。
8. 通用部分稳定后，再上提到 `src/runtime/jni/envs/`。

---

## 6. 硬规则

- 不自动造类/方法/字段；查询不到就是 null + 事件。
- **Java 平台方法一律进 `src/runtime/jni/envs/`**：`java.*` / `javax.*` 的类与
  方法（`String`、`HashMap`、`Integer`、`CertificateFactory` 证书链等）属于框架，
  缺了就补平台包；项目 env 只声明目标类、Android 对象图与设备值。
- 不内置设备值：包名、型号、证书、时间必须由项目声明。
- 不编父类、不编方法体、不为了让调用“先过”而返回假对象。
- runtime 不出现目标名字；项目不写通用框架。
- 不引入 DSL、数据文件、静态注册器；不新增 `RuntimeConfig` 必填参数。
- 目标专属资源（so、properties、证书）放项目 `assets/`，不写死在代码路径里。

---

## 7. 新项目检查清单

- [ ] `code/user_code/<case>/` 目录与 `assets/`
- [ ] `<case>_env.hpp/.cpp` 只包含目标需要的声明与 shim
- [ ] `main.cpp` 在**任何 loadLibrary 之前**调用 `installJavaEnvironment`
- [ ] 正常路径 `JniEvent` 为 0
- [ ] 缺失路径有测试并断言事件
- [ ] `<case>_env.cpp` 里没有 runner/文件加载/框架代码
- [ ] `runtime/jni/envs/` 没有出现目标名字
- [ ] CMake target 与运行命令写进本目录 README

---

## 8. 现有例子

`libeleven_case/` 是最小完整项目：

```text
libeleven_case/
├── libeleven_env.hpp    # installEnvironment 声明
├── libeleven_env.cpp    # ChallengeElevenFragment + data(I) 的声明
├── main.cpp             # 装环境 → 加载 ROM libc 与 assets/libeleven.so → 调 data(n)
└── assets/libeleven.so
```

```bash
cmake --build build --target duckemu_libeleven_case
./build/user_code/duckemu_libeleven_case 1 5
```

`jdgs_case/` 是带对象图、数组、变参调用和 guest 文件系统的真实目标：

```text
jdgs_case/
├── jdgs_env.hpp/.cpp    # Bridge + Android 包签名对象图；Object[] 参数与结果
├── main.cpp             # init(103) → sign(101) → 解析 [Ljava/lang/Object;
├── plugin.hpp/.cpp      # worker 适配，app id jdgs16
├── jdgs_cert.h          # 内嵌证书
└── assets/
    ├── libjdg.so
    └── root/.../base.apk   # 精简 APK，经 filesystem_roots 挂载
```

`CertificateFactory` / `Certificate.getEncoded` / `Integer` 等 Java 平台部分在
`src/runtime/jni/envs/`；项目只保留 `Signature.toByteArray` 这类 Android 侧
入口和内嵌证书字节。

```bash
cmake --build build --target duckemu_jdgs_case
./build/user_code/duckemu_jdgs_case
```

`szstone_case/` 是带设备快照与系统属性画像的真实目标：

```text
szstone_case/
├── szstone_env.hpp/.cpp   # SzSdk 的 10 个 native 与 lf 的 Android 对象图
├── szstone_codec.hpp      # runner/plugin 共用的 hex、z 参数解析
├── szstone.properties     # 抓取设备的系统属性（只放 lf 读到的键）
├── main.cpp               # 加载 libc/libszstone → 调 lf
├── plugin.hpp/.cpp        # worker 适配，app id szstone
└── assets/
    ├── libszstone.so
    ├── lf_in.bin / lf_out.bin
    └── root/              # /proc、build.prop 等设备快照
```

`CertificateFactory` / `X509Certificate` / `X500Principal` / `BigInteger` 证书链
同样来自平台包：项目只给 `Signature.toByteArray` 的 DER 字节，解析和
`getName()` / `toString(16)` 的渲染由 `src/runtime/jni/envs/` 完成。

```bash
cmake --build build --target duckemu_szstone_case
./build/user_code/duckemu_szstone_case
# 注入时钟 + 随机种子后逐字节可复现
./build/user_code/duckemu_szstone_case --random-seed 7 \
  --wall-time-ns 1792886400000000000 --monotonic-time-ns 1000000000
```

`dy7_case/` 是 native 走类层级 + Java 回调的目标：

```text
dy7_case/
├── dy7_env.hpp/.cpp       # MS -> MS0 -> k3、k3 的 native 与 null 回调、Context
├── dy7_case.hpp/.cpp      # metasec 初始化序列 + god7 调用
├── main.cpp               # 加载 libc/libmetasec_ml → 初始化 → 签名
├── plugin.hpp/.cpp        # worker 适配，app id dy7
├── dy7.properties         # Mi 8 / API 32 属性画像
└── assets/libmetasec_ml.so
```

`String.getBytes`、`System.getProperty`、`Thread`、`Boolean`/`Long`/`Integer`
的 boxing 全部在平台包；项目只声明 `com/bytedance/mobsec/metasec/ml/MS` 这一族
类层级和 `Context` 的设备值。native 沿 `FindClass`/`GetSuperclass` 走到
`ms/bd/c/k3` 再 `RegisterNatives`，所以层级必须用 `declareClass(desc, super)`
按 k3 → MS0 → MS 的顺序建立。

```bash
cmake --build build --target duckemu_dy7_case
DY_WALL_TIME_MS=1792886400000 ./build/user_code/duckemu_dy7_case --print-headers
```
