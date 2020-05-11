# screx
Screen extender - Second screen using VNC and EVDI

Based off <https://github.com/rhofour/evdi-vnc>

## Usage
Make sure evdi, libevdi and libvncserver are installed.

Copy your edid.bin file to the folder where screx is run or specify its location
with `# screx -e file.bin`

Load the evdi kernel module with `# modprobe evdi`

Run `# screx`

## Building
Run `make`, you fill find the executable in the bin folder

## License
GNU GPL v2