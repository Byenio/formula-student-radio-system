import serial
import wave
import struct

SERIAL_PORT = 'COM7'
BAUD_RATE = 921600
SAMPLE_RATE = 16000

step_table = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

index_table = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


class AdpcmState:
    def __init__(self):
        self.predicted_sample = 0
        self.step_index = 0


def decode_adpcm_frame(compressed_data, state):
    pcm_data = bytearray()
    for byte in compressed_data:
        nibble = (byte >> 4) & 0x0F
        step = step_table[state.step_index]
        diffq = step >> 3
        if nibble & 4: diffq += step
        if nibble & 2: diffq += step >> 1
        if nibble & 1: diffq += step >> 2
        if nibble & 8:
            state.predicted_sample -= diffq
        else:
            state.predicted_sample += diffq

        if state.predicted_sample > 32767:
            state.predicted_sample = 32767
        elif state.predicted_sample < -32768:
            state.predicted_sample = -32768

        state.step_index += index_table[nibble]
        if state.step_index < 0:
            state.step_index = 0
        elif state.step_index > 88:
            state.step_index = 88
        pcm_data.extend(struct.pack('<h', int(state.predicted_sample)))

        nibble = byte & 0x0F
        step = step_table[state.step_index]
        diffq = step >> 3
        if nibble & 4: diffq += step
        if nibble & 2: diffq += step >> 1
        if nibble & 1: diffq += step >> 2
        if nibble & 8:
            state.predicted_sample -= diffq
        else:
            state.predicted_sample += diffq

        if state.predicted_sample > 32767:
            state.predicted_sample = 32767
        elif state.predicted_sample < -32768:
            state.predicted_sample = -32768

        state.step_index += index_table[nibble]
        if state.step_index < 0:
            state.step_index = 0
        elif state.step_index > 88:
            state.step_index = 88
        pcm_data.extend(struct.pack('<h', int(state.predicted_sample)))

    return bytes(pcm_data)


def main():
    ser = None
    wav_file = None
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
        state = AdpcmState()

        wav_file = wave.open("output.wav", "wb")
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(SAMPLE_RATE)

        print("Listening for packets... Press Ctrl+C to stop and save audio.")

        while True:
            byte = ser.read(1)
            if byte == b'\xAA':
                header = ser.read(2)
                if len(header) < 2: continue

                pkt_type = header[0]
                pkt_len = header[1]

                payload = ser.read(pkt_len)
                if len(payload) != pkt_len: continue

                if pkt_type == 0x01:
                    pcm = decode_adpcm_frame(payload, state)
                    wav_file.writeframes(pcm)

                elif pkt_type == 0x02:
                    batt = payload[0]
                    temp = payload[1]
                    print(f"Telemetry - SOC: {batt}%, Temp: {temp}C")

    except KeyboardInterrupt:
        print("\nStopping and saving output.wav...")
    finally:
        if wav_file is not None:
            wav_file.close()
        if ser is not None and ser.is_open:
            ser.close()


if __name__ == "__main__":
    main()