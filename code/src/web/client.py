#!/usr/bin/env python3
"""DuckDroid2 Web API 的 Python 客户端。

只依赖标准库，直接调用 FastAPI 暴露的抖音接口，拿到的是已提取的字段。
典型用法：

    python client.py health
    python client.py search 鸭子 --count 5
    python client.py video 7637479447470211042
    python client.py comments 7637479447470211042 --count 10
    python client.py user MS4wLjABAAAA...
    python client.py posts MS4wLjABAAAA... --count 10
    python client.py demo --keyword 鸭子 --save ./data

FastAPI 地址默认 http://127.0.0.1:8000，可用 --base-url 或
DUCKAPI_BASE_URL 覆盖。
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

DEFAULT_BASE_URL = os.getenv("DUCKAPI_BASE_URL", "http://127.0.0.1:8000")


class ApiError(RuntimeError):
    def __init__(self, status: int, payload: Any) -> None:
        self.status = status
        self.payload = payload
        message = ""
        if isinstance(payload, dict):
            error = payload.get("error") or {}
            message = error.get("message") or json.dumps(
                payload, ensure_ascii=False
            )
        else:
            message = str(payload)
        super().__init__(f"HTTP {status}: {message}")


class DuckApi:
    """对 FastAPI 的最小封装；每个方法返回接口的原始 JSON。"""

    def __init__(self, base_url: str = DEFAULT_BASE_URL, timeout: float = 180.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def _call(
        self,
        method: str,
        path: str,
        *,
        params: dict[str, Any] | None = None,
        body: dict[str, Any] | None = None,
    ) -> Any:
        url = self.base_url + path
        if params:
            query = {
                name: value
                for name, value in params.items()
                if value is not None
            }
            if query:
                url += "?" + urllib.parse.urlencode(
                    query, quote_via=urllib.parse.quote
                )
        data = None
        headers: dict[str, str] = {}
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"

        request = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return json.load(response)
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", "replace")
            try:
                payload: Any = json.loads(raw)
            except json.JSONDecodeError:
                payload = {"ok": False, "raw": raw}
            raise ApiError(exc.code, payload) from exc

    # ---------------------------------------------------------------- 接口

    def health(self) -> Any:
        return self._call("GET", "/health")

    def sign(
        self,
        url: str,
        *,
        headers: str | None = None,
        cookie: str | None = None,
        wall_time_ms: int | None = None,
        session: str | None = None,
    ) -> Any:
        body: dict[str, Any] = {"url": url}
        if headers is not None:
            body["headers"] = headers
        if cookie is not None:
            body["cookie"] = cookie
        if wall_time_ms is not None:
            body["wall_time_ms"] = wall_time_ms
        if session is not None:
            body["session"] = session
        return self._call("POST", "/dy/sign", body=body)

    def search(
        self,
        keyword: str,
        *,
        offset: int = 0,
        count: int = 10,
        session: str | None = None,
        raw: bool = False,
    ) -> Any:
        return self._call(
            "POST",
            "/dy/search",
            body={
                "keyword": keyword,
                "offset": offset,
                "count": count,
                "session": session,
                "raw": raw,
            },
        )

    def video(
        self,
        aweme_id: str,
        *,
        session: str | None = None,
        raw: bool = False,
    ) -> Any:
        return self._call(
            "GET",
            f"/dy/video/{urllib.parse.quote(str(aweme_id))}",
            params={"session": session, "raw": raw},
        )

    def comments(
        self,
        aweme_id: str,
        *,
        cursor: int = 0,
        count: int = 20,
        session: str | None = None,
        raw: bool = False,
    ) -> Any:
        return self._call(
            "GET",
            f"/dy/comments/{urllib.parse.quote(str(aweme_id))}",
            params={
                "cursor": cursor,
                "count": count,
                "session": session,
                "raw": raw,
            },
        )

    def user(
        self,
        sec_uid: str,
        *,
        session: str | None = None,
        raw: bool = False,
    ) -> Any:
        return self._call(
            "GET",
            f"/dy/user/{urllib.parse.quote(sec_uid, safe='')}",
            params={"session": session, "raw": raw},
        )

    def user_posts(
        self,
        sec_uid: str,
        *,
        max_cursor: int = 0,
        count: int = 20,
        session: str | None = None,
        raw: bool = False,
    ) -> Any:
        return self._call(
            "GET",
            f"/dy/user/{urllib.parse.quote(sec_uid, safe='')}/posts",
            params={
                "max_cursor": max_cursor,
                "count": count,
                "session": session,
                "raw": raw,
            },
        )


# ------------------------------------------------------------------ CLI 输出


def dump(payload: Any) -> None:
    print(json.dumps(payload, ensure_ascii=False, indent=2))


def save_json(directory: Path, name: str, payload: Any) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    return path


def demo(api: DuckApi, keyword: str, count: int, save_dir: Path | None) -> int:
    health = api.health()
    print(f"[*] API: {api.base_url}  status={health.get('status')}")

    search = api.search(keyword, count=count)
    videos = search.get("data") or []
    print(f"\n[1/5] 搜索「{keyword}」: {len(videos)} 条, "
          f"has_more={search.get('pagination', {}).get('has_more')}")
    for video in videos[:5]:
        print(f"  - {video.get('author_nickname')}: "
              f"{(video.get('desc') or '')[:40]}  "
              f"(赞:{video.get('digg_count')})")
    if not videos:
        print("[-] 搜索没有结果，演示结束")
        return 1

    aweme_id = str(videos[0]["aweme_id"])
    detail = api.video(aweme_id)
    video = detail.get("data") or {}
    print(f"\n[2/5] 视频详情 {aweme_id}")
    print(f"  {(video.get('desc') or '')[:60]}")
    print(f"  作者: {video.get('author_nickname')}  时长: {video.get('duration')}ms")
    print(f"  赞 {video.get('digg_count')} / 评 {video.get('comment_count')} "
          f"/ 转 {video.get('share_count')}")

    sec_uid = video.get("author_sec_uid") or videos[0].get("author_sec_uid") or ""
    comments = api.comments(aweme_id, count=count)
    items = comments.get("data") or []
    print(f"\n[3/5] 评论: {len(items)} 条, "
          f"total={comments.get('pagination', {}).get('total')}")
    for comment in items[:5]:
        print(f"  - {comment.get('user_nickname')}: "
              f"{(comment.get('text') or '')[:40]}  "
              f"(赞:{comment.get('digg_count')})")

    if sec_uid:
        user = api.user(sec_uid)
        info = user.get("data") or {}
        print(f"\n[4/5] 用户主页 {info.get('nickname')}")
        print(f"  粉丝 {info.get('follower_count')} / 关注 "
              f"{info.get('following_count')} / 获赞 "
              f"{info.get('total_favorited')} / 作品 "
              f"{info.get('aweme_count')}")

        posts = api.user_posts(sec_uid, count=count)
        post_items = posts.get("data") or []
        print(f"\n[5/5] 用户作品: {len(post_items)} 条, "
              f"has_more={posts.get('pagination', {}).get('has_more')}")
        for post in post_items[:5]:
            print(f"  - {(post.get('desc') or '')[:40]}  "
                  f"(赞:{post.get('digg_count')})")
    else:
        user, posts = None, None
        print("\n[4/5] 跳过用户主页（缺少 sec_uid）")
        print("[5/5] 跳过用户作品（缺少 sec_uid）")

    if save_dir is not None:
        files = [
            save_json(save_dir, "search.json", search),
            save_json(save_dir, "video.json", detail),
            save_json(save_dir, "comments.json", comments),
        ]
        if user is not None:
            files.append(save_json(save_dir, "user.json", user))
        if posts is not None:
            files.append(save_json(save_dir, "user_posts.json", posts))
        print("\n[+] 已保存:")
        for path in files:
            print(f"  - {path}")

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="DuckDroid2 Web API 的抖音数据客户端",
    )
    parser.add_argument(
        "--base-url",
        default=DEFAULT_BASE_URL,
        help=f"FastAPI 地址（默认 {DEFAULT_BASE_URL}）",
    )
    parser.add_argument(
        "--timeout", type=float, default=180.0, help="单次 HTTP 超时秒数"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("health", help="检查 API 与 DuckDroid2 System 是否可达")

    sign = sub.add_parser("sign", help="只生成签名头")
    sign.add_argument("url")
    sign.add_argument("--headers", default=None, help="整段 header blob")
    sign.add_argument("--cookie", default=None, help="cookie 串")
    sign.add_argument("--wall-time-ms", type=int, default=None)
    sign.add_argument("--session", default=None)

    search = sub.add_parser("search", help="搜索视频")
    search.add_argument("keyword")
    search.add_argument("--offset", type=int, default=0)
    search.add_argument("--count", type=int, default=10)
    search.add_argument("--session", default=None)
    search.add_argument("--raw", action="store_true")

    video = sub.add_parser("video", help="视频详情")
    video.add_argument("aweme_id")
    video.add_argument("--session", default=None)
    video.add_argument("--raw", action="store_true")

    comments = sub.add_parser("comments", help="评论列表")
    comments.add_argument("aweme_id")
    comments.add_argument("--cursor", type=int, default=0)
    comments.add_argument("--count", type=int, default=20)
    comments.add_argument("--session", default=None)
    comments.add_argument("--raw", action="store_true")

    user = sub.add_parser("user", help="用户主页")
    user.add_argument("sec_uid")
    user.add_argument("--session", default=None)
    user.add_argument("--raw", action="store_true")

    posts = sub.add_parser("posts", help="用户作品")
    posts.add_argument("sec_uid")
    posts.add_argument("--max-cursor", type=int, default=0)
    posts.add_argument("--count", type=int, default=20)
    posts.add_argument("--session", default=None)
    posts.add_argument("--raw", action="store_true")

    demo_cmd = sub.add_parser("demo", help="搜索→详情→评论→用户→作品的完整演示")
    demo_cmd.add_argument("--keyword", default="鸭子")
    demo_cmd.add_argument("--count", type=int, default=10)
    demo_cmd.add_argument("--save", default=None, help="保存 JSON 的目录")

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    api = DuckApi(args.base_url, args.timeout)

    try:
        if args.command == "health":
            dump(api.health())
        elif args.command == "sign":
            dump(
                api.sign(
                    args.url,
                    headers=args.headers,
                    cookie=args.cookie,
                    wall_time_ms=args.wall_time_ms,
                    session=args.session,
                )
            )
        elif args.command == "search":
            dump(
                api.search(
                    args.keyword,
                    offset=args.offset,
                    count=args.count,
                    session=args.session,
                    raw=args.raw,
                )
            )
        elif args.command == "video":
            dump(api.video(args.aweme_id, session=args.session, raw=args.raw))
        elif args.command == "comments":
            dump(
                api.comments(
                    args.aweme_id,
                    cursor=args.cursor,
                    count=args.count,
                    session=args.session,
                    raw=args.raw,
                )
            )
        elif args.command == "user":
            dump(api.user(args.sec_uid, session=args.session, raw=args.raw))
        elif args.command == "posts":
            dump(
                api.user_posts(
                    args.sec_uid,
                    max_cursor=args.max_cursor,
                    count=args.count,
                    session=args.session,
                    raw=args.raw,
                )
            )
        elif args.command == "demo":
            save_dir = Path(args.save) if args.save else None
            return demo(api, args.keyword, args.count, save_dir)
        else:  # pragma: no cover - argparse 已限制
            parser.error(f"unknown command {args.command}")
    except ApiError as exc:
        print(f"[-] {exc}", file=sys.stderr)
        if isinstance(exc.payload, dict):
            print(json.dumps(exc.payload, ensure_ascii=False, indent=2), file=sys.stderr)
        return 1
    except urllib.error.URLError as exc:
        print(f"[-] 无法连接 {api.base_url}: {exc.reason}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
