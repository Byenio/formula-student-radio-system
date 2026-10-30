# Baseline matrix -- tables

Steady state: from 20 s after the start of each run to 5 s before its end. Rates are medians of per-second values.

## Delivery -- fa885d3

| scenario                  | direction | sent/s | received/s | delivered |
|---------------------------|-----------|--------|------------|-----------|
| S2 driver PTT             | car → pit | 100    | 99         | 99.0 %    |
| S3 pit PTT                | pit → car | 100    | 100        | 100.0 %   |
| S4 telemetry              | car → pit | 50     | 50         | 100.0 %   |
| S5 telemetry + driver PTT | car → pit | 150    | 149        | 99.3 %    |
| S6 telemetry + pit PTT    | car → pit | 50     | 50         | 100.0 %   |
| S6 telemetry + pit PTT    | pit → car | 100    | 100        | 100.0 %   |

## Master CPU -- fa885d3

| scenario                  | radio | audio | RS-485 | stats | other | total % |
|---------------------------|-------|-------|--------|-------|-------|---------|
| S1 idle                   | 0.08  | 1.66  | 0.01   | 0.29  | 0.07  | 2.10    |
| S2 driver PTT             | 9.71  | 1.64  | 0.01   | 0.29  | 0.07  | 11.72   |
| S3 pit PTT                | 8.47  | 1.77  | 0.01   | 0.29  | 0.07  | 10.60   |
| S4 telemetry              | 4.89  | 1.64  | 0.28   | 0.29  | 0.08  | 7.17    |
| S5 telemetry + driver PTT | 13.44 | 1.63  | 0.28   | 0.28  | 0.06  | 15.70   |
| S6 telemetry + pit PTT    | 13.39 | 1.75  | 0.28   | 0.29  | 0.07  | 15.78   |

## Timed sections -- fa885d3

Mean and worst-case elapsed time per execution, in µs. Elapsed includes any interrupts and higher-priority tasks that ran in between.

| board  | section       | scenario                  | runs/s   | mean µs  | max µs  | util % |
|--------|---------------|---------------------------|----------|----------|---------|--------|
| master | audio.decode  | S3 pit PTT                | 100.0    | 11.687   | 15.22   | 0.117  |
| master | audio.decode  | S6 telemetry + pit PTT    | 99.9     | 11.692   | 15.73   | 0.117  |
| master | audio.encode  | S1 idle                   | 100.0    | 140.001  | 147.12  | 1.4    |
| master | audio.encode  | S2 driver PTT             | 100.0    | 137.7    | 144.56  | 1.377  |
| master | audio.encode  | S3 pit PTT                | 100.0    | 140.016  | 146.88  | 1.4    |
| master | audio.encode  | S4 telemetry              | 100.0    | 138.769  | 146.88  | 1.388  |
| master | audio.encode  | S5 telemetry + driver PTT | 100.0    | 136.716  | 149.05  | 1.367  |
| master | audio.encode  | S6 telemetry + pit PTT    | 100.0    | 138.805  | 150.99  | 1.388  |
| master | audio.frame   | S1 idle                   | 100.0    | 159.586  | 167.95  | 1.596  |
| master | audio.frame   | S2 driver PTT             | 100.0    | 158.461  | 166.38  | 1.585  |
| master | audio.frame   | S3 pit PTT                | 100.0    | 170.616  | 179.16  | 1.706  |
| master | audio.frame   | S4 telemetry              | 100.0    | 158.185  | 167.37  | 1.582  |
| master | audio.frame   | S5 telemetry + driver PTT | 100.0    | 157.249  | 171.09  | 1.572  |
| master | audio.frame   | S6 telemetry + pit PTT    | 100.0    | 169.408  | 182.61  | 1.694  |
| master | audio.wake    | S1 idle                   | 100.0    | 6.069    | 6.32    | 0.061  |
| master | audio.wake    | S2 driver PTT             | 100.0    | 5.962    | 6.7     | 0.06   |
| master | audio.wake    | S3 pit PTT                | 100.0    | 6.076    | 9.69    | 0.061  |
| master | audio.wake    | S4 telemetry              | 100.0    | 6.074    | 9.88    | 0.061  |
| master | audio.wake    | S5 telemetry + driver PTT | 100.0    | 6.053    | 12.19   | 0.061  |
| master | audio.wake    | S6 telemetry + pit PTT    | 100.0    | 6.076    | 10.74   | 0.061  |
| master | radio.airtime | S2 driver PTT             | 100.0    | 979.932  | 1120.87 | 9.799  |
| master | radio.airtime | S4 telemetry              | 50.2     | 795.321  | 1331.08 | 3.989  |
| master | radio.airtime | S5 telemetry + driver PTT | 150.3    | 917.857  | 1170.16 | 13.792 |
| master | radio.airtime | S6 telemetry + pit PTT    | 50.3     | 796.224  | 1307.84 | 4.002  |
| master | radio.send    | S2 driver PTT             | 100.0    | 853.317  | 854.87  | 8.533  |
| master | radio.send    | S4 telemetry              | 50.2     | 737.47   | 1352.69 | 3.699  |
| master | radio.send    | S5 telemetry + driver PTT | 150.3    | 818.548  | 961.51  | 12.3   |
| master | radio.send    | S6 telemetry + pit PTT    | 50.3     | 740.597  | 1209.63 | 3.721  |
| master | rs485.drain   | S4 telemetry              | 53.3     | 47.053   | 228.61  | 0.251  |
| master | rs485.drain   | S5 telemetry + driver PTT | 53.2     | 47.195   | 227.01  | 0.251  |
| master | rs485.drain   | S6 telemetry + pit PTT    | 53.2     | 47.118   | 245.45  | 0.251  |
| master | stats.self    | S1 idle                   | 10.0     | 445.771  | 1366.48 | 0.446  |
| master | stats.self    | S2 driver PTT             | 10.0     | 1318.292 | 2328.36 | 1.318  |
| master | stats.self    | S3 pit PTT                | 10.0     | 475.283  | 2245.53 | 0.476  |
| master | stats.self    | S4 telemetry              | 10.0     | 441.641  | 2132.78 | 0.442  |
| master | stats.self    | S5 telemetry + driver PTT | 10.0     | 1108.995 | 3084.21 | 1.109  |
| master | stats.self    | S6 telemetry + pit PTT    | 10.0     | 496.407  | 2982.08 | 0.497  |
| slave  | loop          | S1 idle                   | 151711.2 | 5.861    | 224.29  | 88.918 |
| slave  | loop          | S2 driver PTT             | 137970.9 | 6.515    | 881.34  | 89.89  |
| slave  | loop          | S3 pit PTT                | 143947.6 | 6.215    | 801.51  | 89.458 |
| slave  | loop          | S4 telemetry              | 146012.9 | 6.117    | 1152.85 | 89.32  |
| slave  | loop          | S5 telemetry + driver PTT | 132330.7 | 6.823    | 897.44  | 90.29  |
| slave  | loop          | S6 telemetry + pit PTT    | 137888.1 | 6.519    | 1155.07 | 89.887 |
| slave  | radio.send    | S3 pit PTT                | 99.6     | 686.038  | 691.91  | 6.834  |
| slave  | radio.send    | S6 telemetry + pit PTT    | 99.6     | 686.064  | 691.95  | 6.834  |
| slave  | relay         | S1 idle                   | 151711.2 | 1.613    | 220.05  | 24.466 |
| slave  | relay         | S2 driver PTT             | 137970.9 | 2.23     | 877.1   | 30.766 |
| slave  | relay         | S3 pit PTT                | 143947.6 | 1.879    | 699.98  | 27.052 |
| slave  | relay         | S4 telemetry              | 146012.9 | 1.855    | 1148.56 | 27.092 |
| slave  | relay         | S5 telemetry + driver PTT | 132330.7 | 2.522    | 891.68  | 33.368 |
| slave  | relay         | S6 telemetry + pit PTT    | 137888.1 | 2.165    | 1150.01 | 29.852 |
| slave  | stats.self    | S1 idle                   | 10.0     | 280.313  | 791.53  | 0.281  |
| slave  | stats.self    | S2 driver PTT             | 10.0     | 281.337  | 793.6   | 0.282  |
| slave  | stats.self    | S3 pit PTT                | 10.0     | 281.69   | 797.95  | 0.282  |
| slave  | stats.self    | S4 telemetry              | 10.0     | 281.54   | 792.82  | 0.282  |
| slave  | stats.self    | S5 telemetry + driver PTT | 10.0     | 281.365  | 793.62  | 0.282  |
| slave  | stats.self    | S6 telemetry + pit PTT    | 10.0     | 281.538  | 798.1   | 0.282  |
| slave  | usblink       | S1 idle                   | 151711.2 | 2.25     | 11.46   | 34.13  |
| slave  | usblink       | S2 driver PTT             | 137970.9 | 2.285    | 24.17   | 31.524 |
| slave  | usblink       | S3 pit PTT                | 143947.6 | 2.333    | 102.56  | 33.589 |
| slave  | usblink       | S4 telemetry              | 146012.9 | 2.262    | 23.32   | 33.035 |
| slave  | usblink       | S5 telemetry + driver PTT | 132330.7 | 2.3      | 23.36   | 30.439 |
| slave  | usblink       | S6 telemetry + pit PTT    | 137888.1 | 2.351    | 114.8   | 32.42  |
| slave  | usbx          | S1 idle                   | 151711.2 | 1.172    | 3.97    | 17.781 |
| slave  | usbx          | S2 driver PTT             | 137970.9 | 1.173    | 4.49    | 16.191 |
| slave  | usbx          | S3 pit PTT                | 143947.6 | 1.174    | 8.38    | 16.903 |
| slave  | usbx          | S4 telemetry              | 146012.9 | 1.173    | 4.4     | 17.122 |
| slave  | usbx          | S5 telemetry + driver PTT | 132330.7 | 1.174    | 4.48    | 15.538 |
| slave  | usbx          | S6 telemetry + pit PTT    | 137888.1 | 1.175    | 8.34    | 16.201 |

## Errors and drops -- fa885d3

| counter (median /s)      | s1  | s2  | s3  | s4  | s5  | s6  |
|--------------------------|-----|-----|-----|-----|-----|-----|
| master radio.crc_err     | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| slave radio.crc_err      | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| master rs485.bad_crc     | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| master rs485.noise       | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| master rs485.framing     | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| master rs485.queue_full  | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| slave relay.backpressure | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| slave usb.tx_ring_full   | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| master statlink.dropped  | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| slave statlink.dropped   | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
