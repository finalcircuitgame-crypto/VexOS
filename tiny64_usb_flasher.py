import ctypes
import json
import os
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import ttk, messagebox

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
DIST_DIR = os.path.join(PROJECT_ROOT, "dist")
BOOTX64_SRC = os.path.join(DIST_DIR, "BOOTX64.EFI")
KERNEL_SRC = os.path.join(DIST_DIR, "kernel.elf")


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def run_ps(script: str) -> str:
    proc = subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            script,
        ],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or proc.stdout.strip() or f"PowerShell failed: {proc.returncode}")
    return proc.stdout


def get_removable_volumes():
    script = r"""
$ErrorActionPreference='Stop'
Get-Volume |
  Where-Object { $_.DriveLetter -ne $null -and $_.DriveType -eq 'Removable' } |
  Select-Object DriveLetter,FileSystemLabel,FileSystem,SizeRemaining,Size |
  ConvertTo-Json -Depth 2
"""
    try:
        out = run_ps(script)
        if not out.strip():
            return []
        data = json.loads(out)
        if isinstance(data, dict):
            return [data]
        return data
    except:
        return []


def get_usb_disks():
    script = r"""
$ErrorActionPreference='Stop'
Get-Disk |
  Where-Object { $_.BusType -eq 'USB' -and $_.OperationalStatus -eq 'Online' } |
  Select-Object Number,FriendlyName,Size,PartitionStyle,IsSystem,IsBoot,IsReadOnly |
  ConvertTo-Json -Depth 2
"""
    try:
        out = run_ps(script)
        if not out.strip():
            return []
        data = json.loads(out)
        if isinstance(data, dict):
            return [data]
        return data
    except:
        return []


def sizeof_fmt(num: int) -> str:
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if num < 1024:
            return f"{num:.1f} {unit}"
        num /= 1024
    return f"{num:.1f} PB"


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Tiny64 USB Flasher Pro")
        self.geometry("900x650")
        self.configure(bg="#f0f0f0")

        self.style = ttk.Style()
        self.style.theme_use('clam')
        self.style.configure("TFrame", background="#f0f0f0")
        self.style.configure("TLabel", background="#f0f0f0", font=("Segoe UI", 10))
        self.style.configure("Header.TLabel", font=("Segoe UI", 12, "bold"))
        self.style.configure("TButton", font=("Segoe UI", 10))
        self.style.configure("Action.TButton", font=("Segoe UI", 10, "bold"))
        self.style.configure("Danger.TButton", foreground="red")

        self.disks = []
        self.volumes = []
        self._lock = threading.Lock()

        self._build_ui()
        self.refresh()

    def _build_ui(self):
        # Top Action Bar
        top = ttk.Frame(self, padding=10)
        top.pack(fill=tk.X)

        self.refresh_btn = ttk.Button(top, text="🔄 Refresh Devices", command=self.refresh)
        self.refresh_btn.pack(side=tk.LEFT, padx=5)

        ttk.Separator(top, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=10)

        self.quick_update_btn = ttk.Button(top, text="⚡ Quick Update (TINY64)", command=self.quick_update, style="Action.TButton")
        self.quick_update_btn.pack(side=tk.LEFT, padx=5)

        self.copy_btn = ttk.Button(top, text="📂 Copy to Selected Volume", command=self.copy_only)
        self.copy_btn.pack(side=tk.LEFT, padx=5)

        ttk.Separator(top, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=10)

        self.flash_btn = ttk.Button(top, text="💀 Format + Flash Disk", command=self.flash_selected, style="Danger.TButton")
        self.flash_btn.pack(side=tk.LEFT, padx=5)

        # Confirm Wipe
        confirm_frame = ttk.Frame(self, padding=(10, 0))
        confirm_frame.pack(fill=tk.X)
        ttk.Label(confirm_frame, text="To enable destructive actions, type 'WIPE':").pack(side=tk.LEFT, padx=5)
        self.confirm_entry = ttk.Entry(confirm_frame, width=10)
        self.confirm_entry.pack(side=tk.LEFT, padx=5)
        self.confirm_entry.bind("<KeyRelease>", lambda e: self._check_wipe_auth())

        # Main Content
        main_paned = ttk.PanedWindow(self, orient=tk.VERTICAL)
        main_paned.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        # USB Disks Section
        disk_frame = ttk.Frame(main_paned)
        ttk.Label(disk_frame, text="Physical USB Disks (Destructive Formatting Target)", style="Header.TLabel").pack(anchor=tk.W, pady=5)
        self.tree = ttk.Treeview(disk_frame, columns=("num", "name", "size", "style", "boot", "sys", "ro"), show="headings", height=6)
        for k, t, w in [
            ("num", "ID", 40),
            ("name", "Friendly Name", 300),
            ("size", "Size", 100),
            ("style", "Style", 80),
            ("boot", "Boot", 60),
            ("sys", "System", 70),
            ("ro", "Read-Only", 80),
        ]:
            self.tree.heading(k, text=t)
            self.tree.column(k, width=w, anchor=tk.W)
        self.tree.pack(fill=tk.BOTH, expand=True)
        main_paned.add(disk_frame, weight=2)

        # Volumes Section
        vol_frame = ttk.Frame(main_paned)
        ttk.Label(vol_frame, text="Removable Volumes (Safe Copy Target)", style="Header.TLabel").pack(anchor=tk.W, pady=5)
        self.vol_tree = ttk.Treeview(vol_frame, columns=("dl", "label", "fs", "size", "free"), show="headings", height=6)
        for k, t, w in [
            ("dl", "Drive", 60),
            ("label", "Label", 200),
            ("fs", "FS", 80),
            ("size", "Total Size", 100),
            ("free", "Free Space", 100),
        ]:
            self.vol_tree.heading(k, text=t)
            self.vol_tree.column(k, width=w, anchor=tk.W)
        self.vol_tree.pack(fill=tk.BOTH, expand=True)
        main_paned.add(vol_frame, weight=1)

        # Status & Log
        bottom = ttk.Frame(self, padding=10)
        bottom.pack(fill=tk.BOTH, expand=False)

        self.progress = ttk.Progressbar(bottom, mode="determinate")
        self.progress.pack(fill=tk.X, pady=(0, 5))

        self.status_var = tk.StringVar(value="Ready")
        ttk.Label(bottom, textvariable=self.status_var, font=("Segoe UI", 9, "italic")).pack(anchor=tk.W)

        self.log = tk.Text(bottom, height=8, font=("Consolas", 9), bg="#1e1e1e", fg="#d4d4d4")
        self.log.pack(fill=tk.BOTH, expand=True, pady=5)

    def _check_wipe_auth(self):
        is_wipe = self.confirm_entry.get().strip().upper() == "WIPE"
        if is_wipe and is_admin():
            self.flash_btn.configure(state=tk.NORMAL)
        else:
            self.flash_btn.configure(state=tk.DISABLED)

    def log_line(self, s: str):
        def _log():
            self.log.insert(tk.END, f"> {s}\n")
            self.log.see(tk.END)
        self.after(0, _log)

    def set_status(self, s: str):
        self.after(0, lambda: self.status_var.set(s))

    def refresh(self):
        self.set_status("Scanning for devices...")
        self.tree.delete(*self.tree.get_children())
        self.vol_tree.delete(*self.vol_tree.get_children())

        def _task():
            disks = get_usb_disks()
            volumes = get_removable_volumes()

            def _update_ui():
                for d in disks:
                    self.tree.insert("", tk.END, values=(
                        d.get("Number"), d.get("FriendlyName"),
                        sizeof_fmt(int(d.get("Size", 0))),
                        d.get("PartitionStyle"), str(d.get("IsBoot")),
                        str(d.get("IsSystem")), str(d.get("IsReadOnly"))
                    ))

                tiny64_found = None
                for v in volumes:
                    dl = v.get("DriveLetter")
                    label = v.get("FileSystemLabel") or ""
                    fs = v.get("FileSystem") or ""
                    size = sizeof_fmt(int(v.get("Size", 0)))
                    free = sizeof_fmt(int(v.get("SizeRemaining", 0)))
                    item = self.vol_tree.insert("", tk.END, values=(f"{dl}:\\", label, fs, size, free))
                    if label.upper() == "TINY64":
                        tiny64_found = item

                if tiny64_found:
                    self.vol_tree.selection_set(tiny64_found)
                    self.quick_update_btn.configure(state=tk.NORMAL)
                    self.log_line("Auto-detected TINY64 drive.")
                else:
                    self.quick_update_btn.configure(state=tk.DISABLED)

                self._check_wipe_auth()
                self.set_status(f"Scan complete. {len(disks)} disks, {len(volumes)} volumes found.")

            self.after(0, _update_ui)

        threading.Thread(target=_task, daemon=True).start()

    def _safe_copy(self, dst_root, clean=True):
        if not os.path.isfile(BOOTX64_SRC) or not os.path.isfile(KERNEL_SRC):
            raise FileNotFoundError("Build files missing in dist/. Build project first.")

        self.progress["value"] = 10
        if clean:
            self.log_line(f"Cleaning existing files on {dst_root}...")
            kernel_path = os.path.join(dst_root, "kernel.elf")
            if os.path.exists(kernel_path):
                os.remove(kernel_path)

        self.progress["value"] = 30
        efi_boot = os.path.join(dst_root, "EFI", "BOOT")
        os.makedirs(efi_boot, exist_ok=True)

        boot_dst = os.path.join(efi_boot, "BOOTX64.EFI")
        kernel_dst = os.path.join(dst_root, "kernel.elf")

        self.log_line("Copying BOOTX64.EFI...")
        with open(BOOTX64_SRC, "rb") as s, open(boot_dst, "wb") as d:
            d.write(s.read())
        self.progress["value"] = 60

        self.log_line("Copying kernel.elf...")
        with open(KERNEL_SRC, "rb") as s, open(kernel_dst, "wb") as d:
            d.write(s.read())
        self.progress["value"] = 100
        self.log_line("Syncing...")
        # Simple flush
        try:
            subprocess.run(["powershell", "-Command", "ls " + dst_root], capture_output=True)
        except: pass
        self.log_line("Done.")

    def quick_update(self):
        sel = self.vol_tree.selection()
        if not sel:
            # Try to find TINY64 volume automatically
            for item in self.vol_tree.get_children():
                if "TINY64" in str(self.vol_tree.item(item, "values")[1]).upper():
                    self.vol_tree.selection_set(item)
                    sel = (item,)
                    break

        if not sel:
            messagebox.showwarning("No target", "Could not find a drive labeled 'TINY64' for quick update.")
            return

        values = self.vol_tree.item(sel[0], "values")
        dst_root = values[0]

        def _task():
            try:
                self.set_status(f"Updating {dst_root}...")
                self._safe_copy(dst_root, clean=True)
                messagebox.showinfo("Success", f"Tiny64 updated on {dst_root}")
            except Exception as e:
                messagebox.showerror("Update failed", str(e))
            finally:
                self.set_status("Ready")
                self.after(0, lambda: self.progress.configure(value=0))

        threading.Thread(target=_task, daemon=True).start()

    def copy_only(self):
        sel = self.vol_tree.selection()
        if not sel:
            messagebox.showwarning("Selection Required", "Select a volume from the list first.")
            return

        values = self.vol_tree.item(sel[0], "values")
        dst_root = values[0]

        if not messagebox.askyesno("Confirm Copy", f"Copy Tiny64 files to {dst_root}?\nExisting kernel.elf will be replaced."):
            return

        self.quick_update()

    def flash_selected(self):
        sel = self.tree.selection()
        if not sel:
            messagebox.showwarning("Selection Required", "Select a physical disk from the top list.")
            return

        if self.confirm_entry.get().strip().upper() != "WIPE":
            messagebox.showerror("Auth Failed", "You must type 'WIPE' to enable this destructive operation.")
            return

        values = self.tree.item(sel[0], "values")
        disk_number = int(values[0])
        name = values[1]

        if not messagebox.askyesno("FINAL WARNING",
            f"This will COMPLETELY ERASE Disk {disk_number} ({name}).\n\n"
            "ALL DATA WILL BE LOST. Continue?"):
            return

        self.set_status(f"Formatting Disk {disk_number}...")
        self.progress["value"] = 5

        def _task():
            try:
                self.log_line(f"Wiping disk {disk_number}...")
                script = rf"""
$ErrorActionPreference='Stop'
$disk = Get-Disk -Number {disk_number}
if ($disk.BusType -ne 'USB') {{ throw 'Refusing: not a USB disk' }}
if ($disk.IsBoot -or $disk.IsSystem) {{ throw 'Refusing: selected disk is boot/system' }}
$disk | Clear-Disk -RemoveData -RemoveOEM -Confirm:$false
Initialize-Disk -Number {disk_number} -PartitionStyle GPT
$part = New-Partition -DiskNumber {disk_number} -UseMaximumSize -AssignDriveLetter
$vol = Format-Volume -Partition $part -FileSystem FAT32 -NewFileSystemLabel 'TINY64' -Confirm:$false
($vol | Get-Volume).DriveLetter
"""
                self.progress["value"] = 20
                out = run_ps(script).strip().splitlines()[-1].strip()
                if not out or len(out) != 1:
                    raise RuntimeError(f"Failed to get drive letter. Output: {out}")

                dst_root = f"{out}:\\"
                self.log_line(f"Format successful. Drive letter: {out}")
                self._safe_copy(dst_root, clean=False)
                messagebox.showinfo("Success", f"Disk {disk_number} flashed and ready!")
            except Exception as e:
                messagebox.showerror("Flash failed", str(e))
                self.log_line(f"ERROR: {e}")
            finally:
                self.set_status("Ready")
                self.after(0, lambda: self.progress.configure(value=0))
                self.refresh()

        threading.Thread(target=_task, daemon=True).start()


def main():
    if not is_admin():
        # Optional: auto-relaunch logic could go here, but usually users prefer knowing why.
        pass
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()



if __name__ == "__main__":
    main()
