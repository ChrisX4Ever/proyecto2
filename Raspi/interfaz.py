import sys
import socket
import json
import time
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout,
    QHBoxLayout, QGridLayout, QLabel, QLineEdit,
    QPushButton, QComboBox, QCheckBox
)
from PySide6.QtCore import QThread, Signal, Slot, QTimer
import numpy as np
import pyqtgraph as pg
from scipy.signal import find_peaks

# ================= CONFIGURACIÓN DE RED =================
# Escuchar en todas las interfaces (0.0.0.0) para evitar problemas si la IP de la Pi cambia
Host_Listening = "0.0.0.0" 
PORT_TCP = 1111
PORT_UDP = 3333

# ¡IMPORTANTE!: AJUSTA ESTA IP A LA DE TU ESP32 ACTUAL
ESP32_IP = "192.168.5.31" 

# =======================================================
#   HILO TCP (SERVIDOR)
# =======================================================
class TCPServerThread(QThread):
    data_received = Signal(str)
    connection_status = Signal(str)
    client_socket_ready = Signal(object) 

    def __init__(self, host, port):
        super().__init__()
        self.host = host
        self.port = port
        self.server_sock = None
        self.client_sock = None
        self.running = True 
        
    def run(self):
        try:
            self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            print(f"Servidor TCP iniciado en puerto {self.port}")
            self.server_sock.bind((self.host, self.port))
            self.server_sock.listen(1)
            self.connection_status.emit(f"Esperando ESP32 en puerto {self.port}...")

            self.server_sock.settimeout(1.0) 
            self.client_sock = None
            
            # Esperar conexión
            while self.running and not self.client_sock:
                try:
                    self.client_sock, addr = self.server_sock.accept()
                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"Error accept: {e}")
                    continue
                
            if not self.running: return 

            print(f"ESP TCP conectada desde: {addr}")
            self.connection_status.emit(f"Conectado: {addr[0]}")
            self.client_socket_ready.emit(self.client_sock) 
            
            self.client_sock.settimeout(1.0) 
            
            # Bucle de datos
            while self.running: 
                try:
                    data = self.client_sock.recv(1024)
                    if data:
                        self.data_received.emit(data.decode('utf-8', errors='ignore').strip())
                    else:
                        print("TCP: Conexión cerrada por el cliente (EOF).")
                        self.running = False 
                        break 
                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"Error bucle TCP: {e}")
                    self.running = False 
                    break

        except Exception as e_conn:
            print(f"Error Servidor TCP: {e_conn}")
            
        finally:
            self.cleanup()

    def cleanup(self):
        if self.client_sock:
            try: self.client_sock.close()
            except: pass
        if self.server_sock:
            try: self.server_sock.close()
            except: pass
        self.client_socket_ready.emit(None) 
        if self.running: 
            self.connection_status.emit("Desconectado")
        print("Hilo TCP finalizado.")

    def stop(self): 
        self.running = False
        self.wait() 

# =======================================================
#   HILO UDP (RECEPCIÓN)
# =======================================================
class UDPReceiverThread(QThread):
    data_received = Signal(str)

    def __init__(self, host, port):
        super().__init__()
        self.host = host
        self.port = port
        self.running = True
        self.sock = None

    def run(self):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.sock.bind((self.host, self.port))
            print(f"UDP Receptor iniciado en puerto {self.port}")
            self.sock.settimeout(0.5) 
            
            while self.running:
                try:
                    data, addr = self.sock.recvfrom(2048) 
                    payload = data.decode("utf-8", errors="ignore") 
                    self.data_received.emit(payload)
                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"Error UDP recv: {e}")
                    break

        except Exception as e:
            print(f"Error Bind UDP: {e}")
        finally:
            if self.sock:
                self.sock.close()
            print("Hilo UDP detenido.")

    def stop(self):
        self.running = False
        self.wait()

# =======================================================
#   PROCESAMIENTO DE DATOS (Soporte BMI + BME)
# =======================================================
class DataProcessor:
    def __init__(self):
        self.signal_buffer_x = np.array([])
        self.signal_buffer_size = 500  
        self.fs = 50 

    def process_data(self, json_data: str, config: dict):
        try:
            record = json.loads(json_data) 
        except json.JSONDecodeError:
            return None, None 

        val = 0.0
        sel_sensor = config["sensor"] 
        sel_axis = config["param_type"] 

        # --- LÓGICA DE SELECCIÓN DE SENSOR ---
        if sel_sensor == "temperatura":
            # BME688: Valor escalar único
            if "temp_c" in record:
                val = float(record["temp_c"])
            else:
                return None, None
        else:
            # BMI270: Valor vectorial [x,y,z]
            sensor_key = ""
            idx = 0
            
            if sel_sensor == "acelerometro": sensor_key = "acc_m_s2"
            elif sel_sensor == "giroscopio": sensor_key = "gyr_rad_s"
            
            if "x" in sel_axis: idx = 0
            elif "y" in sel_axis: idx = 1
            elif "z" in sel_axis: idx = 2

            if sensor_key in record:
                try:
                    val = float(record[sensor_key][idx])
                except (IndexError, ValueError):
                    return None, None
            else:
                return None, None

        # --- Buffer Circular ---
        self.signal_buffer_x = np.append(self.signal_buffer_x, val)
        if len(self.signal_buffer_x) > self.signal_buffer_size:
            self.signal_buffer_x = self.signal_buffer_x[-self.signal_buffer_size:]
            
        current_signal = self.signal_buffer_x.copy()

        # --- Procesamiento FFT / RMS ---
        if config["output_type"] == "Procesado":
            if 'rms_window' in config and config['rms_window'] > 0:
                window = current_signal[-config['rms_window']:]
                if len(window) > 0:
                    rms_value = np.sqrt(np.mean(window**2))
                    return record["ts_ms"], rms_value 

            if 'fft_window' in config and config['fft_window'] > 0:
                fft_window = current_signal[-config['fft_window']:]
                N = len(fft_window)
                if N > 0:
                    yf = np.fft.fft(fft_window)
                    T = 1.0 / self.fs
                    xf = np.fft.fftfreq(N, T)[:N//2]
                    modulo = 2.0/N * np.abs(yf[0:N//2])
                    
                    if config.get("fft_peaks"):
                        peaks, properties = find_peaks(modulo, height=0.1, distance=5)
                        peak_freqs = xf[peaks]
                        if len(peak_freqs) > 0:
                            max_peak_idx = np.argmax(modulo[peaks])
                            peak_info = f"Freq: {peak_freqs[max_peak_idx]:.2f} Hz"
                            return record["ts_ms"], peak_info 
                            
                    return xf, modulo

            return record["ts_ms"], val

        return record["ts_ms"], val


# =======================================================
#   GUI PRINCIPAL
# =======================================================
class MainWindow(QMainWindow):
    
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Analisis Edge - ESP32 S3")
        self.setGeometry(100, 100, 1000, 600)

        self.active_client_sock = None
        self.server_thread = None   
        self.udp_thread = None      
        
        self.processor = DataProcessor()
        self.current_config = {}

        self.time_data = []
        self.value_data = []
        self.max_points = 300 

        self._setup_ui()
        self._setup_plot()
        self._update_config() 

        self.plot_timer = QTimer()
        self.plot_timer.timeout.connect(self.update_plot)
        
    def _setup_ui(self):
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)

        self.lbl_status = QLabel("Estado: Desconectado")
        main_layout.addWidget(self.lbl_status)

        # Controles
        control_layout = QHBoxLayout()
        control_layout.addWidget(QLabel("Modo:"))
        self.mode_select = QComboBox()
        self.mode_select.addItems(["TCP", "UDP"])
        self.mode_select.currentIndexChanged.connect(self.request_mode_change)
        control_layout.addWidget(self.mode_select)
        
        control_layout.addWidget(QLabel("Sensor:"))
        self.sensor_select = QComboBox()
        # AÑADIDO: Opción "temperatura" para BME688
        self.sensor_select.addItems(["acelerometro", "giroscopio", "temperatura"])
        self.sensor_select.currentIndexChanged.connect(self.reset_graph_data)
        control_layout.addWidget(self.sensor_select)
        
        control_layout.addWidget(QLabel("Eje:"))
        self.type_select = QComboBox()
        self.type_select.addItems(["ax", "ay", "az"])
        control_layout.addWidget(self.type_select)
        
        main_layout.addLayout(control_layout)

        # Configuración Edge
        edge_layout = QGridLayout()
        edge_layout.addWidget(QLabel("Salida:"), 0, 0)
        self.output_select = QComboBox()
        self.output_select.addItems(["Raw", "Procesado"])
        edge_layout.addWidget(self.output_select, 0, 1)
        
        edge_layout.addWidget(QLabel("Window RMS:"), 1, 0)
        self.rms_input = QLineEdit("50")
        edge_layout.addWidget(self.rms_input, 1, 1)
        
        edge_layout.addWidget(QLabel("Window FFT:"), 2, 0)
        self.fft_input = QLineEdit("0")
        edge_layout.addWidget(self.fft_input, 2, 1)
        
        self.fft_peaks_checkbox = QCheckBox("Detectar picos FFT")
        edge_layout.addWidget(self.fft_peaks_checkbox, 3, 0, 1, 2)
        main_layout.addLayout(edge_layout)

        # Botones
        btn_layout = QHBoxLayout()
        self.btn_start = QPushButton("ENVIAR START")
        self.btn_start.clicked.connect(lambda: self.send_control_command("START"))
        btn_layout.addWidget(self.btn_start)
        
        self.btn_stop = QPushButton("ENVIAR STOP")
        self.btn_stop.clicked.connect(lambda: self.send_control_command("STOP"))
        btn_layout.addWidget(self.btn_stop)
        
        self.btn_connect = QPushButton("Iniciar Servidor (Esperar ESP32)") 
        self.btn_connect.clicked.connect(self.toggle_server)
        btn_layout.addWidget(self.btn_connect)
        main_layout.addLayout(btn_layout)

        # Plot
        self.graph = pg.PlotWidget()
        self.graph.plotItem.showGrid(True, True)
        self.plot_curve = self.graph.plot(pen='y')
        main_layout.addWidget(self.graph)

    def _setup_plot(self):
        self.graph.setLabel("left", "Amplitud")
        self.graph.setLabel("bottom", "Muestras")

    def _update_config(self):
        self.current_config = {
            "sensor": self.sensor_select.currentText(),
            "param_type": self.type_select.currentText(),
            "output_type": self.output_select.currentText(),
            "rms_window": int(self.rms_input.text()) if self.rms_input.text().isdigit() else 0,
            "fft_window": int(self.fft_input.text()) if self.fft_input.text().isdigit() else 0,
            "fft_peaks": self.fft_peaks_checkbox.isChecked()
        }
        
    def reset_graph_data(self):
        """ Limpia gráfica y actualiza etiquetas según el sensor """
        self.time_data.clear()
        self.value_data.clear()
        self.plot_curve.setData([])
        
        sensor = self.sensor_select.currentText()
        if sensor == "temperatura":
            self.graph.setLabel("left", "Temperatura (°C)")
            self.type_select.setEnabled(False)
        else:
            self.type_select.setEnabled(True)
            if sensor == "acelerometro":
                self.graph.setLabel("left", "Aceleración (m/s²)")
            elif sensor == "giroscopio":
                self.graph.setLabel("left", "Vel. Angular (rad/s)")

    # ======================================================
    #   GESTIÓN DE RED
    # ======================================================
    def start_udp_listener(self):
        if self.udp_thread is None:
            self.udp_thread = UDPReceiverThread(Host_Listening, PORT_UDP)
            self.udp_thread.data_received.connect(self.on_data_received)
            self.udp_thread.start()
            self.update_status_label(f"Modo UDP. Escuchando en puerto {PORT_UDP}")
        
    def stop_udp_listener(self):
        if self.udp_thread:
            self.udp_thread.stop()
            self.udp_thread = None
            # No cambiamos status aquí porque puede que estemos pasando a TCP

    def force_start_tcp_server(self):
        print("[GUI] Forzando reinicio del Servidor TCP...")
        if self.server_thread:
            self.server_thread.stop()
            self.server_thread = None
        self.active_client_sock = None
        
        self.server_thread = TCPServerThread(Host_Listening, PORT_TCP)
        self.server_thread.data_received.connect(self.on_data_received)
        self.server_thread.connection_status.connect(self.update_status_label)
        self.server_thread.client_socket_ready.connect(self.set_active_socket)
        self.server_thread.start()
        
        self.btn_connect.setText("Detener Conexión")
        self.update_status_label("Esperando reconexión TCP...")

    # ======================================================
    #   CAMBIO DE MODO ROBUSTO (TCP <-> UDP)
    # ======================================================
    def request_mode_change(self):
        mode_command = self.mode_select.currentText()
        
        if mode_command == "TCP":
            # 1. Avisar a ESP32
            self.send_control_command("TCP")
            
            # 2. Cerrar UDP local
            self.stop_udp_listener() 
            
            # 3. Forzar apertura de TCP Server para recibir la reconexión
            time.sleep(0.5)
            self.force_start_tcp_server()
            
        elif mode_command == "UDP":
            # 1. Avisar a ESP32
            self.send_control_command("UDP")
            
            # 2. Limpiar referencia a TCP
            self.active_client_sock = None 
            time.sleep(0.1) 
            
            # 3. Abrir UDP local
            self.start_udp_listener() 
            
            # 4. Cerrar TCP server local
            if self.server_thread and self.server_thread.isRunning(): 
                self.server_thread.stop()
                self.server_thread = None
        
            # 5. Enviar START por UDP
            self.send_control_command("START") 

    def toggle_server(self):
        is_running = self.server_thread is not None or self.udp_thread is not None
        
        if not is_running:
            self.stop_udp_listener() 
            self.server_thread = None 
            self.active_client_sock = None
            
            self.server_thread = TCPServerThread(Host_Listening, PORT_TCP)
            self.server_thread.data_received.connect(self.on_data_received)
            self.server_thread.connection_status.connect(self.update_status_label)
            self.server_thread.client_socket_ready.connect(self.set_active_socket)
            self.server_thread.start()

            self.btn_connect.setText("Detener Conexión")
            self.reset_graph_data()
            
        else:
            if self.server_thread:
                self.server_thread.stop()
                self.server_thread = None
                self.active_client_sock = None
            
            self.stop_udp_listener() 
            self.btn_connect.setText("Iniciar Servidor (Esperar ESP32)")
            self.lbl_status.setText("Estado: Detenido")

    @Slot(object)
    def set_active_socket(self, sock):
        self.active_client_sock = sock
        if sock:
            print(f"[GUI] Socket TCP conectado. ESP32 re-conectada.")
            self.update_status_label("Conectado por TCP.")
            # AUTO-START: Enviar START automáticamente al reconectar por TCP
            QTimer.singleShot(500, lambda: self.send_control_command("START"))
        else:
            print(f"[GUI] Socket cliente TCP limpiado.")

    @Slot(str)
    def update_status_label(self, msg):
        self.lbl_status.setText(msg)

    @Slot(str)
    def on_data_received(self, payload):
        self._update_config()
        ts, value = self.processor.process_data(payload, self.current_config)

        if value is None: return

        if isinstance(value, (int, float)):
            self.value_data.append(value)
            if len(self.value_data) > self.max_points:
                self.value_data = self.value_data[-self.max_points:]
            self.time_data.append(len(self.value_data)) 
        else:
            print(f"[EDGE] {value}")

    def update_plot(self):
        if self.value_data:
            self.plot_curve.setData(self.value_data)

    def send_control_command(self, cmd):
        msg = f"{cmd}\n"
        
        # Intentar TCP primero
        if self.active_client_sock: 
            try:
                self.active_client_sock.sendall(msg.encode('utf-8'))
                print(f"--> [TCP] Enviado: {msg.strip()}")
                return
            except Exception as e:
                print(f"Fallo envío TCP: {e}. Intentando UDP...")
                self.active_client_sock = None 

        # Fallback a UDP para START/STOP/MODOS
        if cmd in ["START", "STOP", "TCP", "UDP"]:
            try:
                send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                esp_addr = (ESP32_IP, PORT_UDP) 
                
                send_sock.sendto(msg.encode('utf-8'), esp_addr)
                send_sock.close()
                print(f"--> [UDP] Enviado: {msg.strip()} a {esp_addr}")
            except Exception as e:
                print(f"Error envío UDP: {e}")

    def closeEvent(self, event):
        if self.server_thread: self.server_thread.stop()
        if self.udp_thread: self.udp_thread.stop()
        event.accept()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    window.plot_timer.start(50) 
    sys.exit(app.exec())