"""DuckDroid2 Web API —— System（UDS 网关）之上的 FastAPI 封装。

分层：

    HTTP 客户端 -> FastAPI (本文件) -> DouyinService -> UDS 帧协议 -> System
                                                      \\-> httpx -> 抖音

前半段沿用 DuckDroid2 的设计：连接本身决定 app，System 只转帧，Web API 只做
参数搬运与错误码映射。后半段是本项目新增的抖音业务：`dy7` 出七组签名头，
DouyinService 用同一份 cookie 发请求并提取字段。

生产所需的鉴权、限流、连接池、任务队列、面板都不在这里；这里只保证
「签名 → 请求 → 结构化结果」这条链路完整、可观测、可替换。
"""

from __future__ import annotations

import json
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from .config import settings
from .douyin import DouyinError, DouyinService
from .system_client import SystemClient, SystemClientError

# 到达 guest 之前就失败的请求 -> 400；资源/排队问题 -> 503；
# 会话丢失需要重新分配 -> 409；guest 自身失败 -> 502。
_CLIENT_ERRORS = {
    "BAD_FRAME",
    "BAD_JSON",
    "BAD_VERSION",
    "BAD_REQUEST",
    "UNKNOWN_OP",
    "APP_MISMATCH",
}
_UNAVAILABLE_ERRORS = {
    "OVERLOADED",
    "INITIALIZING",
    "QUEUE_TIMEOUT",
    "DEADLINE_EXCEEDED",
    "CALL_TIMEOUT",
}
_SESSION_ERRORS = {"SESSION_NOT_FOUND", "SESSION_GONE"}


class GenericSignRequest(BaseModel):
    """直接转给 System 的通用请求；params 由目标自己解释。"""

    params: dict[str, str] = Field(default_factory=dict)
    session: str | None = None
    deadline_ms: int | None = Field(default=None, ge=1)


class JdSignRequest(BaseModel):
    """京东 16.1.50 `Bridge.main(101)` 的参数。

    全部可省略：省略的键不会发给 System，插件会使用内置的 coral 样本。
    """

    request: str | None = None
    device_payload: str | None = None
    dfp_eid: str | None = None
    pay_version: str | None = None
    openudid: str | None = None
    sdk_config: str | None = None
    session: str | None = None
    deadline_ms: int | None = Field(default=None, ge=1)

    def to_params(self) -> dict[str, str]:
        names = (
            "request",
            "device_payload",
            "dfp_eid",
            "pay_version",
            "openudid",
            "sdk_config",
        )
        return {
            name: value
            for name in names
            if (value := getattr(self, name)) is not None
        }


class DySignRequest(BaseModel):
    """抖音 dy7（libmetasec_ml.so 40.8.0）的原始签名请求。

    `headers` 是整段 "key\\r\\nvalue\\r\\n..." 头块；不传则用 `cookie`，
    再退回到服务端配置的默认 cookie。
    """

    url: str = Field(min_length=1)
    headers: str | None = None
    cookie: str | None = None
    wall_time_ms: int | None = Field(default=None, ge=1)
    session: str | None = None


class DySearchRequest(BaseModel):
    keyword: str = Field(min_length=1)
    offset: int = Field(default=0, ge=0)
    count: int = Field(default=10, ge=1, le=50)
    session: str | None = None
    raw: bool = False


client = SystemClient(settings)
service = DouyinService(settings, client)


@asynccontextmanager
async def lifespan(_: FastAPI):
    yield
    service.close()


app = FastAPI(
    title="DuckDroid2 Web API",
    description=(
        "把 DuckDroid2 System 的 UDS 帧协议包成 HTTP JSON，并在 dy7 签名之上"
        "提供抖音搜索、详情、评论、用户与作品接口。"
    ),
    version="0.2.0",
    lifespan=lifespan,
)


@app.exception_handler(DouyinError)
async def douyin_error_handler(_: Any, exc: DouyinError) -> JSONResponse:
    error: dict[str, Any] = {"code": exc.code, "message": str(exc)}
    if exc.payload:
        error["detail"] = exc.payload
    return JSONResponse(
        {"ok": False, "error": error}, status_code=exc.http_status
    )


def _status_for(response: dict[str, Any]) -> int:
    if response.get("ok"):
        return 200
    code = (response.get("error") or {}).get("code", "")
    if code in _CLIENT_ERRORS:
        return 400
    if code in _UNAVAILABLE_ERRORS:
        return 503
    if code in _SESSION_ERRORS:
        return 409
    return 502


def _call(
    params: dict[str, str],
    *,
    session: str | None,
    deadline_ms: int | None,
) -> dict[str, Any]:
    try:
        return client.sign(params, session=session, deadline_ms=deadline_ms)
    except SystemClientError as exc:
        status = 503 if exc.code == "SYSTEM_UNAVAILABLE" else 502
        raise HTTPException(
            status_code=status,
            detail={"ok": False, "error": {"code": exc.code, "message": str(exc)}},
        ) from exc


def _json(status: int, payload: dict[str, Any]) -> JSONResponse:
    return JSONResponse(payload, status_code=status)


@app.get("/", include_in_schema=False)
def index() -> dict[str, Any]:
    return {
        "service": "duckdroid2-webapi",
        "app": settings.app,
        "socket": settings.socket_path,
        "docs": "/docs",
        "endpoints": [
            "/health",
            "/sign",
            "/jd/sign",
            "/dy/sign",
            "/dy/search",
            "/dy/video/{aweme_id}",
            "/dy/comments/{aweme_id}",
            "/dy/user/{sec_uid}",
            "/dy/user/{sec_uid}/posts",
        ],
    }


@app.get("/health")
def health() -> JSONResponse:
    reachable = client.ping()
    payload = {
        "status": "ok" if reachable else "unavailable",
        "app": settings.app,
        "socket": settings.socket_path,
    }
    return _json(200 if reachable else 503, payload)


@app.post("/sign")
def sign(request: GenericSignRequest) -> JSONResponse:
    """通用转发：响应原样返回 System 的结果。"""
    response = _call(
        request.params,
        session=request.session,
        deadline_ms=request.deadline_ms,
    )
    return _json(_status_for(response), response)


@app.post("/jd/sign")
def jd_sign(request: JdSignRequest) -> JSONResponse:
    """调用京东签名器（app id: jdgs16，`Bridge.main(101)`）。

    成功时 `result.value` 是 b1–b7 的 JSON 字符串；这里额外解析成 `headers`，
    同时保留原始 `signature`。
    """
    response = _call(
        request.to_params(),
        session=request.session,
        deadline_ms=request.deadline_ms,
    )
    if not response.get("ok"):
        return _json(_status_for(response), response)

    signature = (response.get("result") or {}).get("value", "")
    headers: dict[str, str] | None = None
    if isinstance(signature, str):
        try:
            parsed = json.loads(signature)
        except json.JSONDecodeError:
            parsed = None
        if isinstance(parsed, dict):
            headers = parsed

    return _json(
        200,
        {
            "ok": True,
            "app": response.get("app", settings.app),
            "instance": response.get("instance", ""),
            # System 的 session id 就是 instance uuid；带回去即可固定到同一实例。
            "session": response.get("instance", ""),
            "signature": signature,
            "headers": headers,
            "meta": response.get("meta", {}),
        },
    )


# ------------------------------------------------------------------ 抖音


@app.post("/dy/sign")
def dy_sign(request: DySignRequest) -> dict[str, Any]:
    """只做签名：完整 URL 进，七组头出（dy7 / `god7`）。"""
    return service.sign(
        request.url,
        headers=request.headers,
        cookie=request.cookie,
        wall_time_ms=request.wall_time_ms,
        session=request.session,
    )


@app.post("/dy/search")
def dy_search(request: DySearchRequest) -> dict[str, Any]:
    return service.search(
        request.keyword,
        offset=request.offset,
        count=request.count,
        session=request.session,
        raw=request.raw,
    )


@app.get("/dy/video/{aweme_id}")
def dy_video(
    aweme_id: str,
    session: str | None = None,
    raw: bool = False,
) -> dict[str, Any]:
    return service.video(aweme_id, session=session, raw=raw)


@app.get("/dy/comments/{aweme_id}")
def dy_comments(
    aweme_id: str,
    cursor: int = Query(default=0, ge=0),
    count: int = Query(default=20, ge=1, le=50),
    session: str | None = None,
    raw: bool = False,
) -> dict[str, Any]:
    return service.comments(
        aweme_id,
        cursor=cursor,
        count=count,
        session=session,
        raw=raw,
    )


@app.get("/dy/user/{sec_uid}")
def dy_user(
    sec_uid: str,
    session: str | None = None,
    raw: bool = False,
) -> dict[str, Any]:
    return service.user(sec_uid, session=session, raw=raw)


@app.get("/dy/user/{sec_uid}/posts")
def dy_user_posts(
    sec_uid: str,
    max_cursor: int = Query(default=0, ge=0),
    count: int = Query(default=20, ge=1, le=50),
    session: str | None = None,
    raw: bool = False,
) -> dict[str, Any]:
    return service.user_posts(
        sec_uid,
        max_cursor=max_cursor,
        count=count,
        session=session,
        raw=raw,
    )


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("app.main:app", host="127.0.0.1", port=8000)
