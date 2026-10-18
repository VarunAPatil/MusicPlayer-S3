# USB NCM with ESP32-side DSP

This is a separate ESP-IDF firmware project for the ESP32-S3. It preserves the original `firmware/usb-ncm` receiver and its laptop-generated spectrum-bar protocol. Use this project when the laptop should forward PCM samples and the ESP32 should perform the spectrum analysis.

## Build and flash

From this directory, with ESP-IDF activated:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p YOUR_SERIAL_PORT flash
```

Flash this firmware for the **Wired bars only (ESP32 DSP over USB NCM)** mode. It uses the existing USB NCM network configuration (`192.168.77.1`, UDP port `4210`) and the board's MAX7219 pins: CLK GPIO18, DIN GPIO16, LOAD GPIO17. OLED video and lyrics remain supported by this receiver.

On the host, run `host/audio_pcm_streamer.py`. It captures a PulseAudio/PipeWire output monitor at 16 kHz mono signed 16-bit PCM and sends 517-byte UDP packets over the USB NCM network interface:

| Byte offset | Size | Format | Meaning |
| ---: | ---: | --- | --- |
| 0 | 1 | ASCII `A` (`0x41`) | Audio packet marker |
| 1–4 | 4 | Unsigned 32-bit, big-endian | Packet sequence number |
| 5–516 | 512 | 256 signed 16-bit little-endian samples | Mono PCM audio |

The packet is binary, not CSV or a WAV file. Each packet represents 16 ms of audio (256 samples at 16 kHz); the sender targets 62.5 packets/s. USB NCM carries these UDP datagrams as network traffic to `192.168.77.1:4210`.

## DSP pipeline

`audio_dsp.c` maintains a 1024-sample frame and advances it by 256 samples per packet. It applies a Hann window, packs the real input into ESP-DSP's interleaved complex-float buffer, runs `dsps_fft2r_fc32()` followed by `dsps_bit_rev_fc32()`, and reads the resulting magnitudes. It maps 40–7500 Hz into eight logarithmically spaced bands, then applies adaptive normalization and attack/decay smoothing. The 16 kHz sample rate gives an 8 kHz Nyquist limit. `CENTER_MIRROR`, `MATRIX_ROTATION`, `MIRROR_X`, `MIRROR_Y`, and `DIGITS_ARE_COLUMNS` in `main/main.c` control matrix layout and orientation.

The project declares Espressif's managed `espressif/esp-dsp` component (`^1.8.2`) in `main/idf_component.yml`; ESP-IDF downloads and builds it through the component manager. ESP-DSP uses interleaved complex values (`real, imaginary` for each FFT bin), with a 1024-point transform stored in 2048 floats. See the [ESP-DSP FFT API](https://github.com/espressif/esp-dsp/blob/master/modules/fft/include/dsps_fft2r.h) and [Espressif FFT example](https://github.com/espressif/esp-dsp/tree/master/examples/fft).

See the repository [README](../../README.md) for full setup and host usage instructions.
