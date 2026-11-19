# ============================================================================
#  ShardGrid — Network Display API Server
#  Replaces damaged OLED: reads ESP32 Central serial output over USB,
#  parses events, and serves a live dashboard accessible from any device
#  on the same network.
#
#  Usage:  python server.py --port COM5        (Windows)
#          python server.py --port /dev/ttyUSB0 (Linux/Mac)
#
#  Then open  http://<your-ip>:8077  from any device on the LAN.
# ============================================================================

import argparse
import asyncio
import json
import re
import threading
import time
from collections import deque
from contextlib import asynccontextmanager
from datetime import datetime

import serial
import serial.tools.list_ports
import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, FileResponse
from fastapi.staticfiles import StaticFiles
from pathlib import Path

# ---------------------------------------------------------------------------
#  Global State
# ---------------------------------------------------------------------------

class SystemState:
    """Thread-safe container for everything parsed from ESP32 serial."""

    def __init__(self):
        self.lock = threading.Lock()

        # Core display replacement
        self.reconstructed_message = None      # The final recovered plaintext
        self.reconstructed_len = 0
        self.reconstructed_at = None           # ISO timestamp

        # Key & Payload sharing across laptops
        self.encryption_key = None
        self.last_plaintext = None
        self.last_plaintext_len = 0
        self.shares = []

        # Node status
        self.state = "INIT"
        self.ready = False
        self.tampered = False
        self.system_locked = False
        self.key_loaded = False

        # Share tracking
        self.share_a_received = False
        self.share_b_received = False
        self.shares_distributed = False

        # Incoming message from Uno
        self.last_plaintext = None
        self.last_plaintext_len = 0

        # Event log (last 200 lines)
        self.event_log = deque(maxlen=200)

        # Serial connection
        self.serial_connected = False
        self.serial_port = None

        # WebSocket subscribers
        self.ws_clients: set[WebSocket] = set()

    def snapshot(self) -> dict:
        with self.lock:
            return {
                "reconstructed_message": self.reconstructed_message,
                "reconstructed_len": self.reconstructed_len,
                "reconstructed_at": self.reconstructed_at,
                "encryption_key": self.encryption_key,
                "last_plaintext": self.last_plaintext,
                "last_plaintext_len": self.last_plaintext_len,
                "shares": self.shares,
                "state": self.state,
                "ready": self.ready,
                "tampered": self.tampered,
                "system_locked": self.system_locked,
                "key_loaded": self.key_loaded,
                "share_a_received": self.share_a_received,
                "share_b_received": self.share_b_received,
                "shares_distributed": self.shares_distributed,
                "serial_connected": self.serial_connected,
                "serial_port": self.serial_port,
                "event_count": len(self.event_log),
            }

    def add_event(self, msg: str, etype: str = "info"):
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        event = {"time": ts, "type": etype, "message": msg}
        with self.lock:
            self.event_log.append(event)
        return event

    def get_events(self, last_n: int = 50) -> list:
        with self.lock:
            items = list(self.event_log)
        return items[-last_n:]


state = SystemState()

# ---------------------------------------------------------------------------
#  Serial Reader (background thread)
# ---------------------------------------------------------------------------

# Regex patterns for parsing Central ESP32 serial output
RE_RECONSTRUCTED = re.compile(
    r"\[CENTRAL\] Reconstructed message \((\d+) bytes?\): (.+)"
)
RE_PLAINTEXT = re.compile(
    r"\[CENTRAL\] Received plaintext message \((\d+) bytes?\): (.+)"
)
RE_SHARE_A = re.compile(r"\[CENTRAL\] Received share .+ back from Node A")
RE_SHARE_B = re.compile(r"\[CENTRAL\] Received share .+ back from Node B")


def _broadcast(event: dict):
    """Queue a WebSocket broadcast (called from serial thread)."""
    # We'll use the asyncio loop from the main thread
    if hasattr(_broadcast, "loop") and _broadcast.loop:
        asyncio.run_coroutine_threadsafe(
            _ws_broadcast(event), _broadcast.loop
        )


async def _ws_broadcast(event: dict):
    dead = set()
    for ws in state.ws_clients.copy():
        try:
            await ws.send_json(event)
        except Exception:
            dead.add(ws)
    state.ws_clients -= dead


def parse_serial_line(line: str):
    """Parse one line of ESP32 serial output and update state."""
    line = line.strip()
    if not line:
        return

    # --- Reconstructed message (THE key output that replaces OLED) ---
    m = RE_RECONSTRUCTED.match(line)
    if m:
        msg_len = int(m.group(1))
        msg_text = m.group(2)
        with state.lock:
            state.reconstructed_message = msg_text
            state.reconstructed_len = msg_len
            state.reconstructed_at = datetime.now().isoformat()
            state.state = "DISPLAY_MSG"
        event = state.add_event(
            f"MESSAGE RECOVERED: \"{msg_text}\" ({msg_len} bytes)",
            "reconstruct"
        )
        _broadcast({"type": "reconstructed", "data": {
            "message": msg_text, "length": msg_len,
            "timestamp": state.reconstructed_at
        }})
        _broadcast({"type": "event", "data": event})
        return

    # --- Incoming plaintext from Uno ---
    m = RE_PLAINTEXT.match(line)
    if m:
        pt_len = int(m.group(1))
        pt_text = m.group(2)
        with state.lock:
            state.last_plaintext = pt_text
            state.last_plaintext_len = pt_len
            state.state = "PROCESSING"
        event = state.add_event(
            f"Plaintext received from Uno: \"{pt_text}\" ({pt_len}B)", "data"
        )
        _broadcast({"type": "event", "data": event})
        return

    # --- Share tracking ---
    if RE_SHARE_A.match(line):
        with state.lock:
            state.share_a_received = True
        event = state.add_event("Share returned from Node A", "share")
        _broadcast({"type": "share", "data": {"node": "A", "status": "received"}})
        _broadcast({"type": "event", "data": event})
        return

    if RE_SHARE_B.match(line):
        with state.lock:
            state.share_b_received = True
        event = state.add_event("Share returned from Node B", "share")
        _broadcast({"type": "share", "data": {"node": "B", "status": "received"}})
        _broadcast({"type": "event", "data": event})
        return

    # --- Key events ---
    if line == "READY":
        with state.lock:
            state.ready = True
            state.state = "READY"
        event = state.add_event("System READY", "success")
        _broadcast({"type": "state", "data": {"state": "READY"}})
        _broadcast({"type": "event", "data": event})
        return

    if "SHARE_DIST_OK" in line:
        with state.lock:
            state.shares_distributed = True
            state.share_a_received = False
            state.share_b_received = False
            state.state = "WAITING_SHARES"
        event = state.add_event("Shares distributed to Node A & B", "success")
        _broadcast({"type": "state", "data": {"state": "WAITING_SHARES"}})
        _broadcast({"type": "event", "data": event})
        return

    if line == "RECONSTRUCT_OK":
        event = state.add_event("Reconstruction verified OK", "success")
        _broadcast({"type": "event", "data": event})
        return

    if line == "TAMPER_ALERT":
        with state.lock:
            state.tampered = True
            state.system_locked = True
            state.state = "TAMPERED"
        event = state.add_event("TAMPER DETECTED — keys destroyed!", "danger")
        _broadcast({"type": "state", "data": {"state": "TAMPERED"}})
        _broadcast({"type": "event", "data": event})
        return

    if line == "KEY_SET_OK":
        with state.lock:
            state.key_loaded = True
        event = state.add_event("AES-256 key provisioned", "success")
        _broadcast({"type": "event", "data": event})
        return

    if line == "ZEROIZE_DONE":
        with state.lock:
            state.system_locked = True
            state.state = "LOCKED"
        event = state.add_event("ZEROIZATION COMPLETE — system locked", "danger")
        _broadcast({"type": "state", "data": {"state": "LOCKED"}})
        _broadcast({"type": "event", "data": event})
        return

    if "Key loaded from EEPROM" in line:
        with state.lock:
            state.key_loaded = True
        event = state.add_event("Key loaded from EEPROM", "info")
        _broadcast({"type": "event", "data": event})
        return

    if "SSS splitting" in line or "Message split" in line:
        event = state.add_event(line.strip(), "info")
        _broadcast({"type": "event", "data": event})
        return

    if "Transmitting Share" in line:
        with state.lock:
            state.state = "TRANSMITTING"
        event = state.add_event(line.strip(), "info")
        _broadcast({"type": "state", "data": {"state": "TRANSMITTING"}})
        _broadcast({"type": "event", "data": event})
        return

    # --- Generic log line ---
    if line.startswith("[") or line.startswith("==="):
        etype = "info"
        if "error" in line.lower() or "fail" in line.lower():
            etype = "error"
        elif "TAMPER" in line:
            etype = "danger"
        event = state.add_event(line, etype)
        _broadcast({"type": "event", "data": event})


def serial_reader_thread(port_name: str, baud: int = 115200):
    """Background thread: read ESP32 Central serial output forever."""
    while True:
        try:
            state.add_event(f"Connecting to {port_name} @ {baud}...", "info")
            ser = serial.Serial(port_name, baud, timeout=1)
            with state.lock:
                state.serial_connected = True
                state.serial_port = port_name
            state.add_event(f"Serial connected: {port_name}", "success")
            _broadcast({"type": "serial", "data": {"connected": True, "port": port_name}})

            while True:
                raw = ser.readline()
                if raw:
                    try:
                        line = raw.decode("utf-8", errors="replace").strip()
                    except Exception:
                        line = str(raw)
                    if line:
                        parse_serial_line(line)

        except serial.SerialException as e:
            with state.lock:
                state.serial_connected = False
            state.add_event(f"Serial error: {e} — retrying in 3s", "error")
            _broadcast({"type": "serial", "data": {"connected": False}})
            time.sleep(3)
        except Exception as e:
            state.add_event(f"Unexpected error: {e}", "error")
            time.sleep(3)


# ---------------------------------------------------------------------------
#  FastAPI Application
# ---------------------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):
    # Store the event loop so the serial thread can broadcast
    _broadcast.loop = asyncio.get_running_loop()
    yield

app = FastAPI(
    title="ShardGrid Network Display",
    description="OLED replacement — serves reconstructed messages over LAN",
    version="1.0.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# Serve static files (the dashboard HTML/CSS/JS)
templates_dir = Path(__file__).parent / "templates"
web_dashboard_dir = Path(__file__).resolve().parent.parent / "web-dashboard"

# --- REST Endpoints ---

@app.get("/", response_class=HTMLResponse)
async def serve_root():
    """Serve the complete ShardGrid Web Dashboard (LAN synchronized)."""
    index_file = web_dashboard_dir / "index.html"
    if index_file.exists():
        return HTMLResponse(content=index_file.read_text(encoding="utf-8"))
    return HTMLResponse(content=(templates_dir / "display.html").read_text(encoding="utf-8"))

@app.get("/oled", response_class=HTMLResponse)
async def serve_oled_view():
    """Serve the dedicated Virtual OLED full-screen replacement."""
    html_path = templates_dir / "display.html"
    return HTMLResponse(content=html_path.read_text(encoding="utf-8"))

# Mount CSS/JS for web-dashboard
if web_dashboard_dir.exists():
    app.mount("/css", StaticFiles(directory=str(web_dashboard_dir / "css")), name="css")
    app.mount("/js", StaticFiles(directory=str(web_dashboard_dir / "js")), name="js")


@app.get("/api/status")
async def get_status():
    """Full system status snapshot."""
    return state.snapshot()


@app.post("/api/sync")
async def sync_state(payload: dict):
    """Sync key, message, and shares from any laptop dashboard."""
    key = payload.get("key")
    msg = payload.get("message")
    shares = payload.get("shares")
    reconstructed = payload.get("reconstructed")

    with state.lock:
        if key:
            state.encryption_key = key
            state.key_loaded = True
            state.add_event(f"Encryption key shared: {key[:16]}...", "success")
        if msg is not None:
            state.last_plaintext = msg
            state.last_plaintext_len = len(msg)
            if shares:
                state.shares = shares
            state.state = "PROCESSING"
            state.add_event(f"Plaintext payload synced: \"{msg}\"", "data")
        if reconstructed:
            state.reconstructed_message = reconstructed
            state.reconstructed_len = len(reconstructed)
            state.reconstructed_at = datetime.now().isoformat()
            state.state = "DISPLAY_MSG"
            state.add_event(f"MESSAGE RECOVERED: \"{reconstructed}\"", "reconstruct")

    # Broadcast to all connected WebSockets
    if key:
        await _ws_broadcast({"type": "key_update", "key": key})
    if msg is not None:
        await _ws_broadcast({"type": "message_sent", "message": msg, "shares": state.shares})
    if reconstructed:
        await _ws_broadcast({"type": "reconstructed", "data": {
            "message": reconstructed,
            "length": len(reconstructed),
            "timestamp": state.reconstructed_at
        }})

    return {"status": "ok", "state": state.snapshot()}


@app.get("/api/sync")
async def get_sync():
    """Retrieve currently active synchronized state."""
    return state.snapshot()


@app.get("/api/message")
async def get_message():
    """Get the latest reconstructed message (OLED replacement)."""
    with state.lock:
        return {
            "message": state.reconstructed_message,
            "length": state.reconstructed_len,
            "timestamp": state.reconstructed_at,
            "last_plaintext": state.last_plaintext,
            "encryption_key": state.encryption_key,
            "state": state.state,
        }


@app.get("/api/events")
async def get_events(last: int = 50):
    """Get recent event log entries."""
    return state.get_events(last)


@app.get("/api/health")
async def health():
    return {
        "status": "ok",
        "serial_connected": state.serial_connected,
        "serial_port": state.serial_port,
        "uptime_clients": len(state.ws_clients),
    }


# --- WebSocket (real-time push) ---

@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    state.ws_clients.add(ws)
    try:
        # Send current state on connect
        await ws.send_json({
            "type": "init",
            "data": state.snapshot(),
            "events": state.get_events(50),
        })

        # Process incoming synchronization from clients
        while True:
            text = await ws.receive_text()
            if not text:
                continue
            try:
                data = json.loads(text)
            except Exception:
                continue

            msg_type = data.get("type")
            if msg_type == "key_update":
                key = data.get("key")
                with state.lock:
                    state.encryption_key = key
                    state.key_loaded = True
                state.add_event(f"Encryption key synced: {key[:16]}...", "success")
                await _ws_broadcast({"type": "key_update", "key": key})

            elif msg_type == "message_sent":
                msg = data.get("message", "")
                shares = data.get("shares", [])
                with state.lock:
                    state.last_plaintext = msg
                    state.last_plaintext_len = len(msg)
                    state.shares = shares
                    state.state = "PROCESSING"
                state.add_event(f"Payload transmitted: \"{msg}\"", "data")
                await _ws_broadcast({"type": "message_sent", "message": msg, "shares": shares})

            elif msg_type == "tx_started":
                msg = data.get("message", "")
                with state.lock:
                    state.last_plaintext = msg
                    state.state = "RECEIVING"
                state.add_event(f"Optical pulse transmission started (LED blinking)...", "info")
                await _ws_broadcast({"type": "tx_started", "message": msg})

            elif msg_type == "reconstructed":
                rec_data = data.get("data") if isinstance(data.get("data"), dict) else data
                rec = rec_data.get("message", "")
                rec_len = rec_data.get("length", len(rec))
                with state.lock:
                    state.reconstructed_message = rec
                    state.reconstructed_len = rec_len
                    state.reconstructed_at = datetime.now().isoformat()
                    state.state = "DISPLAY_MSG"
                state.add_event(f"MESSAGE RECOVERED: \"{rec}\"", "reconstruct")
                await _ws_broadcast({"type": "reconstructed", "data": {
                    "message": rec,
                    "length": rec_len,
                    "timestamp": state.reconstructed_at
                }})

    except WebSocketDisconnect:
        pass
    finally:
        state.ws_clients.discard(ws)


# ---------------------------------------------------------------------------
#  CLI Entry Point
# ---------------------------------------------------------------------------

def list_serial_ports():
    """List available serial ports for user convenience."""
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("  (no serial ports detected)")
    for p in ports:
        print(f"  {p.device:12s}  {p.description}")


def main():
    parser = argparse.ArgumentParser(
        description="ShardGrid Network Display — OLED replacement server"
    )
    parser.add_argument(
        "--port", "-p", nargs="?", const="auto", default=None,
        help="Serial port of Central ESP32 (e.g. COM5). "
             "Omit value to auto-detect."
    )
    parser.add_argument(
        "--baud", "-b", type=int, default=115200,
        help="Baud rate (default: 115200)"
    )
    parser.add_argument(
        "--host", default="0.0.0.0",
        help="Bind address (default: 0.0.0.0 — all interfaces)"
    )
    parser.add_argument(
        "--web-port", type=int, default=8077,
        help="HTTP port (default: 8077)"
    )
    parser.add_argument(
        "--list-ports", action="store_true",
        help="List available serial ports and exit"
    )
    parser.add_argument(
        "--demo", action="store_true",
        help="Run without serial (demo/test mode)"
    )
    args = parser.parse_args()

    if args.list_ports:
        print("Available serial ports:")
        list_serial_ports()
        return

    # Auto-detect port if not specified or set to auto
    serial_port = args.port
    if (not serial_port or serial_port == "auto") and not args.demo:
        ports = serial.tools.list_ports.comports()
        # Prefer CH9102 / ESP32 port first (Central Node), then CH340 / other serial
        esp_ports = [p for p in ports if "CH9102" in p.description or "CP210" in p.description or "ESP32" in p.description]
        if not esp_ports:
            esp_ports = [p for p in ports if "CH340" in p.description or "USB" in p.description.upper() or "SERIAL" in p.description.upper()]
        
        if esp_ports:
            serial_port = esp_ports[0].device
            print(f"Auto-detected serial port: {serial_port} ({esp_ports[0].description})")
        else:
            print("No serial port detected. Running in Network Hub mode (LAN Sync active).")
            serial_port = None

    print(r"""
  ____  _                   _  ____      _     _
 / ___|| |__   __ _ _ __ __| |/ ___|_ __(_) __| |
 \___ \| '_ \ / _` | '__/ _` | |  _| '__| |/ _` |
  ___) | | | | (_| | | | (_| | |_| | |  | | (_| |
 |____/|_| |_|\__,_|_|  \__,_|\____|_|  |_|\__,_|
      Network Display — OLED Replacement Server
    """)

    import socket
    hostname = socket.gethostname()
    local_ips = socket.gethostbyname_ex(hostname)[2]
    local_ips = [ip for ip in local_ips if not ip.startswith("127.")]

    print(f"  Serial port : {serial_port or 'DEMO MODE (no serial)'}")
    print(f"  Baud rate   : {args.baud}")
    print(f"  Web server  : http://0.0.0.0:{args.web_port}")
    print()
    print("  Access from any device on your network:")
    for ip in local_ips:
        print(f"    → http://{ip}:{args.web_port}")
    print()

    # Start serial reader thread
    if serial_port:
        t = threading.Thread(
            target=serial_reader_thread,
            args=(serial_port, args.baud),
            daemon=True,
        )
        t.start()
    else:
        state.add_event("DEMO MODE — no serial connected", "warning")

    # Start web server
    uvicorn.run(app, host=args.host, port=args.web_port, log_level="warning")


if __name__ == "__main__":
    main()
