from fastapi import FastAPI, WebSocket
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
import json, time, threading
import serial
import serial.tools.list_ports

CONFIG_PATH = "config.json"

DEFAULT_CONFIG = {
    # Row / distance configuration
    "row_spacing_cm": 60.96,
    "current_row": 2,
    "home_row": 1,          # galley / home base row

    # Time-based calibration
    "pwm": 170,
    "cm_per_sec": 45.0,

    # Serial
    "serial_port": "COM3",  # leave "" to auto-detect
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

# Tracks the final row after the full round-trip is completed
pending_final_row = None


def auto_detect_port() -> str:
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        dev = (p.device or "").lower()
        if (
            ("arduino" in desc)
            or ("wch" in desc)
            or ("usb serial" in desc)
            or ("acm" in dev)
            or ("usb" in hwid)
        ):
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
    global pending_final_row

    while True:
        try:
            lines = serial_mgr.read_lines_nonblocking()
            for line in lines:
                if not line:
                    continue

                serial_mgr.last_status = line

                if line == "DONE":
                    serial_mgr.last_done_ts = time.time()
                    if pending_final_row is not None:
                        cfg["current_row"] = pending_final_row
                        save_config(cfg)
                        pending_final_row = None

                elif line.startswith("STOPPED"):
                    pending_final_row = None

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
    home_row: int
    pwm: int
    cm_per_sec: float


@app.get("/api/config")
def get_config():
    return {
        "row_spacing_cm": cfg["row_spacing_cm"],
        "current_row": cfg["current_row"],
        "home_row": cfg.get("home_row", 1),
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
    cfg["home_row"] = int(body.home_row)
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
    global pending_final_row

    target = int(req.target_row)
    current = int(cfg["current_row"])
    home_row = int(cfg.get("home_row", 1))
    spacing = float(cfg["row_spacing_cm"])

    pwm = clamp_int(cfg.get("pwm", 170), 0, 255)
    cm_per_sec = float(cfg.get("cm_per_sec", 45.0))
    if cm_per_sec <= 0:
        raise RuntimeError("cm_per_sec must be > 0. Calibrate first.")

    delta_rows_out = target - current
    distance_out_cm = abs(delta_rows_out) * spacing
    duration_out_ms = int((distance_out_cm / cm_per_sec) * 1000)

    delta_rows_back = home_row - target
    distance_back_cm = abs(delta_rows_back) * spacing
    duration_back_ms = int((distance_back_cm / cm_per_sec) * 1000)

    direction_out = "F" if delta_rows_out > 0 else "B"
    direction_back = "F" if delta_rows_back > 0 else "B"

    # If already at target, still do 10s stop and return home unless already at home too
    if target == current and target == home_row:
        serial_mgr.send_line("STOP")
        return {
            "ok": True,
            "msg": "Already at target and home row",
            "current_row": current
        }

    # Command format:
    # TRIP <out_dir> <pwm> <out_ms> <pause_ms> <back_dir> <back_ms>
    # Example:
    # TRIP F 170 3000 10000 B 3000
    pause_ms = 10000
    pending_final_row = home_row
    serial_mgr.send_line(
        f"TRIP {direction_out} {pwm} {duration_out_ms} {pause_ms} {direction_back} {duration_back_ms}"
    )

    return {
        "ok": True,
        "from_row": current,
        "target_row": target,
        "home_row": home_row,
        "out_direction": direction_out,
        "out_distance_cm": distance_out_cm,
        "out_duration_ms": duration_out_ms,
        "pause_ms": pause_ms,
        "back_direction": direction_back,
        "back_distance_cm": distance_back_cm,
        "back_duration_ms": duration_back_ms,
        "pwm": pwm
    }


@app.post("/api/stop")
def stop_cart():
    global pending_final_row
    pending_final_row = None
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
            await ws.receive_text()
    except Exception:
        pass
    finally:
        ws_clients.discard(ws)