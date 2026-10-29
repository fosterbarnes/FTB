# servefiles

Scripts for sending files from a PC to a 3DS running FTB.

## ftbsend.py (FTB Receive)

Sends files and folders to the **Receive** screen in FTB. Requires [Python 3](https://www.python.org/downloads/). No other installs.

1. On the 3DS, open FTB > Receive. Keep it open.
2. On the PC, run the script with the files or folders you want to send.

```
python ftbsend.py game.cia saves
```

The script finds the 3DS on your Wi-Fi by itself. If you have more than one 3DS, it lists them and asks which one to use. If it finds none, it says so. Check that the 3DS is on the same network, or pass `--ip`.

**Options**

  - `--ip HOST[:PORT]`: use this address and skip searching. The port defaults to 80.
  - `--to /3ds/`: SD card folder for saved files (default `/`).
  - `--save`: save CIA, TIK, CETK, and 3DSX files to SD instead of installing them.
  - `--replace`: overwrite saved files that already exist.

CIA, TIK, CETK, and 3DSX files are installed. Other files are saved to the `--to` folder. A folder keeps its name, so `saves` sends `saves/a.sav`.

Accept the transfer on the 3DS first. The script waits for that, then sends. It prints one line per file and exits with 1 if any file failed.

**Drop scripts**

  - `drop cias or folder here.bat` (Windows): drag files or folders onto it.
  - `drop cias or folder here.sh` (Linux and macOS): pass files or folders as arguments. With no arguments, it sends the current folder.

## servefiles.py and sendurls.py (FBI)

These send to FBI's Remote Install on the 3DS, on port 5000. FTB's Receive screen also accepts them.

**servefiles.py usage**: python servefiles.py (3ds ip) (file / directory) \[host ip\] \[host port\]

  - Supported file extensions: .cia, .tik, .cetk, .3dsx

**sendurls.py usage**: python sendurls.py (3ds ip) (url)...
