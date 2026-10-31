# dy7_case

抖音 40.8.0（`com.ss.android.ugc.aweme`）`libmetasec_ml.so` 的七神签名调用。
从 `DuckDroid/code/user_code/dy_7` 迁入当前项目：行为、常量与补的 Java 环境
保持一致，接线改成 `installEnvironment` + `AppPlugin`（app id `dy7`）。

## 目录

```text
dy7_case/
├── dy7_env.hpp/.cpp        # MS -> MS0 -> k3 层级、k3 的三个入口、Context
├── dy7_case.hpp/.cpp       # metasec 初始化序列 + god7 调用
├── main.cpp                # 本地 runner
├── plugin.hpp/.cpp         # worker 适配，app id dy7
├── dy7.properties          # Mi 8 / Android 12 (API 32) 属性画像
└── assets/libmetasec_ml.so # 目标 SO（从 APK 的 lib/arm64-v8a 提取）
```

## 运行

```bash
cmake --build build --target duckemu_dy7_case
./build/user_code/duckemu_dy7_case
# 注入墙钟后 X-Khronos / X-Argus 与参考 runner 逐字节一致
DY_WALL_TIME_MS=1792886400000 ./build/user_code/duckemu_dy7_case --print-headers
# 钉住随机源，跨进程输出相同
./build/user_code/duckemu_dy7_case --wall-time-ns 1792886400000000000 \
  --monotonic-time-ns 9000000000 --random-seed 7
```

成功时输出 native 返回的 CRLF 头块，顺序固定为：

```text
X-Perseus / X-Medusa / X-Argus / X-Gorgon / X-Helios / X-Ladon / X-Khronos
```

worker / system 侧的 app id 是 `dy7`：

```bash
./build/system/duckemu_system \
  --app id=dy7,library=$PWD/code/user_code/dy7_case/assets/libmetasec_ml.so,rom-libc=$ROM/libc.so,property-file=$PWD/code/user_code/dy7_case/dy7.properties
```

## 调用路径

40.8.0 的真机签名入口不再是 JNI 分发器的 `0x2000006`（dex 里已无调用点），
而是 `libmetasec_ml.so` 内部的一个 C 函数：

```c
char *god7(const char *url, const char *headers);
// libmetasec_ml.so + 0x2e01c0
// headers: "key\r\nvalue\r\n..."，返回同样格式的 7 组头
```

`Case::init` 先复现 metasec 的初始化，`Case::sign` 再调用它：

```text
0x1000003  set Context
0x1000001  decode SDK version
0x4000001  register config JSON
0x4000002  create handle
0x200000f  config center
0x2000002  set device id
0x2000003  set install id
```

这些调用走 `ms/bd/c/k3.a`，签名
`(IIJLjava/lang/String;Ljava/lang/Object;)Ljava/lang/Object;`；native 入口由
`JNI_OnLoad` 里的 `RegisterNatives` 注册。Java 侧分两层：

- `code/src/runtime/jni/envs/java_lang.cpp`：`String.getBytes`、
  `System.getProperty`、`Thread`（含 `getStackTrace` 的空数组）、
  `Boolean` / `Long` / `Integer` 的 boxing。
- 本项目 `dy7_env.cpp`：`MS`、`MS0`、`k3` 的层级与 native 声明，
  `k3.b` 的 null 回调，`Context.getPackageName/getApplicationContext/getFilesDir`。

## 可注入项

| 环境变量 | 作用 | 默认 |
| --- | --- | --- |
| `DY_REPEAT` | 连续签名次数 | `1` |
| `DY_WALL_TIME_MS` | 固定墙钟（毫秒），每次 Sign 前注入；不设置则用宿主时间 | 宿主墙钟 |
| `DY_URL` | 请求 URL（`god7` 第 1 个参数） | 内置 40.8.0 日志 URL |
| `DY_HEADERS` | 整段头串（`key\r\nvalue\r\n...`），设置后优先于 `DY_COOKIE` | 未设置 |
| `DY_COOKIE` | 没有 `DY_HEADERS` 时拼成 `cookie\r\n<value>\r\n` | 内置 cookie |
| `DY_PROPS_FILE` | 回放整份 `getprop`（支持 `key=value` 与 `[key]: [value]`） | `dy7.properties` |
| `DY_PRINT_HEADERS` | 额外按名字打印每个头 | 未设置 |
| `DY_JNI_EVENTS` | 把严格 Java 世界的缺失事件逐条打到 stderr | 未设置 |

runner 另有命令行参数 `--repeat N`、`--wall-time-ns N`、`--monotonic-time-ns N`、
`--random-seed N`、`--print-headers`、`--trace DIR`。

`X-Khronos` 是秒级时间戳，同一墙钟下 `X-Argus` / `X-Khronos` 与参考 runner
逐字节一致；其余头含随机/环境因子，不要求和抓包一致。`X-Perseus` /
`X-Gorgon` / `X-Ladon` 即使钉住获取随机源的顺序也不是逐字节可复现的：
metasec 自己的 worker 线程会并发取随机数。

## 属性画像

`dy7.properties` 用的是参考 runner 的 Mi 8 画像：

```text
ro.build.version.sdk=32
ro.build.version.release=12
ro.product.model=MI 8
ro.product.brand=Xiaomi
ro.product.manufacturer=Xiaomi
ro.product.name=dipper
ro.product.device=dipper
ro.build.fingerprint=Xiaomi/dipper/dipper:8.1.0/OPM1.171019.011/V9.5.5.0.OEAMIFA:user/release-keys
```

换设备时和 `dy7_case.cpp` 开头的 `kDeviceId` / `kInstallId` / `kDefaultUrl` /
`kDefaultCookie` 一起换。

## 已知事件与差距

- `GetStaticMethodID(MS, "a", "()V")` 在严格模式下会记录一次
  `missing-method`：APK 里确实没有这个方法（`MS` 空，`MS0` 只有
  `Bill/Louis/Zeoy/Francies`，`k3` 只有长签名的 `a/c/b`），真机上同样会抛
  `NoSuchMethodError`；native 探测后继续，不影响输出。gate 只允许这一条事件。
  默认日志级别是 Error，stderr 不输出；需要看时设 `DY_JNI_EVENTS=1`。
- `tgkill` / `nanosleep` / `ioctl(TCGETS)` 已按 DuckDroid 语义实现
  （信号返回 0、睡眠推进实例时钟、ioctl 对非 tty 返回 `ENOTTY`），见
  `docs/devlogs/2026-10-31-32-抖音错误面清理.md`。
- 全模块指令级 trace 一次签名约 3.6 GB `code.log` + 3.9 GB `rw.log`，
  跑 `--trace` 时注意目标盘容量。

## 换版本

APK 版本没变时 `kSignOffset` 和 config blob 不用动；版本变了要用真机 hook
打印的 `libmetasec_ml.so + 0x...` 更新偏移，并重新确认 `kConfigCenter` /
`kConfigBlob`。
