import sys
import json
import socket
import threading
import time
from datetime import datetime
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QPushButton, QLabel, QComboBox, QTextEdit, 
    QVBoxLayout, QWidget, QHBoxLayout, QGroupBox, QFormLayout
)
from PyQt5.QtCore import Qt, QTimer

try:
    from gpiozero import LED, Button
    HAS_GPIO = True
except ImportError:
    class DummyComponent:
        def when_pressed(self): pass
        def when_released(self): pass
        def on(self): pass
        def off(self): pass
        def close(self): pass
    LED = DummyComponent
    Button = DummyComponent
    HAS_GPIO = False

class SensorGUI(QMainWindow):
    HOST = "192.168.50.82"
    PORT = 2222
    FREQUENCIES = ["100 Hz", "800 Hz", "1600 Hz"]

    def __init__(self):
        super().__init__()
        self.setWindowTitle("Sensor ESP32 - BMI270")
        self.setGeometry(100, 100, 600, 600)
        self.connected = False
        self.capturing = False
        self.sample_count = 0
        self.start_time = None
        self.client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.client.settimeout(5)

        if HAS_GPIO:
            self.led_gpio = LED(22)
            self.btn_start_gpio = Button(17)
            self.btn_stop_gpio = Button(27)
            self.btn_start_gpio.when_pressed = self.start_capture
            self.btn_stop_gpio.when_pressed = self.stop_capture
        else:
            self.log_event("Advertencia: Modulos GPIO no disponibles")

        self.init_ui()
        self.timer = QTimer()
        self.timer.timeout.connect(self.update_rate)
        self.timer.start(1000)

        threading.Thread(target=self.receive_data_loop, daemon=True).start()
        self.connect_to_server()
        
    def init_ui(self):
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)

        control_group = QGroupBox("Control")
        control_layout = QHBoxLayout(control_group)
        
        self.btn_connect = QPushButton("Conectar/Desconectar")
        self.btn_connect.clicked.connect(self.toggle_connection)
        
        self.btn_capture = QPushButton("Iniciar Captura")
        self.btn_capture.clicked.connect(self.start_capture)
        self.btn_capture.setEnabled(False)
        
        self.btn_stop = QPushButton("Detener Captura")
        self.btn_stop.clicked.connect(self.stop_capture)
        self.btn_stop.setEnabled(False)
        
        control_layout.addWidget(self.btn_connect)
        control_layout.addWidget(self.btn_capture)
        control_layout.addWidget(self.btn_stop)
        main_layout.addWidget(control_group)

        settings_group = QGroupBox("Configuracion")
        settings_layout = QFormLayout(settings_group)
        
        self.cmb_freq = QComboBox()
        self.cmb_freq.addItems(self.FREQUENCIES)
        self.cmb_freq.setCurrentText("100 Hz")
        
        settings_layout.addRow(QLabel("Frecuencia de Muestreo:"), self.cmb_freq)
        main_layout.addWidget(settings_group)

        status_group = QGroupBox("Estado y Tasa")
        status_layout = QHBoxLayout(status_group)
        self.lbl_status = QLabel("Estado: Desconectado")
        self.lbl_status.setStyleSheet("color: red;")
        self.lbl_samples = QLabel("Muestras: 0")
        self.lbl_rate = QLabel("Tasa: 0 Hz")
        status_layout.addWidget(self.lbl_status)
        status_layout.addWidget(self.lbl_samples)
        status_layout.addWidget(self.lbl_rate)
        main_layout.addWidget(status_group)

        data_group = QGroupBox("Datos Acelerometro (g)")
        data_layout = QHBoxLayout(data_group)
        self.lbl_ax = QLabel("ax: 0.00")
        self.lbl_ay = QLabel("ay: 0.00")
        self.lbl_az = QLabel("az: 0.00")
        data_layout.addWidget(self.lbl_ax)
        data_layout.addWidget(self.lbl_ay)
        data_layout.addWidget(self.lbl_az)
        main_layout.addWidget(data_group)
        
        self.txt_log = QTextEdit()
        self.txt_log.setReadOnly(True)
        self.txt_log.setFixedHeight(200)
        main_layout.addWidget(QLabel("Log de Eventos:"))
        main_layout.addWidget(self.txt_log)

    def toggle_connection(self):
        if self.connected:
            self.disconnect_from_server()
        else:
            self.connect_to_server()

    def connect_to_server(self):
        if self.connected:
            return

        self.log_event(f"Intentando conectar a {self.HOST}:{self.PORT}...")
        try:
            self.client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.client.settimeout(5)
            self.client.connect((self.HOST, self.PORT))
            
            self.connected = True
            self.lbl_status.setText("Estado: Conectado")
            self.lbl_status.setStyleSheet("color: green;")
            self.btn_connect.setText("Desconectar")
            self.btn_capture.setEnabled(True)
            self.log_event("Conexion exitosa con el Servidor Puente")
            if HAS_GPIO:
                self.led_gpio.on()
            
        except socket.timeout:
            self.log_event(f"Error de conexion: Tiempo de espera agotado")
            self.connected = False
        except ConnectionRefusedError:
            self.log_event(f"Error de conexion: Coneccion rechazada Asegurese que el Servidor Puente este corriendo en {self.HOST}:{self.PORT} y que no haya firewall bloqueando")
            self.connected = False
        except Exception as e:
            self.log_event(f"Error de conexion: {e}")
            self.connected = False

    def disconnect_from_server(self):
        if not self.connected:
            return
            
        self.stop_capture()
        try:
            self.client.shutdown(socket.SHUT_RDWR)
            self.client.close()
        except OSError:
            pass
            
        self.connected = False
        self.lbl_status.setText("Estado: Desconectado")
        self.lbl_status.setStyleSheet("color: red;")
        self.btn_connect.setText("Conectar")
        self.btn_capture.setEnabled(False)
        self.btn_stop.setEnabled(False)
        if HAS_GPIO:
            self.led_gpio.off()
        self.log_event("Desconectado del Servidor Puente")

    def send_command(self, command):
        if self.connected:
            try:
                self.client.sendall((command + "\n").encode())
                self.log_event(f"Comando enviado: {command}")
                return True
            except Exception as e:
                self.log_event(f"Error al enviar comando: {e} Desconectando")
                self.disconnect_from_server()
        return False

    def start_capture(self):
        if self.capturing:
            return
            
        selected_freq_str = self.cmb_freq.currentText()
        freq = selected_freq_str.split(" ")[0]
        
        if self.send_command(f"SRATE {freq}"):
            if self.send_command("START"):
                self.capturing = True
                self.btn_capture.setEnabled(False)
                self.btn_stop.setEnabled(True)
                self.start_time = datetime.now()
                self.sample_count = 0
                self.lbl_samples.setText("Muestras: 0")
                self.lbl_rate.setText("Tasa: 0 Hz")
                self.log_event(f"Captura iniciada a {freq} Hz (Comando enviado a ESP32)")
            else:
                self.log_event("Error: No se pudo enviar comando START")
        else:
            self.log_event("Error: No se pudo enviar comando SRATE ")

    def stop_capture(self):
        if self.send_command("STOP"):
            self.capturing = False
            self.btn_capture.setEnabled(True)
            self.btn_stop.setEnabled(False)
            self.log_event("Captura detenida (Comando enviado a ESP32)")

    def receive_data_loop(self):
        buffer = ""
        while True:
            if not self.connected:
                time.sleep(1)
                continue
            
            try:
                data = self.client.recv(1024)
                if not data:
                    self.log_event("Servidor cerro la coneccion")
                    self.disconnect_from_server()
                    continue
                
                buffer += data.decode('utf-8', errors='ignore')
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    if not line.strip():
                        continue
                    
                    self.sample_count += 1
                    self.lbl_samples.setText(f"Muestras: {self.sample_count}")
                    self.process_sample(line)

            except socket.timeout:
                pass
            except Exception as e:
                if self.connected:
                    self.log_event(f"Error de recepcion: {e} Desconectando")
                    self.disconnect_from_server()
                
            time.sleep(0.01)

    def process_sample(self, line):
        try:
            if line.startswith('{') and line.endswith('}'):
                sample = json.loads(line)
                
                if "acc_g" in sample:
                    ax, ay, az = sample["acc_g"]
                elif "acc_m_s2" in sample:
                    ax, ay, az = sample["acc_m_s2"]
                else:
                    ax = ay = az = 0
                
                self.lbl_ax.setText(f"ax: {ax:.2f}")
                self.lbl_ay.setText(f"ay: {ay:.2f}")
                self.lbl_az.setText(f"az: {az:.2f}")
                
                sample["timestamp"] = datetime.now().isoformat()
                with open("data_session_gui.jsonl", "a") as f:
                    f.write(json.dumps(sample) + "\n")
            else:
                self.log_event(f"Dato no JSON: {line[:50]}...")
        except json.JSONDecodeError:
            self.log_event(f"Error JSON al procesar muestra: {line[:50]}...")
        except Exception as e:
            self.log_event(f"Error procesando muestra: {e}")

    def update_rate(self):
        if self.start_time and self.sample_count > 0 and self.capturing:
            elapsed = (datetime.now() - self.start_time).total_seconds()
            if elapsed > 0:
                rate = self.sample_count / elapsed
                self.lbl_rate.setText(f"Tasa: {rate:.1f} Hz")
            
    def closeEvent(self, event):
        self.disconnect_from_server()
        if HAS_GPIO:
            self.led_gpio.close()
        event.accept()

    def log_event(self, msg):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self.txt_log.append(f"[{ts}] {msg}")
        
if __name__ == '__main__':
    app = QApplication(sys.argv)
    window = SensorGUI()
    window.show()
    sys.exit(app.exec_())