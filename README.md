# KernelUNO v1.0

A Unix-like shell and RAM filesystem for Arduino UNO R3. Write files, control GPIO pins, read sensors, and run simple scripts all from the serial terminal.

<img width="769" height="659" alt="554" src="https://github.com/user-attachments/assets/82aa5f0c-bf22-4f83-865a-ba3b4258011e" />


# Commands (26):

- `ls`
- `cd`
- `pwd`
- `mkdir`
- `touch`
- `cat`
- `echo`
- `rm`
- `info`
- `pinmode`
- `write`
- `read`
- `gpio`
- `pwm`
- `sh`
- `uptime`
- `uname`
- `dmesg`
- `df`
- `free`
- `whoami`
- `clear`
- `reboot`
- `find`
- `alias`
- `slots`

## How It Works

The code manages a virtual filesystem stored in RAM:
- Maximum 10 files/directories
- Max 32 bytes per file content
- 12 character names
- Automatic `/home` and `/dev` directories created on boot
- `/dev/pin2`, `/dev/pin3`, `/dev/pin4` are special files for GPIO

GPIO control uses standard Arduino functions: `pinMode()`, `digitalWrite()`, `digitalRead()`, and `analogWrite()`.

Input is buffered from the serial connection and parsed line-by-line. Commands are case-insensitive.

## Planned Features

- EEPROM support
- I2C interface
- Date cmd
- neofetch cmd

## License

BSD3 - Original by [Arc1011](https://github.com/Arc1011/KernelUNO)

