# LBE-142x GPS Locked Clock Source Configuration Tool

This project provides a cross-platform configuration tool for Leo Bodnar LBE-1420 (single output) and LBE-1421 (dual output) GPS locked clock source devices.

It allows users to configure device settings, set frequencies, and monitor device status on both Windows and GNU/Linux systems (tested on Native Windows11 x64 & Native Ubuntu24.04LTS x64).

## Features

### Basic Features (Both Models LBE-1420/LBE-1421)
- Cross-platform compatibility (Windows and GNU/Linux)
- Set output frequencies with Hz precision
- Enable/disable outputs
- PLL/FLL mode selection
- Output power level control (normal/low power)
- Temporary frequency settings without EEPROM save
- Blink device LEDs for identification
- Retrieve and display comprehensive device status

### Advanced Features (LBE-1421 Only)
- Dual output frequency control
- 1PPS on OUT1 (Pulse Per Second) output configuration

## Prerequisites

### Windows
- Microsoft Visual Studio 2022 / MINGW64
- CMake (version 3.10 or higher)
- libusb (1.0 or higher) see https://github.com/libusb/libusb/releases

### GNU/Linux
- GCC or Clang
- CMake (version 3.10 or higher)
- libudev-dev
  - Ubuntu install libudev-dev dependency
   ```
   sudo apt update
   sudo apt install libudev-dev
   ```

## Building the Project

1. Clone the repository:
   ```
   git clone https://github.com/bvernoux/lbe-142x.git
   cd lbe-142x
   ```

2. Download and extract the LibUSB library for Visual Studio 2022 and MinGW64:
   - Go to the [LibUSB releases page](https://github.com/libusb/libusb/releases)
   - Download the latest release (e.g., `libusb-1.0.27.7z`)
   - Extract the contents to a `libusb` directory in the root of the project
     (The directory structure should look like: `lbe-142x/libusb/include`, `lbe-142x/libusb/MinGW64`, etc.)

3. Create a build directory and run CMake:

   For Windows (Visual Studio 2022):
      ```
      rm -rf build_VS2022
      mkdir build_VS2022 && cd build_VS2022
      cmake -G "Visual Studio 17 2022" -A x64 ..
      ```
   For Windows MinGW64:
      ```
      rm -rf build_MinGW64
      mkdir build_MinGW64 && cd build_MinGW64
      cmake -G "MinGW Makefiles" ..
      ```
   For GNU/Linux:
      ```
      rm -rf build
      mkdir build && cd build
      cmake ..
      ```

4. Build the project:

   For Windows (Visual Studio 2022):
   - Option1:
     - Open the generated solution file and build using Visual Studio
   - Option2 do the same step as for MinGW64 and GNU/Linux

   For MinGW64 and GNU/Linux:
      ```
      cmake --build . --config Release
      ```
   
      For a specific build type (default is Release):
      ```
      cmake --build . --config Debug
      ```

## Usage

After building the project, you can run the `lbe-142x` executable with various command-line options:

```
lbe-142x v1.1 17 Feb 2024 Leo Bodnar LBE-142x GPS locked clock source config
Usage: lbe-142x [OPTIONS]
Options:
  --f1 <freq> Set frequency for output 1 (1-1400000000 Hz) and save to flash
  --f1t <freq> Set temporary frequency for output 1
  --f2 <freq> Set frequency for output 2 (1-1400000000 Hz) and save to flash (LBE-1421 only)
  --f2t <freq> Set temporary frequency for output 2 (LBE-1421 only)
  --out <0|1> Enable or disable outputs
  --pll <0|1> Set PLL(0) or FLL(1) mode
  --pps <0|1> Enable or disable 1PPS on OUT1 (LBE-1421 only)
  --pwr1 <0|1> Set OUT1 power level: normal(0) or low(1)
  --pwr2 <0|1> Set OUT2 power level: normal(0) or low(1) (LBE-1421 only)
  --blink Blink output LED(s) for 3 seconds
  --status Display current device status
  
Note: --f1 <freq> Set frequency for output 1 for LBE-1420 can be set up to 1600000000 Hz
```

Examples:

1. Set frequency for output 1 to 10 MHz and save to flash:
   ```
   ./lbe-142x --f1 10000000
   ```

2. Set temporary frequency for output 2 to 10.5 MHz (LBE-1421 only):
   ```
   ./lbe-142x --f2t 10500000
   ```

3. Enable FLL mode:
   ```
   ./lbe-142x --pll 1
   ```

4. Enable 1PPS on OUT1 (LBE-1421 only):
   ```
   ./lbe-142x --pps 1
   ```

5. Set low power mode for output 1:
   ```
   ./lbe-142x --pwr1 1
   ```

6. Display device status:
   ```
   ./lbe-142x --status
   ```

## Status Display

The `--status` command shows comprehensive device information:
```
Device Status (0xXX):
  GPS Lock: Yes/No
  PLL Lock: Yes/No
  Antenna: OK/Short Circuit
  Output(s) Enabled: Yes/No
  OUT1 Frequency: XXXXX Hz
  OUT1 Power Level: Normal/Low
  OUT2 Frequency: XXXXX Hz (LBE-1421 only)
  OUT2 Power Level: Normal/Low (LBE-1421 only)
  1PPS on OUT1: Enabled/Disabled (LBE-1421 only)
  Mode: PLL/FLL
```

## Troubleshooting

### GNU/Linux
If you encounter permission issues when accessing the device, you may need to add a udev rule.

Create a file named `/etc/udev/rules.d/99-lbe.rules` with the following content:

```
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", MODE="0660", GROUP="plugdev"
```
Then add current user to group plugdev
```
sudo usermod -aG plugdev $(whoami)
```

Then, reload the udev rules:

```
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## Contributing

Contributions to this project are welcome. Please fork the repository and submit a pull request with your changes.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Leo Bodnar Electronics for the LBE-142x devices and documentation / protocol
- Simon Unsworth (https://github.com/simontheu/lbe-1420) for the initial implementation reference
