from fastapi import FastAPI, WebSocket
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
import json, re, time, threading
import serial
import serial.tools.list_ports

CONFIG_PATH = "config.json"
DEFAULT_CONFIG = {
    "seat_spacing_cm": 80.0,     # user-editable in UI
    "home_row": 1,               # where the cart starts (row number)
    "serial_port": "",           # auto-detect if empty
    "baud": 9600
}

def load_config():
    try:
        with open(CONFIG_PATH, "r") as f:
            return {**DEFAULT_CONFIG, **json.load(f)}
    except Exception:
        return DEFAULT_CONFIG.copy()

def save_config(cfg):
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)

cfg = load_config()

def auto_detect_port() -> str:
    ports = list(serial.tools.list_ports.comports())
    # common Arduino identifiers
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "arduino" in desc or "wch" in desc or "usb serial" in desc or "acm" in p.device.lower():
            return p.device
    # fallback: first port
    return ports[0].device if ports else ""

class SerialManager:
    def __init__(self):
        self.lock = threading.Lock()
        self.ser = None
        self.last_status = "DISCONNECTED"

    def connect(self):
        with self.lock:
            if self.ser and self.ser.is_open:
                return
            port = cfg["serial_port"] or auto_detect_port()
            if not port:
                raise RuntimeError("No serial port found. Plug in Arduino and try again.")
            cfg["serial_port"] = port
            save_config(cfg)

            self.ser = serial.Serial(port, cfg["baud"], timeout=1)
            time.sleep(2)  # allow Arduino reset
            self.last_status = f"CONNECTED {port}"

    def send_line(self, line: str):
        with self.lock:
            if not self.ser or not self.ser.is_open:
                self.connect()
            self.ser.write((line.strip() + "\n").encode("utf-8"))

    def read_lines_nonblocking(self):
        out = []
        with self.lock:
            if not self.ser or not self.ser.is_open:
                return out
            while self.ser.in_waiting:
                out.append(self.ser.readline().decode(errors="ignore").strip())
        return out

serial_mgr = SerialManager()
ws_clients = set()

def serial_reader_loop():
    while True:
        try:
            lines = serial_mgr.read_lines_nonblocking()
            for line in lines:
                if line:
                    serial_mgr.last_status = line
                    dead = []
                    for ws in list(ws_clients):
                        try:
                            ws.send_text(line)
                        except Exception:
                            dead.append(ws)
                    for ws in dead:
                        ws_clients.discard(ws)
        except Exception as e:
            serial_mgr.last_status = f"SERIAL_ERR: {e}"
        time.sleep(0.05)

threading.Thread(target=serial_reader_loop, daemon=True).start()

app = FastAPI()
app.mount("/static", StaticFiles(directory="static"), name="static")

@app.get("/")
def root():
    with open("static/index.html", "r", encoding="utf-8") as f:
        return HTMLResponse(f.read())

class ConfigUpdate(BaseModel):
    seat_spacing_cm: float
    home_row: int

@app.get("/api/config")
def get_config():
    return {
        "seat_spacing_cm": cfg["seat_spacing_cm"],
        "home_row": cfg["home_row"],
        "serial_port": cfg["serial_port"],
        "baud": cfg["baud"],
        "last_status": serial_mgr.last_status
    }

@app.post("/api/config")
def update_config(body: ConfigUpdate):
    cfg["seat_spacing_cm"] = float(body.seat_spacing_cm)
    cfg["home_row"] = int(body.home_row)
    save_config(cfg)
    return {"ok": True, **cfg}

class SeatRequest(BaseModel):
    seat: str

def parse_row(seat: str) -> int:
    m = re.match(r"^\s*(\d+)\s*[A-Za-z]?\s*$", seat)
    if not m:
        raise ValueError("Invalid seat format. Use e.g. 12B")
    return int(m.group(1))

@app.post("/api/call")
def call_cart(req: SeatRequest):
    row = parse_row(req.seat)
    distance_cm = abs(row - cfg["home_row"]) * cfg["seat_spacing_cm"]
    serial_mgr.send_line(f"GO {distance_cm:.1f}")
    return {"ok": True, "seat": req.seat, "row": row, "distance_cm": distance_cm}

@app.post("/api/stop")
def stop_cart():
    serial_mgr.send_line("STOP")
    return {"ok": True}

@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    ws_clients.add(ws)
    try:
        await ws.send_text("WS_CONNECTED")
        while True:
            await ws.receive_text()  # keepalive
    except Exception:
        pass
    finally:
        ws_clients.discard(ws)
