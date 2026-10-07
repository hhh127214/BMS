#!/usr/bin/env python
# =====================================================================
# 16/ 测试夹具 —— 极简 TLS 回显服务端（Python ssl，单向回显一次即关）
#
# 为什么必须有它：
#   TLS 客户端要"端到端"证明，就必须有一个**真的** TLS 服务端。
#   自己用 SChannel 在测试进程里再写一个服务端等于把被测代码抄一遍
#   （自证），而 Python 的 ssl 模块是 OpenSSL，是**第三方实现** ——
#   它能和我们握手成功，才说明我们发的不是自娱自乐的字节流。
#
# 证书：同目录 echo_cert.pem / echo_key.pem（自签，CN=localhost，
#       SAN=DNS:localhost,IP:127.0.0.1）。自签是**故意的**：
#       T53 就靠"未信任根 CA 必须被拒"来证明证书校验确实开着。
#
# 用法： tls_echo_server.py <port> [default|tls13only|tls12only|tls11only]
#   default    允许 TLS1.2/1.3（跟随 Python 默认）
#   tls13only  只允许 TLS1.3 —— 用来证明版本协商不是摆设
#   tls12only  只允许 TLS1.2
#   tls11only  只允许 TLS1.1（OpenSSL3 需降到 SECLEVEL=0），
#              用来把"tls12_only 窄化"这件事做成**可观测**差异
#
# 退出： 父进程 kill 即可；收到 EOF on stdin 也退出（便于手工调试）。
# =====================================================================
import os
import socket
import ssl
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
CERT = os.path.join(HERE, "echo_cert.pem")
KEY = os.path.join(HERE, "echo_key.pem")

# 回显循环的空闲超时（秒）。小报文回完就靠它收尾，别调太大，
# 否则每个用例都要多等这么久。
IDLE_TIMEOUT_S = float(os.environ.get("TLS_ECHO_IDLE", "1.0"))


def build_ctx(mode):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    if mode == "tls13only":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    elif mode == "tls12only":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    elif mode == "tls11only":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_1
        ctx.maximum_version = ssl.TLSVersion.TLSv1_1
        # OpenSSL3 默认 SECLEVEL=2 会禁用 TLS1.1；降到 0 才允许
        ctx.set_ciphers("DEFAULT:@SECLEVEL=0")
        try:
            ctx.set_ciphersuites("")
        except Exception:
            pass
    return ctx


def serve(port, mode):
    ctx = build_ctx(mode)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    srv.settimeout(0.5)
    print("READY %d %s" % (port, mode), flush=True)

    # ★ 只在**显式**要求时才把 stdin EOF 当停止信号。
    #   绝对不能用 "sys.stdin.isatty()" 来判断"是不是交互式终端"：
    #   Windows 上把 stdin 接到 NUL 设备时 isatty() 返回 **真**
    #   （NUL 是字符设备），于是被 CreateProcess 拉起时看门狗线程会
    #   立刻读到 EOF → 服务端打印完 READY 就自己退出。
    #   现象是"端口明明刚打印了 READY，却连不上"，且日志里什么都没有 ——
    #   极其难查（本项目实测踩过，见 16/docs/README.md 坑记录）。
    #   默认由父进程 TerminateProcess 收掉。
    stop = {"v": False}
    if os.environ.get("TLS_ECHO_STDIN_SHUTDOWN") == "1":
        def stdin_watch():
            try:
                sys.stdin.read(1)
            except Exception:
                pass
            stop["v"] = True

        threading.Thread(target=stdin_watch, daemon=True).start()

    while not stop["v"]:
        try:
            cs, _ = srv.accept()
        except socket.timeout:
            continue
        except Exception:
            break
        try:
            ss = ctx.wrap_socket(cs, server_side=True)
            # ★ 回显必须是**循环**而不是"收一次就回"：
            #   大报文（256 KiB）在 TCP/TLS 上会被切成很多段，recv() 一次
            #   通常只拿到其中一段。收一次就回 → 客户端拿到的字节数
            #   比发出去的少，测试会把"服务端没读完"误判成"客户端丢包"。
            #   循环的退出条件用**空闲超时**：小报文回完就没数据了，
            #   等 1 秒自然收尾（不能等 EOF —— 客户端在等回显，不会先关）。
            ss.settimeout(IDLE_TIMEOUT_S)
            while True:
                try:
                    data = ss.recv(1 << 16)
                except socket.timeout:
                    break
                if not data:
                    break
                ss.sendall(data)
            try:
                ss.unwrap()
            except Exception:
                pass
            ss.close()
        except Exception as e:  # noqa
            # 握手失败是**预期**路径之一（证书校验测试、明文打到 TLS 口），
            # 只记一行日志，不影响服务端继续服务下一条连接。
            print("conn_error %s: %s" % (type(e).__name__, e), flush=True)
            try:
                cs.close()
            except Exception:
                pass
    try:
        srv.close()
    except Exception:
        pass


if __name__ == "__main__":
    p = int(sys.argv[1]) if len(sys.argv) > 1 else 2443
    m = sys.argv[2] if len(sys.argv) > 2 else "default"
    serve(p, m)
