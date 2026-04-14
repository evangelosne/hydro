from fastapi import FastAPI, WebSocket, Body
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
import json, time, threading, asyncio
import serial

CONFIG_PATH = "bluetooth_config.json"

DEFAULT_CONFIG = {
    "row_spacing_cm": 60.96,
    "current_row": 1,
    "home_row": 1,
    "pwm": 170,
    "cm_per_sec": 12.2,
    "serial_port_1": "COM5",
    "serial_port_2": "COM8",
    "baud": 9600,
}


def load_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
            return {**DEFAULT_CONFIG, **data}
    except Exception:
        return DEFAULT_CONFIG.copy()


def save_config(cfg):
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)


cfg = load_config()
pending_final_row = None


def clamp_int(x, lo, hi):
    return max(lo, min(hi, int(float(x))))


_loop: asyncio.AbstractEventLoop = None


class SerialManager:
    def __init__(self, port_key: str, label: str):
        self.port_key     = port_key
        self.label        = label
        self.lock         = threading.RLock()
        self.ser          = None
        self.last_status  = "DISCONNECTED"
        self.last_done_ts = 0.0

    def connect(self):
        with self.lock:
            if self.ser and self.ser.is_open:
                return
            port = (cfg.get(self.port_key) or "").strip()
            if not port:
                raise RuntimeError(
                    f"[{self.label}] No port configured. "
                    f"Set {self.port_key} in bluetooth_config.json."
                )
            self.ser = serial.Serial(port, int(cfg["baud"]), timeout=1)
            time.sleep(1)
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


serial_mgr_1 = SerialManager("serial_port_1", "CART1")
serial_mgr_2 = SerialManager("serial_port_2", "CART2")
ALL_CARTS    = [serial_mgr_1, serial_mgr_2]

ws_clients: set[WebSocket] = set()


async def _broadcast(msg: str):
    dead = set()
    for ws in ws_clients:
        try:
            await ws.send_text(msg)
        except Exception:
            dead.add(ws)
    ws_clients.difference_update(dead)


def stop_all_carts():
    for mgr in ALL_CARTS:
        try:
            mgr.send_line("STOP")
        except Exception:
            pass


def pause_other_carts(except_mgr):
    """Send PAUSE to every cart except the one that triggered the stop."""
    for mgr in ALL_CARTS:
        if mgr is not except_mgr:
            try:
                mgr.send_line("PAUSE")
            except Exception:
                pass


def resume_other_carts(except_mgr):
    """Send RESUME to every cart except the one that self-resumed."""
    for mgr in ALL_CARTS:
        if mgr is not except_mgr:
            try:
                mgr.send_line("RESUME")
            except Exception:
                pass


def serial_reader_loop():
    global pending_final_row

    while True:
        try:
            for mgr in ALL_CARTS:
                lines = mgr.read_lines_nonblocking()
                for line in lines:
                    if not line:
                        continue

                    mgr.last_status = line
                    tagged = f"[{mgr.label}] {line}"

                    if _loop is not None:
                        asyncio.run_coroutine_threadsafe(_broadcast(tagged), _loop)

                    # ── One cart hit an obstacle → pause all others ──
                    if line.startswith("SAFETY_STOP"):
                        pause_other_carts(mgr)
                        if _loop is not None:
                            asyncio.run_coroutine_threadsafe(
                                _broadcast(f"[SYSTEM] {mgr.label} obstacle detected — all carts paused"),
                                _loop,
                            )

                    # ── One cart resumed → resume all others ──
                    elif line == "RESUMED":
                        resume_other_carts(mgr)
                        if _loop is not None:
                            asyncio.run_coroutine_threadsafe(
                                _broadcast(f"[SYSTEM] {mgr.label} obstacle cleared — all carts resuming"),
                                _loop,
                            )

                    elif line == "DONE":
                        mgr.last_done_ts = time.time()
                        both_done = all(
                            (time.time() - m.last_done_ts) < 5.0
                            for m in ALL_CARTS
                        )
                        if both_done and pending_final_row is not None:
                            cfg["current_row"] = pending_final_row
                            save_config(cfg)
                            pending_final_row = None

                    elif line.startswith("STOPPED"):
                        pending_final_row = None

        except Exception:
            pass

        time.sleep(0.05)


threading.Thread(target=serial_reader_loop, daemon=True).start()

app = FastAPI()
app.mount("/static", StaticFiles(directory="static"), name="static")


@app.on_event("startup")
async def _grab_loop():
    global _loop
    _loop = asyncio.get_running_loop()


@app.get("/")
def root():
    with open("static/index.html", "r", encoding="utf-8") as f:
        return HTMLResponse(f.read())


@app.get("/api/config")
def get_config():
    return {
        "row_spacing_cm": cfg["row_spacing_cm"],
        "current_row":    cfg["current_row"],
        "home_row":       cfg.get("home_row", 1),
        "pwm":            cfg.get("pwm", 170),
        "cm_per_sec":     cfg.get("cm_per_sec", 12.2),
        "serial_port_1":  cfg.get("serial_port_1", "COM4"),
        "serial_port_2":  cfg.get("serial_port_2", "COM6"),
        "baud":           cfg.get("baud", 9600),
        "last_status_1":  serial_mgr_1.last_status,
        "last_status_2":  serial_mgr_2.last_status,
    }


@app.post("/api/config")
def update_config(body: dict = Body(...)):
    if "row_spacing_cm" in body and body["row_spacing_cm"] not in (None, ""):
        cfg["row_spacing_cm"] = float(body["row_spacing_cm"])
    if "current_row" in body and body["current_row"] not in (None, ""):
        cfg["current_row"] = int(float(body["current_row"]))
    if "home_row" in body and body["home_row"] not in (None, ""):
        cfg["home_row"] = int(float(body["home_row"]))
    if "pwm" in body and body["pwm"] not in (None, ""):
        cfg["pwm"] = clamp_int(body["pwm"], 0, 255)
    if "cm_per_sec" in body and body["cm_per_sec"] not in (None, ""):
        cfg["cm_per_sec"] = float(body["cm_per_sec"])
    if "serial_port_1" in body:
        cfg["serial_port_1"] = str(body.get("serial_port_1") or "").strip()
    if "serial_port_2" in body:
        cfg["serial_port_2"] = str(body.get("serial_port_2") or "").strip()
    if "baud" in body and body["baud"] not in (None, ""):
        cfg["baud"] = int(float(body["baud"]))
    save_config(cfg)
    return {"ok": True, **cfg}


class GotoRowRequest(BaseModel):
    target_row: int


@app.post("/api/goto_row")
def goto_row(req: GotoRowRequest):
    global pending_final_row

    target     = int(req.target_row)
    current    = int(cfg["current_row"])
    home_row   = int(cfg.get("home_row", 1))
    spacing    = float(cfg["row_spacing_cm"])
    pwm        = clamp_int(cfg.get("pwm", 170), 0, 255)
    cm_per_sec = float(cfg.get("cm_per_sec", 12.2))

    if cm_per_sec <= 0:
        raise RuntimeError("cm_per_sec must be > 0.")

    delta_rows_out   = target - current
    distance_out_cm  = abs(delta_rows_out) * spacing
    duration_out_ms  = int((distance_out_cm / cm_per_sec) * 1000)

    delta_rows_back  = home_row - target
    distance_back_cm = abs(delta_rows_back) * spacing
    duration_back_ms = int((distance_back_cm / cm_per_sec) * 1000)

    direction_out  = "F" if delta_rows_out  > 0 else "B"
    direction_back = "F" if delta_rows_back > 0 else "B"

    if target == current and target == home_row:
        stop_all_carts()
        return {"ok": True, "msg": "Already at target and home row", "current_row": current}

    pause_ms = 10000
    pending_final_row = home_row

    cmd = (
        f"TRIP {direction_out} {pwm} {duration_out_ms} "
        f"{pause_ms} {direction_back} {duration_back_ms}"
    )

    for mgr in ALL_CARTS:
        mgr.send_line(cmd)

    return {
        "ok":               True,
        "from_row":         current,
        "target_row":       target,
        "home_row":         home_row,
        "out_direction":    direction_out,
        "out_distance_cm":  distance_out_cm,
        "out_duration_ms":  duration_out_ms,
        "pause_ms":         pause_ms,
        "back_direction":   direction_back,
        "back_distance_cm": distance_back_cm,
        "back_duration_ms": duration_back_ms,
        "pwm":              pwm,
        "cmd_sent":         cmd,
    }


@app.post("/api/stop")
def stop_cart():
    global pending_final_row
    pending_final_row = None
    stop_all_carts()
    return {"ok": True}


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