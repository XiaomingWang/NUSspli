# NUSspli
Install content directly from the Nintendo Update Servers to your Wii U.

# Features
- Download titles from Nintendo's servers (NUS).
- Install the downloaded titles to external or internal storage.
- Search tickets on NUS and 'That title key site'.
- Create fake tickets at will or if not found.
- Shows the download speed.
- On screen keyboard.
- Can download anything available on the NUS.
- Complete HOME Menu support.
- Custom folder names for downloaded titles.

# Usage
To download a title, search on a Title Database for a title ID (Ex: WiiUBrew's database)\
To create a fake ticket, you will need the title ID and the encryption key (Avaible on 'That title key site').

To install the app, download and unzip the contents of the [latest release](https://github.com/V10lator/NUSspli/releases) and depending on how you will run the app, follow the next steps:

### Homebrew Launcher
- Move the folder to (SD:/wiiu/apps/).
- Run the app from HBL through Haxchi, Browserhax or any exploit you want.

### Home Menu
- Install a CFW.
- Move the folder to (SD:/install/) and install it with WUPInstaller.
- Run it from the HOME Menu.

# Building
- On Linux/WSL/Mac, install devkitPro.
- Install WUT with devktpros pacman.
- Clone the repo.
- Open the folder in a terminal and type `make`.
- If everything goes fine, you will have the resulting file "NUSspli.rpx".

# Info
NUSspli is based on [WUPDownloader](https://github.com/Pokes303/WUPDownloader) by Poke303.
