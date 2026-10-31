# DuckDroid2

A C++ user-mode Android ARM64 emulation runtime (an unidbg-style reimplementation).

用 C++ 重写的 Android ARM64 用户态模拟执行运行时：在 x86_64 Linux 上直接运行目标 `.so`，
按 Linux arm64 ABI 补齐 syscall、bionic TLS、线程/futex、VFS、属性区与 JNI 边界。

> 代码里的目标前缀仍是 `duckemu_*`（历史命名），对外名称统一为 DuckDroid2。
>
> 目标：让真实的 Android native 签名 so 稳定跑起来，输出与真机/参考实现一致，并能作为
> 多实例服务对外提供。与 [unidbg](https://github.com/zhkl0228/unidbg) 思路同源，但实现是
> 独立的 C++ 重写，不共享代码。

**当前状态（2026-10-30）：M0–M8 全部通过；生产构建 47/47 tests，分析构建
（`-DDUCKEMU_ENABLE_TRACE=ON`）50/50。** 三个真实目标已接入：libeleven、
京东 16.1.50、得物 6.1.5.10。System → Worker → RuntimeInstance 的服务链路跑通，
并附带一个最小的 FastAPI HTTP 案例；分析面有指令级 trace 与绝对地址 hook。
路线图见 [docs/里程碑.md](docs/里程碑.md)。

---

## 它解决什么问题

Android 的 native 代码（签名、加固、风控算法等）以 `.so` 形式存在，依赖 bionic、
Linux 内核、JNI 与 ART。要脱离真机做分析或批量调用，通常有三条路：

1. 逆成伪代码重写 —— 成本高，行为容易失真；
2. 嵌一个真 Android/ART —— 重、慢、生命周期难管；
3. **模拟执行**：不启动 Android，只在宿主上实现目标代码所依赖的那部分语义。

DuckDroid2 走第 3 条。它加载 ROM 里的真 libc（LineageOS 21 / Android 14），补
syscall、TLS、线程/futex、VFS 与 JNI 边界；导入符号、JNI 表项、syscall 统一通过
thunk / SVC 回到宿主 C++。

和 unidbg 一样做“补环境”，不一样的地方在于：真 bionic、显式 Java 环境、进程隔离、
可服务化。两者的取舍写在
[docs/DuckDroid2与unidbg-四种取舍.md](docs/DuckDroid2与unidbg-四种取舍.md)。

## 已实现

### 执行与地址空间

- **执行后端**：Unicorn（arm64），每个 guest 线程一个 `uc_engine`，实例级执行锁
  保证同一时刻只有一个 engine 在跑 guest 代码（单核串行，保住 `LDXR/STXR` 语义）。
- **地址空间**：宿主 mmap 作为唯一权威，支持 `mmap/munmap/mprotect/MAP_FIXED/brk`，
  所有 engine 共享同一份内存视图；写代码页触发跨 engine 的 TB 失效；scudo 需要的
  TBI alias（`1<<56` / `2<<56`）映射到同一块宿主页。
- **Guest → Host 调用桥**：只读 thunk 页 + `svc #(0x8000|index)`，Linux syscall、
  host stub、JNI 表项、导入符号共用一条分发路径；嵌套 `call_guest` 带上下文
  save/restore（上限 32 层）。

### 线程

- `clone`（`CLONE_SETTLS` / `PARENT_SETTID` / `CHILD_CLEARTID`，arm64 的
  `CONFIG_CLONE_BACKWARDS` 参数顺序）、`futex WAIT/WAKE`（含 timespec 超时）、
  `SYS_exit` / `exit_group`。
- futex 驱动的阻塞完成式（`PendingSyscall`：不重放、不回退 PC）；主线程也有
  自己的宿主线程；每线程 TLS、errno 与栈隔离。

### 真 bionic 与 loader

- 从 ROM 加载 `libc/libm/libdl/libz/liblog/libc++`，处理 `DT_NEEDED`、APS2 打包
  重定位、`DT_RELR`、`PT_TLS` / TLSDESC、IFUNC/IRELATIVE；运行 `__libc_preinit`，
  `malloc` 走真 scudo，`pthread_create/join` 走真 bionic。
- **动态加载**：ld-android 的 `__loader_dlopen/android_dlopen_ext/dlsym/dlvsym/
dlerror/dlclose/dladdr/dl_iterate_phdr` 是真实实现；构造器与 IFUNC 在嵌套 guest
  帧里跑完才返回，`dl_iterate_phdr` 带真实 program header。
- **CFI shadow**：虚拟 linker 在依赖加载后调用一次 `__cfi_init`，映射 64 MiB
  零表，经过 `__cfi_slowpath` 的间接调用不再读地址 0。

### 系统调用与 VFS

- syscall handler 按子系统分文件（memory / fs / thread / event / misc），覆盖
  preinit、scudo、pthread、文件 IO、事件循环与目标实际用到的面。
- **只读根 + 可写沙箱**：ROM/宿主根只读；每个实例可挂一个宿主写目录作为覆盖层，
  支持 `O_CREAT/O_TRUNC/O_APPEND/O_EXCL`、`mkdirat/unlinkat/renameat/ftruncate/
fsync` 与注入的 uid/gid/时间戳，读操作先查沙箱再回落只读根。
- **`/proc`**：优先使用目标自带的真机快照，缺失时合成 `/proc/{version,cpuinfo,
self/status,self/cmdline,self/comm,self/maps,self/mem}` 等；`readlinkat` 支持
  `/proc/self/{exe,cwd,root,fd/N}`。
- **合成挂载**：`/dev/{null,zero,random,urandom}`（含 `rdev` 与读写模式语义）、
  `/dev/__properties__`。
- **属性区**：runtime 生成 Android 14 的三份二进制镜像（`property_info`、
  `properties_serial`、`u:object_r:<ctx>:s0`），由真 libc 在 preinit 里自己解析，
  runtime 不 interpose 任何属性符号；`--property-file` 按 build.prop 语法覆盖。
- **fd 与事件循环**：fd 表共享文件描述，支持 `dup/dup3/fcntl`；`pipe2`、
  `eventfd2`、`timerfd`、`poll/ppoll`、`pselect6`、`epoll` 由 deadline、timerfd
  压缩截止与 activity 唤醒驱动；`statfs/fstatfs` 按挂载点分类。

### 设备画像与确定性

- **`DeviceProfile`** 统一喂 `uname`、`getuid/getgid`、`prctl(PR_GET_NAME)`、
  `/proc/{version,cpuinfo,self/status}`、`stat` 的 uid/gid、`AT_HWCAP`、
  `/proc/self/exe` 与 `/data` 容量；目标按需覆盖，默认保持中性画像。
- **`RuntimeClock`** 可注入墙钟/单调钟，注入后每次读 +1 ms；
  `clock_gettime`、`gettimeofday`、`java.lang.System` 共用同一个时钟。
- **`RandomSource`** 可由 `--random-seed` 播种，`getrandom(2)`、`/dev/{u,}random`
  与 `AT_RANDOM`（进而 `__stack_chk_guard`）共用一条流。注入时钟 + 种子后，
  同一实例跨进程可逐字节复现。

### JNI

- `JNIEnv` 233 项 / `JavaVM` 8 项函数表；`RegisterNatives`、`Call*Method`、
  `Call*MethodV` 的 AAPCS64 va_list、数组（`New*Array` / `*ArrayRegion` /
  `NewObjectArray`）、对象字段描述符、`void` 返回签名。
- Java 世界严格声明：类/方法/字段未由环境声明时返回 null + `JniEvent`，不自动创建。
- `JavaBinder` 链式声明助手：首个错误记录后短路后续调用，失败时不会把 0 当作
  class/field id 继续建图；目标环境包（jdgs/szstone）已改用它。

### 服务化与 Web 案例

- **System**（每个 app 一个 UDS listener，网关/监督）→ **Worker**（按 app 的
  supervisor，不建 RuntimeInstance）→ **Instance 子进程**（`exec` 之后
  `RuntimeInstance::create`）；warm pool、会话亲和、超时 `SIGKILL`、崩溃重生。
- 目标以插件接入：`code/user_code/<case>/` + worker registry 里的 app id，
  runtime / worker / system 不随目标改动。
- **Web**：`code/src/web/` 是一个最小的 FastAPI 案例，把 System 的 UDS 帧协议包成
  `/health`、`/sign`、`/jd/sign` 三个 HTTP JSON 接口；不含鉴权、限流、连接池与任务
  队列，仅作接入样板。

### 可观测性

- `svc.log`（9 列、tab 分隔、args/note 为 JSON，可解析、可 diff 的 SVC 语义日志）、
  `MemoryEvent`、`ThreadEvent`、`ModuleEvent`、`JniEvent`、每线程 SPSC 诊断环、
  x29 帧链回溯与 `trace_viewer`。
- **指令级 trace**（`-DDUCKEMU_ENABLE_TRACE=ON`）：`code.log` / `bl.log` /
  `rw.log` 严格沿用 QBDI 四件套的列结构、行号和 JSON 约定，另加 `threads.tsv`
  记线程归属。目标 runner 用 `--trace DIR` 一个参数：`svc.log` 写 `DIR/svc.log`，
  指令 trace 写 `DIR/insn/`。trace 从被调用函数的第一条指令开始，捕获进入时的
  LR，记到 `PC == LR` 为止（QBDI nested-VM 的结尾）；hook 覆盖是目标模块，由
  代码决定，不是 CLI 的 size 参数。区间内实际执行的指令进 `code.log`；跳出
  覆盖区间的 `bl/blr` 进 `bl.log`，目标落在 thunk 页时解析成
  `JNIEnv::NewStringUTF` 这类宿主函数名。`rw.log` 的 `w` 条目在指令 POST 阶段
  落盘，hexdump 是写后内容。
- **地址 hook**（生产构建也编译）：绝对 guest 地址 + 回调；回调在指令执行前
  运行，且在 trace 的 PRE 采样之前。可读/写 X/W 寄存器、读/写/强写 guest
  内存、`setPc` 跳过、按顶层调用归零的 `hit_count`。用于观测和故障注入
  （白盒 AES/DFA 这类需要精确改寄存器或内存的场景）。
- **回放**:这个东西就没必要实现，因为你把随机和时间戳固定下来，参数一样的情况下，不可能变化的，所以没必要再做一个所谓的回放和dump运行时内存
- **hook**:只做原生的，在执行的代码更高层做hook，不做那种直接侵入执行代码的

### 真实目标

| 目标          | 入口                                                | 结果                                                                                                                                   |
| ------------- | --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| libeleven     | `ChallengeElevenFragment.data(I)Ljava/lang/String;` | 输出 `n:<epoch_ms>`；`data(1..100)` 的 `n:` 段与参考 runner 逐值一致                                                                   |
| 京东 16.1.50  | `Bridge.main(101)`                                  | 同一注入时钟下与 DuckDroid 参考 runner 逐字节一致；带 9 条目精简 APK 与 Mi 8 属性画像（SDK 32）                                        |
| 得物 6.1.5.10 | `SzSdk.lf([BZZZ)[B`                                 | 挂真机 APK 后 body 长度 3263 与抓包一致；注入时钟 + 种子后 3183 跨进程逐字节复现；端到端 `/call` HTTP 200 / `c=10000`，解出 92 字符 sk |

## 尚未实现

- **信号语义**：只有 no-op；`EINTR` 是接口预留，没有真正的信号投递。
- **TLS 卸载语义**：静态/动态 TLS、DTV 扩容与 dlsym(STT_TLS) 已按 bionic 实现；
  `dlclose` 仍是 no-op，模块表与 DTV 槽不会因卸载回收。
- **futex 面**：`WAIT_BITSET`、PI、robust list、requeue 都没有（普通 WAIT/WAKE 的
  timespec 超时已支持）。
- **目录 fd 与 `getdents64`**：`opendir/readdir` 还不可用；合成挂载的子项枚举也
  依赖它。
- **`ioctl` 与 io 变体**：按当前目标需求暂不做（审计 #6/#8 要是真写，猴年马月去了，真给Linux实现了）。
- **严格 CFI**：shadow 是零表，`__loader_cfi_fail` 被绑成返回桩，检查被跳过而不是按真机终止(这个案例非常非常少，懒得去专门维护了)。
- **回溯**：x29 帧链不是 CFI/DWARF；release so 默认 omit-frame-pointer，链会很短。
- **16 KiB 页**：只在 4 KiB 页上验证(能跑就行，以后有时间再写)。

## 架构

```text
System（每 app 一个 UDS listener，网关/监督）
└── Worker（按 app 的 supervisor，不建 RuntimeInstance）
    └── Instance 子进程（exec 之后 RuntimeInstance::create）
        ├── RuntimeMemory              地址空间唯一权威（region 表 + 宿主后备）
        ├── ThreadScheduler            guest 线程 = 宿主线程 = uc_engine
        │   ├── RuntimeThread × N
        │   ├── PendingSyscall         阻塞 syscall 的延后完成（含 deadline）
        │   └── WaitQueue              futex
        ├── UnicornBackend             实例级 engine 注册表 + 广播 + region 重放
        ├── Loader                     ELF / 重定位 / TLS / IFUNC / 真 bionic
        │   └── DlService              __loader_dl*、构造器、dl_iterate_phdr
        ├── VirtualFileSystem          只读根 + 可写沙箱 + /proc + 合成挂载
        │   ├── FdTable                共享文件描述（dup/fcntl）
        │   └── EventFiles             pipe/eventfd/timerfd/epoll
        ├── DeviceProfile              设备身份、uname、/proc、uid/gid、HWCAP
        ├── PropertyTable              三份属性区镜像（真 libc 解析）
        ├── RandomSource               实例随机流（getrandom / /dev/*random / AT_RANDOM）
        ├── JniState / JavaWorld       JNIEnv/JavaVM、类/方法/字段、JavaBinder
        ├── RuntimeClock               可注入墙钟/单调钟
        ├── ThunkTable                 guest 内 host 函数 thunk 页
        └── EventBus                   语义事件 + 诊断通道
```

关键不变量：

1. **guest 线程 = 宿主线程 = `uc_engine`**，1:1:1。
2. **同一时刻只有一个 engine 执行 guest 代码**；线程阻塞时释放执行锁。
3. **地址空间只有一份权威**（`RuntimeMemory`），所有 engine 共享并保持权限/代码缓存同步。
4. **一个 SVC 只分发一次**：阻塞的 syscall 不重放，进入时分配 `seq`、完成时发布 `id`。
5. **fork 点早于任何 engine / 宿主线程**：实例进程在 `exec` 之后才创建
   `RuntimeInstance`，不在活实例上 fork。

## 目录

```text
code/
├── src/common/              # ID、Result/Error、日志、UDS 封装
├── src/protocol/            # 4B 长度帧、JSON 消息、路由
├── src/runtime/
│   ├── instance/            # RuntimeInstance 门面
│   ├── memory/              # 地址空间、region、thunk 页
│   ├── backend/unicorn/     # engine、backend、hook
│   ├── threads/             # RuntimeThread、调度器、futex、clone、TLS
│   ├── svc/                 # dispatcher + syscall handlers（memory/fs/thread/event/misc）
│   ├── loader/              # ELF、重定位、TLS、IFUNC、bionic globals、DlService
│   ├── vfs/                 # /proc、只读根、可写沙箱、/dev、fd 表、事件文件
│   ├── props/               # property_info / 属性区镜像 / PropertyTable
│   ├── device/              # DeviceProfile
│   ├── random/              # RandomSource
│   ├── jni/                 # JNIEnv/JavaVM、JavaWorld、JavaBinder、refs、envs
│   ├── clock/               # RuntimeClock
│   └── observer/            # 语义事件、诊断环、svc.log、guest 栈回溯
├── src/worker/              # Worker supervisor、实例进程、AppPlugin
├── src/system/              # UDS 网关、WorkerPool
├── src/web/                 # FastAPI 最小 HTTP 案例
├── user_code/               # 目标项目（libeleven_case、jdgs_case、szstone_case、dy7_case）与注册表
├── tests/                   # integration + unit（49 个）
├── tools/spike/             # M0 的 Unicorn 九条探针
├── tools/trace_viewer/      # svc.log 查看器
└── tools/sign_client/       # UDS 客户端与压测
docs/
├── 里程碑.md                # 执行期唯一权威路线图（开工先读）
├── devlogs/                 # 设计与评审记录（每个里程碑/审计项一份）
├── DuckDroid2与unidbg-四种取舍.md
└── 日记.md、目录说明.md
```

## 构建与测试

要求：

- Linux x86_64（当前只在 Manjaro / GCC 16 上验证）
- CMake ≥ 3.20、Ninja、C++20 编译器
- `clang` 与 `llvm-objcopy`（把 arm64 汇编测试固件编成扁平 blob）
- `nlohmann_json` ≥ 3.11（UDS 线格式）
- 仓库子模块：Unicorn（只启用 aarch64）；也可用
  `-DDUCKEMU_USE_SYSTEM_UNICORN=ON` 链接系统 Unicorn
- 真 bionic / JNI / 服务 gate 需要本地 ROM 与样本；默认路径在
  `code/tests/CMakeLists.txt` 的 `DUCKEMU_*` cache 变量里，可用
  `-DDUCKEMU_ANDROID_ENV_ROOT=...` 改根目录
- Web 案例额外需要 Python 3、FastAPI、uvicorn（可选）
- 指令级 trace 额外需要 capstone >= 5（仅 `-DDUCKEMU_ENABLE_TRACE=ON` 时）

```bash
git clone --recursive <repo-url> DuckDroid2
cd DuckDroid2

cmake -S code -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

可选：

```bash
# 严格构建
cmake -S code -B build-werror -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDUCKEMU_WARNINGS_AS_ERRORS=ON

# ASan + UBSan
cmake -S code -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DDUCKEMU_SANITIZE_ADDRESS=ON -DDUCKEMU_SANITIZE_UNDEFINED=ON

# 查看 svc trace
./build/trace_viewer summary build/tests/traces/02_pure_native.svc.log

# 分析构建：指令级 trace（生产构建不链接 capstone）
cmake -S code -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDUCKEMU_ENABLE_TRACE=ON
cmake --build build-trace -j"$(nproc)"

# libeleven：从函数入口记到进入时的 LR
./build-trace/user_code/duckemu_libeleven_case --trace /tmp/le_insn 1
# 京东：Bridge.main 是跳板，hook 覆盖目标模块，结尾同样是 LR
./build-trace/user_code/duckemu_jdgs_case --trace ./trace_logs/jd

# 产出 DIR/svc.log + DIR/insn/{code,bl,rw}.log + threads.tsv
# 一次京东 main(101) 实测：code.log 1,171,888 行 / 129 MiB，
# rw.log 2,155,668 行 / 132 MiB，合计约 261 MiB（约 5.2 s）。
# trace_logs/ 已在 .gitignore 中。
```

当前 47 个测试（生产构建；`DUCKEMU_ENABLE_TRACE=ON` 为 50），按阶段分组：

- 探针：`m0_probes`
- 裸 blob：`01_tls_read`、`02_pure_native`、`02_stack_dump`、`03_pthread_join`
- loader / bionic：`04_so_load`、`04b_libc_map`、`04b_tlsdesc`、`04c_globals`、
  `04c_preinit`、`04c_vfs`、`04c_syscalls`、`04c_bionic`
- 运行时面：`04d_dlopen`、`04e_dup_fcntl`、`04f_vfs_write`、`04g_event_loop`、
  `04h_device_profile`、`04i_properties`、`04j_dev_nodes`、`04k_readlink`、
  `04l_random_seed`、`04m_statfs`
- JNI：`05_jni_env`、`05_jni_stub`、`05_jni_arrays`、`05_jni_real`
- 时钟：`06_clock_injection`
- 服务化：`07a_instance_host`、`07b_worker_supervisor`、`07c_system_gateway`、
  `07d_session_reset`、`07_end_to_end`、`07e_jdgs_plugin`、`07f_szstone_plugin`、
  `07g_dy7_plugin`
- 分析面：`08_address_hook`；TRACE=ON 另有 `08_insn_trace`、
  `08_insn_trace_threads`、`insn_decode_test`
- 单元：`elf_image_test`、`runtime_memory_test`、`ring_buffer_test`、
  `guest_stack_test`、`svc_trace_format_test`、`protocol_test`、`jni_world_test`、
  `jni_layout_test`、`runtime_clock_test`、`module_missing_dep_test`

### 跑真实目标

```bash
# libeleven：输出 n:<epoch_ms>
./build/user_code/duckemu_libeleven_case 1 5
# 确定性回放
./build/user_code/duckemu_libeleven_case --wall-time-ns 1792886400000000000 1 5

# 京东 16.1.50：需要精简 APK（APK 不入库，.gitignore 排除 *.apk）
./build/user_code/duckemu_jdgs_case

# 得物 6.1.5.10：设备快照 + 属性画像，注入时钟 + 种子后逐字节复现
./build/user_code/duckemu_szstone_case --random-seed 7 \
  --wall-time-ns 1792886400000000000 --monotonic-time-ns 1000000000

# 抖音 40.8.0：七组头；同一墙钟下 X-Argus / X-Khronos 与参考 runner 一致
DY_WALL_TIME_MS=1792886400000 ./build/user_code/duckemu_dy7_case --print-headers
```

各目标自己的运行参数、输入面与对拍方法见
[`code/user_code/README.md`](code/user_code/README.md) 及各自 case 下的 README。

### 跑服务

```bash
ROM=/path/to/ROM/out/target/product/lemonade/symbols/system/lib64
SO=$PWD/code/user_code/libeleven_case/assets/libeleven.so

./build/duckemu_system \
  --socket-dir /tmp/duckemu \
  --worker-exec "$PWD/build/duckemu_worker" \
  --wall-time-ns 1792886400000000000 \
  --app "id=libeleven,library=$SO,rom-libc=$ROM/bootstrap/libc.so,library-path=$ROM/bootstrap:$ROM,instances=2"

python3 code/tools/sign_client/sign_client.py /tmp/duckemu/libeleven.sock 7
```

app 规格还支持 `filesystem-write-root=`、`property-file=`、`random-seed=`、
`workers=`；worker/system 会把它拆成对应子进程参数。

### 跑 Web 案例

先按上面的方式启动 `duckemu_system`，再：

```bash
cd code/src/web
python3 -m uvicorn app.main:app --host 127.0.0.1 --port 8000
```

配置来自环境变量：`DUCKEMU_APP`（默认 `jdgs16`）、`DUCKEMU_SOCKET_DIR`
（默认 `/tmp/duckemu-jdgs`）、`DUCKEMU_SOCKET`（显式指定 socket 时优先）、
`DUCKEMU_DEADLINE_MS`、`DUCKEMU_CONNECT_TIMEOUT_S`。接口见
[`code/src/web/README.md`](code/src/web/README.md)；这是接入样板，不是生产服务。

## 性能基线

只在本机测过，且**没有和 unidbg 做同目标对拍**：

| 目标         | 配置                                            | 结果                                   |
| ------------ | ----------------------------------------------- | -------------------------------------- |
| libeleven    | 1 Worker × 4 实例，8 客户端，10000 次 `data(7)` | 8176 req/s，p50 0.622 ms，p99 1.322 ms |
| 京东 16.1.50 | 2 实例 / 2 客户端                               | 约 23 req/s，平均 87 ms                |
| 京东 16.1.50 | 4 实例 / 8 客户端                               | 约 39 req/s，平均 201 ms               |

libeleven 很简单（基本是魔改 base64），这组数字只说明链路通；京东单次签名约
100 ms guest CPU，单实例串行。得物尚未做吞吐基线。

## 路线图

| 里程碑           | 内容                                      | Gate                                                                            | 状态 |
| ---------------- | ----------------------------------------- | ------------------------------------------------------------------------------- | ---- |
| M0 探针          | 用源码级事实钉死 Unicorn 语义（九条探针） | 九条全有实测数据                                                                | ✅   |
| M1 最小闭环      | 裸 blob、TLS 正确、SVC 可分发             | `01_tls_read`、`02_pure_native`                                                 | ✅   |
| M2 可观测性      | svc trace、内存事件、帧链 dump            | `02_pure_native`、`02_stack_dump`                                               | ✅   |
| M3 线程          | clone/futex/阻塞完成式、每线程 engine     | `03_pthread_join`                                                               | ✅   |
| M4 loader        | ELF 加载、重定位、依赖递归                | `04_so_load`                                                                    | ✅   |
| M4.5 loader 补全 | APS2/RELR、TLS/TLSDESC、IFUNC、真依赖     | `04b_libc_map`                                                                  | ✅   |
| M4.6 真 bionic   | preinit/scudo、VFS/`/proc`、真 pthread    | `04c_bionic`                                                                    | ✅   |
| M5 JNI           | JNIEnv、`RegisterNatives`、严格 Java 环境 | `05_jni_stub`                                                                   | ✅   |
| M6 真实目标      | libeleven / 京东 / 得物 / 抖音结果一致    | `06_clock_injection`、`07e_jdgs_plugin`、`07f_szstone_plugin`、`07g_dy7_plugin` | ✅   |
| M7 服务化        | System/Worker/Instance、UDS、warm pool    | `07_end_to_end`                                                                 | ✅   |
| M8 分析面        | 指令级 trace（LR 结尾）、绝对地址 hook    | `insn_decode_test`、`08_insn_trace`、`08_insn_trace_threads`、`08_address_hook` | ✅   |

执行规则：一个里程碑 = 一个可 review 的 PR；gate 是测试文件，不是口头结论；
不提前实现下一阶段的抽象。M6 的 reset 语义由 `07d_session_reset` 覆盖
（回收 = 换实例进程）。

M7 之后的工作按“下一个真实目标缺什么补什么”推进。2026-10-30 已完成：
动态 `dlopen` 与 fd 基础、可写 VFS 沙箱与事件循环、`DeviceProfile`、属性区、
`/dev` 字符节点、审计 #3/#5/#7（readlinkat/RNG/statfs）、得物接入与 CFI
shadow。2026-10-31 接入抖音（futex bitset、`GetObjectRefType`、4 MiB 栈）。
已知最大缺口是目录 fd + `getdents64`；下一个目标（快手）接入时按实际踩到的面补。

## 设计文档

- [docs/里程碑.md](docs/里程碑.md) —— 唯一权威路线图与当前状态
- [docs/devlogs/](docs/devlogs/) —— 每个阶段的评审结论、设计、实测结果
  - [架构评审结论](docs/devlogs/2026-10-27-01-架构评审结论.md)
  - [调用桥与线程模型决策](docs/devlogs/2026-10-27-02-调用桥与线程模型决策.md)
  - [M0 探针结果](docs/devlogs/2026-10-27-03-M0探针结果.md)
  - [M1 TLS 布局与门禁修正](docs/devlogs/2026-10-28-04-M1-TLS布局与门禁修正.md) /
    [M1 结果](docs/devlogs/2026-10-28-05-M1结果.md)
  - [M2 设计](docs/devlogs/2026-10-28-06-M2设计.md) / [M2 结果](docs/devlogs/2026-10-28-07-M2结果.md)
  - [M3 设计](docs/devlogs/2026-10-28-08-M3设计.md) / [M3 结果](docs/devlogs/2026-10-28-09-M3结果.md)
  - [M4 设计](docs/devlogs/2026-10-28-10-M4设计.md) / [M4 结果](docs/devlogs/2026-10-28-11-M4结果.md)
  - [路线修正：真 bionic](docs/devlogs/2026-10-28-12-路线修正-真bionic.md)
  - [M4.5 设计](docs/devlogs/2026-10-28-13-M4.5设计.md) / [M4.5 结果](docs/devlogs/2026-10-28-14-M4.5结果.md)
  - [M4.6 设计](docs/devlogs/2026-10-28-15-M4.6设计.md)
  - [M5 设计](docs/devlogs/2026-10-29-16-M5设计.md)
  - [M6 进度：libeleven 调用](docs/devlogs/2026-10-29-17-M6进度-libeleven调用.md)
  - [M6 时钟注入](docs/devlogs/2026-10-29-18-M6时钟注入.md)
  - [M7 设计](docs/devlogs/2026-10-29-19-M7设计-进程模型与网关.md) / [M7 结果](docs/devlogs/2026-10-29-20-M7结果.md)
  - [京东 16.1.50 接入](docs/devlogs/2026-10-29-20-JD接入与JNI面补全.md)
  - [动态 dlopen 与 fd 基础](docs/devlogs/2026-10-30-21-动态dlopen与fd基础.md)
  - [可写沙箱与事件循环](docs/devlogs/2026-10-30-22-可写沙箱与事件循环.md)
  - [DeviceProfile](docs/devlogs/2026-10-30-23-DeviceProfile.md)
  - [属性区](docs/devlogs/2026-10-30-24-属性区.md)
  - [dev 节点](docs/devlogs/2026-10-30-25-dev节点.md)
  - [审计 #3/#5/#7](docs/devlogs/2026-10-30-26-审计3-5-7.md)
  - [得物 szstone 接入与 CFI](docs/devlogs/2026-10-30-27-得物szstone接入与CFI.md)
  - [抖音 dy7 接入](docs/devlogs/2026-10-31-31-抖音dy7接入.md)
- [DuckDroid2 与 unidbg 的四种取舍](docs/DuckDroid2与unidbg-四种取舍.md)
- [docs/目录说明.md](docs/目录说明.md) —— 目标目录结构与分层约束
- [code/user_code/ENVIRONMENT.md](code/user_code/ENVIRONMENT.md) —— 目标项目的 Java 环境写法

## 已知限制

- 单核串行：父线程在 guest 里死循环会饿死子线程；没有抢占，生产超时靠
  System/Worker 对实例进程 `SIGKILL`。
- Unicorn 的 `timeout` 参数已弃用（其超时线程与下一轮运行存在竞态）；
  DuckDroid2 统一传 `timeout = 0`。
- 绑定 ROM：`libc_shared_globals` 偏移、静态 TLS 布局、clone 语义都跟当前
  LineageOS 21 / `lemonade` 走；换 ROM 要重新核对。
- 只验证了 Linux x86_64 宿主 + arm64 guest；16 KiB 页设备未覆盖。
- 真实目标只有 libeleven、京东 16.1.50 与得物 6.1.5.10 三个；libeleven 很简单，
  京东单次约 100 ms guest CPU，性能基线不代表复杂 so。
- 京东的精简 APK 与得物真机 APK 不入库（`.gitignore` 排除 `*.apk`），新克隆需
  自行放置；得物的设备快照与属性画像在 `code/user_code/szstone_case/`。
- 没有和 unidbg 做同目标、同输入、同机器的性能对拍。

## 致谢

- [Unicorn Engine](https://github.com/unicorn-engine/unicorn)（含 QEMU TCG）——
  当前的 ARM64 执行后端。
- [unidbg](https://github.com/zhkl0228/unidbg) —— 补环境思路的先行者与参照。
- AOSP bionic —— TLS 布局、线程语义、属性区与 `libc_shared_globals` 的来源。
- LineageOS / Android 14 ROM —— 真 libc 与系统库样本。
- QBDI trace 工具链 —— `svc.log` 继承了其文本格式的约定（tab 分隔、`0x` 十六进制、
  JSON 列），但索引体系不同。
- [nlohmann/json](https://github.com/nlohmann/json) —— UDS 线格式。
- [FastAPI](https://fastapi.tiangolo.com/) —— Web 案例的 HTTP 封装。

## 许可证

Copyright (C) 2026 DuckDroid2 contributors.

DuckDroid2 以 **GPL-2.0-only** 发布，完整文本见 [LICENSE](LICENSE)。

默认构建会静态链接 Unicorn Engine（GPL-2.0-only），因此分发包含 Unicorn 的二进制时，
整体按 GPLv2 处理。第三方依赖清单见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
`code/third_party/dynarmic` 已作为可选子模块预留，当前构建不包含。

新增源文件建议在文件头加 `SPDX-License-Identifier: GPL-2.0-only`。
