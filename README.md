# SCDA P2P – LAN multiplayer over ZeroTier for Splinter Cell Double Agent (PC)

Ubisoft's online services for Double Agent are gone, but the game still has a LAN mode.
This mod lets that LAN mode work over a ZeroTier virtual network so friends can play
over the internet.

## Why plain ZeroTier didn't work
`SCDA_online.exe` asks Windows for its network adapters at startup and ties its network
connection to one of them, in practice the first one (usually your real Ethernet/Wi-Fi).
So LAN broadcasts never reach the ZeroTier network. On top of that, the LAN session search
is sent to `255.255.255.255` (UDP port 8889), which Windows only sends out of the main adapter.

The mod's `dinput8.dll` fixes both:
- it makes the game's network layer see only the ZeroTier adapter, and
- it copies every LAN search to the ZeroTier network: once as a ZeroTier subnet broadcast, and
  once directly to every address in the ZeroTier range (in case the network doesn't pass broadcasts).

Tested working between two PCs over the internet (v0.6).

## Installing
1. Unzip `SCDA_P2P_Mod_v*.zip` anywhere (unzipping it inside the game folder lets the installer find the game by itself).
2. Double-click **`Install.bat`**. It finds the game, or asks you for the folder that contains `SCDA-Online`, then asks:
   - **Yes:** install the mod into the existing game folder.
   - **No:** make a separate **multiplayer-only copy** (about 2.1 GB) somewhere else, with the mod already in it.
     The single-player files (about 8 GB) aren't needed for it, so you can delete the full install afterwards if you only play multiplayer.
3. A desktop shortcut **SCDA Multiplayer (P2P)** is created. Use it to play.

The installer never overwrites your `SCDA_P2P.ini` settings, and it backs up any other `dinput8.dll`
it finds as `dinput8.dll.backup`.

Command-line use: `Install.ps1 -GameDir <folder> -Mode Mod|OnlineOnly [-TargetDir <folder>] [-NoShortcut]`.

Note: the multiplayer-only copy has been play-tested over the internet. Its menus have an option to
return to single player, which can't work without the single-player files.

## Files (all go in `SCDA-Online\System`)
| File | What it does |
|---|---|
| `SCDA_P2P_Launcher.exe` | Starts ZeroTier, joins your network, starts the game, cleans up when the game closes |
| `dinput8.dll` | Adapter fix. Loaded by the game automatically; forwards DirectInput to Windows |
| `SCDA_P2P.ini` | Settings (network ID, chosen IP, options) |
| `SCDA_P2P.log` | Written by `dinput8.dll` each time the game starts – check it if games aren't found |

Uninstall: delete those files.

## One-time setup
1. **Create a ZeroTier network** (one person does this) at https://my.zerotier.com.
   - Leave *IPv4 Auto-Assign* on (the default).
   - Turn on *Enable Broadcast* in the network settings (recommended; the mod also works without it).
   - Either set *Access Control* to **Public**, or authorize each player's node ID when they join.
   - Share the 16-character **network ID** with your friends.
2. **Every player installs ZeroTier One** from https://www.zerotier.com/download/.

## Playing
1. Run `SCDA_P2P_Launcher.exe` and accept the Windows administrator prompt
   (needed to talk to the ZeroTier service and add a firewall rule).
2. Paste the network ID; it is remembered next time.
3. Press **PLAY**. The launcher:
   - starts the ZeroTier service if needed,
   - adds a Windows Firewall rule for the game (first time only),
   - joins the network and waits for a ZeroTier IP,
   - writes that IP into `SCDA_P2P.ini` so the game uses it,
   - launches the game; when the game closes it closes the ZeroTier tray app (optional) and closes itself.
     You stay a member of the network, so next time it reconnects without re-authorization.
4. In the game choose **LAN**. One player creates a session, the others should see it in the list.

Options in the launcher:
- *Close the ZeroTier app when the game closes* – closes only the tray app. The ZeroTier service is never stopped or changed, so ZeroTier keeps working for anything else.

Shortcut tip: `SCDA_P2P_Launcher.exe --play` skips the window and goes straight to the game
with the saved network ID.

## Troubleshooting
- Open `SCDA_P2P.log`. You should see `game will use "ZeroTier Virtual Port" ip=...`.
  If it says *no adapter matched*, the game saw all adapters unchanged and the fix didn't apply.
- "ACCESS_DENIED" in the launcher: the network admin must tick *Auth* for your node ID
  (shown in the launcher, with a Copy button).
- Still no games listed: make sure everyone launched through the launcher, and that all players
  can ping each other's ZeroTier IPs.
- `SCDA_P2P.log` records the game's network traffic (`NET send ->` / `NET recv <-` lines, up to
  `TraceLines=` lines, default 300, `0` turns it off). The host should show `NET recv <-` from the
  searching player's ZeroTier IP on port 8889; if it doesn't, the search isn't arriving.
- If your ZeroTier network's range is bigger than about 1000 addresses, the direct copies only cover
  the /24 around your own IP. List other players' ZeroTier IPs under `Peers=` in `SCDA_P2P.ini`
  (comma separated), or turn on *Enable Broadcast* for the network.

## Building
`build.ps1` builds both files into `dist\` (`-Install` also copies them into `SCDA-Online\System`).
`package.ps1 [-Version 0.1]` builds and creates `dist\SCDA_P2P_Mod_v<Version>.zip` (installer, README, binaries, source).
- Launcher: the .NET Framework 4 `csc.exe` that comes with Windows.
- `dinput8.dll`: Visual Studio C++ tools (x86, static CRT), or a 32-bit MinGW compiler.

Source: `src\launcher\Launcher.cs`, `src\dll\dinput8_proxy.c`.
