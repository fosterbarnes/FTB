# servefiles

FBI's Python scripts for sending installs from a PC to a 3DS. Requires [Python](https://www.python.org/downloads/).

On the 3DS, open FTB > FBI Legacy > Receive URLs over the network (or FBI's Remote Install), then run a script with the IP it shows. They send to port 5000.

**servefiles.py usage**: python servefiles.py (3ds ip) (file / directory) \[host ip\] \[host port\]

  - Supported file extensions: .cia, .tik, .cetk, .3dsx

**sendurls.py usage**: python sendurls.py (3ds ip) (url)...

**Drop scripts**: drag CIAs or a folder onto `drop cias or folder here.bat` (Windows), or run `drop cias or folder here.sh` to send the current folder. Both ask for the 3DS IP and run `servefiles.py`.

To send any file from a browser instead, use FTB's Web Transfer screen.
