# D7VK for Anarchy Online (Project Rubi-Ka)

An **unofficial** build of [D7VK](https://github.com/WinterSnowfall/d7vk) with extra
performance changes, made for the Project Rubi-Ka Anarchy Online client.

> [!IMPORTANT]
> This is not an official D7VK release. Please report problems in
> [this fork's issues](https://github.com/wannabuh/d7vk/issues), **not** to the upstream
> D7VK project.

## What's different from D7VK v2.3

- **Windowed present without a GPU readback.** When the game shows a finished frame in
  windowed mode, D7VK no longer copies the whole frame from the GPU back to the CPU first.
- **Cheaper per-draw vertex uploads.**
- **Deferred mode (optional).** D3D7 calls are recorded on the game's thread and executed
  on a separate worker thread, so the game can prepare the next frame in parallel. In busy
  areas with many players this raised the frame rate substantially in testing. Turned on
  with `ddraw.deferredMode = True` in `dxvk.conf`. The game may run up to one frame ahead
  of what is on screen.

## Requirements

- A GPU and driver with **Vulkan 1.4** support.
- **Linux:** Wine. Tested with Wine 11.x.
- **Windows:** Windows 10 or 11. **Not tested yet**, see [Windows notes](#windows-notes).

## What's in the download

| File | Purpose |
|---|---|
| `ddraw.dll` | D7VK itself (32-bit, which is what Anarchy Online uses) |
| `dxvk.conf` | Settings file. Turns on deferred mode. |
| `LICENSE` | D7VK's license |
| `AO-INSTALL.md` | These instructions |

## Choosing the launcher's DirectX layer

Both installs below need this first. D7VK replaces the launcher's **dgVoodoo** layers, and
the two can't be used at the same time: dgVoodoo ships its own `DDraw.dll`, which clashes
with D7VK's `ddraw.dll`.

1. Open the PRK launcher and go to **Settings**.
2. Set the DirectX layer to anything **except** a dgVoodoo option, for example
   `aoia-resfix`.
3. Close the Settings window.

Widescreen resolutions (2560x1440 was tested) work with D7VK and the `aoia-resfix` layer.

## Linux install

1. Choose a non-dgVoodoo DirectX layer (see above).
2. Copy `ddraw.dll` and `dxvk.conf` into the client folder, the folder that contains
   `AnarchyOnline.exe`.
3. Start the game from the launcher as usual.

No Wine settings are needed: Wine loads a `ddraw.dll` found next to the game by itself.

**Check that it's active** by starting the launcher from a terminal with a log folder set:

```sh
mkdir -p ~/ao-logs
D7VK_LOG_PATH=~/ao-logs ./PRK.Launcher
```

After starting the game, `~/ao-logs/AnarchyOnline_ddraw.log` should contain
`D7VK: Deferred mode enabled`.

**If the log file doesn't appear**, your Wine prefix is set to prefer Wine's own DirectDraw.
Run `winecfg` for the prefix the launcher uses (`WINEPREFIX=/path/to/prefix winecfg`), open
the **Libraries** tab, add `ddraw`, and set it to **Native then Builtin**.

## Windows install

1. Choose a non-dgVoodoo DirectX layer and close Settings **before** copying anything (see
   above). This order matters on Windows, see the notes below.
2. Copy `ddraw.dll` and `dxvk.conf` into the client folder, the folder that contains
   `AnarchyOnline.exe`.
   If Windows asks whether to **replace** an existing `ddraw.dll`, a dgVoodoo layer is
   probably still selected. Cancel and go back to step 1.
3. Start the game from the launcher as usual.

**Check that it's active:** after starting the game, a file named `AnarchyOnline_ddraw.log`
should appear next to `AnarchyOnline.exe` and contain `D7VK: Deferred mode enabled`.

### Windows notes

- **Untested.** Nobody has tried this build on Windows yet. Upstream D7VK is developed for
  Wine and is not tested on Windows either. Reports, good or bad, are very welcome.
- **The launcher can remove or overwrite `ddraw.dll`.** Windows file names are not
  case-sensitive, so dgVoodoo's `DDraw.dll` and D7VK's `ddraw.dll` count as the same file.
  Selecting a dgVoodoo layer overwrites D7VK, and switching away from dgVoodoo can delete
  it. After changing the DirectX layer, check that `ddraw.dll` is still D7VK's (about 9 MB)
  and copy it again if needed.
- **AMD Radeon RX 5000 / 6000 (RDNA1 / RDNA2):** D7VK is built on DXVK 3.x, whose release
  notes report performance problems with these GPUs on Windows.
- **Overlays** that hook into games (Discord, MSI Afterburner / RivaTuner, Steam) can cause
  problems with Vulkan-based layers. Try disabling them if the game crashes or shows a black
  screen.

## Settings (`dxvk.conf`)

The included file contains a single line:

```
ddraw.deferredMode = True
```

- If you see freezes, hangs or missing objects, remove that line (or set it to `False`) first,
  then report the problem.
- All other D7VK and DXVK options from the upstream `dxvk.conf` work as well, for example
  `d3d9.maxFrameRate = 141` to cap the frame rate.

## Uninstall

1. Delete `ddraw.dll` and `dxvk.conf` from the client folder.
2. Select the DirectX layer you want in the launcher settings again.

## Reporting a problem

Open an issue on [this fork](https://github.com/wannabuh/d7vk/issues) with:

- Your OS (Linux distribution or Windows version), GPU and driver version, and Wine version
  on Linux.
- The `AnarchyOnline_ddraw.log` file.
- Whether the problem goes away with `ddraw.deferredMode = False`.

## Source and license

The source for this build is the `main` branch of
[wannabuh/d7vk](https://github.com/wannabuh/d7vk). Each change also has its own branch
(`windowed-present`, `upbuffer-refs`, `deferred-mode`). D7VK is distributed under the
zlib/libpng license, see `LICENSE`.
