from fastapi import FastAPI, WebSocket
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
import json, re, time, threading
import serial
import serial.tools.list_ports

CONFIG_PATH = "config.json"

DEFAULT_CONFIG = {
    # Row / distance configuration
    "row_spacing_cm": 60.96,   # 2 ft in cm
    "current_row": 2,          # where the robot is right now

    # Time-based calibration
    "pwm": 170,                # motor speed 0..255
    "cm_per_sec": 45.0,        # YOU MUST calibrate this for your robot/surface

    # Serial
    "serial_port": "COM4",     # leave "" to auto-detect
    "baud": 9600
}

def load_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
            merged = {**DEFAULT_CONFIG, **data}
            return merged
    except Exception:
        return DEFAULT_CONFIG.copy()

def save_config(cfg):
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)

cfg = load_config()

def auto_detect_port() -> str:
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        dev = (p.device or "").lower()
        if ("arduino" in desc) or ("wch" in desc) or ("usb serial" in desc) or ("acm" in dev) or ("usb" in hwid):
            return p.device
    return ports[0].device if ports else ""

def clamp_int(x, lo, hi):
    return max(lo, min(hi, int(x)))

class SerialManager:
    def __init__(self):
        self.lock = threading.RLock()
        self.ser = None
        self.last_status = "DISCONNECTED"
        self.last_done_ts = 0.0

    def connect(self):
        with self.lock:
            if self.ser and self.ser.is_open:
                return
            port = (cfg.get("serial_port") or "").strip() or auto_detect_port()
            if not port:
                raise RuntimeError("No serial port found. Plug in Arduino and try again.")
            cfg["serial_port"] = port
            save_config(cfg)

            self.ser = serial.Serial(port, int(cfg["baud"]), timeout=1)
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
                if not line:
                    continue

                serial_mgr.last_status = line
                if line == "DONE":
                    serial_mgr.last_done_ts = time.time()

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

# -------------------------
# Config API
# -------------------------

class ConfigUpdate(BaseModel):
    row_spacing_cm: float
    current_row: int
    pwm: int
    cm_per_sec: float

@app.get("/api/config")
def get_config():
    return {
        "row_spacing_cm": cfg["row_spacing_cm"],
        "current_row": cfg["current_row"],
        "pwm": cfg.get("pwm", 170),
        "cm_per_sec": cfg.get("cm_per_sec", 45.0),
        "serial_port": cfg.get("serial_port", ""),
        "baud": cfg.get("baud", 9600),
        "last_status": serial_mgr.last_status
    }

@app.post("/api/config")
def update_config(body: ConfigUpdate):
    cfg["row_spacing_cm"] = float(body.row_spacing_cm)
    cfg["current_row"] = int(body.current_row)
    cfg["pwm"] = clamp_int(body.pwm, 0, 255)
    cfg["cm_per_sec"] = float(body.cm_per_sec)
    save_config(cfg)
    return {"ok": True, **cfg}

# -------------------------
# Row movement API
# -------------------------

class GotoRowRequest(BaseModel):
    target_row: int

@app.post("/api/goto_row")
def goto_row(req: GotoRowRequest):
    target = int(req.target_row)
    current = int(cfg["current_row"])
    spacing = float(cfg["row_spacing_cm"])

    delta_rows = target - current
    if delta_rows == 0:
        serial_mgr.send_line("STOP")
        return {"ok": True, "msg": "Already at target", "current_row": current}

    direction = "F" if delta_rows > 0 else "B"
    distance_cm = abs(delta_rows) * spacing

    pwm = clamp_int(cfg.get("pwm", 170), 0, 255)
    cm_per_sec = float(cfg.get("cm_per_sec", 45.0))
    if cm_per_sec <= 0:
        raise RuntimeError("cm_per_sec must be > 0. Calibrate first.")

    duration_ms = int((distance_cm / cm_per_sec) * 1000)

    # Send to Arduino
    serial_mgr.send_line(f"MOVE {direction} {pwm} {duration_ms}")

    # Simple version: update immediately (optimistic)
    # Better version: update when Arduino prints DONE.
    cfg["current_row"] = target
    save_config(cfg)

    return {
        "ok": True,
        "from_row": current,
        "to_row": target,
        "direction": direction,
        "distance_cm": distance_cm,
        "duration_ms": duration_ms,
        "pwm": pwm
    }

@app.post("/api/stop")
def stop_cart():
    serial_mgr.send_line("STOP")
    return {"ok": True}

# -------------------------
# WebSocket for serial logs
# -------------------------

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