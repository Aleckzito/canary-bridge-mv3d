import json
import tkinter as tk
from tkinter import ttk, messagebox
from urllib import request, error


BASE_URL = "http://127.0.0.1:18991"


def fetch_json(url: str, method: str = "GET") -> dict:
    req = request.Request(url, method=method)
    with request.urlopen(req, timeout=5) as response:
        payload = response.read().decode("utf-8")
    return json.loads(payload)


class CanaryControlGUI:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Canary Python Control Plane")
        self.root.geometry("520x280")

        self.status_var = tk.StringVar(value="Desconocido")
        self.players_var = tk.StringVar(value="0")
        self.bridge_var = tk.StringVar(value=BASE_URL)
        self.result_var = tk.StringVar(value="Listo.")

        frame = ttk.Frame(root, padding=16)
        frame.pack(fill="both", expand=True)

        info = ttk.LabelFrame(frame, text="Estado del servidor", padding=12)
        info.pack(fill="x")

        ttk.Label(info, text="Bridge:").grid(row=0, column=0, sticky="w")
        ttk.Label(info, textvariable=self.bridge_var).grid(row=0, column=1, sticky="w")
        ttk.Label(info, text="Game State:").grid(row=1, column=0, sticky="w")
        ttk.Label(info, textvariable=self.status_var).grid(row=1, column=1, sticky="w")
        ttk.Label(info, text="Players Online:").grid(row=2, column=0, sticky="w")
        ttk.Label(info, textvariable=self.players_var).grid(row=2, column=1, sticky="w")

        actions = ttk.LabelFrame(frame, text="Operaciones", padding=12)
        actions.pack(fill="x", pady=(12, 0))

        buttons = [
            ("Refresh", self.refresh_status),
            ("Save", lambda: self.run_command("save")),
            ("Reload All", lambda: self.run_command("reload-all")),
            ("Open", lambda: self.run_command("open")),
            ("Close", lambda: self.run_command("close")),
            ("Shutdown", lambda: self.run_command("shutdown")),
        ]

        for idx, (text, cmd) in enumerate(buttons):
            ttk.Button(actions, text=text, command=cmd).grid(row=0, column=idx, padx=4, pady=4, sticky="ew")
            actions.columnconfigure(idx, weight=1)

        result = ttk.LabelFrame(frame, text="Resultado", padding=12)
        result.pack(fill="both", expand=True, pady=(12, 0))
        ttk.Label(result, textvariable=self.result_var, wraplength=450, justify="left").pack(anchor="w")

        self.refresh_status()

    def refresh_status(self) -> None:
        try:
            data = fetch_json(f"{BASE_URL}/status")
            self.status_var.set(data.get("game_state", "unknown"))
            self.players_var.set(str(data.get("players_online", 0)))
            self.result_var.set("Estado actualizado.")
        except Exception as exc:  # noqa: BLE001
            self.result_var.set(f"No se pudo leer el bridge: {exc}")

    def run_command(self, command: str) -> None:
        try:
            data = fetch_json(f"{BASE_URL}/command/{command}", method="POST")
            self.result_var.set(json.dumps(data, ensure_ascii=False))
            self.refresh_status()
        except error.HTTPError as exc:
            self.result_var.set(f"HTTP {exc.code}: {exc.reason}")
        except Exception as exc:  # noqa: BLE001
            self.result_var.set(f"Fallo ejecutando '{command}': {exc}")
            if command == "shutdown":
                messagebox.showwarning("Canary", self.result_var.get())


if __name__ == "__main__":
    app = tk.Tk()
    CanaryControlGUI(app)
    app.mainloop()
