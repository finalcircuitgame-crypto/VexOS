"""VexOS USB Flasher for Windows.

The UI deliberately separates the safe update operation from the destructive
whole-disk preparation operation. Quick Update never repartitions a disk.
Erase & Prepare requires elevation, identity revalidation, and typed consent.
"""

from __future__ import annotations

import base64
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
DIST_DIR = os.path.join(PROJECT_ROOT, "dist")
BOOTX64_SRC = os.path.join(DIST_DIR, "BOOTX64.EFI")
KERNEL_SRC = os.path.join(DIST_DIR, "kernel.elf")
STARTUP_SRC = os.path.join(PROJECT_ROOT, "build", "startup.nsh")
APP_TITLE = "VexOS USB Flasher"
WINDOW_BG = "#0B1020"
PANEL_BG = "#131C2F"
PANEL_ALT = "#18243A"
TEXT = "#E8EEF9"
MUTED = "#9AAAC2"
ACCENT = "#59B7FF"
GREEN = "#55D6A1"
RED = "#FF6B78"
BORDER = "#293852"


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except (AttributeError, OSError):
        return False


def relaunch_as_admin() -> bool:
    """Relaunch this script elevated. Returns False if Windows refused it."""
    try:
        params = subprocess.list2cmdline(sys.argv)
        result = ctypes.windll.shell32.ShellExecuteW(
            None, "runas", sys.executable, params, PROJECT_ROOT, 1
        )
        return int(result) > 32
    except (AttributeError, OSError, ValueError):
        return False


def run_powershell(script: str) -> str:
    proc = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            script,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    if proc.returncode:
        detail = proc.stderr.strip() or proc.stdout.strip()
        raise RuntimeError(detail or f"PowerShell exited with code {proc.returncode}.")
    return proc.stdout.strip()


def parse_json_array(output: str) -> list[dict]:
    if not output:
        return []
    value = json.loads(output)
    if isinstance(value, dict):
        return [value]
    return value if isinstance(value, list) else []


def get_usb_disks() -> list[dict]:
    """Return online physical USB disks, not volumes or removable-letter guesses."""
    script = r"""
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Get-Disk |
  Where-Object { $_.BusType -eq 'USB' -and $_.OperationalStatus -contains 'Online' } |
  Select-Object Number,FriendlyName,SerialNumber,UniqueId,Size,PartitionStyle,
                IsSystem,IsBoot,IsReadOnly,IsOffline |
  ConvertTo-Json -Depth 3 -Compress
"""
    return parse_json_array(run_powershell(script))


def get_usb_volumes() -> list[dict]:
    """Map mounted drive letters back to their physical USB disks."""
    script = r"""
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$result = @(
  Get-Partition | ForEach-Object {
    $partition = $_
    $disk = Get-Disk -Number $partition.DiskNumber -ErrorAction SilentlyContinue
    if ($disk -and $disk.BusType -eq 'USB' -and $partition.DriveLetter) {
      $volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
      if ($volume) {
        [pscustomobject]@{
          DriveLetter = [string]$partition.DriveLetter
          DiskNumber = [int]$partition.DiskNumber
          Label = [string]$volume.FileSystemLabel
          FileSystem = [string]$volume.FileSystem
          Size = [int64]$volume.Size
          Free = [int64]$volume.SizeRemaining
        }
      }
    }
  }
)
$result | ConvertTo-Json -Depth 3 -Compress
"""
    return parse_json_array(run_powershell(script))


def human_size(value: int | str | None) -> str:
    number = float(value or 0)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if number < 1024 or unit == "TiB":
            return f"{number:,.1f} {unit}"
        number /= 1024
    return f"{number:,.1f} PiB"


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_build() -> list[tuple[str, str]]:
    missing = [path for path in (BOOTX64_SRC, KERNEL_SRC) if not os.path.isfile(path)]
    if missing:
        names = "\n".join(os.path.relpath(path, PROJECT_ROOT) for path in missing)
        raise FileNotFoundError(
            "VexOS build files are missing:\n"
            f"{names}\n\nBuild VexOS first, then reopen the flasher."
        )

    copies = [
        (BOOTX64_SRC, "EFI\\BOOT\\BOOTX64.EFI"),
        (KERNEL_SRC, "kernel.elf"),
    ]
    if os.path.isfile(STARTUP_SRC):
        copies.append((STARTUP_SRC, "startup.nsh"))

    for source, _ in copies:
        if os.path.getsize(source) == 0:
            raise RuntimeError(f"Build artifact is empty: {source}")
    return copies


def validate_usb_volume(
    drive_letter: str, expected_disk_number: int | None = None
) -> dict:
    letter = drive_letter.upper()
    if len(letter) != 1 or not letter.isalpha():
        raise ValueError("Invalid destination drive letter.")

    expected = expected_disk_number if expected_disk_number is not None else -1
    script = rf"""
$ErrorActionPreference = 'Stop'
$partition = Get-Partition -DriveLetter '{letter}' -ErrorAction Stop
$disk = Get-Disk -Number $partition.DiskNumber -ErrorAction Stop
if ($disk.BusType -ne 'USB') {{ throw 'Safety check failed: destination is not a USB disk.' }}
if ($disk.IsBoot -or $disk.IsSystem) {{ throw 'Safety check failed: refusing a Windows boot/system disk.' }}
if ($disk.IsReadOnly -or $disk.IsOffline) {{ throw 'The selected USB disk is read-only or offline.' }}
if ({expected} -ge 0 -and $disk.Number -ne {expected}) {{ throw 'USB disk identity changed. Refresh devices and try again.' }}
$volume = Get-Volume -Partition $partition -ErrorAction Stop
[pscustomobject]@{{
  DriveLetter = [string]$partition.DriveLetter
  DiskNumber = [int]$disk.Number
  FileSystem = [string]$volume.FileSystem
  Free = [int64]$volume.SizeRemaining
}} | ConvertTo-Json -Compress
"""
    result = json.loads(run_powershell(script))
    if str(result.get("FileSystem", "")).upper() != "FAT32":
        raise RuntimeError(
            f"{letter}: is {result.get('FileSystem') or 'not formatted as FAT32'}. "
            "Use Erase & Prepare, or format the USB as FAT32 yourself."
        )
    return result


def copy_verified(source: str, destination: str) -> str:
    """Copy to a temporary sibling, verify it, then replace the destination."""
    temporary = destination + ".vexos-tmp"
    try:
        shutil.copyfile(source, temporary)
        source_hash = sha256_file(source)
        temporary_hash = sha256_file(temporary)
        if source_hash != temporary_hash:
            raise IOError(f"SHA-256 verification failed for {os.path.basename(source)}.")
        os.replace(temporary, destination)
        if sha256_file(destination) != source_hash:
            raise IOError(f"Final SHA-256 verification failed for {os.path.basename(source)}.")
        return source_hash
    finally:
        try:
            if os.path.exists(temporary):
                os.remove(temporary)
        except OSError:
            pass


def copy_boot_files(
    drive_letter: str,
    progress=None,
    log=None,
    expected_disk_number: int | None = None,
) -> dict:
    copies = validate_build()
    volume = validate_usb_volume(drive_letter, expected_disk_number)
    letter = drive_letter.upper()
    root = f"{letter}:\\"
    if not os.path.isdir(root):
        raise FileNotFoundError(f"USB volume {root} is not accessible.")

    required = sum(os.path.getsize(source) for source, _ in copies)
    if int(volume.get("Free") or 0) < required + 1024 * 1024:
        raise OSError(
            f"Not enough free space on {letter}:. "
            f"VexOS needs at least {human_size(required + 1024 * 1024)} free."
        )

    results = []
    for index, (source, relative_destination) in enumerate(copies, start=1):
        destination = os.path.join(root, *relative_destination.split("\\"))
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        if log:
            log(f"Installing {relative_destination}")
        digest = copy_verified(source, destination)
        results.append(
            {
                "file": relative_destination.replace("\\", "/"),
                "bytes": os.path.getsize(destination),
                "sha256": digest,
            }
        )
        if progress:
            progress(int(index * 100 / len(copies)))

    return {"root": root, "files": results}


def flash_usb_disk(disk: dict, progress=None, log=None) -> dict:
    """Erase one revalidated physical USB disk and install the boot files."""
    validate_build()

    number = int(disk["Number"])
    unique_id = str(disk.get("UniqueId") or "")
    serial = str(disk.get("SerialNumber") or "")
    size = int(disk["Size"])
    unique_b64 = base64.b64encode(unique_id.encode("utf-8")).decode("ascii")
    serial_b64 = base64.b64encode(serial.encode("utf-8")).decode("ascii")

    if log:
        log(f"Rechecking the identity of USB disk {number}...")

    script = rf"""
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$number = {number}
$expectedSize = [int64]{size}
$expectedUnique = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{unique_b64}'))
$expectedSerial = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{serial_b64}'))
$disk = Get-Disk -Number $number -ErrorAction Stop
if ($disk.BusType -ne 'USB') {{ throw 'Safety check failed: selected disk is no longer a USB disk.' }}
if ($disk.IsBoot -or $disk.IsSystem) {{ throw 'Safety check failed: refusing to modify a Windows boot/system disk.' }}
if ($disk.IsReadOnly) {{ throw 'Selected USB disk is read-only.' }}
if ($disk.IsOffline) {{ throw 'Selected USB disk is offline.' }}
if ([int64]$disk.Size -ne $expectedSize) {{ throw 'Disk capacity changed since selection. Refresh devices.' }}
if ($expectedUnique -and [string]$disk.UniqueId -ne $expectedUnique) {{ throw 'Disk identity changed. Refresh devices.' }}
if ($expectedSerial -and [string]$disk.SerialNumber -ne $expectedSerial) {{ throw 'Disk serial changed. Refresh devices.' }}
$disk | Clear-Disk -RemoveData -RemoveOEM -Confirm:$false
Initialize-Disk -Number $number -PartitionStyle GPT
$partition = New-Partition -DiskNumber $number -UseMaximumSize -AssignDriveLetter
$null = Format-Volume -Partition $partition -FileSystem FAT32 -NewFileSystemLabel 'VEXOS' -Confirm:$false -Force
$volume = Get-Volume -Partition $partition -ErrorAction Stop
if (-not $volume.DriveLetter) {{ throw 'Formatting completed but Windows did not assign a drive letter.' }}
[pscustomobject]@{{ DriveLetter = [string]$volume.DriveLetter; FileSystem = [string]$volume.FileSystem }} |
  ConvertTo-Json -Compress
"""
    if progress:
        progress(10)
    output = run_powershell(script)
    formatted = json.loads(output)
    letter = str(formatted.get("DriveLetter") or "").strip()
    if len(letter) != 1:
        raise RuntimeError(f"Could not determine the new USB drive letter: {output}")
    if str(formatted.get("FileSystem", "")).upper() != "FAT32":
        raise RuntimeError("The USB partition was created, but it is not FAT32.")

    if log:
        log(f"USB prepared as {letter}: (GPT + FAT32). Installing VexOS files...")
    if progress:
        progress(20)

    result = copy_boot_files(
        letter,
        progress=lambda value: progress(20 + int(value * 0.8)) if progress else None,
        log=log,
        expected_disk_number=number,
    )
    result["drive_letter"] = letter
    return result


class VexOSFlasher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("980x790")
        self.minsize(860, 700)
        self.configure(bg=WINDOW_BG)

        self.disks: dict[int, dict] = {}
        self.volumes_by_disk: dict[int, list[dict]] = {}
        self.volume_choices: dict[str, dict] = {}
        self.busy = False
        self.scanning = False

        self._configure_style()
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self.close_window)
        self.refresh_devices()

    def _configure_style(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure(
            "Vex.Treeview",
            background=PANEL_BG,
            fieldbackground=PANEL_BG,
            foreground=TEXT,
            rowheight=32,
            borderwidth=0,
            font=("Segoe UI", 9),
        )
        style.configure(
            "Vex.Treeview.Heading",
            background=PANEL_ALT,
            foreground=MUTED,
            font=("Segoe UI", 9, "bold"),
            relief="flat",
        )
        style.map(
            "Vex.Treeview",
            background=[("selected", "#21476B")],
            foreground=[("selected", "#FFFFFF")],
        )
        style.configure(
            "Vex.TCombobox",
            fieldbackground=PANEL_ALT,
            background=PANEL_ALT,
            foreground=TEXT,
            arrowcolor=TEXT,
            padding=7,
        )
        style.configure(
            "Vex.Horizontal.TProgressbar",
            troughcolor=PANEL_ALT,
            background=ACCENT,
            borderwidth=0,
            thickness=10,
        )

    def _label(self, parent, text, color=TEXT, size=10, bold=False, **kwargs):
        return tk.Label(
            parent,
            text=text,
            bg=kwargs.pop("bg", parent.cget("bg")),
            fg=color,
            font=("Segoe UI", size, "bold" if bold else "normal"),
            anchor=kwargs.pop("anchor", "w"),
            **kwargs,
        )

    def _button(self, parent, text, command, bg, fg="#FFFFFF", **kwargs):
        # Allow individual buttons to request compact or roomy padding without
        # passing duplicate padx/pady keyword arguments to tkinter.Button.
        button_padx = kwargs.pop("padx", 14)
        button_pady = kwargs.pop("pady", 10)
        button_active_bg = kwargs.pop("activebackground", bg)
        button_font = kwargs.pop("font", ("Segoe UI", 10, "bold"))

        return tk.Button(
            parent,
            text=text,
            command=command,
            bg=bg,
            fg=fg,
            activebackground=button_active_bg,
            activeforeground=fg,
            disabledforeground="#68758B",
            relief=tk.FLAT,
            bd=0,
            padx=button_padx,
            pady=button_pady,
            cursor="hand2",
            font=button_font,
            **kwargs,
        )

    def _build_ui(self):
        header = tk.Frame(self, bg=WINDOW_BG)
        header.pack(fill=tk.X, padx=24, pady=(20, 12))

        title_area = tk.Frame(header, bg=WINDOW_BG)
        title_area.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self._label(title_area, "VexOS", size=24, bold=True).pack(anchor=tk.W)
        self._label(
            title_area,
            "USB INSTALLER",
            color=ACCENT,
            size=9,
            bold=True,
        ).pack(anchor=tk.W, pady=(0, 4))
        self._label(
            title_area,
            "Choose a USB drive, then choose how to install.",
            color=MUTED,
            size=10,
        ).pack(anchor=tk.W)

        self.refresh_button = self._button(
            header, "↻  Refresh drives", self.refresh_devices, PANEL_ALT, fg=TEXT
        )
        self.refresh_button.pack(side=tk.RIGHT, anchor=tk.N, pady=8)

        self._section_heading("01", "SELECT YOUR USB DRIVE", "Only physical USB disks are listed.")
        device_panel = tk.Frame(self, bg=PANEL_BG, highlightbackground=BORDER, highlightthickness=1)
        device_panel.pack(fill=tk.BOTH, padx=24, pady=(8, 10))

        columns = ("disk", "device", "capacity", "volumes", "partition")
        self.disk_tree = ttk.Treeview(
            device_panel,
            columns=columns,
            show="headings",
            height=5,
            selectmode="browse",
            style="Vex.Treeview",
        )
        headings = (
            ("disk", "DISK", 58),
            ("device", "USB DEVICE", 280),
            ("capacity", "CAPACITY", 112),
            ("volumes", "MOUNTED VOLUMES", 230),
            ("partition", "LAYOUT", 100),
        )
        for key, label, width in headings:
            self.disk_tree.heading(key, text=label)
            self.disk_tree.column(key, width=width, minwidth=55, anchor=tk.W, stretch=key in ("device", "volumes"))
        self.disk_tree.pack(fill=tk.X, padx=8, pady=8)
        self.disk_tree.bind("<<TreeviewSelect>>", self.on_disk_selected)

        details = tk.Frame(self, bg=PANEL_BG, highlightbackground=BORDER, highlightthickness=1)
        details.pack(fill=tk.X, padx=24, pady=(0, 12))
        self._label(details, "SELECTED DRIVE", color=MUTED, size=8, bold=True).pack(
            anchor=tk.W, padx=14, pady=(11, 3)
        )
        self.selected_details = tk.StringVar(
            value="No drive selected. Insert a USB drive, refresh, and select it above."
        )
        tk.Label(
            details,
            textvariable=self.selected_details,
            bg=PANEL_BG,
            fg=TEXT,
            font=("Segoe UI", 10),
            justify=tk.LEFT,
            anchor=tk.W,
            wraplength=850,
        ).pack(fill=tk.X, padx=14, pady=(0, 8))

        volume_row = tk.Frame(details, bg=PANEL_BG)
        volume_row.pack(fill=tk.X, padx=14, pady=(0, 12))
        self._label(volume_row, "FAT32 volume for safe update:", color=MUTED, size=9).pack(
            side=tk.LEFT, padx=(0, 10)
        )
        self.volume_var = tk.StringVar()
        self.volume_combo = ttk.Combobox(
            volume_row,
            textvariable=self.volume_var,
            state="readonly",
            width=48,
            style="Vex.TCombobox",
        )
        self.volume_combo.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.volume_combo.bind("<<ComboboxSelected>>", self.on_volume_selected)

        self._section_heading("02", "CHOOSE AN INSTALL METHOD", "The safe option is recommended when a FAT32 USB volume already exists.")
        actions = tk.Frame(self, bg=WINDOW_BG)
        actions.pack(fill=tk.X, padx=24, pady=(8, 12))
        actions.grid_columnconfigure(0, weight=1)
        actions.grid_columnconfigure(1, weight=1)

        quick = tk.Frame(actions, bg=PANEL_BG, highlightbackground="#28644F", highlightthickness=1)
        quick.grid(row=0, column=0, sticky="nsew", padx=(0, 7))
        self._label(quick, "SAFE UPDATE", color=GREEN, size=9, bold=True).pack(
            anchor=tk.W, padx=16, pady=(14, 5)
        )
        self._label(quick, "Update existing USB", size=15, bold=True).pack(
            anchor=tk.W, padx=16
        )
        self._label(
            quick,
            "Copies the VexOS bootloader and kernel onto an existing FAT32 volume. Keeps your other files. No administrator permission.",
            color=MUTED,
            size=9,
            wraplength=390,
            justify=tk.LEFT,
        ).pack(anchor=tk.W, padx=16, pady=(6, 12))
        self.quick_button = self._button(
            quick, "Update selected USB  →", self.copy_selected, "#176B55"
        )
        self.quick_button.pack(fill=tk.X, padx=16, pady=(0, 15))

        full = tk.Frame(actions, bg=PANEL_BG, highlightbackground="#70404A", highlightthickness=1)
        full.grid(row=0, column=1, sticky="nsew", padx=(7, 0))
        self._label(full, "DESTRUCTIVE • ERASES THE DRIVE", color=RED, size=9, bold=True).pack(
            anchor=tk.W, padx=16, pady=(14, 5)
        )
        self._label(full, "Erase & prepare USB", size=15, bold=True).pack(
            anchor=tk.W, padx=16
        )
        self._label(
            full,
            "Deletes every partition and file, creates GPT + FAT32, then installs VexOS. Requires administrator permission.",
            color=MUTED,
            size=9,
            wraplength=390,
            justify=tk.LEFT,
        ).pack(anchor=tk.W, padx=16, pady=(6, 12))
        self.full_button = self._button(
            full, "Erase USB and install VexOS  →", self.flash_selected, "#9B3548"
        )
        self.full_button.pack(fill=tk.X, padx=16, pady=(0, 15))

        activity = tk.Frame(self, bg=PANEL_BG, highlightbackground=BORDER, highlightthickness=1)
        activity.pack(fill=tk.BOTH, expand=True, padx=24, pady=(0, 10))
        top = tk.Frame(activity, bg=PANEL_BG)
        top.pack(fill=tk.X, padx=14, pady=(10, 6))
        self._label(top, "INSTALL PROGRESS", color=MUTED, size=8, bold=True).pack(side=tk.LEFT)
        self.progress_text = tk.StringVar(value="Ready")
        self._label(top, "", color=MUTED, size=9).pack(side=tk.LEFT, expand=True)
        tk.Label(
            top, textvariable=self.progress_text, bg=PANEL_BG, fg=TEXT,
            font=("Segoe UI", 9, "bold")
        ).pack(side=tk.RIGHT)

        self.progress = ttk.Progressbar(
            activity, maximum=100, mode="determinate", style="Vex.Horizontal.TProgressbar"
        )
        self.progress.pack(fill=tk.X, padx=14, pady=(0, 9))

        log_header = tk.Frame(activity, bg=PANEL_BG)
        log_header.pack(fill=tk.X, padx=14, pady=(0, 5))
        self._label(log_header, "ACTIVITY", color=MUTED, size=8, bold=True).pack(side=tk.LEFT)
        self._button(log_header, "Clear", self.clear_log, PANEL_ALT, fg=MUTED, padx=8, pady=3).pack(
            side=tk.RIGHT
        )
        log_frame = tk.Frame(activity, bg=PANEL_BG)
        log_frame.pack(fill=tk.BOTH, expand=True, padx=14, pady=(0, 12))
        self.log = tk.Text(
            log_frame,
            height=6,
            bg="#080D18",
            fg="#CFD9EA",
            insertbackground=TEXT,
            relief=tk.FLAT,
            font=("Cascadia Mono", 8),
            padx=10,
            pady=8,
            wrap=tk.WORD,
            state=tk.DISABLED,
        )
        scrollbar = ttk.Scrollbar(log_frame, orient=tk.VERTICAL, command=self.log.yview)
        self.log.configure(yscrollcommand=scrollbar.set)
        self.log.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        self.status_var = tk.StringVar(value="Scanning for USB drives...")
        self.status_label = tk.Label(
            self,
            textvariable=self.status_var,
            bg=WINDOW_BG,
            fg=MUTED,
            font=("Segoe UI", 9),
            anchor=tk.W,
        )
        self.status_label.pack(fill=tk.X, padx=26, pady=(0, 12))

        self._update_action_states()

    def _section_heading(self, number: str, title: str, subtitle: str):
        row = tk.Frame(self, bg=WINDOW_BG)
        row.pack(fill=tk.X, padx=24, pady=(8, 0))
        tk.Label(
            row,
            text=number,
            bg="#1D3857",
            fg=ACCENT,
            font=("Segoe UI", 9, "bold"),
            padx=8,
            pady=4,
        ).pack(side=tk.LEFT, padx=(0, 9))
        text_area = tk.Frame(row, bg=WINDOW_BG)
        text_area.pack(side=tk.LEFT, fill=tk.X)
        self._label(text_area, title, size=10, bold=True).pack(anchor=tk.W)
        self._label(text_area, subtitle, color=MUTED, size=8).pack(anchor=tk.W, pady=(1, 0))

    def _ui(self, callback):
        try:
            self.after(0, callback)
        except tk.TclError:
            pass

    def log_line(self, text: str):
        def append():
            self.log.configure(state=tk.NORMAL)
            self.log.insert(tk.END, f"• {text}\n")
            self.log.see(tk.END)
            self.log.configure(state=tk.DISABLED)
        self._ui(append)

    def clear_log(self):
        self.log.configure(state=tk.NORMAL)
        self.log.delete("1.0", tk.END)
        self.log.configure(state=tk.DISABLED)

    def set_progress(self, value: int):
        value = max(0, min(100, int(value)))
        self._ui(lambda: (
            self.progress.configure(value=value),
            self.progress_text.set(f"{value}%")
        ))

    def set_status(self, text: str, color=MUTED):
        self._ui(lambda: (
            self.status_var.set(text),
            self.status_label.configure(fg=color)
        ))

    def set_busy(self, busy: bool):
        self.busy = busy

        def update():
            self.refresh_button.configure(state=tk.DISABLED if busy else tk.NORMAL)
            self._update_action_states()
        self._ui(update)

    def refresh_devices(self):
        if self.busy or self.scanning:
            return
        self.scanning = True
        self.status_var.set("Scanning for physical USB drives...")
        self.status_label.configure(fg=MUTED)
        self.refresh_button.configure(state=tk.DISABLED)
        self.disk_tree.delete(*self.disk_tree.get_children())
        self.disks.clear()
        self.volumes_by_disk.clear()
        self.volume_choices.clear()
        self.volume_combo.configure(values=())
        self.volume_var.set("")
        self.selected_details.set("Scanning USB devices...")

        def worker():
            try:
                disks = get_usb_disks()
                volumes = get_usb_volumes()
                by_disk: dict[int, list[dict]] = {}
                for volume in volumes:
                    by_disk.setdefault(int(volume["DiskNumber"]), []).append(volume)

                def update():
                    self.disks = {int(disk["Number"]): disk for disk in disks}
                    self.volumes_by_disk = by_disk
                    for number, disk in self.disks.items():
                        mounted = by_disk.get(number, [])
                        volume_summary = ", ".join(
                            f"{v['DriveLetter']}: {v.get('FileSystem') or 'Unknown'}"
                            for v in mounted
                        ) or "No mounted volume"
                        self.disk_tree.insert(
                            "",
                            tk.END,
                            iid=str(number),
                            values=(
                                f"Disk {number}",
                                disk.get("FriendlyName") or "USB device",
                                human_size(disk.get("Size")),
                                volume_summary,
                                disk.get("PartitionStyle") or "RAW",
                            ),
                        )
                    self.scanning = False
                    self.refresh_button.configure(state=tk.NORMAL)
                    if disks:
                        self.status_var.set(
                            f"Found {len(disks)} physical USB drive(s). Select one to see its options."
                        )
                    else:
                        self.status_var.set(
                            "No online USB drives found. Insert a USB drive and select Refresh drives."
                        )
                    self.status_label.configure(fg=MUTED)
                    self.selected_details.set(
                        "Select a USB drive above. Internal disks are not shown."
                        if disks else
                        "No USB drive detected. Check the connection, then click Refresh drives."
                    )
                    self._update_action_states()

                self._ui(update)
            except Exception as exc:
                error_text = str(exc)
                def failed():
                    self.scanning = False
                    self.refresh_button.configure(state=tk.NORMAL)
                    self.status_var.set("USB scan failed. See Activity for details.")
                    self.status_label.configure(fg=RED)
                    self.selected_details.set("The drive scan failed.")
                    self.log_line(f"USB scan failed: {error_text}")
                    self._update_action_states()
                self._ui(failed)

        threading.Thread(target=worker, daemon=True).start()

    def selected_disk(self) -> dict | None:
        selection = self.disk_tree.selection()
        if not selection:
            return None
        try:
            return self.disks.get(int(selection[0]))
        except (ValueError, KeyError):
            return None

    def selected_volume(self) -> dict | None:
        return self.volume_choices.get(self.volume_var.get())

    def on_disk_selected(self, _event=None):
        disk = self.selected_disk()
        if not disk:
            self.selected_details.set("Select a USB drive above.")
            self._update_action_states()
            return

        number = int(disk["Number"])
        volumes = self.volumes_by_disk.get(number, [])
        model = disk.get("FriendlyName") or "USB device"
        detail = (
            f"Disk {number}  •  {model}  •  {human_size(disk.get('Size'))}\n"
            f"Serial: {disk.get('SerialNumber') or 'Not reported'}  •  "
            f"Partition layout: {disk.get('PartitionStyle') or 'RAW'}"
        )
        if disk.get("IsBoot") or disk.get("IsSystem"):
            detail += "\nWindows marks this disk as a boot/system disk. Flashing is blocked."
        elif disk.get("IsReadOnly"):
            detail += "\nThis USB disk is read-only. Change its protection before flashing."
        self.selected_details.set(detail)

        self.volume_choices.clear()
        choices = []
        for volume in volumes:
            if str(volume.get("FileSystem", "")).upper() != "FAT32":
                continue
            letter = str(volume.get("DriveLetter") or "").upper()
            if len(letter) != 1:
                continue
            label = (
                f"{letter}:  •  {volume.get('Label') or 'No label'}  •  "
                f"{human_size(volume.get('Free'))} free"
            )
            choices.append(label)
            self.volume_choices[label] = volume

        self.volume_combo.configure(values=choices)
        if choices:
            self.volume_var.set(choices[0])
        else:
            self.volume_var.set("")
            self.selected_details.set(
                detail + "\nNo mounted FAT32 volume is available for a safe update. "
                "Use Erase & Prepare to create one."
            )
        self._update_action_states()

    def on_volume_selected(self, _event=None):
        self._update_action_states()

    def _update_action_states(self):
        disk = self.selected_disk()
        valid_disk = bool(
            disk
            and not disk.get("IsBoot")
            and not disk.get("IsSystem")
            and not disk.get("IsReadOnly")
            and not disk.get("IsOffline")
        )
        quick_ready = bool(valid_disk and self.selected_volume())
        full_ready = bool(valid_disk)

        if self.busy or self.scanning:
            quick_ready = False
            full_ready = False

        self.quick_button.configure(state=tk.NORMAL if quick_ready else tk.DISABLED)
        self.full_button.configure(state=tk.NORMAL if full_ready else tk.DISABLED)

    def copy_selected(self):
        if self.busy:
            return
        disk = self.selected_disk()
        volume = self.selected_volume()
        if not disk or not volume:
            messagebox.showwarning(
                "Choose a FAT32 volume",
                "Select a USB drive that has a mounted FAT32 volume.",
                parent=self,
            )
            return

        letter = str(volume["DriveLetter"]).upper()
        number = int(disk["Number"])
        confirmed = messagebox.askyesno(
            "Update existing USB",
            f"Install VexOS to {letter}: on Disk {number}?\n\n"
            "This copies the UEFI bootloader, kernel, and startup script if present.\n\n"
            "• Existing unrelated files are preserved.\n"
            "• The USB is not formatted or repartitioned.\n"
            "• Administrator permission is not required.",
            parent=self,
        )
        if not confirmed:
            return

        self._start_worker(
            f"Updating {letter}:",
            lambda: copy_boot_files(
                letter,
                progress=self.set_progress,
                log=self.log_line,
                expected_disk_number=number,
            ),
            "VexOS update complete",
        )

    def flash_selected(self):
        if self.busy:
            return
        disk = self.selected_disk()
        if not disk:
            messagebox.showwarning(
                "Select a USB drive",
                "Select a physical USB drive from the list first.",
                parent=self,
            )
            return
        if disk.get("IsBoot") or disk.get("IsSystem") or disk.get("IsReadOnly"):
            messagebox.showerror(
                "Drive is protected",
                "Windows identifies this drive as protected or read-only. It cannot be flashed.",
                parent=self,
            )
            return

        if not is_admin():
            relaunch = messagebox.askyesno(
                "Administrator permission required",
                "Erase & Prepare needs administrator permission to repartition the USB.\n\n"
                "Restart VexOS USB Flasher as administrator?",
                parent=self,
            )
            if relaunch:
                if relaunch_as_admin():
                    self.destroy()
                else:
                    messagebox.showerror(
                        "Could not elevate",
                        "Windows did not start the elevated flasher. Try opening PowerShell as Administrator.",
                        parent=self,
                    )
            return

        number = int(disk["Number"])
        identity = (
            f"Disk {number}: {disk.get('FriendlyName') or 'USB device'}\n"
            f"Capacity: {human_size(disk.get('Size'))}\n"
            f"Serial: {disk.get('SerialNumber') or 'Not reported'}"
        )
        confirmed = messagebox.askyesno(
            "This will erase the entire USB",
            f"{identity}\n\n"
            "EVERY PARTITION AND FILE ON THIS DISK WILL BE DELETED.\n"
            "The installer will create a GPT/FAT32 VexOS boot drive.\n\n"
            "Continue only if this is definitely the USB drive you want to erase.",
            icon="warning",
            parent=self,
        )
        if not confirmed:
            return

        phrase = f"ERASE {number}"
        typed = simpledialog.askstring(
            "Final confirmation",
            f"To confirm, type exactly:\n\n{phrase}",
            parent=self,
        )
        if typed != phrase:
            messagebox.showinfo(
                "Cancelled",
                "The disk was not changed. The confirmation text did not match.",
                parent=self,
            )
            return

        self._start_worker(
            f"Preparing Disk {number}...",
            lambda: flash_usb_disk(disk, progress=self.set_progress, log=self.log_line),
            "VexOS USB prepared",
            refresh_after=True,
        )

    def _start_worker(self, status: str, operation, success_title: str, refresh_after=False):
        self.set_busy(True)
        self.status_var.set(status)
        self.status_label.configure(fg=ACCENT)
        self.progress.configure(value=0)
        self.progress_text.set("Starting")
        self.log_line(status)

        def worker():
            try:
                result = operation()
                files = result.get("files", [])
                detail = "\n".join(
                    f"{item['file']} — {item['bytes']:,} bytes — SHA-256 verified"
                    for item in files
                )
                destination = result.get("root") or f"{result.get('drive_letter')}:\\"
                self.log_line(f"Success. VexOS is installed at {destination}")
                self.set_progress(100)

                def success():
                    messagebox.showinfo(
                        success_title,
                        f"VexOS boot files are ready at {destination}\n\n{detail}",
                        parent=self,
                    )
                    self.status_var.set(f"Complete — {destination}")
                    self.status_label.configure(fg=GREEN)
                self._ui(success)
            except Exception as exc:
                error_text = str(exc)
                self.log_line(f"ERROR: {error_text}")
                self.set_status("Operation failed. See Activity for the error.", RED)
                self._ui(
                    lambda: messagebox.showerror(
                        "Installation failed", error_text, parent=self
                    )
                )
            finally:
                self.set_busy(False)
                if refresh_after:
                    self._ui(self.refresh_devices)

        threading.Thread(target=worker, daemon=True).start()

    def close_window(self):
        if self.busy:
            messagebox.showwarning(
                "Operation in progress",
                "A USB operation is still running. Wait for it to finish before closing the flasher.",
                parent=self,
            )
            return
        self.destroy()


def main():
    if os.name != "nt":
        raise SystemExit("VexOS USB Flasher is a Windows-only utility.")
    app = VexOSFlasher()
    app.mainloop()


if __name__ == "__main__":
    main()
