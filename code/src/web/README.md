# DuckDroid2 Web API

把 **DuckDroid2** 的签名能力包成 FastAPI，并在签名之上直接提供抖音数据接口：
搜索、视频详情、评论、用户主页、用户作品。附一个只用标准库的 Python 客户端，
调用 FastAPI 拿结构化数据。

```text
client.py ──HTTP──> FastAPI ──> DouyinService ──UDS 帧──> DuckDroid2 System
                                     │                         │
                                     │                         └─ Worker(dy7)
                                     │                              └─ libmetasec_ml.so
                                     └──httpx──> aweme.snssdk.com
```

分工沿用 DuckDroid2 的进程模型：System 只路由 UDS 帧，connections 本身决定
app；Web API 只做参数搬运、错误码映射和业务数据提取。签名仍然在真实的
`libmetasec_ml.so`（抖音 40.8.0）里执行，没有任何算法重写。

## 1. 前置条件

- DuckDroid2 已构建，`build/duckemu_system` 与 `build/duckemu_worker` 存在；
- ROM 的 `bootstrap/libc.so` 存在；
- 本仓库用 [uv](https://docs.astral.sh/uv/) 管理依赖。

默认路径按本机目录布局：

```text
NewCode/Android/environment/
├── DuckDroid2/
├── ROM/
└── DuckDroid2WebAPI/     # 本仓库
```

## 2. 启动顺序

### 2.1 启动 DuckDroid2 System（dy7 签名通道）

```bash
./scripts/start_dy7_system.sh
```

脚本会创建 `/tmp/duckemu-dy7/dy7.sock`。等价的手工命令：

```bash
DUCKEMU=../DuckDroid2
ROM=../ROM/out/target/product/lemonade/symbols
$DUCKEMU/build/duckemu_system \
  --socket-dir /tmp/duckemu-dy7 \
  --worker-exec $DUCKEMU/build/duckemu_worker \
  --app "id=dy7,\
library=$DUCKEMU/code/user_code/dy7_case/assets/libmetasec_ml.so,\
rom-libc=$ROM/system/lib64/bootstrap/libc.so,\
property-file=$DUCKEMU/code/user_code/dy7_case/dy7.properties,\
library-path=$ROM/system/lib64:$ROM/system/lib64/bootstrap,\
instances=1,workers=1,init-timeout-us=120000000,invoke-timeout-us=120000000"
```

首次请求会等待实例预热（约 9 s）；预热完成后单次签名约 **0.1 s**。

### 2.2 启动 FastAPI

```bash
uv run python main.py
# 或
uv run uvicorn app.main:app --host 127.0.0.1 --port 8000
```

- 交互文档：<http://127.0.0.1:8000/docs>
- 健康检查：`curl http://127.0.0.1:8000/health`

## 3. Python 客户端

```bash
# 健康检查
uv run python client.py health

# 搜索
uv run python client.py search 鸭子 --count 5

# 视频详情
uv run python client.py video 7637479447470211042

# 评论
uv run python client.py comments 7637479447470211042 --count 10

# 用户主页 / 用户作品
uv run python client.py user MS4wLjABAAAAxTaTx3G7QVOYqKppZ6qwCWcuUgAa21w-bhJHoXp4gcA
uv run python client.py posts MS4wLjABAAAAxTaTx3G7QVOYqKppZ6qwCWcuUgAa21w-bhJHoXp4gcA --count 10

# 一条龙演示，并把结果存成 JSON
uv run python client.py demo --keyword 鸭子 --count 10 --save ./data

# 只签名
uv run python client.py sign 'https://aweme.snssdk.com/aweme/v1/feed/?aid=1128&...'
```

客户端也可以用在自己的代码里：

```python
from client import DuckApi

api = DuckApi("http://127.0.0.1:8000")
print(api.search("鸭子", count=5)["data"][0]["desc"])
print(api.comments("7637479447470211042")["data"][:3])
```

## 4. HTTP 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/health` | System socket 是否可达 |
| POST | `/sign` | 通用转发：`params` 原样交给 System |
| POST | `/jd/sign` | 京东签名示例（需要 `DUCKEMU_APP=jdgs16`） |
| POST | `/dy/sign` | 只签名：`url` → 七组头 |
| POST | `/dy/search` | 搜索：`keyword`、`offset`、`count` |
| GET | `/dy/video/{aweme_id}` | 视频详情 |
| GET | `/dy/comments/{aweme_id}` | 评论：`cursor`、`count` |
| GET | `/dy/user/{sec_uid}` | 用户主页 |
| GET | `/dy/user/{sec_uid}/posts` | 用户作品：`max_cursor`、`count` |

数据接口统一返回：

```json
{
  "ok": true,
  "app": "dy7",
  "session": "dy7-w0-g0-i0-g0",
  "sign_meta": {"queue_us": 0, "invoke_us": 120000, "init_us": 0},
  "data": ["..."],
  "pagination": {"cursor": 20, "has_more": true, "total": 298}
}
```

`/dy/sign` 返回：

```json
{
  "ok": true,
  "app": "dy7",
  "session": "dy7-w0-g0-i0-g0",
  "headers": {"X-Argus": "...", "X-Gorgon": "...", "...": "..."},
  "signature": "X-Perseus\r\n...",
  "meta": {"invoke_us": 118000}
}
```

所有数据接口都支持 `session` 参数（固定到同一实例）和 `raw=true`（附带抖音
原始 JSON；体积可能较大）。错误统一为：

```json
{"ok": false, "error": {"code": "DOUYIN_STATUS", "message": "..."}}
```

## 5. 配置

全部通过环境变量，默认值与 DuckDroid2 `dy7_case` 的 Mi 8 / Android 12 画像
一致。换设备时同时改这里和 `dy7.properties` / `dy7_case.cpp`。

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `DUCKEMU_APP` | `dy7` | System listener 对应的 app |
| `DUCKEMU_SOCKET_DIR` | `/tmp/duckemu-dy7` | socket 目录 |
| `DUCKEMU_SOCKET` | 由 app + 目录拼出 | 直接指定 socket 路径 |
| `DUCKEMU_DEADLINE_MS` | `120000` | 一次签名允许的重试总时长 |
| `DOUYIN_BASE_URL` | `https://aweme.snssdk.com` | 接口域名 |
| `DOUYIN_DEVICE_ID` | `3951009026851867` | 设备 id |
| `DOUYIN_INSTALL_ID` | `3489221505207928` | 安装 id / `iid` |
| `DOUYIN_AID` | `1128` | 抖音 aid |
| `DOUYIN_VERSION_CODE` | `400800` | 版本号 |
| `DOUYIN_VERSION_NAME` | `40.8.0` | 版本名 |
| `DOUYIN_DEVICE_TYPE` | `MI 8` | 机型 |
| `DOUYIN_DEVICE_BRAND` | `Xiaomi` | 品牌 |
| `DOUYIN_CHANNEL` | `huawei_1128_64` | 渠道 |
| `DOUYIN_OS_API` / `DOUYIN_OS_VERSION` | `32` / `12` | Android API / 版本 |
| `DOUYIN_RESOLUTION` / `DOUYIN_DPI` | `1080*2248` / `440` | 屏幕 |
| `DOUYIN_CDID` | `c10a83da-...` | 设备指纹 id |
| `DOUYIN_COOKIE` | dy7 内置 cookie | 签名与 HTTP 共用 |
| `DOUYIN_HEADERS` | 未设置 | 整段 header blob，优先于 cookie |
| `DOUYIN_USER_AGENT` | 40.8.0 MI 8 UA | 请求 UA |
| `DOUYIN_TIMEOUT_S` | `20` | 抖音 HTTP 超时 |
| `DOUYIN_VERIFY_TLS` | `false` | 是否校验证书 |
| `DUCKAPI_HOST` / `DUCKAPI_PORT` | `127.0.0.1` / `8000` | `main.py` 监听地址 |

## 6. 签名口径

dy7 调用的是 `libmetasec_ml.so + 0x2e01c0` 的 `god7(url, headers)`：

- **URL**：必须包含最终 query，签名和真正发出的 URL 是同一个字符串；
  `aid=1128` 必须在，否则只会得到 `X-Neptune`。
- **headers**：整段 `key\r\nvalue\r\n...`；不传时插件用
  `cookie\r\n<cookie>\r\n`。本项目默认传 cookie，并让 HTTP 请求带同一份
  cookie。
- **POST body**：不进签名，只影响 `X-SS-STUB`（body 的 MD5 大写）。
- 七组头：`X-Perseus / X-Medusa / X-Argus / X-Gorgon / X-Helios / X-Ladon /
  X-Khronos`。`X-Khronos` 是秒级时间戳；其余含随机/环境因子。

## 7. 已验证（2026-10-31）

用本机 DuckDroid2 System（app id `dy7`）与真实抖音接口跑通：

- 视频详情 `/aweme/v1/aweme/detail/` → `status_code=0`，返回 53 KB；
- 评论 `/aweme/v2/comment/list/` → 20 条，`total=298`；
- 用户主页 `/aweme/v1/user/profile/other/` → 完整 user；
- 用户作品 `/aweme/v1/aweme/post/` → 12 条，`has_more=1`；
- 搜索 `/aweme/v2/search_feed/` → 6 条，`has_more=1`；
- 预热后单次签名约 0.1 s，冷启动实例约 9 s。

## 8. 边界

- 一个 Web API 进程对应一个 app/socket，和 DuckDroid2「连接决定 app」的
  设计一致；要同时服务京东/抖音，就起多个实例或再加一层路由。
- 鉴权、限流、队列、指标、面板不在本仓库；生产部署请放在反向代理之后。
- 抖音接口与风控会变；设备画像、cookie、版本号一起换，签名偏移由
  DuckDroid2 的 `dy7_case` 负责。
