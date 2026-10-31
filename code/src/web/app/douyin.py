"""抖音 API 客户端：签名交给 DuckDroid2 dy7，HTTP 与数据提取留在这里。

分层与 `system_client.py` 一致：

    FastAPI -> DouyinService -> SystemClient -> UDS -> System -> Worker(dy7)
                             \\-> httpx -> aweme.snssdk.com

签名器（libmetasec_ml.so 40.8.0）只认 URL 与 cookie/header blob；请求体不进
签名，POST 的 body 只影响 X-SS-STUB。这个结论来自 `dy7_case` 的 `god7` 入口，
并被真实接口验证过（搜索、详情、评论、用户、作品）。
"""

from __future__ import annotations

import hashlib
import time
import urllib.parse
from typing import Any

import httpx

from .config import Settings
from .system_client import SystemClient, SystemClientError

# 到达 guest 之前就失败的请求 -> 400；资源/排队问题 -> 503；
# 会话丢失需要重新分配 -> 409；guest 自身/上游失败 -> 502。
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


def _status_for_code(code: str) -> int:
    if code in _CLIENT_ERRORS:
        return 400
    if code in _UNAVAILABLE_ERRORS:
        return 503
    if code in _SESSION_ERRORS:
        return 409
    return 502


class DouyinError(Exception):
    """签名、上游 HTTP 或抖音业务状态失败。"""

    def __init__(
        self,
        message: str,
        *,
        code: str = "DOUYIN_ERROR",
        http_status: int = 502,
        payload: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.http_status = http_status
        self.payload = payload


def _first_url(container: Any) -> str:
    if not isinstance(container, dict):
        return ""
    urls = container.get("url_list") or []
    return urls[0] if urls else ""


class DouyinService:
    """一个进程内共享：一个 SystemClient + 一个 httpx 连接池。"""

    def __init__(self, settings: Settings, system: SystemClient) -> None:
        self._settings = settings
        self._system = system
        self._cookies = settings.cookies
        self._http = httpx.Client(
            timeout=settings.douyin_timeout_s,
            verify=settings.douyin_verify_tls,
            follow_redirects=True,
        )

    # ---------------------------------------------------------------- 签名

    def _system_sign(
        self, params: dict[str, str], *, session: str | None
    ) -> dict[str, Any]:
        try:
            response = self._system.sign(params, session=session)
        except SystemClientError as exc:
            raise DouyinError(
                str(exc),
                code=exc.code,
                http_status=503 if exc.code == "SYSTEM_UNAVAILABLE" else 502,
            ) from exc
        if not response.get("ok"):
            error = response.get("error") or {}
            code = error.get("code") or "SIGN_FAILED"
            raise DouyinError(
                error.get("message") or code,
                code=code,
                http_status=_status_for_code(code),
                payload=response,
            )
        return response

    def _sign_headers(self, url: str, *, session: str | None) -> tuple[dict[str, str], dict[str, Any]]:
        params: dict[str, str] = {"url": url}
        if self._settings.douyin_headers:
            params["headers"] = self._settings.douyin_headers
        elif self._settings.douyin_cookie:
            params["cookie"] = self._settings.douyin_cookie

        response = self._system_sign(params, session=session)
        result = response.get("result") or {}
        headers = {
            name: value
            for name, value in result.items()
            if name != "sign" and isinstance(value, str)
        }
        if not headers:
            raise DouyinError("签名器没有返回任何请求头", code="EMPTY_SIGNATURE")
        return headers, response

    def sign(
        self,
        url: str,
        *,
        headers: str | None = None,
        cookie: str | None = None,
        wall_time_ms: int | None = None,
        session: str | None = None,
    ) -> dict[str, Any]:
        """直接给一个完整 URL 生成七组头（`/dy/sign` 使用）。"""
        params: dict[str, str] = {"url": url}
        if headers:
            params["headers"] = headers
        elif cookie:
            params["cookie"] = cookie
        elif self._settings.douyin_headers:
            params["headers"] = self._settings.douyin_headers
        elif self._settings.douyin_cookie:
            params["cookie"] = self._settings.douyin_cookie
        if wall_time_ms is not None:
            params["wall_time_ms"] = str(wall_time_ms)

        response = self._system_sign(params, session=session)
        result = response.get("result") or {}
        heads = {
            name: value
            for name, value in result.items()
            if name != "sign" and isinstance(value, str)
        }
        return {
            "ok": True,
            "app": response.get("app", self._settings.app),
            "session": response.get("instance", ""),
            "headers": heads,
            "signature": result.get("sign", ""),
            "meta": response.get("meta", {}),
        }

    # ------------------------------------------------------------ HTTP 请求

    def _common_params(self) -> dict[str, str]:
        now = time.time()
        settings = self._settings
        return {
            "iid": settings.douyin_install_id,
            "device_id": settings.douyin_device_id,
            "ac": "wifi",
            "channel": settings.douyin_channel,
            "aid": settings.douyin_aid,
            "app_name": "aweme",
            "version_code": settings.douyin_version_code,
            "version_name": settings.douyin_version_name,
            "device_platform": "android",
            "os": "android",
            "ssmix": "a",
            "device_type": settings.douyin_device_type,
            "device_brand": settings.douyin_device_brand,
            "language": "zh",
            "os_api": settings.douyin_os_api,
            "os_version": settings.douyin_os_version,
            "resolution": settings.douyin_resolution,
            "dpi": settings.douyin_dpi,
            "_rticket": str(int(now * 1000)),
            "ts": str(int(now)),
            "cdid": settings.douyin_cdid,
        }

    def _build_url(self, path: str, extra: dict[str, str] | None = None) -> str:
        params = self._common_params()
        if extra:
            params.update(extra)
        # 与 App 的拼接口径一致；签名和真正发出的 URL 必须是同一个字符串。
        query = urllib.parse.urlencode(params, quote_via=urllib.parse.quote)
        return f"{self._settings.douyin_base_url}{path}?{query}"

    def _request(
        self,
        method: str,
        path: str,
        extra: dict[str, str] | None = None,
        *,
        body: str | None = None,
        session: str | None = None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        url = self._build_url(path, extra)
        sign_headers, sign_response = self._sign_headers(url, session=session)

        headers: dict[str, str] = {
            "User-Agent": self._settings.douyin_user_agent,
            "X-SS-REQ-TICKET": str(int(time.time() * 1000)),
            "Accept-Encoding": "gzip, deflate",
            **sign_headers,
        }
        content: bytes | None = None
        if body is not None:
            content = body.encode("utf-8")
            headers["Content-Type"] = "application/x-www-form-urlencoded"
            headers["X-SS-STUB"] = hashlib.md5(content).hexdigest().upper()

        try:
            response = self._http.request(
                method,
                url,
                headers=headers,
                cookies=self._cookies,
                content=content,
            )
        except httpx.HTTPError as exc:
            raise DouyinError(
                f"请求抖音失败: {exc}", code="DOUYIN_TRANSPORT"
            ) from exc

        if response.status_code != 200:
            raise DouyinError(
                f"抖音返回 HTTP {response.status_code}",
                code="DOUYIN_HTTP",
                payload={"status": response.status_code, "body": response.text[:500]},
            )
        try:
            payload = response.json()
        except ValueError as exc:
            raise DouyinError(
                "抖音返回的不是 JSON",
                code="DOUYIN_BAD_JSON",
                payload={"body": response.text[:500]},
            ) from exc
        if not isinstance(payload, dict):
            raise DouyinError("抖音返回的 JSON 不是对象", code="DOUYIN_BAD_JSON")

        status_code = payload.get("status_code")
        if status_code != 0:
            raise DouyinError(
                payload.get("status_msg") or f"status_code={status_code}",
                code="DOUYIN_STATUS",
                payload={"status_code": status_code},
            )
        return payload, sign_response

    @staticmethod
    def _envelope(sign_response: dict[str, Any]) -> dict[str, Any]:
        return {
            "ok": True,
            "app": sign_response.get("app", "dy7"),
            # System 的 instance uuid 就是会话 id，带回去可固定到同一实例。
            "session": sign_response.get("instance", ""),
            "sign_meta": sign_response.get("meta", {}),
        }

    # ---------------------------------------------------------------- 接口

    def search(
        self,
        keyword: str,
        *,
        offset: int = 0,
        count: int = 10,
        session: str | None = None,
        raw: bool = False,
    ) -> dict[str, Any]:
        """搜索视频：POST /aweme/v2/search_feed/。"""
        body = urllib.parse.urlencode(
            {
                "keyword": keyword,
                "pull_type": "1",
                "offset": str(offset),
                "count": str(count),
                "search_source": "normal_search",
                "search_id": str(int(time.time() * 1000)),
            }
        )
        payload, signed = self._request(
            "POST", "/aweme/v2/search_feed/", body=body, session=session
        )

        videos = payload.get("aweme_list")
        if videos is None:
            videos = [
                item.get("aweme")
                for item in payload.get("data") or []
                if item.get("type") == 1 and item.get("aweme")
            ]
        videos = [video for video in videos or [] if video]

        next_offset = payload.get("offset")
        if not isinstance(next_offset, int):
            next_offset = offset + len(videos)

        result = self._envelope(signed)
        result["data"] = [self.extract_video(video) for video in videos]
        result["pagination"] = {
            "offset": next_offset,
            "has_more": bool(payload.get("has_more")),
        }
        if raw:
            result["raw"] = payload
        return result

    def video(
        self, aweme_id: str, *, session: str | None = None, raw: bool = False
    ) -> dict[str, Any]:
        """视频详情：GET /aweme/v1/aweme/detail/。"""
        payload, signed = self._request(
            "GET",
            "/aweme/v1/aweme/detail/",
            {"aweme_id": str(aweme_id)},
            session=session,
        )
        detail = payload.get("aweme_detail")
        if not detail:
            raise DouyinError("视频不存在或已被删除", code="NOT_FOUND", http_status=404)
        result = self._envelope(signed)
        result["data"] = self.extract_video(detail)
        if raw:
            result["raw"] = payload
        return result

    def comments(
        self,
        aweme_id: str,
        *,
        cursor: int = 0,
        count: int = 20,
        session: str | None = None,
        raw: bool = False,
    ) -> dict[str, Any]:
        """评论列表：GET /aweme/v2/comment/list/（必须签名）。"""
        payload, signed = self._request(
            "GET",
            "/aweme/v2/comment/list/",
            {
                "aweme_id": str(aweme_id),
                "cursor": str(cursor),
                "count": str(count),
            },
            session=session,
        )
        comments = payload.get("comments") or []
        next_cursor = payload.get("cursor")
        if not isinstance(next_cursor, int):
            next_cursor = cursor + len(comments)

        result = self._envelope(signed)
        result["data"] = [self.extract_comment(comment) for comment in comments]
        result["pagination"] = {
            "cursor": next_cursor,
            "has_more": bool(payload.get("has_more")),
            "total": int(payload.get("total") or 0),
        }
        if raw:
            result["raw"] = payload
        return result

    def user(
        self, sec_uid: str, *, session: str | None = None, raw: bool = False
    ) -> dict[str, Any]:
        """用户主页：GET /aweme/v1/user/profile/other/。"""
        payload, signed = self._request(
            "GET",
            "/aweme/v1/user/profile/other/",
            {"sec_user_id": sec_uid},
            session=session,
        )
        user = payload.get("user")
        if not user:
            raise DouyinError("用户不存在", code="NOT_FOUND", http_status=404)
        result = self._envelope(signed)
        result["data"] = self.extract_user(user)
        if raw:
            result["raw"] = payload
        return result

    def user_posts(
        self,
        sec_uid: str,
        *,
        max_cursor: int = 0,
        count: int = 20,
        session: str | None = None,
        raw: bool = False,
    ) -> dict[str, Any]:
        """用户作品：GET /aweme/v1/aweme/post/（必须签名）。"""
        payload, signed = self._request(
            "GET",
            "/aweme/v1/aweme/post/",
            {
                "sec_user_id": sec_uid,
                "max_cursor": str(max_cursor),
                "count": str(count),
            },
            session=session,
        )
        videos = payload.get("aweme_list") or []
        result = self._envelope(signed)
        result["data"] = [self.extract_video(video) for video in videos]
        result["pagination"] = {
            "max_cursor": int(payload.get("max_cursor") or 0),
            "has_more": bool(payload.get("has_more")),
        }
        if raw:
            result["raw"] = payload
        return result

    # ------------------------------------------------------------ 数据提取

    @staticmethod
    def extract_video(aweme: dict[str, Any]) -> dict[str, Any]:
        author = aweme.get("author") or {}
        stats = aweme.get("statistics") or {}
        video = aweme.get("video") or {}
        music = aweme.get("music") or {}
        play_addr = video.get("play_addr") or {}
        cover = video.get("cover") or {}

        return {
            "aweme_id": aweme.get("aweme_id"),
            "desc": aweme.get("desc", ""),
            "create_time": aweme.get("create_time"),
            "author_uid": author.get("uid"),
            "author_nickname": author.get("nickname"),
            "author_unique_id": author.get("unique_id"),
            "author_sec_uid": author.get("sec_uid"),
            "digg_count": stats.get("digg_count", 0),
            "comment_count": stats.get("comment_count", 0),
            "share_count": stats.get("share_count", 0),
            "play_count": stats.get("play_count", 0),
            "collect_count": stats.get("collect_count", 0),
            "duration": video.get("duration", 0),
            "video_url": _first_url(play_addr),
            "cover_url": _first_url(cover),
            "music_title": music.get("title", ""),
            "share_url": aweme.get("share_url", ""),
        }

    @staticmethod
    def extract_comment(comment: dict[str, Any]) -> dict[str, Any]:
        user = comment.get("user") or {}
        return {
            "cid": comment.get("cid"),
            "text": comment.get("text", ""),
            "create_time": comment.get("create_time"),
            "digg_count": comment.get("digg_count", 0),
            "reply_count": comment.get("reply_comment_total", 0),
            "user_uid": user.get("uid"),
            "user_nickname": user.get("nickname"),
            "user_sec_uid": user.get("sec_uid"),
            "aweme_id": comment.get("aweme_id"),
        }

    @staticmethod
    def extract_user(user: dict[str, Any]) -> dict[str, Any]:
        return {
            "uid": user.get("uid"),
            "sec_uid": user.get("sec_uid"),
            "unique_id": user.get("unique_id", ""),
            "nickname": user.get("nickname"),
            "signature": user.get("signature", ""),
            "follower_count": user.get("follower_count", 0),
            "following_count": user.get("following_count", 0),
            "total_favorited": user.get("total_favorited", 0),
            "aweme_count": user.get("aweme_count", 0),
            "avatar_url": _first_url(user.get("avatar_larger")),
        }

    def close(self) -> None:
        self._http.close()
