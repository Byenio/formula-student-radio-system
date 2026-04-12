import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
os.environ['PATH'] = current_dir + os.pathsep + os.environ['PATH']
if hasattr(os, 'add_dll_directory'):
    os.add_dll_directory(current_dir)

import serial
import pyaudio
import opuslib

SERIAL_PORT = 'COM7'
BAUD_RATE = 921600
SAMPLE_RATE = 16000
FRAME_SIZE = 320


def get_vb_cable_index(p):
    for i in range(p.get_device_count()):
        info = p.get_device_info_by_index(i)
        print(info["name"])
        if info["maxOutputChannels"] > 0 and "CABLE" in info["name"].upper():
            return i
    return None


def main():
    ser = None
    p = None
    stream = None
    expected_seq = None

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
        decoder = opuslib.Decoder(SAMPLE_RATE, 1)

        p = pyaudio.PyAudio()

        device_idx = get_vb_cable_index(p)
        if device_idx is not None:
            print(f"Routed audio to VB-Audio Virtual Cable (Device {device_idx})")
        else:
            print("VB-Audio Virtual Cable not found! Using default audio device.")

        stream = p.open(format=pyaudio.paInt16,
                        channels=1,
                        rate=SAMPLE_RATE,
                        output=True,
                        output_device_index=device_idx)

        print("Listening for packets... Hold the Nucleo button to transmit!")
        print("Press Ctrl+C to stop.")

        buffer = bytearray()
        state = 0
        pkt_type = 0
        pkt_len = 0
        seq_num = 0

        while True:
            waiting = ser.in_waiting
            if waiting > 0:
                buffer.extend(ser.read(waiting))
            else:
                data = ser.read(1)
                if data:
                    buffer.extend(data)
                continue

            while True:
                if state == 0:
                    idx = buffer.find(b'\xAA')
                    if idx != -1:
                        buffer = buffer[idx + 1:]
                        state = 1
                    else:
                        buffer.clear()
                        break

                elif state == 1:
                    if len(buffer) >= 1:
                        pkt_type = buffer[0]
                        if pkt_type in (1, 2):
                            buffer = buffer[1:]
                            state = 2
                        else:
                            state = 0
                    else:
                        break

                elif state == 2:
                    if len(buffer) >= 1:
                        pkt_len = buffer[0]
                        if pkt_len <= 160:
                            buffer = buffer[1:]
                            state = 3
                        else:
                            state = 0
                    else:
                        break

                elif state == 3:
                    if len(buffer) >= 1:
                        seq_num = buffer[0]
                        buffer = buffer[1:]
                        state = 4
                    else:
                        break

                elif state == 4:
                    if len(buffer) >= pkt_len:
                        payload = bytes(buffer[:pkt_len])
                        buffer = buffer[pkt_len:]
                        state = 0

                        if pkt_type == 0x01:
                            if expected_seq is not None and seq_num != expected_seq:
                                missing = (seq_num - expected_seq) % 256
                                if missing < 10:
                                    for _ in range(missing):
                                        try:
                                            plc_pcm = decoder.decode(None, FRAME_SIZE)
                                            stream.write(plc_pcm)
                                        except opuslib.OpusError:
                                            pass

                            expected_seq = (seq_num + 1) % 256

                            try:
                                pcm = decoder.decode(payload, FRAME_SIZE)
                                stream.write(pcm)
                            except opuslib.OpusError:
                                pass

                        elif pkt_type == 0x02:
                            if pkt_len >= 2:
                                batt = payload[0]
                                temp = payload[1]
                                print(f"Telemetry - SOC: {batt}%, Temp: {temp}C")
                    else:
                        break

    except KeyboardInterrupt:
        print("\nStopping audio stream...")
    finally:
        if stream is not None:
            stream.stop_stream()
            stream.close()
        if p is not None:
            p.terminate()
        if ser is not None and ser.is_open:
            ser.close()


if __name__ == "__main__":
    main()