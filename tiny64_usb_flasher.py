"""VexOS USB Flasher for Windows.

Copy mode updates an existing USB volume without formatting it.
Flash mode repartitions a selected USB disk as GPT + FAT32 and installs
the UEFI removable-media bootloader and kernel. Flash mode is destructive.
"""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import threading
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
DIST_DIR = os.path.join(PROJECT_ROOT, "dist")
BOOTX64_SRC = os.path.join(DIST_DIR, "BOOTX64.EFI")
KERNEL_SRC = os.path.join(DIST_DIR, "kernel.elf")
STARTUP_SRC = os.path.join(PROJECT_ROOT, "build", "startup.nsh")
APP_TITLE = "VexOS USB Flasher"


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except (AttributeError, OSError):
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
    # Do not rely on DriveType=Removable: Windows reports many USB sticks as
    # fixed disks. Resolve each mounted partition back to its physical USB disk.
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
          DiskName = [string]$disk.FriendlyName
        }
      }
    }
  }
)
$result | ConvertTo-Json -Depth 3 -Compress
"""
    return parse_json_array(run_powershell(script))


def human_size(value: int | str | None) -> str:
    number = int(value or 0)
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


def validate_build() -> None:
    missing = [path for path in (BOOTX64_SRC, KERNEL_SRC) if not os.path.isfile(path)]
    if missing:
        names = "\n".join(os.path.relpath(path, PROJECT_ROOT) for path in missing)
        raise FileNotFoundError(
            "VexOS has not been built yet. Missing:\n"
            f"{names}\n\nBuild it from WSL with: make"
        )
    for path in (BOOTX64_SRC, KERNEL_SRC):
        if os.path.getsize(path) == 0:
            raise RuntimeError(f"Build artifact is empty: {path}")


def validate_usb_volume(drive_letter: str, expected_disk_number: int | None = None) -> None:
    letter = drive_letter.upper()
    if len(letter) != 1 or not letter.isalpha():
        raise ValueError("Invalid destination drive letter.")
    script = rf"""
$ErrorActionPreference = 'Stop'
$partition = Get-Partition -DriveLetter '{letter}' -ErrorAction Stop
$disk = Get-Disk -Number $partition.DiskNumber -ErrorAction Stop
if ($disk.BusType -ne 'USB') {{ throw 'Refusing to copy: destination is not on a USB disk.' }}
if ($disk.IsBoot -or $disk.IsSystem) {{ throw 'Refusing to copy: destination belongs to a Windows boot/system disk.' }}
if ({expected_disk_number if expected_disk_number is not None else -1} -ge 0 -and $disk.Number -ne {expected_disk_number if expected_disk_number is not None else -1}) {{ throw 'USB disk identity changed. Refresh devices and try again.' }}
"""
    run_powershell(script)


def copy_boot_files(drive_letter: str, progress=None, log=None,
                    expected_disk_number: int | None = None) -> dict:
    validate_build()
    if len(drive_letter) != 1 or not drive_letter.isalpha():
        raise ValueError("Invalid destination drive letter.")

    validate_usb_volume(drive_letter, expected_disk_number)
    root = f"{drive_letter.upper()}:\\"
    if not os.path.isdir(root):
        raise FileNotFoundError(f"USB volume {root} is not accessible.")

    efi_dir = os.path.join(root, "EFI", "BOOT")
    os.makedirs(efi_dir, exist_ok=True)

    copies = [
        (BOOTX64_SRC, os.path.join(efi_dir, "BOOTX64.EFI")),
        (KERNEL_SRC, os.path.join(root, "kernel.elf")),
    ]
    if os.path.isfile(STARTUP_SRC):
        copies.append((STARTUP_SRC, os.path.join(root, "startup.nsh")))

    results = []
    for index, (source, destination) in enumerate(copies, start=1):
        if log:
            log(f"Copying {os.path.relpath(source, PROJECT_ROOT)}...")
        shutil.copyfile(source, destination)
        if sha256_file(source) != sha256_file(destination):
            raise IOError(f"Verification failed after copying {os.path.basename(source)}.")
        results.append({
            "file": os.path.relpath(destination, root),
            "bytes": os.path.getsize(destination),
            "sha256": sha256_file(destination),
        })
        if progress:
            progress(int(index * 100 / len(copies)))

    return {"root": root, "files": results}


def flash_usb_disk(disk: dict, progress=None, log=None) -> dict:
    """Destructively prepare one verified USB disk, then copy VexOS boot files."""
    validate_build()

    number = int(disk["Number"])
    unique_id = str(disk.get("UniqueId") or "")
    serial = str(disk.get("SerialNumber") or "")
    size = int(disk["Size"])

    # Base64 keeps device identifiers safe when embedded in a PowerShell script.
    import base64
    unique_b64 = base64.b64encode(unique_id.encode("utf-8")).decode("ascii")
    serial_b64 = base64.b64encode(serial.encode("utf-8")).decode("ascii")

    if log:
        log(f"Re-checking USB disk {number} before changing its partition table...")

    script = rf"""
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$number = {number}
$expectedSize = [int64]{size}
$expectedUnique = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{unique_b64}'))
$expectedSerial = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{serial_b64}'))
$disk = Get-Disk -Number $number -ErrorAction Stop
if ($disk.BusType -ne 'USB') {{ throw 'Safety check failed: selected disk is no longer a USB disk.' }}
if ($disk.IsBoot -or $disk.IsSystem) {{ throw 'Safety check failed: refusing to modify a boot or system disk.' }}
if ($disk.IsReadOnly) {{ throw 'Selected USB disk is read-only.' }}
if ($disk.IsOffline) {{ throw 'Selected USB disk is offline.' }}
if ([int64]$disk.Size -ne $expectedSize) {{ throw 'Safety check failed: disk capacity changed since selection. Refresh devices.' }}
if ($expectedUnique -and [string]$disk.UniqueId -ne $expectedUnique) {{ throw 'Safety check failed: disk identity changed. Refresh devices.' }}
if ($expectedSerial -and [string]$disk.SerialNumber -ne $expectedSerial) {{ throw 'Safety check failed: disk serial changed. Refresh devices.' }}
$disk | Clear-Disk -RemoveData -RemoveOEM -Confirm:$false
Initialize-Disk -Number $number -PartitionStyle GPT
$partition = New-Partition -DiskNumber $number -UseMaximumSize -AssignDriveLetter
$volume = Format-Volume -Partition $partition -FileSystem FAT32 -NewFileSystemLabel 'VEXOS' -Confirm:$false -Force
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
        log(f"USB disk prepared as {letter}:\\ (GPT / FAT32).")
        log("Installing UEFI bootloader and kernel...")
    if progress:
        progress(20)
    result = copy_boot_files(letter, progress=lambda value: progress(20 + int(value * 0.8)) if progress else None, log=log, expected_disk_number=number)
    result["drive_letter"] = letter
    return result


class VexOSFlasher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("1020x720")
        self.minsize(850, 600)
        self.configure(bg="#111827")

        self.disks: dict[int, dict] = {}
        self.volumes: dict[str, dict] = {}
        self.busy = False

        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TFrame", background="#111827")
        style.configure("Panel.TFrame", background="#1F2937")
        style.configure("TLabel", background="#111827", foreground="#E5E7EB", font=("Segoe UI", 10))
        style.configure("Title.TLabel", font=("Segoe UI", 20, "bold"), foreground="#FFFFFF")
        style.configure("Sub.TLabel", foreground="#9CA3AF")
        style.configure("TButton", font=("Segoe UI", 10), padding=(10, 7))
        style.configure("Accent.TButton", font=("Segoe UI", 10, "bold"))
        style.configure("Danger.TButton", foreground="#B91C1C", font=("Segoe UI", 10, "bold"))
        style.configure("Treeview", font=("Segoe UI", 9), rowheight=27)
        style.configure("Treeview.Heading", font=("Segoe UI", 9, "bold"))
        style.configure("Horizontal.TProgressbar", troughcolor="#374151", background="#38BDF8")

        self._build_ui()
        self.refresh_devices()

    def _build_ui(self):
        header = ttk.Frame(self, padding=(18, 16, 18, 10))
        header.pack(fill=tk.X)
        ttk.Label(header, text="VexOS USB Flasher", style="Title.TLabel").pack(anchor=tk.W)
        ttk.Label(
            header,
            text="Prepare bootable UEFI USB media or safely update an existing VexOS volume.",
            style="Sub.TLabel",
        ).pack(anchor=tk.W, pady=(3, 0))

        toolbar = ttk.Frame(self, padding=(14, 5))
        toolbar.pack(fill=tk.X)
        self.refresh_button = ttk.Button(toolbar, text="Refresh devices", command=self.refresh_devices)
        self.refresh_button.pack(side=tk.LEFT, padx=(0, 8))
        self.copy_button = ttk.Button(toolbar, text="Update selected volume", command=self.copy_selected)
        self.copy_button.pack(side=tk.LEFT)

        content = ttk.Frame(self, padding=(14, 8))
        content.pack(fill=tk.BOTH, expand=True)

        disk_panel = ttk.LabelFrame(content, text="Physical USB disks — flashing erases the entire selected disk", padding=10)
        disk_panel.pack(fill=tk.BOTH, expand=True, pady=(0, 10))
        self.disk_tree = ttk.Treeview(
            disk_panel,
            columns=("number", "model", "size", "serial", "style", "readonly"),
            show="headings",
            height=7,
            selectmode="browse",
        )
        for key, label, width in (
            ("number", "Disk", 55), ("model", "Device", 300), ("size", "Capacity", 100),
            ("serial", "Serial", 170), ("style", "Partition style", 110), ("readonly", "Read-only", 85),
        ):
            self.disk_tree.heading(key, text=label)
            self.disk_tree.column(key, width=width, anchor=tk.W)
        self.disk_tree.pack(fill=tk.BOTH, expand=True)
        ttk.Button(disk_panel, text="Prepare + flash selected USB (ERASES DISK)", style="Danger.TButton",
                   command=self.flash_selected).pack(anchor=tk.E, pady=(9, 0))

        volume_panel = ttk.LabelFrame(content, text="Mounted volumes on USB disks — copy mode preserves other files", padding=10)
        volume_panel.pack(fill=tk.BOTH, expand=True)
        self.volume_tree = ttk.Treeview(
            volume_panel,
            columns=("drive", "disk", "label", "filesystem", "size", "free"),
            show="headings",
            height=5,
            selectmode="browse",
        )
        for key, label, width in (
            ("drive", "Drive", 70), ("disk", "Disk", 55), ("label", "Label", 180),
            ("filesystem", "Filesystem", 100), ("size", "Capacity", 100), ("free", "Free", 100),
        ):
            self.volume_tree.heading(key, text=label)
            self.volume_tree.column(key, width=width, anchor=tk.W)
        self.volume_tree.pack(fill=tk.BOTH, expand=True)

        footer = ttk.Frame(self, padding=(14, 6, 14, 14))
        footer.pack(fill=tk.BOTH)
        self.status = tk.StringVar(value="Ready")
        ttk.Label(footer, textvariable=self.status, style="Sub.TLabel").pack(anchor=tk.W, pady=(0, 5))
        self.progress = ttk.Progressbar(footer, maximum=100, mode="determinate")
        self.progress.pack(fill=tk.X, pady=(0, 7))
        self.log = tk.Text(
            footer, height=8, bg="#0B1220", fg="#D1D5DB", insertbackground="#FFFFFF",
            relief=tk.FLAT, font=("Cascadia Mono", 9), padx=10, pady=8,
        )
        self.log.pack(fill=tk.BOTH, expand=True)

    def _ui(self, callback):
        self.after(0, callback)

    def log_line(self, text: str):
        def append():
            self.log.insert(tk.END, f"• {text}\n")
            self.log.see(tk.END)
        self._ui(append)

    def set_progress(self, value: int):
        self._ui(lambda: self.progress.configure(value=max(0, min(100, value))))

    def set_status(self, text: str):
        self._ui(lambda: self.status.set(text))

    def show_error(self, title: str, text: str):
        self._ui(lambda: messagebox.showerror(title, text, parent=self))

    def show_info(self, title: str, text: str):
        self._ui(lambda: messagebox.showinfo(title, text, parent=self))

    def set_busy(self, busy: bool):
        self.busy = busy
        state = tk.DISABLED if busy else tk.NORMAL
        self._ui(lambda: (
            self.refresh_button.configure(state=state),
            self.copy_button.configure(state=state),
        ))

    def refresh_devices(self):
        if self.busy:
            return
        self.status.set("Scanning USB devices...")
        self.disk_tree.delete(*self.disk_tree.get_children())
        self.volume_tree.delete(*self.volume_tree.get_children())

        def worker():
            try:
                disks = get_usb_disks()
                volumes = get_usb_volumes()
                self.disks = {int(d["Number"]): d for d in disks}
                self.volumes = {str(v["DriveLetter"]).upper(): v for v in volumes}

                def update():
                    for number, disk in self.disks.items():
                        self.disk_tree.insert("", tk.END, iid=str(number), values=(
                            number, disk.get("FriendlyName") or "USB device",
                            human_size(disk.get("Size")), disk.get("SerialNumber") or "—",
                            disk.get("PartitionStyle") or "RAW",
                            "Yes" if disk.get("IsReadOnly") else "No",
                        ))
                    for letter, volume in self.volumes.items():
                        self.volume_tree.insert("", tk.END, iid=letter, values=(
                            f"{letter}:\\", volume.get("DiskNumber"), volume.get("Label") or "",
                            volume.get("FileSystem") or "—", human_size(volume.get("Size")),
                            human_size(volume.get("Free")),
                        ))
                    self.status.set(f"Found {len(disks)} USB disk(s) and {len(volumes)} mounted USB volume(s).")
                self._ui(update)
            except Exception as exc:
                self.set_status("Device scan failed.")
                self.log_line(f"Device scan error: {exc}")

        threading.Thread(target=worker, daemon=True).start()

    def _selected_volume(self):
        selected = self.volume_tree.selection()
        if not selected:
            messagebox.showwarning("Select a volume", "Choose a mounted USB volume first.", parent=self)
            return None
        letter = selected[0].upper()
        volume = self.volumes.get(letter)
        if not volume:
            messagebox.showerror("Device changed", "That USB volume is no longer available. Refresh devices.", parent=self)
            return None
        return letter, volume

    def copy_selected(self):
        if self.busy:
            return
        selected = self._selected_volume()
        if not selected:
            return
        letter, volume = selected
        if not messagebox.askyesno(
            "Update VexOS files",
            f"Copy the current VexOS bootloader and kernel to {letter}:\\?\n\n"
            "This does not format the volume. Existing files other than the VexOS boot files are preserved.",
            parent=self,
        ):
            return
        self._start_worker(
            f"Updating {letter}:\\",
            lambda: copy_boot_files(letter, progress=self.set_progress, log=self.log_line),
            "VexOS volume updated",
        )

    def flash_selected(self):
        if self.busy:
            return
        selected = self.disk_tree.selection()
        if not selected:
            messagebox.showwarning("Select a disk", "Choose a physical USB disk from the top list.", parent=self)
            return
        number = int(selected[0])
        disk = self.disks.get(number)
        if not disk:
            messagebox.showerror("Device changed", "That USB disk is no longer available. Refresh devices.", parent=self)
            return
        if disk.get("IsBoot") or disk.get("IsSystem"):
            messagebox.showerror("Protected disk", "Windows identifies this as a boot or system disk. It cannot be flashed.", parent=self)
            return
        if disk.get("IsReadOnly"):
            messagebox.showerror("Read-only disk", "This USB disk is read-only.", parent=self)
            return
        if not is_admin():
            messagebox.showwarning(
                "Administrator permission required",
                "Close the flasher and reopen it with Run as administrator before flashing a physical disk.",
                parent=self,
            )
            return

        identity = (
            f"Disk {number}: {disk.get('FriendlyName') or 'USB device'}\n"
            f"Capacity: {human_size(disk.get('Size'))}\n"
            f"Serial: {disk.get('SerialNumber') or 'Not reported'}"
        )
        confirmed = messagebox.askyesno(
            "FINAL WARNING — THIS ERASES THE USB",
            f"{identity}\n\nThis operation deletes every partition and file on this disk, "
            "then creates a GPT/FAT32 VexOS boot volume.\n\nContinue only if this is definitely the USB drive you intend to erase.",
            icon="warning",
            parent=self,
        )
        if not confirmed:
            return
        phrase = f"ERASE {number}"
        typed = simpledialog.askstring(
            "Confirm disk erase",
            f"Type exactly {phrase} to authorize formatting this USB disk:",
            parent=self,
        )
        if typed != phrase:
            messagebox.showinfo("Cancelled", "The disk was not changed.", parent=self)
            return

        self._start_worker(
            f"Preparing USB disk {number}...",
            lambda: flash_usb_disk(disk, progress=self.set_progress, log=self.log_line),
            "VexOS USB prepared",
            refresh_after=True,
        )

    def _start_worker(self, status: str, operation, success_title: str, refresh_after=False):
        self.set_busy(True)
        self.status.set(status)
        self.progress.configure(value=0)

        def worker():
            try:
                result = operation()
                files = result.get("files", [])
                detail = "\n".join(
                    f"{item['file']} — {item['bytes']:,} bytes — SHA-256 verified"
                    for item in files
                )
                destination = result.get("root") or f"{result.get('drive_letter')}:\\"
                self.log_line(f"Completed successfully at {destination}")
                self.show_info(success_title, f"VexOS boot files are ready at {destination}\n\n{detail}")
                self.set_status(f"Completed: {destination}")
            except Exception as exc:
                self.log_line(f"ERROR: {exc}")
                self.show_error("Operation failed", str(exc))
                self.set_status("Operation failed.")
            finally:
                self.set_busy(False)
                self.set_progress(0)
                if refresh_after:
                    self.refresh_devices()


def main():
    if os.name != "nt":
        raise SystemExit("VexOS USB Flasher is a Windows-only utility.")
    app = VexOSFlasher()
    app.mainloop()


if __name__ == "__main__":
    main()
