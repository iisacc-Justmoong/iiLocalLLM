"""WebFetch TLS peer checks pinned-IP Host/SNI and certificate verification."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading


def main():
    executable = str(Path(sys.argv[1]).resolve())
    env = dict(os.environ)
    for key in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LIBRARY_PATH"):
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="web-tls-", dir=Path.cwd()) as temporary:
        root = Path(temporary)
        config = root / "openssl.cnf"
        config.write_text("[req]\ndistinguished_name=dn\nx509_extensions=extensions\nprompt=no\n[dn]\nCN=localhost\n[extensions]\nsubjectAltName=DNS:localhost\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\nextendedKeyUsage=serverAuth\n")
        cert, key = root / "cert.pem", root / "key.pem"
        subprocess.run([shutil.which("openssl"), "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-config", str(config), "-keyout", str(key), "-out", str(cert)], capture_output=True, check=True, timeout=30)
        key.chmod(0o600)
        received = []

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def do_GET(self):
                request = {"path": self.path, "cookie": self.headers.get("Cookie"), "auth": self.headers.get("Authorization")}
                record = {"sni": getattr(self.connection, "hook_server_name", None), "host": self.headers["Host"], "body": request}
                received.append(record)
                body = json.dumps(record).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        context.set_servername_callback(lambda socket, name, _context: setattr(socket, "hook_server_name", name))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            port = server.server_port

            def probe(host, ca):
                result = subprocess.run([executable, f"https://{host}:{port}/hook", str(ca), "private"], cwd=root, env=env,
                                        capture_output=True, text=True, check=True, timeout=15)
                return json.loads(result.stdout)

            trusted = probe("localhost", cert)
            assert trusted["passed"], trusted
            record = json.loads(trusted["result"])
            assert record["sni"] == "localhost", record
            assert record["host"] == f"localhost:{port}", record
            assert record["body"] == {"path": "/hook", "cookie": None, "auth": None}, record
            for result in (probe("127.0.0.1", cert), probe("localhost", "-")):
                assert not result["passed"] and "TLS" in result["error"], result
            assert len(received) == 1, received
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)
    print(json.dumps({"passed": True, "sni": True, "host": True, "untrusted_rejected": True, "wrong_host_rejected": True}))


if __name__ == "__main__":
    main()
