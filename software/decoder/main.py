import serial
import pyaudio
import struct
import time
import sys

# --- CONFIGURATION ---
SERIAL_PORT = 'COM3'  # CHANGE THIS to your Receiver Nucleo's Port!
BAUD_RATE = 921600  # Must match your STM32 baud rate
SAMPLE_RATE = 16000  # Must match your STM32 audio freq
CHUNK_SIZE = 512  # Number of bytes to read at a time

# --- IMA ADPCM DECODER LOGIC ---
# (This must match the C code exactly)
step_table = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

index_table = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
]


class AdpcmState:
    def __init__(self):
        self.predicted_sample = 0
        self.step_index = 0


def decode_adpcm_nibble(nibble, state):
    step = step_table[state.step_index]
    diffq = step >> 3

    if nibble & 4: diffq += step
    if nibble & 2: diffq += step >> 1
    if nibble & 1: diffq += step >> 2

    if nibble & 8:
        state.predicted_sample -= diffq
    else:
        state.predicted_sample += diffq

    # Clamp output
    if state.predicted_sample > 32767:
        state.predicted_sample = 32767
    elif state.predicted_sample < -32768:
        state.predicted_sample = -32768

    state.step_index += index_table[nibble]

    # Clamp step index
    if state.step_index < 0:
        state.step_index = 0
    elif state.step_index > 88:
        state.step_index = 88

    return state.predicted_sample


def decode_adpcm_frame(compressed_data, state):
    pcm_data = bytearray()

    for byte in compressed_data:
        # High nibble first (matches C code packing)
        high_nibble = (byte >> 4) & 0x0F
        sample1 = decode_adpcm_nibble(high_nibble, state)
        pcm_data.extend(struct.pack('<h', sample1))  # Little-endian 16-bit

        # Low nibble second
        low_nibble = byte & 0x0F
        sample2 = decode_adpcm_nibble(low_nibble, state)
        pcm_data.extend(struct.pack('<h', sample2))

    return bytes(pcm_data)


# --- MAIN EXECUTION ---

def list_devices(p):
    print("\n--- Available Audio Output Devices ---")
    info = p.get_host_api_info_by_index(0)
    numdevices = info.get('deviceCount')
    for i in range(0, numdevices):
        dev = p.get_device_info_by_host_api_device_index(0, i)
        if dev.get('maxOutputChannels') > 0:
            print(f"Index {i}: {dev.get('name')}")
    print("--------------------------------------\n")


def main():
    try:
        # 1. Setup Serial
        print(f"Opening Serial Port {SERIAL_PORT} at {BAUD_RATE}...")
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print("Serial Connected!")

        # 2. Setup Audio
        p = pyaudio.PyAudio()

        # List devices so you can pick your REAL speakers, not the Nucleo
        list_devices(p)
        device_index = int(input("Enter the Index # of your HEADPHONES/SPEAKERS: "))

        stream = p.open(format=pyaudio.paInt16,
                        channels=1,
                        rate=SAMPLE_RATE,
                        output=True,
                        output_device_index=device_index,
                        frames_per_buffer=CHUNK_SIZE * 2)  # *2 because 1 byte ADPCM = 2 shorts PCM

        print("\nStreaming Audio... (Ctrl+C to stop)")

        state = AdpcmState()

        # 3. Stream Loop
        while True:
            # We try to read a chunk. If buffer is empty, it blocks (good).
            # If we read 512 bytes compressed -> we get 1024 samples (2048 bytes) PCM
            if ser.in_waiting >= CHUNK_SIZE:
                compressed_data = ser.read(CHUNK_SIZE)

                # Decode
                pcm_data = decode_adpcm_frame(compressed_data, state)

                # Play
                stream.write(pcm_data)

            # Optional: Small sleep if buffer is empty to save CPU
            else:
                time.sleep(0.001)

    except KeyboardInterrupt:
        print("\nStopping...")
    except Exception as e:
        print(f"\nError: {e}")
    finally:
        if 'stream' in locals():
            stream.stop_stream()
            stream.close()
        if 'p' in locals():
            p.terminate()
        if 'ser' in locals() and ser.is_open:
            ser.close()
        print("Closed.")


if __name__ == "__main__":
    main()