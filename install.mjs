#!/usr/bin/env node
/*
 * Adacord installer — detects OS and runs the correct install path.
 * Usage:
 *   node install.mjs [target-dir]
 * Requires: node >=22, pnpm on PATH.
 */

import { execSync, spawnSync } from "node:child_process";
import { existsSync, mkdirSync } from "node:fs";
import { mkdir, rm, writeFile } from "node:fs/promises";
import { platform } from "node:os";
import { resolve } from "node:path";

const REPO = "Rowtha/adacord";
const TARGET = resolve(process.argv[2] ?? "./adacord");
const OS = platform();

const LINUX_ASSET = "adacord-linux.tar.gz";
const SRC_TARBALL = `https://github.com/${REPO}/archive/refs/heads/main.tar.gz`;
const LATEST_RELEASE_API = `https://api.github.com/repos/${REPO}/releases/latest`;

function log(msg) { console.log(`[adacord] ${msg}`); }
function die(msg) { console.error(`[adacord] error: ${msg}`); process.exit(1); }

function checkPnpm() {
    const r = spawnSync("pnpm", ["--version"], { stdio: "pipe", shell: true });
    if (r.status !== 0) die("pnpm not found on PATH. Install pnpm first: https://pnpm.io/installation");
}

function run(cmd, args, cwd) {
    log(`$ ${cmd} ${args.join(" ")}`);
    const r = spawnSync(cmd, args, { stdio: "inherit", cwd, shell: true });
    if (r.status !== 0) die(`command failed: ${cmd} ${args.join(" ")}`);
}

async function downloadFile(url, dest) {
    log(`downloading ${url}`);
    const res = await fetch(url, { redirect: "follow", headers: { "User-Agent": "adacord-installer" } });
    if (!res.ok) die(`download failed (${res.status}): ${url}`);
    const buf = Buffer.from(await res.arrayBuffer());
    await writeFile(dest, buf);
}

async function extractTarGz(archive, outDir) {
    log(`extracting ${archive} -> ${outDir}`);
    // strip 1 flattens the top-level directory in GitHub source tarballs
    run("tar", ["-xzf", archive, "-C", outDir, "--strip-components=1"]);
}

async function fetchLinuxAssetUrl() {
    log(`resolving latest release from ${REPO}`);
    const res = await fetch(LATEST_RELEASE_API, { headers: { "User-Agent": "adacord-installer", Accept: "application/vnd.github+json" } });
    if (!res.ok) die(`could not query GitHub releases (${res.status})`);
    const data = await res.json();
    const asset = data.assets?.find(a => a.name === LINUX_ASSET);
    if (!asset) die(`no "${LINUX_ASSET}" asset found on latest release. Available: ${data.assets?.map(a => a.name).join(", ") ?? "(none)"}`);
    return asset.browser_download_url;
}

async function prepareTargetDir() {
    if (existsSync(TARGET)) {
        die(`target directory already exists: ${TARGET}`);
    }
    mkdirSync(TARGET, { recursive: true });
}

async function installWindows() {
    log("detected: Windows — cloning source (needed for in-place git updates)");
    // git-clone (not tarball) so the built-in git updater can `git pull` later.
    run("git", ["clone", "--depth=1", `https://github.com/${REPO}.git`, TARGET]);

    run("pnpm", ["install"], TARGET);
    run("pnpm", ["build"], TARGET);
    run("pnpm", ["inject"], TARGET);
}

async function installLinux() {
    log("detected: Linux — using prebuilt release");
    await prepareTargetDir();
    const assetUrl = await fetchLinuxAssetUrl();
    const archive = resolve(TARGET, "..", LINUX_ASSET);
    await downloadFile(assetUrl, archive);
    await extractTarGz(archive, TARGET);
    await rm(archive, { force: true });

    run("pnpm", ["install"], TARGET);
    run("pnpm", ["inject"], TARGET);
}

async function main() {
    log(`target: ${TARGET}`);
    checkPnpm();

    if (OS === "win32") await installWindows();
    else if (OS === "linux") await installLinux();
    else die(`unsupported OS: ${OS}. This installer supports Windows and Linux only.`);

    log("done. Discord should now be patched with Adacord.");
}

main().catch(err => die(err?.message ?? String(err)));
