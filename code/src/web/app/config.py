"""Web API 配置。

全部来自环境变量，保持这一层足够薄：它只决定连哪个 System socket、
默认 app 是谁、单个请求最多等多久，以及抖音请求的设备画像。

设备值默认取 DuckDroid2 `dy7_case` 的 Mi 8 / Android 12 画像与抓包身份；
换设备时同时改这里与 DuckDroid2 的 `dy7.properties` / `dy7_case.cpp`。
"""

from __future__ import annotations

import os
from dataclasses import dataclass


def _env_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise RuntimeError(f"{name} must be an integer, got {raw!r}") from exc


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


# DuckDroid2 dy7_case 的内置 cookie（抖音 40.8.0）。签名与 HTTP 请求共用，
# 两边不一致时服务端可能判 device 变化。
DEFAULT_COOKIE = (
    "store-region-src=did; "
    "odin_tt=f6561e4976f49fba3a0e58569effb66e5bac1f85ae38ab95c3771a70a2078fbe"
    "2f50f4f4c13332705443edcae1bcd11056bb0217066416a3a7789b749125abc2c362bb9da"
    "8fdb561b3b58f41c7c6ee20; "
    "ttreq=1$df343d2b353e6df23c254e3bf6519c336fc370a3; "
    "store-region=cn-he; "
    "install_id=3489221505207928"
)

DEFAULT_USER_AGENT = (
    "com.ss.android.ugc.aweme/400800 "
    "(Linux; U; Android 12; zh_CN; MI 8; Build/SP1A.210812.016; "
    "Cronet/TTNetVersion:1f4d25e4 2024-12-20 QuicVersion:4e1fc47f 2024-12-20)"
)


@dataclass(frozen=True)
class Settings:
    app: str
    socket_path: str
    # 一次 HTTP 请求内部允许重试 OVERLOADED / INITIALIZING 的总时长。
    deadline_ms: int = 120_000
    connect_timeout_s: float = 5.0

    # ---- 抖音（dy7 / libmetasec_ml.so 40.8.0）----
    douyin_base_url: str = "https://aweme.snssdk.com"
    douyin_device_id: str = "3951009026851867"
    douyin_install_id: str = "3489221505207928"
    douyin_aid: str = "1128"
    douyin_version_code: str = "400800"
    douyin_version_name: str = "40.8.0"
    douyin_device_type: str = "MI 8"
    douyin_device_brand: str = "Xiaomi"
    douyin_channel: str = "huawei_1128_64"
    douyin_os_api: str = "32"
    douyin_os_version: str = "12"
    douyin_resolution: str = "1080*2248"
    douyin_dpi: str = "440"
    douyin_cdid: str = "c10a83da-a51d-48de-b259-a0cf192b4652"
    douyin_cookie: str = DEFAULT_COOKIE
    # 整段 header blob（"key\r\nvalue\r\n..."）；设置后 dy7 插件优先于 cookie。
    douyin_headers: str | None = None
    douyin_user_agent: str = DEFAULT_USER_AGENT
    douyin_timeout_s: float = 20.0
    douyin_verify_tls: bool = False

    @property
    def cookies(self) -> dict[str, str]:
        """把 dy7 的 cookie 串拆成请求用字典，值里允许出现 '='。"""
        parsed: dict[str, str] = {}
        for item in self.douyin_cookie.split(";"):
            name, separator, value = item.strip().partition("=")
            if separator and name:
                parsed[name] = value
        return parsed

    @classmethod
    def from_env(cls) -> "Settings":
        app = os.getenv("DUCKEMU_APP", "dy7")
        socket_dir = os.getenv("DUCKEMU_SOCKET_DIR", "/tmp/duckemu-dy7")
        # 显式 DUCKEMU_SOCKET 优先，便于把 Web API 指到任意 app 的 listener。
        socket_path = os.getenv("DUCKEMU_SOCKET") or os.path.join(
            socket_dir, f"{app}.sock"
        )
        return cls(
            app=app,
            socket_path=socket_path,
            deadline_ms=_env_int("DUCKEMU_DEADLINE_MS", 120_000),
            connect_timeout_s=float(os.getenv("DUCKEMU_CONNECT_TIMEOUT_S", "5")),
            douyin_base_url=os.getenv("DOUYIN_BASE_URL", "https://aweme.snssdk.com"),
            douyin_device_id=os.getenv("DOUYIN_DEVICE_ID", "3951009026851867"),
            douyin_install_id=os.getenv("DOUYIN_INSTALL_ID", "3489221505207928"),
            douyin_aid=os.getenv("DOUYIN_AID", "1128"),
            douyin_version_code=os.getenv("DOUYIN_VERSION_CODE", "400800"),
            douyin_version_name=os.getenv("DOUYIN_VERSION_NAME", "40.8.0"),
            douyin_device_type=os.getenv("DOUYIN_DEVICE_TYPE", "MI 8"),
            douyin_device_brand=os.getenv("DOUYIN_DEVICE_BRAND", "Xiaomi"),
            douyin_channel=os.getenv("DOUYIN_CHANNEL", "huawei_1128_64"),
            douyin_os_api=os.getenv("DOUYIN_OS_API", "32"),
            douyin_os_version=os.getenv("DOUYIN_OS_VERSION", "12"),
            douyin_resolution=os.getenv("DOUYIN_RESOLUTION", "1080*2248"),
            douyin_dpi=os.getenv("DOUYIN_DPI", "440"),
            douyin_cdid=os.getenv(
                "DOUYIN_CDID", "c10a83da-a51d-48de-b259-a0cf192b4652"
            ),
            douyin_cookie=os.getenv("DOUYIN_COOKIE", DEFAULT_COOKIE),
            douyin_headers=os.getenv("DOUYIN_HEADERS") or None,
            douyin_user_agent=os.getenv("DOUYIN_USER_AGENT", DEFAULT_USER_AGENT),
            douyin_timeout_s=float(os.getenv("DOUYIN_TIMEOUT_S", "20")),
            douyin_verify_tls=_env_bool("DOUYIN_VERIFY_TLS", False),
        )


settings = Settings.from_env()
