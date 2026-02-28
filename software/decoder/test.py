import serial
import struct
import wave
import time

# --- CONFIGURATION ---
SERIAL_PORT = 'COM7'  # CHANGE THIS to your Nucleo Port!
BAUD_RATE = 921600
SAMPLE_RATE = 32000
DURATION = 240  # Record for 5 seconds

# --- ADPCM DECODER TABLES ---
step_table = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]
index_table = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


class AdpcmState:
    def __init__(self):
        self.predicted_sample = 0
        self.step_index = 0


def decode_adpcm(data, state):
    pcm = bytearray()
    for byte in data:
        for i in range(2):  # Process 2 nibbles
            nibble = (byte >> 4) & 0x0F if i == 0 else byte & 0x0F
            step = step_table[state.step_index]
            diffq = step >> 3
            if nibble & 4: diffq += step
            if nibble & 2: diffq += step >> 1
            if nibble & 1: diffq += step >> 2

            if nibble & 8:
                state.predicted_sample -= diffq
            else:
                state.predicted_sample += diffq

            # Clamp
            if state.predicted_sample > 32767:
                state.predicted_sample = 32767
            elif state.predicted_sample < -32768:
                state.predicted_sample = -32768

            state.step_index += index_table[nibble]
            if state.step_index < 0:
                state.step_index = 0
            elif state.step_index > 88:
                state.step_index = 88

            pcm.extend(struct.pack('<h', state.predicted_sample))
    return pcm


def main():
    try:
        print(f"Connecting to {SERIAL_PORT}...")
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print("Connected! Recording...")

        state = AdpcmState()
        raw_audio = bytearray()
        start = time.time()

        while time.time() - start < DURATION:
            if ser.in_waiting:
                chunk = ser.read(ser.in_waiting)
                decoded = decode_adpcm(chunk, state)
                raw_audio.extend(decoded)

        print("Saving to output.wav...")
        with wave.open("output.wav", "wb") as f:
            f.setnchannels(1)
            f.setsampwidth(2)
            f.setframerate(SAMPLE_RATE)
            f.writeframes(raw_audio)
        print("Done! Check your folder for output.wav")

    except Exception as e:
        print(f"Error: {e}")
    finally:
        if 'ser' in locals(): ser.close()


if __name__ == "__main__":
    main()