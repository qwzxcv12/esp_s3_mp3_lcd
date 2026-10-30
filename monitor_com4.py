import sys
import time
import serial

PORT = "COM4"
BAUD = 115200
DURATION = 60  # Listen for 60 seconds

print(f"=== [MONITOR] Dang mo cong {PORT} (115200) trong 60 giay... ===", flush=True)

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
except Exception as e:
    print(f"[ERROR] Khong mo duoc cong {PORT}: {e}", flush=True)
    sys.exit(1)

print("=== [SAN SANG] Hay RUT RA VA CAM LAI ban phim, roi go thu cac phim... ===", flush=True)

start_time = time.time()
buffer = ""

while (time.time() - start_time) < DURATION:
    try:
        data = ser.read(ser.in_waiting or 1)
        if data:
            decoded = data.decode("utf-8", errors="replace")
            sys.stdout.write(decoded)
            sys.stdout.flush()
            buffer += decoded
    except Exception as e:
        print(f"\n[ERROR] Doc serial loi: {e}", flush=True)
        break
    time.sleep(0.01)

ser.close()
print("\n\n=== [KET THUC PHIEN THEO DOI 60S] ===", flush=True)
if buffer.strip():
    print(f"\n>>> NOI DUNG NHAN DUOC:\n{buffer}\n", flush=True)
else:
    print("\n>>> Chua nhan duoc du lieu.", flush=True)
