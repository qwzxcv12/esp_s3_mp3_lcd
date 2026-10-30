import sys
import time
import serial

# Configure utf-8 stdout on Windows
if sys.stdout.encoding != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

PORT = "COM4"
BAUD = 115200

print("=" * 60, flush=True)
print(f"   THEO DÕI REALTIME BÀN PHÍM ESP32-S3 TRÊN {PORT} ({BAUD})", flush=True)
print("   (Bấm phím trên bàn phím USB để xem log, bấm Ctrl+C để dừng)", flush=True)
print("=" * 60, flush=True)

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    ser.setDTR(False)
    ser.setRTS(False)
except Exception as e:
    print(f"[ERROR] Không thể mở cổng {PORT}: {e}", flush=True)
    sys.exit(1)

print("\n>>> ĐANG LẮNG NGHE... Hãy bấm phím, phím ESC để đổi chế độ!\n", flush=True)

try:
    while True:
        data = ser.read(ser.in_waiting or 1)
        if data:
            try:
                decoded = data.decode("utf-8", errors="replace")
                sys.stdout.write(decoded)
                sys.stdout.flush()
            except Exception:
                pass
        time.sleep(0.005)
except KeyboardInterrupt:
    print("\n\n=== ĐÃ DỪNG THEO DÕI ===", flush=True)
finally:
    ser.close()
