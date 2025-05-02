import socket
import time
import threading

SERVER_IP = "192.168.168.174"  # Замените на IP вашей ESP32
SERVER_PORT = 1234
MEASURE_INTERVAL = 1.0  # Интервал измерения в секундах

class SpeedTester:
    def __init__(self):
        self.total_bytes = 0
        self.running = False
        self.lock = threading.Lock()
        
    def start(self):
        self.running = True
        self.total_bytes = 0
        
        # Поток для измерения скорости
        threading.Thread(target=self.measure_speed, daemon=True).start()
        
        # Подключение к серверу
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((SERVER_IP, SERVER_PORT))
            print(f"Connected to {SERVER_IP}:{SERVER_PORT}")
            
            while self.running:
                try:
                    data = s.recv(4096)
                    if not data:
                        break
                        
                    with self.lock:
                        self.total_bytes += len(data)
                except ConnectionError:
                    print("Connection lost")
                    break
                    
        self.running = False
        print("Disconnected")
        
    def measure_speed(self):
        last_bytes = 0
        last_time = time.time()
        
        while self.running:
            time.sleep(MEASURE_INTERVAL)
            
            with self.lock:
                current_bytes = self.total_bytes
                
            current_time = time.time()
            elapsed = current_time - last_time
            bytes_per_sec = (current_bytes - last_bytes) / elapsed
            megabits_per_sec = (bytes_per_sec * 8) / (1024 * 1024)
            
            print(f"Speed: {megabits_per_sec:.2f} Mbps (Total: {current_bytes / (1024 * 1024):.2f} MB)")
            
            last_bytes = current_bytes
            last_time = current_time

if __name__ == "__main__":
    tester = SpeedTester()
    try:
        tester.start()
    except KeyboardInterrupt:
        print("\nStopping...")
