#!/usr/bin/env python3

from __future__ import annotations

import base64
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

APP_NAME = "Adacord Installer"
REPO_URL = "https://github.com/Rowtha/adacord.git"
ICON_BASE64 = (
    "iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAAACXBIWXMAAAsTAAALEwEA"
    "mpwYAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAPYSURBVHgB7Zy7jtQwFIYd"
    "RAFiBU+BRAcSDQXzzDwCxWxDDQWIR6BACMFS4d2Z2BnbE8d3n9j+P2mVHY9zO/Z/zrHj"
    "DGMAAAAAAAAAAMBITAF1ecZjxRByfu5Zj5zHrEM4v7b/NE3n8tN2T/hczflu1m5KO9Dl"
    "xnLfYcr5ufjMhfH3Zf0HHrGOEY22W+Of6NIFSTzczpqsqjZUzwpYer7NfZ3K5Z/8XJte"
    "FXC2pOj5XG7Fd5dgcZEGmZsaJguSGDbmRrkM2tXosgF8e7ItXa1J10HYhUdDFVdE12lo"
    "IlymsawgozeAzbi8VkY0cgPYejivmY6OGgNMI7ssXiwWjKQALv9U45uDsLXeXzIWjNAA"
    "i9G3RsS2hEh+V6oRendBWm9XDLlWcesgxcYHSEOJ6X4yrsRxWUZ6bYCsxpfup0SDDuWCV"
    "F9+2rr8uqxTclww7FyQNKraCL4Bm2UcFwwXhM2MRn0o42qMEmoYMguypZWmKlQXVCoNb"
    "dYFuZ7zunqry6jq/iWDcPcP5dewTTtsNchazMhBcw0gDfD703EukE50kt+zzfJnrw+bRr"
    "z7Oh/36Su9nvr/v+9H9uTl4eoZcwzDZUF/Ps8GXuxpNpjg7puoVzhKNtsA0ntM/0WBMN"
    "TSLR3lXJQv3ddSzkV5qYbAXBAx2RSgBKksvpG51oRybdOsEqAAYrLHgAxK0Lq8NZU0em"
    "RtJdiuV7105gEUQEzwGzKho8GA9wa0A//8eCv21/defLAU2FX5vHn+7sBS+PvlqB1Pdl"
    "Xr9Rg8jBOYXnMdKICYkBhwbknp232V4BETuFFvLnVmI5O6+1aWEhWDuC6wYtkRFEBMTB"
    "aUSwlsc39ZHKmEZCqNE6AAYlLGAUlKcNdTTnIiUAmpmHNNCeOETaAAYnKMhKOU4CTaBy"
    "c/MDkf4ObNIfVGMBJugZxzQXmV4MiCcvngDZKl5AMUQEyJJ2JZlOCbBVnLGwEKIKbkM+"
    "EgJVzN+4f2eKO8FaAAYmqsitCU4KrHzNnISCW0AhRATM11QWEJimXVA7IgkJX9roxLXM"
    "XQClAAMbtVgO/az4JZEGZDR2C/MSBw7WfGLOh86F+3+rqgKfA9hJu3fu8PQAHE7FgBxm"
    "oH8clXCdnPH7gW1RcogJj9vyETqYTk03quwLNdjy9QADHtvCMWqAR1TxZ1PrGJVYInUA"
    "Axe5483Oy5Pz7M7w/45uW29f2h5bLHvzi8Zw4wEm6BPccAWw8STjhtnBC72mJl3VGSF4"
    "ECiGn/pwqIlJALKICYfn6so7IScgEFENPaIgKV1CdWqWSxHRQAAAAAAAAAAACAitwDddQb"
    "MB9RPU8AAAAASUVORK5CYII="
)


@dataclass(frozen=True)
class DiscordVariant:
    branch: str
    config_directory: str
    system_directories: tuple[str, ...]
    executables: tuple[str, ...]
    flatpak_id: str


@dataclass(frozen=True)
class DiscordInstall:
    variant: DiscordVariant
    root: Path
    resources: Path
    source: str


VARIANTS = (
    DiscordVariant(
        "Stable",
        "discord",
        ("discord", "Discord"),
        ("Discord", "discord"),
        "com.discordapp.Discord",
    ),
    DiscordVariant(
        "PTB",
        "discordptb",
        ("discord-ptb", "discordptb", "DiscordPTB"),
        ("DiscordPTB", "discordptb"),
        "com.discordapp.DiscordPTB",
    ),
    DiscordVariant(
        "Canary",
        "discordcanary",
        ("discord-canary", "discordcanary", "DiscordCanary"),
        ("DiscordCanary", "discordcanary"),
        "com.discordapp.DiscordCanary",
    ),
    DiscordVariant(
        "Development",
        "discorddevelopment",
        ("discord-development", "discorddevelopment", "DiscordDevelopment"),
        ("DiscordDevelopment", "discorddevelopment"),
        "com.discordapp.DiscordDevelopment",
    ),
)

PROCESS_NAMES = {
    executable.casefold()
    for variant in VARIANTS
    for executable in variant.executables
}
ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


class InstallerError(RuntimeError):
    pass


class OperationCancelled(RuntimeError):
    pass


def version_key(path: Path) -> tuple[int, ...]:
    numbers = tuple(int(number) for number in re.findall(r"\d+", path.name))
    return numbers or (0,)


def valid_resources(path: Path) -> bool:
    return path.is_dir() and (
        (path / "app.asar").exists() or (path / "_app.asar").exists()
    )


def locate_discord_resources(root: Path) -> tuple[Path, Path] | None:
    try:
        root = root.expanduser().resolve()
    except OSError:
        return None

    if root.name == "resources" and valid_resources(root):
        application_directory = root.parent
        install_root = (
            application_directory.parent
            if application_directory.name.startswith("app-")
            else application_directory
        )
        return install_root, root

    if root.name.startswith("app-") and valid_resources(root / "resources"):
        return root.parent, root / "resources"
    if not root.is_dir():
        return None

    try:
        app_directories = sorted(
            (
                child
                for child in root.iterdir()
                if child.is_dir() and child.name.startswith("app-")
            ),
            key=version_key,
            reverse=True,
        )
    except OSError:
        app_directories = []

    for app_directory in app_directories:
        resources = app_directory / "resources"
        if valid_resources(resources):
            return root, resources

    resources = root / "resources"
    if valid_resources(resources):
        return root, resources
    return None


def resolve_discord_install(
    root: Path,
    variant: DiscordVariant,
    source: str,
) -> DiscordInstall | None:
    located = locate_discord_resources(root)
    if located is None:
        return None
    install_root, resources = located
    return DiscordInstall(variant, install_root, resources, source)


def normalized_variant_name(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.casefold())


def variant_for_custom_install(
    install_root: Path,
    resources: Path,
) -> DiscordVariant | None:
    build_info = resources / "build_info.json"
    try:
        channel = json.loads(build_info.read_text(encoding="utf-8")).get(
            "releaseChannel",
            "",
        )
    except (OSError, UnicodeError, json.JSONDecodeError, AttributeError):
        channel = ""

    normalized_channel = normalized_variant_name(str(channel))
    channel_names = {
        "stable": "Stable",
        "ptb": "PTB",
        "canary": "Canary",
        "development": "Development",
    }
    expected_branch = channel_names.get(normalized_channel)
    if expected_branch is not None:
        return next(
            variant for variant in VARIANTS if variant.branch == expected_branch
        )

    directories = [install_root, resources.parent]
    try:
        directories.extend(
            child
            for child in install_root.iterdir()
            if child.is_dir() and child.name.startswith("app-")
        )
    except OSError:
        pass

    detection_order = (*VARIANTS[1:], VARIANTS[0])
    for variant in detection_order:
        if any(
            (directory / executable).is_file()
            for directory in directories
            for executable in variant.executables
        ):
            return variant

    path_names = {
        normalized_variant_name(part)
        for part in (*install_root.parts, *resources.parts)
    }
    for variant in detection_order:
        known_names = {
            normalized_variant_name(variant.config_directory),
            normalized_variant_name(variant.flatpak_id),
            *(normalized_variant_name(name) for name in variant.system_directories),
            *(normalized_variant_name(name) for name in variant.executables),
        }
        if path_names & known_names:
            return variant
    return None


def resolve_custom_discord_install(
    root: Path,
    source: str = "Custom",
) -> DiscordInstall | None:
    located = locate_discord_resources(root)
    if located is None:
        return None
    install_root, resources = located
    variant = variant_for_custom_install(install_root, resources)
    if variant is None:
        return None
    return DiscordInstall(variant, install_root, resources, source)


def variant_for_process(name: str) -> DiscordVariant | None:
    folded = name.casefold()
    for variant in VARIANTS:
        if folded in {executable.casefold() for executable in variant.executables}:
            return variant
    return None


def find_running_discord() -> list[DiscordInstall]:
    installs: list[DiscordInstall] = []
    proc = Path("/proc")
    if not proc.is_dir():
        return installs

    for process_directory in proc.iterdir():
        if not process_directory.name.isdigit():
            continue
        try:
            executable = (process_directory / "exe").resolve(strict=True)
            variant = variant_for_process(executable.name)
            if variant is None:
                command_name = (process_directory / "comm").read_text().strip()
                variant = variant_for_process(command_name)
            if variant is None:
                continue

            executable_directory = executable.parent
            root = (
                executable_directory.parent
                if executable_directory.name.startswith("app-")
                else executable_directory
            )
            install = resolve_discord_install(root, variant, "Running process")
            if install is not None:
                installs.append(install)
        except (OSError, PermissionError):
            continue
    return installs


def candidate_roots(home: Path, environment: dict[str, str]):
    config_home = Path(environment.get("XDG_CONFIG_HOME", home / ".config"))
    data_home = Path(environment.get("XDG_DATA_HOME", home / ".local/share"))

    for variant in VARIANTS:
        yield variant, config_home / variant.config_directory, "User install"
        yield variant, data_home / variant.config_directory, "User data install"
        yield (
            variant,
            home / ".var/app" / variant.flatpak_id / "config" / variant.config_directory,
            "Flatpak updater install",
        )
        yield (
            variant,
            data_home
            / "flatpak/app"
            / variant.flatpak_id
            / "current/active/files/discord",
            "User Flatpak",
        )
        yield (
            variant,
            Path("/var/lib/flatpak/app")
            / variant.flatpak_id
            / "current/active/files/discord",
            "System Flatpak",
        )
        for base in (Path("/usr/share"), Path("/opt"), Path("/usr/lib"), Path("/usr/lib64")):
            for directory in variant.system_directories:
                yield variant, base / directory, "System install"


def discover_discord_installs(
    environment: dict[str, str] | None = None,
) -> list[DiscordInstall]:
    environment = environment or os.environ.copy()
    home = Path(environment.get("HOME", str(Path.home())))
    discovered: list[DiscordInstall] = []
    seen: set[tuple[str, str]] = set()

    for install in find_running_discord():
        key = (install.variant.branch, str(install.root))
        if key not in seen:
            seen.add(key)
            discovered.append(install)

    for variant, root, source in candidate_roots(home, environment):
        install = resolve_discord_install(root, variant, source)
        if install is None:
            continue
        key = (install.variant.branch, str(install.root))
        if key not in seen:
            seen.add(key)
            discovered.append(install)
    return discovered


def patch_state(install: DiscordInstall) -> str:
    current = resolve_discord_install(install.root, install.variant, install.source)
    if current is None:
        return "unknown"
    app_asar = current.resources / "app.asar"
    original_asar = current.resources / "_app.asar"
    if app_asar.exists() and original_asar.exists():
        return "patched"
    if app_asar.exists() and not original_asar.exists():
        return "unpatched"
    return "unknown"


def load_shell_environment() -> dict[str, str]:
    environment = os.environ.copy()
    bash = shutil.which("bash")
    if bash is None:
        return environment

    try:
        result = subprocess.run(
            [bash, "-lc", "source ~/.bashrc >/dev/null 2>&1; env -0"],
            check=False,
            capture_output=True,
            timeout=10,
        )
        if result.returncode != 0:
            return environment
        for item in result.stdout.split(b"\0"):
            if b"=" not in item:
                continue
            key, value = item.split(b"=", 1)
            environment[key.decode(errors="replace")] = value.decode(errors="replace")
    except (OSError, subprocess.SubprocessError):
        pass
    return environment


try:
    import gi

    gi.require_version("Gtk", "3.0")
    gi.require_version("Gdk", "3.0")
    gi.require_version("GdkPixbuf", "2.0")
    gi.require_version("Pango", "1.0")
    from gi.repository import Gdk, GdkPixbuf, GLib, Gtk, Pango
except (ImportError, ValueError) as error:
    message = (
        "Adacord Installer requires GTK 3 Python bindings.\n\n"
        "Debian/Ubuntu: sudo apt install python3-gi gir1.2-gtk-3.0\n"
        "Fedora: sudo dnf install python3-gobject gtk3"
    )
    zenity = shutil.which("zenity")
    if zenity:
        subprocess.run([zenity, "--error", "--title=Adacord Installer", f"--text={message}"])
    else:
        print(message, file=sys.stderr)
        print(error, file=sys.stderr)
    raise SystemExit(1)


CSS = b"""
window {
    background-color: #f7f7fa;
    color: #26232a;
}
.header-band {
    background-color: #232026;
    border-bottom: 4px solid #da718f;
}
.app-title {
    color: #faf8fb;
    font-size: 28px;
    font-weight: 700;
}
.app-subtitle {
    color: #cbc5cf;
    font-size: 15px;
}
.field-label {
    color: #5f5965;
    font-size: 13px;
    font-weight: 600;
}
.status-label {
    color: #26232a;
    font-size: 15px;
    font-weight: 600;
}
.step-label {
    color: #69646f;
    font-size: 13px;
}
.step-done {
    color: #26734d;
    font-weight: 600;
}
.log-view {
    background-color: #1b1a1e;
    color: #efebf1;
    font-family: Monospace;
    font-size: 12px;
}
entry, combobox button {
    min-height: 32px;
}
button {
    min-height: 32px;
    border-radius: 4px;
}
button.suggested-action {
    background-image: none;
    background-color: #c65f7e;
    border-color: #b65070;
    color: #ffffff;
}
button.suggested-action:hover {
    background-color: #b65070;
}
progressbar progress {
    background-color: #da718f;
}
progressbar trough {
    min-height: 9px;
    background-color: #e1dee4;
}
"""


class InstallerWindow(Gtk.Window):
    def __init__(self, smoke_test_seconds: float | None = None):
        super().__init__(title=APP_NAME)
        self.set_default_size(900, 720)
        self.set_size_request(720, 640)
        self.set_position(Gtk.WindowPosition.CENTER)
        self.connect("delete-event", self.on_delete_event)

        self.script_directory = Path(sys.argv[0]).resolve().parent
        self.target = Path(
            os.environ.get("ADACORD_INSTALL_DIR", self.script_directory / "adacord")
        ).expanduser().resolve()
        self.command_environment = load_shell_environment()
        self.discord_installs: list[DiscordInstall] = []
        self.running = False
        self.active_operation = "install"
        self.cancel_requested = threading.Event()
        self.process_lock = threading.Lock()
        self.current_process: subprocess.Popen[str] | None = None
        self.worker: threading.Thread | None = None
        self.operation_close_discord = True

        self.install_steps = ("Source", "Dependencies", "Build", "Inject")
        self.uninject_steps = ("Locate", "Prerequisites", "Close Discord", "Uninject")

        self.load_theme()
        self.build_ui()
        self.refresh_discord_installs()

        if smoke_test_seconds is not None:
            GLib.timeout_add(int(smoke_test_seconds * 1000), self.close_smoke_test)

    def load_theme(self):
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(),
            provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )

        loader = GdkPixbuf.PixbufLoader.new_with_type("png")
        loader.write(base64.b64decode(ICON_BASE64))
        loader.close()
        self.app_icon = loader.get_pixbuf()
        self.set_icon(self.app_icon)

    @staticmethod
    def label(text: str, style_class: str, xalign: float = 0) -> Gtk.Label:
        label = Gtk.Label(label=text, xalign=xalign)
        label.get_style_context().add_class(style_class)
        return label

    @staticmethod
    def icon_button(icon_name: str, tooltip: str) -> Gtk.Button:
        button = Gtk.Button.new_from_icon_name(icon_name, Gtk.IconSize.BUTTON)
        button.set_tooltip_text(tooltip)
        button.set_size_request(38, 34)
        return button

    def build_ui(self):
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.add(root)

        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=18)
        header.set_border_width(24)
        header.get_style_context().add_class("header-band")
        root.pack_start(header, False, False, 0)

        icon = Gtk.Image.new_from_pixbuf(
            self.app_icon.scale_simple(58, 58, GdkPixbuf.InterpType.BILINEAR)
        )
        header.pack_start(icon, False, False, 0)

        header_text = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        header_text.set_valign(Gtk.Align.CENTER)
        header.pack_start(header_text, True, True, 0)
        header_text.pack_start(self.label("Manage Adacord", "app-title"), False, False, 0)
        header_text.pack_start(
            self.label(
                "Install or remove Adacord from Discord on Linux.",
                "app-subtitle",
            ),
            False,
            False,
            0,
        )

        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        content.set_margin_start(28)
        content.set_margin_end(28)
        content.set_margin_top(20)
        content.set_margin_bottom(18)
        root.pack_start(content, True, True, 0)

        content.pack_start(self.label("Working folder", "field-label"), False, False, 0)
        self.target_entry = Gtk.Entry()
        self.target_entry.set_text(str(self.target))
        self.target_entry.set_editable(False)
        content.pack_start(self.target_entry, False, False, 0)

        content.pack_start(
            self.label("Discord installation", "field-label"),
            False,
            False,
            3,
        )
        discord_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        content.pack_start(discord_row, False, False, 0)
        self.discord_combo = Gtk.ComboBoxText()
        self.discord_combo.set_hexpand(True)
        self.discord_combo.connect("changed", self.on_discord_changed)
        discord_row.pack_start(self.discord_combo, True, True, 0)

        self.refresh_button = self.icon_button(
            "view-refresh-symbolic",
            "Scan for Discord again",
        )
        self.refresh_button.connect(
            "clicked",
            lambda _button: self.refresh_discord_installs(),
        )
        discord_row.pack_start(self.refresh_button, False, False, 0)

        self.browse_button = self.icon_button(
            "document-open-symbolic",
            "Choose a Discord folder",
        )
        self.browse_button.connect("clicked", self.browse_discord)
        discord_row.pack_start(self.browse_button, False, False, 0)

        self.discord_state = self.label("", "step-label")
        content.pack_start(self.discord_state, False, False, 0)

        self.close_discord = Gtk.CheckButton(
            label="Close Discord automatically before inject or uninject"
        )
        self.close_discord.set_active(True)
        content.pack_start(self.close_discord, False, False, 2)

        self.steps_grid = Gtk.Grid(column_spacing=10, row_spacing=0)
        self.steps_grid.set_column_homogeneous(True)
        content.pack_start(self.steps_grid, False, False, 8)
        self.step_labels: list[Gtk.Label] = []
        for index, step in enumerate(self.install_steps):
            label = self.label(f"{index + 1}  {step}", "step-label")
            label.set_ellipsize(Pango.EllipsizeMode.END)
            self.steps_grid.attach(label, index, 0, 1, 1)
            self.step_labels.append(label)

        self.progress = Gtk.ProgressBar()
        self.progress.set_fraction(0)
        content.pack_start(self.progress, False, False, 0)

        self.status = self.label("Ready to install", "status-label")
        content.pack_start(self.status, False, False, 1)

        content.pack_start(self.label("Installation log", "field-label"), False, False, 0)
        log_scroll = Gtk.ScrolledWindow()
        log_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        log_scroll.set_shadow_type(Gtk.ShadowType.IN)
        log_scroll.set_min_content_height(190)
        content.pack_start(log_scroll, True, True, 0)

        self.log_view = Gtk.TextView()
        self.log_view.set_editable(False)
        self.log_view.set_cursor_visible(False)
        self.log_view.set_wrap_mode(Gtk.WrapMode.NONE)
        self.log_view.get_style_context().add_class("log-view")
        self.log_buffer = self.log_view.get_buffer()
        log_scroll.add(self.log_view)

        footer = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        footer.set_halign(Gtk.Align.END)
        content.pack_end(footer, False, False, 0)

        self.cancel_button = Gtk.Button(label="Cancel")
        self.cancel_button.set_no_show_all(True)
        self.cancel_button.connect("clicked", self.cancel_operation)
        footer.pack_start(self.cancel_button, False, False, 0)

        self.uninject_button = Gtk.Button(label="Uninject")
        self.uninject_button.connect(
            "clicked",
            lambda _button: self.begin_operation("uninject"),
        )
        footer.pack_start(self.uninject_button, False, False, 0)

        self.install_button = Gtk.Button(label="Install")
        self.install_button.get_style_context().add_class("suggested-action")
        self.install_button.connect(
            "clicked",
            lambda _button: self.begin_operation("install"),
        )
        footer.pack_start(self.install_button, False, False, 0)

    def close_smoke_test(self):
        self.destroy()
        Gtk.main_quit()
        return False

    def selected_discord(self) -> DiscordInstall | None:
        active_id = self.discord_combo.get_active_id()
        if active_id is None:
            return None
        try:
            return self.discord_installs[int(active_id)]
        except (ValueError, IndexError):
            return None

    def refresh_discord_installs(self):
        previous = self.selected_discord()
        previous_key = (
            (previous.variant.branch, str(previous.root)) if previous is not None else None
        )
        self.discord_installs = discover_discord_installs(self.command_environment)
        self.discord_combo.remove_all()

        active_index = 0
        for index, install in enumerate(self.discord_installs):
            self.discord_combo.append(
                str(index),
                f"{install.variant.branch}: {install.root}",
            )
            if previous_key == (install.variant.branch, str(install.root)):
                active_index = index

        if self.discord_installs:
            self.discord_combo.set_active(active_index)
        else:
            self.discord_combo.append("none", "No Discord installation found")
            self.discord_combo.set_active_id("none")
            self.discord_state.set_text("Use the folder button to select a custom install.")

    def on_discord_changed(self, _combo):
        install = self.selected_discord()
        if install is None:
            return
        state = patch_state(install)
        state_text = {
            "patched": "Adacord is currently injected",
            "unpatched": "Discord is ready for injection",
            "unknown": "Discord patch state could not be determined",
        }[state]
        self.discord_state.set_text(f"{install.source} - {state_text}")

    def browse_discord(self, _button):
        chooser = Gtk.FileChooserDialog(
            title="Choose Discord installation",
            parent=self,
            action=Gtk.FileChooserAction.SELECT_FOLDER,
        )
        chooser.add_buttons(
            "Cancel",
            Gtk.ResponseType.CANCEL,
            "Select",
            Gtk.ResponseType.OK,
        )
        response = chooser.run()
        selected = Path(chooser.get_filename()) if response == Gtk.ResponseType.OK else None
        chooser.destroy()
        if selected is None:
            return

        install = resolve_custom_discord_install(selected)
        if install is None:
            self.show_message(
                "Discord not found",
                "The selected folder does not contain a recognizable Discord "
                "installation. Select its root, app version, or resources folder.",
                Gtk.MessageType.ERROR,
            )
            return

        install_key = (install.variant.branch, str(install.root))
        for index, existing in enumerate(self.discord_installs):
            existing_key = (existing.variant.branch, str(existing.root))
            if existing_key == install_key:
                self.discord_combo.set_active_id(str(index))
                return

        self.discord_installs.append(install)
        index = len(self.discord_installs) - 1
        self.discord_combo.append(
            str(index),
            f"{install.variant.branch}: {install.root}",
        )
        self.discord_combo.set_active_id(str(index))

    def show_message(
        self,
        title: str,
        message: str,
        message_type: Gtk.MessageType,
        buttons: Gtk.ButtonsType = Gtk.ButtonsType.OK,
    ) -> int:
        dialog = Gtk.MessageDialog(
            transient_for=self,
            modal=True,
            message_type=message_type,
            buttons=buttons,
            text=title,
        )
        dialog.format_secondary_text(message)
        response = dialog.run()
        dialog.destroy()
        return response

    def begin_operation(self, operation: str):
        if self.running:
            return
        install = self.selected_discord()
        if install is None:
            self.show_message(
                "Discord not found",
                "Select a Discord installation before continuing.",
                Gtk.MessageType.ERROR,
            )
            return

        if operation == "uninject":
            response = self.show_message(
                "Remove Adacord?",
                f"Remove Adacord from {install.variant.branch} Discord?",
                Gtk.MessageType.QUESTION,
                Gtk.ButtonsType.YES_NO,
            )
            if response != Gtk.ResponseType.YES:
                return

        self.active_operation = operation
        self.operation_discord = install
        self.operation_close_discord = self.close_discord.get_active()
        self.cancel_requested.clear()
        self.cancel_button.set_sensitive(True)
        self.log_buffer.set_text("")
        self.set_running(True)
        self.update_steps(0)
        self.progress.set_fraction(0)
        self.status.set_text(
            "Starting uninject..." if operation == "uninject" else "Starting installation..."
        )
        self.worker = threading.Thread(
            target=self.operation_worker,
            args=(operation, install),
            daemon=True,
        )
        self.worker.start()

    def set_running(self, running: bool):
        self.running = running
        self.install_button.set_sensitive(not running)
        self.uninject_button.set_sensitive(not running)
        self.discord_combo.set_sensitive(not running)
        self.refresh_button.set_sensitive(not running)
        self.browse_button.set_sensitive(not running)
        self.close_discord.set_sensitive(not running)
        if running:
            self.cancel_button.set_sensitive(True)
            self.cancel_button.show()
        else:
            self.cancel_button.hide()

    def update_steps(self, completed: int):
        steps = (
            self.uninject_steps
            if self.active_operation == "uninject"
            else self.install_steps
        )
        for index, label in enumerate(self.step_labels):
            context = label.get_style_context()
            context.remove_class("step-done")
            suffix = ""
            if index < completed:
                suffix = " - done"
                context.add_class("step-done")
            label.set_text(f"{index + 1}  {steps[index]}{suffix}")

    def set_stage(self, completed: int, progress: float, status: str):
        GLib.idle_add(self.update_steps, completed)
        GLib.idle_add(self.progress.set_fraction, progress)
        GLib.idle_add(self.status.set_text, status)

    def append_log(self, text: str):
        clean = ANSI_ESCAPE.sub("", text)

        def append():
            end = self.log_buffer.get_end_iter()
            self.log_buffer.insert(end, clean)
            mark = self.log_buffer.create_mark(None, self.log_buffer.get_end_iter(), False)
            self.log_view.scroll_mark_onscreen(mark)
            self.log_buffer.delete_mark(mark)
            return False

        GLib.idle_add(append)

    def log(self, message: str = ""):
        self.append_log(message + "\n")

    def check_cancelled(self):
        if self.cancel_requested.is_set():
            raise OperationCancelled()

    def tool(self, name: str) -> str:
        path = shutil.which(name, path=self.command_environment.get("PATH"))
        if path is None:
            raise InstallerError(f"{name} was not found on PATH.")
        return path

    def run_command(
        self,
        command: list[str],
        cwd: Path | None = None,
        environment: dict[str, str] | None = None,
    ) -> tuple[int, str]:
        self.check_cancelled()
        self.log(f"$ {shlex.join(command)}")
        output: list[str] = []
        try:
            process = subprocess.Popen(
                command,
                cwd=cwd,
                env=environment or self.command_environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                start_new_session=True,
            )
        except OSError as error:
            raise InstallerError(f"Could not start {command[0]}: {error}") from error

        with self.process_lock:
            self.current_process = process
            cancel_after_start = self.cancel_requested.is_set()
        if cancel_after_start and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            assert process.stdout is not None
            for line in process.stdout:
                output.append(line)
                self.append_log(line)
            return_code = process.wait()
        finally:
            with self.process_lock:
                if self.current_process is process:
                    self.current_process = None

        self.check_cancelled()
        return return_code, "".join(output)

    def check_node_version(self, node: str):
        return_code, output = self.run_command([node, "--version"])
        if return_code != 0:
            raise InstallerError("Node.js is installed but could not be started.")
        match = re.search(r"(\d+)", output)
        if match is None or int(match.group(1)) < 22:
            raise InstallerError("Adacord requires Node.js 22 or newer.")

    def verify_repository(self, git: str):
        git_directory = self.target / ".git"
        if git_directory.is_dir():
            return_code, remote = self.run_command(
                [git, "-C", str(self.target), "remote", "get-url", "origin"]
            )
            if return_code != 0 or "rowtha/adacord" not in remote.casefold():
                raise InstallerError(
                    "The working folder contains a different Git repository."
                )
            return_code, _ = self.run_command(
                [git, "-C", str(self.target), "pull", "--ff-only", "origin", "main"]
            )
            if return_code != 0:
                raise InstallerError(
                    "git pull failed. Resolve the checkout changes shown in the log."
                )
            return

        if self.target.exists() and any(self.target.iterdir()):
            raise InstallerError(
                "The working folder is not empty and is not an Adacord checkout."
            )
        self.target.parent.mkdir(parents=True, exist_ok=True)
        return_code, _ = self.run_command(
            [git, "clone", "--depth=1", REPO_URL, str(self.target)]
        )
        if return_code != 0:
            raise InstallerError("git clone failed. Check the network and the log.")

    def ensure_installer_binary(self, node: str) -> Path:
        binary = self.target / "dist/Installer/VencordInstallerCli-linux"
        if binary.is_file() and os.access(binary, os.X_OK):
            return binary

        script = self.target / "scripts/runInstaller.mjs"
        if not script.is_file():
            raise InstallerError("scripts/runInstaller.mjs was not found.")
        return_code, _ = self.run_command(
            [node, str(script), "--", "--help"],
            cwd=self.target,
        )
        if return_code != 0 or not binary.is_file():
            raise InstallerError("Could not download the Linux injector.")
        binary.chmod(binary.stat().st_mode | 0o111)
        return binary

    @staticmethod
    def process_ids_for_discord() -> list[int]:
        process_ids: list[int] = []
        current_uid = os.getuid()
        for process_directory in Path("/proc").iterdir():
            if not process_directory.name.isdigit():
                continue
            try:
                status = (process_directory / "status").read_text()
                uid_match = re.search(r"^Uid:\s+(\d+)", status, re.MULTILINE)
                if uid_match is None or int(uid_match.group(1)) != current_uid:
                    continue
                command_name = (process_directory / "comm").read_text().strip().casefold()
                executable_name = (
                    (process_directory / "exe").resolve(strict=True).name.casefold()
                )
                if command_name in PROCESS_NAMES or executable_name in PROCESS_NAMES:
                    process_ids.append(int(process_directory.name))
            except (OSError, PermissionError):
                continue
        return process_ids

    def close_discord_processes(self):
        process_ids = self.process_ids_for_discord()
        if not process_ids:
            self.log("Discord is not running.")
            return

        self.log(f"Closing {len(process_ids)} Discord process(es)...")
        for process_id in process_ids:
            try:
                os.kill(process_id, signal.SIGTERM)
            except ProcessLookupError:
                pass

        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            remaining = [
                process_id
                for process_id in process_ids
                if Path(f"/proc/{process_id}").exists()
            ]
            if not remaining:
                return
            self.check_cancelled()
            time.sleep(0.2)

        for process_id in process_ids:
            try:
                os.kill(process_id, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def run_injector(
        self,
        binary: Path,
        operation: str,
        install: DiscordInstall,
    ):
        injector_environment = self.command_environment.copy()
        injector_environment["VENCORD_USER_DATA_DIR"] = str(self.target)
        injector_environment["VENCORD_DEV_INSTALL"] = "1"
        operation_flag = "--install" if operation == "install" else "--uninstall"
        command = [str(binary), operation_flag, "--location", str(install.root)]

        if not os.access(install.resources, os.W_OK):
            pkexec = self.tool("pkexec")
            env_program = shutil.which("env") or "/usr/bin/env"
            command = [
                pkexec,
                env_program,
                f"VENCORD_USER_DATA_DIR={self.target}",
                "VENCORD_DEV_INSTALL=1",
                str(binary),
                operation_flag,
                "--location",
                str(install.root),
            ]

        return_code, _ = self.run_command(
            command,
            cwd=self.target,
            environment=injector_environment,
        )
        if return_code != 0:
            raise InstallerError(
                "Injection failed." if operation == "install" else "Uninject failed."
            )

    def install_operation(self, install: DiscordInstall):
        self.log(APP_NAME)
        self.log(f"Working folder: {self.target}")
        self.log(f"Discord: {install.variant.branch}")
        self.log(f"Discord path: {install.root}")
        self.log()

        self.set_stage(0, 0.03, "Checking prerequisites...")
        git = self.tool("git")
        node = self.tool("node")
        pnpm = self.tool("pnpm")
        self.run_command([git, "--version"])
        self.check_node_version(node)
        return_code, _ = self.run_command([pnpm, "--version"])
        if return_code != 0:
            raise InstallerError("pnpm is installed but could not be started.")

        self.set_stage(0, 0.10, "Downloading or updating source...")
        self.verify_repository(git)
        self.check_cancelled()

        self.set_stage(1, 0.35, "Installing dependencies...")
        return_code, _ = self.run_command([pnpm, "install"], cwd=self.target)
        if return_code != 0:
            raise InstallerError("pnpm install failed. Review the package manager log.")

        self.set_stage(2, 0.58, "Building Adacord...")
        return_code, _ = self.run_command([pnpm, "build"], cwd=self.target)
        if return_code != 0:
            raise InstallerError("pnpm build failed. Review the compiler log.")

        self.check_cancelled()
        self.set_stage(3, 0.82, "Injecting into Discord...")
        if self.operation_close_discord:
            self.close_discord_processes()
        binary = self.ensure_installer_binary(node)
        self.run_injector(binary, "install", install)
        if patch_state(install) != "patched":
            raise InstallerError(
                "The injector exited successfully, but the Discord patch was not found."
            )

        self.set_stage(4, 1.0, "Installation complete")
        self.log()
        self.log("SUCCESS: Adacord was installed. Restart Discord to finish.")

    def uninject_operation(self, install: DiscordInstall):
        self.log("Adacord Uninject")
        self.log(f"Working folder: {self.target}")
        self.log(f"Discord: {install.variant.branch}")
        self.log(f"Discord path: {install.root}")
        self.log()

        self.set_stage(0, 0.10, "Locating the existing installation...")
        if not (self.target / "package.json").is_file():
            raise InstallerError(
                "No Adacord checkout was found beside this installer."
            )

        self.set_stage(1, 0.28, "Checking prerequisites...")
        node = self.tool("node")
        self.check_node_version(node)
        binary = self.ensure_installer_binary(node)

        self.set_stage(2, 0.55, "Closing Discord...")
        if self.operation_close_discord:
            self.close_discord_processes()
        self.check_cancelled()

        self.set_stage(3, 0.78, "Removing Adacord from Discord...")
        self.run_injector(binary, "uninject", install)
        if patch_state(install) != "unpatched":
            raise InstallerError(
                "The uninjector exited successfully, but Discord still appears patched."
            )

        self.set_stage(4, 1.0, "Uninject complete")
        self.log()
        self.log("SUCCESS: Adacord was removed. Restart Discord to finish.")

    def operation_worker(self, operation: str, install: DiscordInstall):
        result = "success"
        if operation == "install":
            message = "Adacord was installed successfully.\n\nRestart Discord to load Adacord."
        else:
            message = "Adacord was uninjected successfully.\n\nRestart Discord to finish."

        try:
            if operation == "install":
                self.install_operation(install)
            else:
                self.uninject_operation(install)
        except OperationCancelled:
            result = "cancelled"
            message = (
                "The installation was cancelled."
                if operation == "install"
                else "The uninject operation was cancelled."
            )
            self.log()
            self.log(message)
        except Exception as error:
            result = "failed"
            message = str(error)
            self.log()
            self.log(f"ERROR: {message}")

        GLib.idle_add(self.operation_finished, result, message)

    def operation_finished(self, result: str, message: str):
        self.set_running(False)
        self.refresh_discord_installs()
        if result == "success":
            self.install_button.set_label(
                "Install again" if self.active_operation == "install" else "Install"
            )
            self.show_message(
                "Installation complete"
                if self.active_operation == "install"
                else "Uninject complete",
                message,
                Gtk.MessageType.INFO,
            )
        elif result == "failed":
            self.status.set_text(
                "Installation failed"
                if self.active_operation == "install"
                else "Uninject failed"
            )
            self.show_message(
                "Installation failed"
                if self.active_operation == "install"
                else "Uninject failed",
                message,
                Gtk.MessageType.ERROR,
            )
        else:
            self.status.set_text(
                "Installation cancelled"
                if self.active_operation == "install"
                else "Uninject cancelled"
            )
        return False

    def cancel_operation(self, _button):
        if not self.running:
            return
        response = self.show_message(
            "Cancel operation?",
            "Stop the current installer operation?",
            Gtk.MessageType.QUESTION,
            Gtk.ButtonsType.YES_NO,
        )
        if response != Gtk.ResponseType.YES:
            return

        self.cancel_requested.set()
        self.status.set_text("Cancelling...")
        self.cancel_button.set_sensitive(False)
        with self.process_lock:
            process = self.current_process
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            GLib.timeout_add_seconds(3, self.force_kill_cancelled_process, process)

    def force_kill_cancelled_process(self, process: subprocess.Popen[str]):
        if self.cancel_requested.is_set() and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        return False

    def on_delete_event(self, _window, _event):
        if self.running:
            self.show_message(
                "Installation in progress",
                "Cancel the current operation before closing this window.",
                Gtk.MessageType.INFO,
            )
            return True
        Gtk.main_quit()
        return False


def show_root_error() -> int:
    Gtk.init([])
    dialog = Gtk.MessageDialog(
        transient_for=None,
        modal=True,
        message_type=Gtk.MessageType.ERROR,
        buttons=Gtk.ButtonsType.OK,
        text="Do not run Adacord Installer as root",
    )
    dialog.format_secondary_text(
        "Run it as your desktop user. A PolicyKit prompt will appear only if "
        "the selected Discord installation requires elevated access."
    )
    dialog.run()
    dialog.destroy()
    return 1


def print_diagnostics() -> int:
    environment = load_shell_environment()
    installs = discover_discord_installs(environment)
    data = {
        "python": sys.version.split()[0],
        "gtk": f"{Gtk.get_major_version()}.{Gtk.get_minor_version()}",
        "tools": {
            name: shutil.which(name, path=environment.get("PATH"))
            for name in ("git", "node", "pnpm", "pkexec")
        },
        "discord": [
            {
                "branch": install.variant.branch,
                "root": str(install.root),
                "resources": str(install.resources),
                "source": install.source,
                "state": patch_state(install),
                "writable": os.access(install.resources, os.W_OK),
            }
            for install in installs
        ],
    }
    print(json.dumps(data, indent=2))
    return 0 if installs else 1


def main() -> int:
    if "--diagnose" in sys.argv:
        return print_diagnostics()
    if os.geteuid() == 0:
        return show_root_error()

    smoke_test_seconds = None
    if "--smoke-test" in sys.argv:
        index = sys.argv.index("--smoke-test")
        smoke_test_seconds = (
            float(sys.argv[index + 1])
            if index + 1 < len(sys.argv)
            else 2.0
        )

    Gtk.init([])
    window = InstallerWindow(smoke_test_seconds)
    window.show_all()
    window.cancel_button.hide()
    Gtk.main()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
