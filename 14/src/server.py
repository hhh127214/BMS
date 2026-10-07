"""14/ — 平台后端 HTTP 服务入口

启动：
    python 14/src/server.py --host 127.0.0.1 --port 8765

    （默认端口 8765 —— 本项目固定用这个端口，界面与脚本都以它为准）

同时提供两件事：
  1) /api/*  —— 平台 REST 接口（见 api.py 的路由表）
  2) 其余路径 —— 静态文件，默认指向 15/（前端界面）。
     15/ 还不存在时只给一个提示页，不会 500。

只用 Python 标准库。
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.parse
import datetime

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import api  # noqa: E402
import auth  # noqa: E402
import emsdb  # noqa: E402
import engine  # noqa: E402

PROJECT_ROOT = os.path.dirname(emsdb.MODULE_ROOT)
DEFAULT_STATIC = os.path.join(PROJECT_ROOT, "15")

INDEX_FALLBACK = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>EMS 平台 · 后端已就绪</title>
<style>
 body{font-family:system-ui,"Microsoft YaHei",sans-serif;background:#0B2E52;color:#EAF2FB;
      display:flex;align-items:center;justify-content:center;height:100vh;margin:0}
 .c{max-width:620px;line-height:1.85}
 h1{font-size:20px;font-weight:600;margin:0 0 14px}
 code{background:rgba(255,255,255,.12);padding:2px 6px;border-radius:4px}
 a{color:#7FB6E8}
</style></head><body><div class="c">
<h1>后端已就绪，前端界面尚未生成</h1>
<p>接口自检：<a href="/api/health">/api/health</a></p>
<p>前端在 <code>15/</code> 目录。生成后刷新本页即可。</p>
</div></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    server_version = "EmsPlatform/1.0"
    protocol_version = "HTTP/1.1"

    # ---- 基础 ----
    def log_message(self, fmt, *args):  # 收敛日志，避免刷屏
        if os.environ.get("EMS_API_VERBOSE"):
            sys.stderr.write("[api] " + (fmt % args) + "\n")

    def _send(self, status: int, body: bytes, ctype: str,
              extra: dict | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers",
                         "Content-Type, Authorization")
        self.send_header("Access-Control-Allow-Methods",
                         "GET, POST, PUT, DELETE, OPTIONS")
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_json(self, status: int, payload) -> None:
        body = json.dumps(payload, ensure_ascii=False, default=str).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8")

    # ---- 方法 ----
    def do_OPTIONS(self):
        self._send(204, b"", "text/plain")

    def do_HEAD(self):
        self._request("GET", send_body=False)

    def do_GET(self):
        self._request("GET")

    def do_POST(self):
        self._request("POST")

    def do_PUT(self):
        self._request("PUT")

    def do_DELETE(self):
        self._request("DELETE")

    # ---- 主流程 ----
    def _request(self, method: str, send_body: bool = True) -> None:
        parsed = urllib.parse.urlparse(self.path)
        path = urllib.parse.unquote(parsed.path)
        query = urllib.parse.parse_qs(parsed.query)

        if path.startswith("/api/"):
            self._handle_api(method, path, query)
        elif method == "GET":
            self._serve_static(path)
        else:
            self._send_json(405, {"error": f"{method} 不支持静态路径"})

    def _handle_api(self, method: str, path: str, query: dict) -> None:
        body: dict = {}
        try:
            n = int(self.headers.get("Content-Length") or 0)
            if n > 0:
                raw = self.rfile.read(n).decode("utf-8", errors="replace")
                body = json.loads(raw) if raw.strip() else {}
                if not isinstance(body, dict):
                    body = {"value": body}
        except (ValueError, json.JSONDecodeError):
            self._send_json(400, {"error": "请求体不是合法 JSON"})
            return

        token = self._token(query)
        conn = emsdb.connect(self.server.db_path)
        try:
            user = auth.resolve_token(conn, token)
            status, payload = api.dispatch(conn, method, path, query, body, user)
        finally:
            conn.close()

        if isinstance(payload, dict) and payload.get("__raw__"):
            data = payload["body"].encode("utf-8")
            self._send(200, data, payload.get("content_type", "text/plain"),
                       {"Content-Disposition":
                        f'attachment; filename="{payload.get("filename", "export.csv")}"'})
            return
        self._send_json(status, payload)

    def _token(self, query: dict) -> str:
        h = self.headers.get("Authorization") or ""
        if h.lower().startswith("bearer "):
            return h[7:].strip()
        v = query.get("token")
        return (v[0] if v else "") or ""

    # ---- 静态 ----
    def _serve_static(self, path: str) -> None:
        root = self.server.static_dir
        rel = path.lstrip("/")
        if rel in ("", "index.html"):
            rel = "index.html"
        # 目录穿越防护
        full = os.path.normpath(os.path.join(root, rel))
        if not full.startswith(os.path.normpath(root)):
            self._send_json(403, {"error": "非法路径"})
            return
        if os.path.isfile(full):
            ctype = _ctype_of(full)
            with open(full, "rb") as f:
                self._send(200, f.read(), ctype)
            return
        if rel == "index.html":
            self._send(200, INDEX_FALLBACK.encode("utf-8"),
                       "text/html; charset=utf-8")
            return
        # 前端用 history 路由时回落到 index.html
        idx = os.path.join(root, "index.html")
        if os.path.isfile(idx):
            with open(idx, "rb") as f:
                self._send(200, f.read(), "text/html; charset=utf-8")
            return
        self._send_json(404, {"error": f"静态资源不存在: {rel}"})


def _ctype_of(p: str) -> str:
    ext = os.path.splitext(p)[1].lower()
    return {
        ".html": "text/html; charset=utf-8",
        ".js": "application/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8",
        ".json": "application/json; charset=utf-8",
        ".svg": "image/svg+xml",
        ".png": "image/png",
        ".ico": "image/x-icon",
        ".csv": "text/csv; charset=utf-8",
    }.get(ext, "application/octet-stream")


def create_server(host: str, port: int, db_path: str | None = None,
                  static_dir: str | None = None) -> ThreadingHTTPServer:
    conn = emsdb.init_db(db_path)
    n_new = auth.ensure_default_users(conn)
    auth.purge_expired(conn)
    # 接上仍在跑的实时引擎（若上次平台被重启、而现场进程还在写实录）。
    # 判据是"实录文件还在长"，不是 PID —— 见 engine.py 模块头注释。
    try:
        st = engine.ensure_started(conn)
        n_attach = 1 if st.get("attached") else 0
        live_state = st.get("state")
    except Exception as e:      # 引擎接不上不该拦住整个平台启动
        n_attach, live_state = 0, f"attach-failed: {type(e).__name__}: {e}"
    conn.close()

    srv = ThreadingHTTPServer((host, port), Handler)
    srv.daemon_threads = True
    srv.db_path = db_path or emsdb.DEFAULT_DB_PATH          # type: ignore[attr-defined]
    srv.static_dir = static_dir or DEFAULT_STATIC           # type: ignore[attr-defined]
    srv.new_users = n_new                                   # type: ignore[attr-defined]
    srv.engine_attached = n_attach                          # type: ignore[attr-defined]
    srv.engine_live_state = live_state                      # type: ignore[attr-defined]
    return srv


def main() -> int:
    # 被重定向到日志文件时，stdout 默认是块缓冲 —— 启动信息会「迟迟不出现」。
    # 服务类程序必须行缓冲，否则用 `> log` 跑的人会以为没启动。
    try:
        sys.stdout.reconfigure(line_buffering=True, errors="replace")  # type: ignore[union-attr]
        sys.stderr.reconfigure(line_buffering=True, errors="replace")  # type: ignore[union-attr]
    except (AttributeError, ValueError):
        pass

    ap = argparse.ArgumentParser(description="EMS 平台后端服务")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--db", default=None, help="SQLite 文件路径")
    ap.add_argument("--static", default=None, help="静态目录（默认 15/）")
    args = ap.parse_args()

    srv = create_server(args.host, args.port, args.db, args.static)
    db = srv.db_path                                        # type: ignore[attr-defined]
    st = srv.static_dir                                     # type: ignore[attr-defined]

    if not os.path.isfile(os.path.join(st, "index.html")):
        print(f"[warn] 静态目录尚无 index.html: {st}")

    print("=" * 64)
    print(" EMS 平台后端已启动")
    print(f"   地址      http://{args.host}:{args.port}")
    print(f"   接口自检  http://{args.host}:{args.port}/api/health")
    print(f"   数据库    {db}")
    print(f"   静态目录  {st}")
    print(f"   启动时间  {datetime.datetime.now():%Y-%m-%d %H:%M:%S}")
    print(f"   运行引擎  {srv.engine_live_state}")           # type: ignore[attr-defined]
    if srv.engine_attached:                                 # type: ignore[attr-defined]
        print("   [引擎] 已接上仍在跑的实时引擎，继续增量入库")  # type: ignore[attr-defined]
    if srv.new_users:                                       # type: ignore[attr-defined]
        print(f"   [首次初始化] 已建 {srv.new_users} 个默认账号："  # type: ignore[attr-defined]
              "admin/admin123 · operator/operator123 · viewer/viewer123")
        print("   [注意] 现场部署前必须改默认口令（见 14/docs/README.md §5）")
    print("=" * 64)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[stopped] 收到中断，服务已停止")
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
