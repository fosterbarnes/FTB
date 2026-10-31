<!-- User-owned file. Agents: do not edit unless the user explicitly asks, then change only the requested lines. -->
# ftb

A 3DS title manager, CIA installer, FTP server, and more. Built on [FBI](https://github.com/Steveice10/FBI).

## Download
<div align="center">
<table>
  <tr>
    <th>CIA QR Code (Remote Install)</th>
  </tr>
  <tr>
    <td align="center"><img src="./.res/ciaQr.svg" width="280" vspace="15"></td>
  </tr>
</table>
</div>

<!-- Quick Reference -->
<table border="0">
<tbody>
<tr>
<td valign="top"><a href="https://github.com/fosterbarnes/ftb/releases/download/v0.0.56/FTB.cia"><img src="https://raw.githubusercontent.com/fosterbarnes/res/main/btn/cia.svg" width="180" height="auto" alt="FTB.cia"/></a></td>
<td valign="top"><a href="https://github.com/fosterbarnes/ftb/releases/download/v0.0.56/FTB.3dsx"><img src="https://raw.githubusercontent.com/fosterbarnes/res/main/btn/3dsx.svg" width="180" height="auto" alt="FTB.3dsx"/></a></td>
<td valign="top"><a href="https://github.com/fosterbarnes/ftb/releases/download/v0.0.56/FTB.3ds"><img src="https://raw.githubusercontent.com/fosterbarnes/res/main/btn/3ds.svg" width="180" height="auto" alt="FTB.3ds"/></a></td>
<td valign="top"><a href="https://github.com/fosterbarnes/ftb/releases/download/v0.0.56/FTB.zip"><img src="https://raw.githubusercontent.com/fosterbarnes/res/main/btn/zip.svg" width="180" height="auto" alt="FTB.zip"/></a></td>
</tr>
</tbody>
</table>
<!-- End Quick Reference -->

## Features

- **[CIA Installer](https://github.com/fosterbarnes/ftb/wiki/CIA-Installer):** Quick CIA installer. Pick one or more folders for the app to scan on first launch, then it'll automatically scan those folders moving forward when entering the menu. Configurable.
- **[Title Manager](https://github.com/fosterbarnes/ftb/wiki/Title-Manager):** Redesigned title manager. 2 new layouts (grid, icon list) as well as the classic list view. Can be configured to show primary content on top or bottom screen.
- **[3DS Web Server](https://github.com/fosterbarnes/ftb/wiki/3DS-Web-Server):** Runs a local web server that you can access in basically any web browser. Send, receive, and install files without having to install an FTP client.
- **[FTP Server](https://github.com/fosterbarnes/ftb/wiki/FTP-Server):** This is literally just FTPD inside of FBI.
- **[Self-Updating](https://github.com/fosterbarnes/ftb/wiki/Install#updating):** Updates install and relaunch the app automatically.

Can be installed alongside FBI without changing or replacing it. This project is in active development. Full docs are in the
[wiki](https://github.com/fosterbarnes/ftb/wiki).


## Screenshots

<table>
  <tr>
    <th>Main Menu</th>
    <th>Title Manager (Grid)</th>
  </tr>
  <tr>
    <td align="center"><img src="./.res/screenshots/mainMenu.png" width="400" vspace="15"></td>
    <td align="center"><img src="./.res/screenshots/titleManagerGrid.png" width="400" vspace="15"></td>
  </tr>
  <tr>
    <th>Title Manager (Icon List)</th>
    <th>Title Manager (List)</th>
  </tr>
  <tr>
    <td align="center"><img src="./.res/screenshots/titleManagerIconList.png" width="400" vspace="15"></td>
    <td align="center"><img src="./.res/screenshots/titleManagerList.png" width="400" vspace="15"></td>
  </tr>
  <tr>
    <th>3DS Web Server</th>
    <th>FTP Server</th>
  </tr>
  <tr>
    <td align="center"><img src="./.res/screenshots/webServer.png" width="400" vspace="15"></td>
    <td align="center"><img src="./.res/screenshots/ftpServer.png" width="400" vspace="15"></td>
  </tr>
  <tr>
    <th>CIA Installer</th>
    <th>SD Browser</th>
  </tr>
  <tr>
    <td align="center"><img src="./.res/screenshots/ciaInstaller.png" width="400" vspace="15"></td>
    <td align="center"><img src="./.res/screenshots/sdBrowser.png" width="400" vspace="15"></td>
  </tr>
  <tr>
    <th>FBI Legacy</th>
    <th>Settings</th>
  </tr>
  <tr>
    <td align="center"><img src="./.res/screenshots/fbiLegacy.png" width="400" vspace="15"></td>
    <td align="center"><img src="./.res/screenshots/settings.png" width="400" vspace="15"></td>
  </tr>
</table>

## Why does this exist?

[FBI](https://github.com/Steveice10/FBI) is great. I used it for a long time, but found it slightly annoying to swap back and forth between FTPD and FBI when installing new CIAs after receiving them from a transfer. I figured 'just combine the open source apps to do both functions under one app'. Things just kind of snowballed from there, adding the web server, customizing progress and menu UI, etc. etc.

What I ended up with is an amalgamation of community projects that already existed, but worked together under one cohesive 'roof'. My individual contributions essentially boil down to:

- Creating all new menus, color themes & UI elements.
- Custom Web UI designed for phone or PC, facilitating file transfer and installing .cia files OTA.
- Custom 3D banner.

## Credit
- **FBI:** [Steveice10](https://github.com/Steveice10/FBI)
- **Original banner:** [OctopusRift](http://gbatemp.net/members/octopusrift.356526/), [Apache Thunder](https://gbatemp.net/members/apache-thunder.105648/), and [PabloMK7](http://gbatemp.net/members/pablomk7.345712/)
- **Original logo:** [PabloMK7](http://gbatemp.net/members/pablomk7.345712/)
- **SPI protocol:** [TuxSH](https://github.com/TuxSH/TWLSaveTool)
- **FTP server:** [ftpd](https://github.com/mtheall/ftpd) by Michael Theall, Dave Murphy, and TuxSH (GPL v3)

## License
ftb binaries are GPL v3. See `LICENSE.txt`.
