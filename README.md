# OrbisKit

An all-in-one homebrew utility for jailbroken PS4s running GoldHEN. It's written in C against the
OpenOrbis toolchain.

**[Download the latest .pkg from Releases](https://github.com/hiya1985/OrbisKit/releases/latest)**.
Needs a jailbroken PS4 with GoldHEN loaded (tested on firmware 11.00). See [Installing](#installing).

| Tab | What it does |
|-----|--------------|
| **Home** | Dashboard with server status, uptime, an installed-app summary and 8 quick-action tiles (2 rows of 4). X runs a tile. Triangle picks a tile's action from a list (servers, Files/Apps/Tools/Settings, USB, internal storage, `/data`, payloads, backups, capture sync, largest files, sandbox escape, next theme) or empties it. *Reset all tiles to default* is in the same menu. |
| **Files** | Full file manager. Browse the internal drive and USB drives, mark multiple items, copy, move, delete, rename and create folders with an on-screen keyboard, create and extract `.zip` files, view text/hex files and pictures (PNG, JPG, BMP, GIF, TGA), calculate folder sizes, and see properties and free space. Options opens the folder menu: search by name, sort (name/size/date), bookmarks and quick paths. Long jobs run in the background with a progress bar and can be cancelled. |
| **Network** | **Web file manager** on port **8080**: open `http://<ps4-ip>:8080` in any browser to browse, download, upload (with a progress bar), make folders and delete. For FTP, use GoldHEN's own FTP server (port **2121**); OrbisKit doesn't run a second one. |
| **Payloads** | Lists `.bin` payloads found on USB (`/`, `/payloads`), in `/data/payloads` and in `/data/GoldHEN/payloads`, and sends them to GoldHEN's BinLoader (`127.0.0.1:9090`). Square marks one payload to be sent automatically every time the app starts. |
| **System** | Firmware, live CPU and SoC temperatures, IP address, sandbox status, server status, free space on internal storage and USB drives, and uptime. |
| **Tools** | Screenshot/video sync to `USB:/OrbisKit/captures` (skips files already copied), backup of the system databases (`app.db`, `addcont.db`, `av_content_bg.db`) to `USB:/OrbisKit/backup/`, backup of `/user/home` (saves, profile data) to USB, *Find largest files* (scans `/user` and lists the 200 biggest), a shortcut to the USB drive and sandbox escape retry. |
| **Settings** | Start screen (the Home dashboard, or the tab and folder you used last), web server auto-start, and the colour theme. Changes are saved automatically to `/data/OrbisKit/settings.ini`. |

**Apps tab:** every installed game/app with icon, title, ID, version, game/update/DLC size, and
shortcuts to its folders. **Storage analyzer:** Files → Options → *Find largest files here*
(or Tools → *Find the largest files on internal storage*). Progress bars show speed and time left.
The app opens on Home, or on the tab and folder you last used if you choose that in Settings.

**Web file manager extras:** Apps page with icons, *Send payload* page (send a `.bin` from your
phone/PC to GoldHEN's BinLoader), pictures/text/video/audio open in the browser (videos can seek),
drag-and-drop upload, rename, delete folders, and download any folder as a `.zip`.

**Also:** a line-by-line text editor that saves a `.bak` of the previous version,
CRC32/SHA-256 checksums, four colour themes, a clock, and a debug log at
`/data/OrbisKit/log.txt` that you can open from the System tab with X. The previous run is kept
as `log_prev.txt`.

## Controls

| Button | Action |
|--------|--------|
| L1 / R1 | Switch tabs |
| D-pad / left stick | Move (hold to scroll fast); Left/Right jumps a page |
| X | Open folder / confirm / run |
| O | Go up a folder / cancel / close |
| Square | Mark or unmark an item |
| Triangle | Choose tile action (Home) / actions menu (Files) / rescan (Payloads) |
| Options | Folder menu: search, sort, bookmarks, quick paths |
| L2 / R2 | Refresh the folder |

## Building (Windows)

1. Install the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases)
   and set `OO_PS4_TOOLCHAIN` to its folder.
2. Install [LLVM](https://github.com/llvm/llvm-project/releases) so that `clang` and `ld.lld` are on PATH.
3. **Recommended:** build the [GoldHEN Plugins SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK)
   and set `GOLDHEN_SDK` to its folder (the one containing `include\GoldHEN.h` and
   `libGoldHEN_Hook.a`). Without it the app stays in the normal app sandbox, so it can't reach
   `/system` or `/user/home`.
4. Run:
   ```powershell
   powershell -ExecutionPolicy Bypass -File build.ps1
   ```
   The output is `IV0000-ORBK00001_00-ORBISKIT00000000.pkg`.

`tools\gen_assets.ps1` regenerates the bitmap font (`src\font_data.h`) and the icon.

## Updates

The version is set in one place: `$VERSION` in `build.ps1`.

- `build.ps1` makes the full app package `IV0000-ORBK00001_00-ORBISKIT00000000.pkg`. To move to a
  new version, delete OrbisKit on the PS4 and install the new package. Settings in
  `/data/OrbisKit` are kept.
- `build.ps1 -Patch` makes an update package (`...-update-X.YY.pkg`), but the Package Installer
  currently refuses it ("unable to install"), so it isn't usable yet.

The full app and its updates share a fixed base version (`$MASTER_VER` = `01.00`). Don't change it,
or updates stop matching the installed app.

## Copying a build to the PS4 over Wi-Fi

`build.ps1 -Deploy -Ip <ps4-ip>` (or `deploy.ps1 -Ip <ps4-ip>`) uploads the package to `/data/pkg/`
on the console through GoldHEN's FTP (port 2121), so you can install it from the Package Installer
without a USB stick. Set the `PS4_IP` environment variable to leave out `-Ip`. It also deletes older
OrbisKit packages in `/data/pkg`, so only the new one is left.

Do **not** replace `/user/app/ORBK00001/app.pkg` directly. The PS4 rejects a swapped package with
"The data is corrupted", even when `app.json` and `app.pbm` (which ends with a SHA-256 of itself)
are updated to match.

## Installing

Copy the `.pkg` to a USB drive, plug it into the PS4 and install it from
*Settings > Debug Settings > Game > Package Installer* (GoldHEN must be loaded). You can also
send it with GoldHEN's remote package installer.

## Notes

- Delete can't be undone. Be careful in `/system*`, because damaging system files can stop the
  console from booting.
- On-screen keyboard: X types, Square deletes, Triangle adds a space, L1/R1 move the cursor,
  L2 toggles shift, R2 toggles symbols, Options confirms and O cancels.
- File viewer: Square switches between text and hex. Files over 4 MB show only the first 4 MB.
- Picture viewer: X on a picture opens it. Left/Right (or L1/R1) moves through the folder,
  Triangle hides the info bar and O closes it. The limit is 16 megapixels.
- Zip: each archive is extracted into a new folder named after it. Stored and deflate entries
  are supported. Zip64, encrypted entries and single files over 256 MB are not.
- Zip creation compresses files up to 64 MB and stores larger ones uncompressed. Archives
  are limited to 4 GB and 65,000 entries.
- Search looks through the current folder and its subfolders and shows up to 1,000 results.
- The web server has no password. Anyone on your local network can reach the console's files
  while it is running, so only use it on a network you trust and stop it when you're done.
  The web page only answers when opened by the PS4's IP address (`http://<ps4-ip>:8080`), which stops other websites from reaching it through your
  browser.
- Deleting never empties a mounted drive: a USB drive or game image mounted inside a folder is
  skipped, and a drive's own folder (like `/mnt/usb0`) can't be deleted. A move only
  deletes the original after the copy finished without errors.

## Credits

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain): compiler setup,
  libc and the package tools.
- [GoldHEN](https://github.com/GoldHEN/GoldHEN) and the
  [GoldHEN Plugins SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) by SiSTRo and the GoldHEN team:
  sandbox escape.
- [stb_image and stb_image_write](https://github.com/nothings/stb) by Sean Barrett (public domain /
  MIT): picture decoding, and zip compression and extraction.

## License

OrbisKit is released under the MIT License (see `LICENSE`). The stb libraries keep their own
public-domain / MIT license.
