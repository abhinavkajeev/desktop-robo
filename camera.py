import serial
import matplotlib.pyplot as plt
import numpy as np

# 🔧 CHANGE THIS TO YOUR PORT
ser = serial.Serial('COM7', 9600)

data = []

plt.ion()  # interactive mode (no freeze)

while True:
    try:
        line = ser.readline().decode().strip()

        if line == "FRAME_START":
            data = []

        elif line == "FRAME_END":
            if len(data) >= 100:
                try:
                    img = np.array(data[:100]).reshape((10, 10))
                    
                    plt.clf()  # clear previous frame
                    plt.imshow(img, cmap='gray')
                    plt.title("Camera Output")
                    plt.pause(0.001)

                except:
                    pass

        else:
            try:
                val = int(line)
                if 0 <= val <= 255:
                    data.append(val)
            except:
                pass

    except KeyboardInterrupt:
        print("Stopped")
        break

ser.close()