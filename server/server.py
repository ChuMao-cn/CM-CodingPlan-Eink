from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
import subprocess
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent
last_payload = json.loads((ROOT / "display.json").read_text(encoding="utf-8"))


def read_ark_text():
    try:
        result = subprocess.run(
            ["osascript", str(ROOT / "scrape.applescript")],
            capture_output=True,
            text=True,
            timeout=5,
        )
        return result.stdout.strip() if result.returncode == 0 else ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def parse_usage(text):
    labels = {"five_hour": "近5小时用量", "one_week": "近一周用量", "one_month": "近一月用量"}
    payload = {}
    for key, label in labels.items():
        match = re.search(label + r".{0,300}?(\d+(?:\.\d+)?)\s*%", text, re.S)
        if not match:
            return None
        payload[key] = match.group(1) + "%"

    segment = text.split(labels["five_hour"], 1)[-1].split(labels["one_week"], 1)[0]
    match = re.search(r"(?:(\d+)天)?\s*(?:(\d+)小时)?\s*(?:(\d+)分(?:钟)?)?后重置", segment)
    if not match:
        return None
    days, hours, minutes = (int(value or 0) for value in match.groups())
    payload["reset"] = str(days * 24 * 60 + hours * 60 + minutes) + "分钟"
    return payload


def current_payload():
    global last_payload
    parsed = parse_usage(read_ark_text())
    if parsed:
        last_payload = parsed
    return last_payload


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        (ROOT / "server.log").open("a", encoding="utf-8").write(
            datetime.now().strftime("%Y-%m-%d %H:%M:%S ")
            + self.client_address[0]
            + " "
            + (format % args)
            + "\n"
        )

    def do_GET(self):
        if self.path != "/display.json":
            self.send_response(404)
            self.end_headers()
            return
        payload = current_payload()
        payload["updated"] = datetime.now().strftime("%H:%M")
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    ThreadingHTTPServer(("0.0.0.0", 8000), Handler).serve_forever()
