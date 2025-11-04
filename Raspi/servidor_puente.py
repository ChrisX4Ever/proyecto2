import socket
import threading
import json
import time
from datetime import datetime
from pathlib import Path
import sys

class BridgeServer:
    def __init__(self, host, port_esp, port_ui, data_dir="registros"):
        self.HOST = host
        self.PORT_ESP = port_esp
        self.PORT_UI = port_ui
        self.DATA_DIR = Path(data_dir)
        self.DATA_DIR.mkdir(exist_ok=True)
        self.esp_conn = None
        self.ui_conn = None

    def _log(self, msg):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{ts}] {msg}")
        with open(self.DATA_DIR / "server_log.txt", "a") as f:
            f.write(f"[{ts}] {msg}\n")

    def _save_data(self, payload):
        now = datetime.utcnow()
        record = {
            "timestamp": now.isoformat(timespec="milliseconds") + "Z",
            "payload": payload,
        }
        fname = self.DATA_DIR / f"{now.strftime('%Y%m%d')}.jsonl"
        with fname.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

    def _handle_esp(self, conn, addr):
        self._log(f"ESP conectado desde {addr}")
        self.esp_conn = conn
        buffer = ""
        try:
            while True:
                data = conn.recv(2048)
                if not data:
                    break
                
                buffer += data.decode('utf-8')
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    if not line.strip():
                        continue
                    
                    try:
                        sample = json.loads(line)
                        self._save_data(sample)

                        if self.ui_conn:
                            self.ui_conn.sendall((line + "\n").encode())

                    except json.JSONDecodeError:
                        self._log(f"Error JSON del ESP: {line}")
                        
        except Exception as e:
            self._log(f"Error ESP: {e}")
        finally:
            self.esp_conn = None
            conn.close()
            self._log("ESP desconectado")

    def _handle_ui(self, conn, addr):
        self._log(f"Interfaz conectada desde {addr}")
        self.ui_conn = conn
        try:
            while True:
                data = conn.recv(1024)
                if not data:
                    break
                    
                for msg in data.decode().splitlines():
                    msg = msg.strip()
                    if not msg:
                        continue
                    self._log(f"UI -> {msg}")
                    
                    if self.esp_conn:
                        self.esp_conn.sendall((msg + "\n").encode())
                    else:
                        self._log("No hay ESP para reenviar comando")
        except Exception as e:
            self._log(f"Error UI: {e}")
        finally:
            self.ui_conn = None
            conn.close()
            self._log("Interfaz desconectada")

    def _accept_clients(self, server_socket, tipo):
        while True:
            try:
                conn, addr = server_socket.accept()
                if tipo == "ESP":
                    threading.Thread(target=self._handle_esp, args=(conn, addr), daemon=True).start()
                else:
                    threading.Thread(target=self._handle_ui, args=(conn, addr), daemon=True).start()
            except Exception as e:
                self._log(f"Error aceptando cliente {tipo}: {e}")

    def run(self):
        self._log("Servidor puente iniciado")

        server_esp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_esp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_esp.bind((self.HOST, self.PORT_ESP))
        server_esp.listen(1)
        self._log(f"Escuchando ESP en {self.HOST}:{self.PORT_ESP}")
        threading.Thread(target=self._accept_clients, args=(server_esp, "ESP"), daemon=True).start()

        server_ui = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_ui.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_ui.bind((self.HOST, self.PORT_UI))
        server_ui.listen(1)
        self._log(f"Escuchando UI en {self.HOST}:{self.PORT_UI}")
        threading.Thread(target=self._accept_clients, args=(server_ui, "UI"), daemon=True).start()

        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            self._log("Servidor detenido por el usuario")
            sys.exit(0)

if __name__ == "__main__":
    HOST = "192.168.50.82"
    PORT_ESP = 1111
    PORT_UI = 2222
    server = BridgeServer(HOST, PORT_ESP, PORT_UI)
    server.run()