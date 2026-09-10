# GPIO List

| # | GPIO | Description | Module direction | OpCode Reference | MCU |
|---|---|---|---|---|---|
| 1 | 26 | Cutter error | Recive from PLC | 0x1F | PLC |
| 2 | 27 | Gripper error | Recive from PLC | 0x20 | PLC |
| 3 | 32 | Holder error | Recive from PLC | 0x21 | PLC |
| 4 | 34 | Encoder error | Recive from PLC | 0x22 | PLC |
| 5 | 25 | Cutter (R) Valve | Send to Manifold valves | 0x19 | PLC |
| 6 | 14 | Cutter (L) valve | Send to Manifold valves | 0x1A | PLC |
| 7 | 16 | Gripper valve | Send to Manifold valves | 0x1B | PLC |
| 8 | 17 | Holder valve | Send to Manifold valves | 0x1C | PLC |
| 9 | 21 | Encoder valve | Send to Manifold valves | 0x1D | PLC |
| 10 | 4 | Blower valve | Send to Manifold valves | 0x23 | PLC |
| 11 | 33 | Reset PLC | Send to Manifold valves | 0x1E | PLC |
| 12 | 18 | CLK | Ethernet | - | PLC |
| 13 | 19 | MISO | Ethernet | - | PLC |
| 14 | 23 | MOSI | Ethernet | - | PLC |
| 15 | 5 | CS | Ethernet | - | PLC |
| 16 | 22 | RST | Ethernet | - | PLC |
| 17 | 35 | INT | Ethernet | - | PLC |
| 18 | 16 | RX | RS485 | - | Motion |
| 19 | 17 | TX | RS485 | - | Motion |
| 20 | 18 | Fase A (R) | Encoder | - | Motion |
| 21 | 19 | Fase B (R) | Encoder | - | Motion |
| 22 | 21 | Indice Z (R) | Encoder | - | Motion |
| 23 | 22 | Fase A (L) | Encoder | - | Motion |
| 24 | 23 | Fase B (L) | Encoder | - | Motion |
| 25 | 25 | Indice Z (L) | Encoder | - | Motion |
| 26 | 4 | TX | CanBus | - | Motion |
| 27 | 5 | RX | CanBus | - | Motion |
| 28 | 32 | Laser (R) | Sensors | 0x4D | Motion |
| 29 | 34 | Laser (L) | Sensors | 0x4E | Motion |
| 30 | 14 | CLK | Ethernet | - | Motion |
| 31 | 12 | MISO | Ethernet | - | Motion |
| 32 | 13 | MOSI | Ethernet | - | Motion |
| 33 | 15 | CS | Ethernet | - | Motion |
| 34 | 27 | RST | Ethernet | - | Motion |
| 35 | 33 | INT | Ethernet | - | Motion |
| 36 | 26 | Saefty exhaust | Safety | 0x4F | Motion |
| 37 | 16 | Green | Recive from HMI | 0x40 / 0x44 / 0x45 | Andon |
| 38 | 17 | Yellow | Recive from HMI | 0x49 | Andon |
| 39 | 25 | Buzzer | Recive from HMI | 0x46 / 0x47 / 0x49 | Andon |
| 40 | 21 | Red | Recive from HMI | 0x42 / 0x46 | Andon |
| 41 | 33 | Pressure Sensor FRL | Recive form sensor | 0x50 | Andon |
| 42 | 5 | CS | Ethernet | - | Andon |
| 43 | 18 | CLK | Ethernet | - | Andon |
| 44 | 22 | RST | Ethernet | - | Andon |
| 45 | 23 | MOSI | Ethernet | - | Andon |
| 46 | 35 | INT | Ethernet | - | Andon |
