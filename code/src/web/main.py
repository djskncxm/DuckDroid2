"""本地启动入口：`python main.py` 等价于 uvicorn app.main:app。

环境变量：
    DUCKAPI_HOST  监听地址，默认 127.0.0.1
    DUCKAPI_PORT  监听端口，默认 8000
"""

from __future__ import annotations

import os


def main() -> None:
    import uvicorn

    uvicorn.run(
        "app.main:app",
        host=os.getenv("DUCKAPI_HOST", "127.0.0.1"),
        port=int(os.getenv("DUCKAPI_PORT", "8000")),
    )


if __name__ == "__main__":
    main()
