guide made by claudre
# Adacord

A custom [Vencord](https://github.com/Vendicated/Vencord) fork with a personal set of plugins bundled in, for anyone who wants the same setup.

> This project is a fork of Vencord and is licensed under **GPL-3.0**, the same license Vencord itself uses. Since GPL-3.0 is a copyleft license, this fork stays under GPL-3.0 too.

## Table of Contents

- [Linux Installation](#linux-installation)
- [Windows Installation](#windows-installation)

---

## Linux Installation

> **Note:** This method works reliably for Linux users on a similar CPU architecture (x86_64/AMD64). It does not work across operating systems — see [Windows Installation](#windows-installation) if that's you. This is because Adacord's build system uses `esbuild`, which ships a compiled, OS-specific binary rather than portable code, so a pre-built folder from Linux won't run correctly on Windows.

### What you need installed first

Even with a pre-built folder, the inject step runs through `pnpm`, so you'll need:

- Node.js
- pnpm

On Debian/Ubuntu/Linux Mint:

```bash
sudo apt install nodejs npm
sudo npm install -g pnpm
```

It's also worth installing `git`, just in case:

```bash
sudo apt install git
```

### Steps

1. Get the pre-built folder (e.g. `my-adacord.tar.gz`) and extract it:

   ```bash
   tar -xzf linux-adacord.tar.gz -C ~/Adacord
   cd ~/Adacord
   ```

2. **Fully close Discord.** Discord's files can't be patched while it's running.

3. Run:

   ```bash
   pnpm inject
   ```

   This launches Adacord's installer and points it at the already-built files. Follow the prompts to select your Discord installation.

4. Restart Discord. Your plugins should now appear under Adacord's plugin settings.

### Known rough edges

- **CPU architecture matters, not just "being on Linux."** ARM devices (e.g. Raspberry Pi) will likely hit the same kind of failure as Windows, for the same reason (esbuild's native binary).
- **Very different Linux distros or C libraries** (e.g. Alpine, NixOS) might cause other native-module mismatches beyond esbuild — this hasn't been confirmed one way or the other.
- **If `pnpm inject` fails outright**, fall back to building from source properly (clone this repo, then `pnpm install`, `pnpm build`, `pnpm inject`) — that reinstalls dependencies fresh for your machine.

---

## Windows Installation

> The Linux pre-built-folder shortcut does **not** work on Windows, for the same `esbuild` native-binary reason mentioned above. On Windows, build from source instead — every step here is confirmed against Adacord's own official documentation.

### What you need installed first

Per Adacord's own official docs, you need:

1. **Node.js**
2. **git**
3. **pnpm**

Make sure all three are added to your PATH.

**Installing git:**

```powershell
winget install --id Git.Git -e --source winget
```

Or download the installer from [git-scm.com/download/win](https://git-scm.com/download/win) — during setup, make sure the option to add Git to your PATH/command line is selected.

**Installing Node.js:**

Download and run the installer from [nodejs.org](https://nodejs.org) — this also gives you `npm`, needed for the next step.

**Installing pnpm** (after Node.js/npm is installed):

```powershell
npm install -g pnpm
```

**Verify all three:**

```powershell
git --version
node --version
pnpm --version
```

Each should print a version number. If any say "not recognized," restart your terminal, or that tool wasn't added to PATH during install.

### Steps

1. Clone this repo:

   ```powershell
   git clone https://github.com/Rowtha/adacord.git
   cd adacord
   ```

2. Install dependencies. Adacord's own docs are explicit: use `pnpm`, not `npm` or `yarn`.

   ```powershell
   pnpm install
   ```

3. Build:

   ```powershell
   pnpm build
   ```

4. **Fully close Discord.** If you hit a "files in use" error later, close all Discord processes via Task Manager, or run:

   ```powershell
   taskkill /f /im discord.exe
   ```

5. Inject:

   ```powershell
   pnpm inject
   ```

   This runs Adacord's installer and points it at what you just built. Follow the prompts to select your Discord installation.

6. Fully restart Discord. You should see Adacord's section appear in Discord's settings, along with the bundled plugins.

### Updating later

```powershell
git pull
pnpm build
```

Then restart Discord.

If `git pull` fails due to local changes (this discards any local edits, so back up anything important first):

```powershell
git reset --hard
git pull
pnpm build
```
