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
import simproc  # noqa: E402

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


def _cd(filename: str) -> str:
    """Content-Disposition —— 中文文件名必须走 RFC 5987。

    ★ 踩过一次：导出件叫「远动信息参数表_第2步.csv」，直接塞进响应头时
      send_header 按 latin-1 编码，抛 UnicodeEncodeError —— 浏览器那边看到的是
      空响应，服务端日志里只有一条 Traceback。
    ★ 做法：ASCII 的照原样；非 ASCII 给一个 ASCII 兜底名 + filename*=UTF-8''。
      现代浏览器都取 filename*，用户拿到的仍是中文名。
    """
    try:
        filename.encode("latin-1")
        return 'attachment; filename="%s"' % filename
    except UnicodeEncodeError:
        return ("attachment; filename=\"export.csv\"; filename*=UTF-8''"
                + urllib.parse.quote(filename))


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
        self.send_header("Cache-Control", "no-store")
        # ★ 2026-10-11 收紧：原先无条件回 Access-Control-Allow-Origin: *，
        #   等于允许任意第三方网页直接调本项目的 /api/*。
        #   它与「明文 HTTP + 库里默认口令」是**相乘**关系，不是相加：
        #   任一第三方站点都能在用户已登录本平台的情况下，借浏览器里的
        #   会话去读 /api/* —— 用户完全察觉不到。
        #
        #   托管模式下（15/ 由本进程 --static 发布）前端与 API 同源，
        #   浏览器本来就不依赖 CORS 头；15/tests/e2e_probe.html 用的也是
        #   相对路径 w.fetch('/api/health')，同源，照样不受影响。
        #   真正需要跨域的场景（外部页面调测）请用 --allow-origin 显式开，
        #   并把结果打在启动横幅上 —— 开关状态必须看得见。
        ao = getattr(self.server, "allow_origin", None)
        if ao:
            self.send_header("Access-Control-Allow-Origin", ao)
            self.send_header("Access-Control-Allow-Headers",
                             "Content-Type, Authorization")
            self.send_header("Access-Control-Allow-Methods",
                             "GET, POST, PUT, DELETE, OPTIONS")
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
                       {"Content-Disposition": _cd(payload.get("filename", "export.csv"))})
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
        # ★ 只判 startswith(root) 是**不够**的：`15` 与 `15_backup` 这种
        #   前缀相同的兄弟目录也能过 —— `/../15_backup/x` 归一化后确实以
        #   `...\15` 开头。必须再要求下一个字符是分隔符（或正好等于 root）。
        #   2026-10-19 体检发现，判据见 14/tests/selftest.py [G12]。
        root_n = os.path.normpath(root)
        full = os.path.normpath(os.path.join(root_n, rel))
        if full != root_n and not full.startswith(root_n + os.sep):
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


class ApiServer(ThreadingHTTPServer):
    """多线程服务 + 不制造噪声的报错。

    ★ 为什么要单独一层：`ThreadingHTTPServer` 继承自 `socketserver.BaseServer`，
      它的 `handle_error()` 会把**每一个**请求异常打成一整屏堆栈。而客户端
      先断开（关标签页 / 浏览器取消请求 / 预检被撤）是正常的日常行为，
      Windows 上表现为 WinError 10054，每来一次就是 ~20 行。
      实测一轮巡检的日志：**1228 行里 52 个 Traceback，100% 是它** ——
      真正值得看的错误被淹没在一屏屏噪声里，等于日志不可用。
      与 `Handler.log_message` 同一个纪律：日常噪声关掉，真异常照样留。
    """
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address) -> None:
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionResetError, BrokenPipeError)):
            if os.environ.get("EMS_API_VERBOSE"):
                sys.stderr.write("[api] 客户端先断开了 %s\n" % (client_address,))
            return
        if isinstance(exc, TimeoutError):
            return
        sys.stderr.write("-" * 40 + "\n")
        sys.stderr.write("[api] 处理来自 %s 的请求出错\n" % (client_address,))
        import traceback
        traceback.print_exc()
        sys.stderr.write("-" * 40 + "\n")


def create_server(host: str, port: int, db_path: str | None = None,
                  static_dir: str | None = None,
                  allow_origin: str | None = None) -> ThreadingHTTPServer:
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
    # 按 PID 接管遗留的模拟链路进程（平台重启 ≠ 现场进程退出）。
    # 不做这一步，面板会说「链路未启动」而数据在涨，再点「换一个进程」会拉起
    # 第二套抢同一个端口。引擎只认「实录还在长」，链路的其余四个角色
    # （实时库 / 替身 / 网关 / 主站）只能靠 pid 认回来 —— 见 simproc.attach()。
    try:
        simproc.CHAIN.attach(conn)
        n_chain = simproc.CHAIN.status(conn).get("counts", {}).get("up", 0)
    except Exception:
        n_chain = 0
    conn.close()

    srv = ApiServer((host, port), Handler)
    srv.daemon_threads = True
    srv.db_path = db_path or emsdb.DEFAULT_DB_PATH          # type: ignore[attr-defined]
    srv.static_dir = static_dir or DEFAULT_STATIC           # type: ignore[attr-defined]
    srv.new_users = n_new                                   # type: ignore[attr-defined]
    srv.engine_attached = n_attach                          # type: ignore[attr-defined]
    srv.engine_live_state = live_state                      # type: ignore[attr-defined]
    srv.chain_attached = n_chain                            # type: ignore[attr-defined]
    # None = 不回 Access-Control-Allow-Origin（同源模式，默认，最安全）
    srv.allow_origin = allow_origin or None                 # type: ignore[attr-defined]
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
    ap.add_argument("--allow-origin", default=None,
                    help="跨域白名单，如 http://127.0.0.1:5173；"
                         "不给则不回 Access-Control-Allow-Origin（同源模式）")
    args = ap.parse_args()

    srv = create_server(args.host, args.port, args.db, args.static,
                        args.allow_origin)
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
    ao = srv.allow_origin                                    # type: ignore[attr-defined]
    if ao:
        print(f"   跨域(CORS) 开启 → {ao}")
    else:
        print("   跨域(CORS) 关闭（同源模式，默认）"
              "  ← 外部页面要调测就加 --allow-origin <origin>")
    if srv.engine_attached:                                 # type: ignore[attr-defined]
        print("   [引擎] 已接上仍在跑的实时引擎，继续增量入库")  # type: ignore[attr-defined]
    if srv.chain_attached:                                  # type: ignore[attr-defined]
        print(f"   [模拟链路] 已按 PID 接管 {srv.chain_attached} 个仍在跑的进程"  # type: ignore[attr-defined]
              "（不会重复拉起）")
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
