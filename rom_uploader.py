#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rom_uploader.py —— PicoSystem GBC 模拟器的 ROM 打包 + 上传小工具（Tk GUI）

流程：选 .gb/.gbc/.nes -> 打包成 **未压缩 ustar tar** -> 生成自带地址的 UF2，
     可直接拖入 RPI-RP2 盘符，也可用 picotool 烧到 flash 的 ROM 区。

为什么 tar 不压缩：固件在 flash 里 XIP 直读 ROM，一个字节都不进 RAM。
gzip 过就必须先解压到 RAM，256KB 装不下。
固件 tar 扫描器按 "ustar" 魔数识别，所以必须 USTAR_FORMAT（不能用 GNU 格式）。

依赖：只用 Python 标准库（tkinter + tarfile + subprocess）。
     picotool 为可选，仅“打包并上传”需要。

用法：
    python rom_uploader.py
"""

import os
import sys
import tarfile
import tempfile
import threading
import subprocess
import shutil
import time
import queue
# 注意：这里必须是**无条件的顶层 import**。写成 try/except 时 PyInstaller 会把
# tkinter 判成 "optional import" 悄悄漏掉，打出的 exe 一启动就 AttributeError。
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# ---- 和固件里的常量对齐 ----------------------------------------------
# 统一 ROM 区（NES/GBC 共用）：0x10120000，在全部固件结尾 0x10111D00 之后
# 末尾 64KB 留给存档：NES 16KB + GB 32KB + 余量
SAVE_RESERVE = 65536
FLASH_PROFILES = {
    "16MB 板":    (0x10120000, 16 * 1024 * 1024 - 0x120000 - SAVE_RESERVE),
    "2MB 标准板": (0x10080000, 2 * 1024 * 1024 - 0x80000 - SAVE_RESERVE),
}
ROM_AREA_ADDR = FLASH_PROFILES["16MB 板"][0]
ROM_AREA_BYTES = FLASH_PROFILES["16MB 板"][1]
MAX_ROMS = 64

# 三个固件的槽位地址（方案 1：bootloader + 双槽位，重启跳转，互不干扰）
# 默认路径相对本脚本所在的工程根目录。
FIRMWARE_SLOTS = [
    ("Bootloader 开机选择器", 0x10000000,
     os.path.join("PicoSystem_DUO", "bootloader", "build", "duo_bootloader.bin")),
    ("GBC 固件", 0x10040000,
     os.path.join("PicoSystem_DUO", "gbc", "build", "rp2040_gbc.bin")),
    ("NES 固件", 0x10080000,
     os.path.join("PicoSystem_InfoNes", "build_slot", "PicoSystem_InfoNes.bin")),
]
GB_LOGO = bytes.fromhex(
    "CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999"
    "BBBB67636E0EECCCDDDC999FBBB9333E")   # 卡带头 0x104..0x133
MIN_GB_ROM = 0x8000


# =====================================================================
#  纯逻辑部分（可单独测，不依赖 GUI）
# =====================================================================
def read_gb_info(path):
    """读 GB/GBC 卡带头。返回 dict，不是合法 GB ROM 就抛 ValueError。"""
    size = os.path.getsize(path)
    if size < MIN_GB_ROM:
        raise ValueError("文件太小（%d B），不足 32KB" % size)
    with open(path, "rb") as f:
        head = f.read(0x150)
    if len(head) < 0x150:
        raise ValueError("读不到卡带头")
    if head[0x104:0x104 + 48] != GB_LOGO:
        raise ValueError("不是 GB/GBC ROM（缺 Nintendo logo）")

    cgb = {0x80: "CGB 兼容", 0xC0: "CGB 专用"}.get(head[0x143], "DMG")
    mbc = {
        0x00: "ROM only", 0x01: "MBC1", 0x02: "MBC1+RAM", 0x03: "MBC1+RAM+BAT",
        0x05: "MBC2", 0x06: "MBC2+BAT", 0x08: "ROM+RAM", 0x09: "ROM+RAM+BAT",
        0x0F: "MBC3+TIMER+BAT", 0x10: "MBC3+TIMER+RAM+BAT",
        0x11: "MBC3", 0x12: "MBC3+RAM", 0x13: "MBC3+RAM+BAT",
        0x19: "MBC5", 0x1A: "MBC5+RAM", 0x1B: "MBC5+RAM+BAT",
        0x1C: "MBC5+RUMBLE", 0x1D: "MBC5+RUMBLE+RAM",
        0x1E: "MBC5+RUMBLE+RAM+BAT",
    }.get(head[0x147], "0x%02X" % head[0x147])
    return {
        "kind": "GB",
        "mode": cgb,
        "mbc": mbc,
        "ram_kb": [0, 2, 8, 32, 128, 64][head[0x149] & 7] if (head[0x149] & 7) < 6 else 0,
        "size": size,
    }


def read_nes_info(path):
    """读 iNES 头。返回 dict，不是合法 NES ROM 就抛 ValueError。"""
    size = os.path.getsize(path)
    if size < 0x4000:
        raise ValueError("文件太小（%d B），不足 16KB" % size)
    with open(path, "rb") as f:
        head = f.read(16)
    if len(head) < 16 or head[0:4] != b"NES\x1a":
        raise ValueError("不是 iNES（缺 NES\\x1A 魔数）")
    prg = head[4] * 16384
    chr_ = head[5] * 8192
    if 16 + prg + chr_ > size:
        raise ValueError("头里写的 PRG+CHR %d B 大于文件 %d B" % (16 + prg + chr_, size))
    return {
        "kind": "NES",
        "mode": "NES iNES",
        "mbc": "mapper %d" % ((head[6] >> 4) | (head[7] & 0xF0)),
        "ram_kb": 8 if (head[6] & 0x02) else 0,
        "size": size,
    }


def read_rom_info(path):
    """按扩展名分派：.nes 走 iNES 头，其余走 GB 卡带头。"""
    if os.path.splitext(path)[1].lower() == ".nes":
        return read_nes_info(path)
    return read_gb_info(path)


KNOWN_EXT = (".gb", ".gbc", ".nes", ".fds")


def clean_name(path):
    """tar 里的显示名：去掉 (...) / [...] 补注、", The" 提到前面、限长 21 字。"""
    base = os.path.splitext(os.path.basename(path))[0]
    for ch in "([":
        while ch in base:
            i = base.index(ch)
            j = base.find("]" if ch == "[" else ")", i)
            if j < 0:
                base = base[:i]
                break
            base = base[:i] + base[j + 1:]
    base = " ".join(base.split())
    for art in ("The", "A", "An"):
        tail = ", " + art
        if base.endswith(tail):
            base = art + " " + base[:-len(tail)]
    if len(base) > 21:
        cut = base[:21]
        sp = cut.rfind(" ")
        base = (cut[:sp] if sp > 8 else cut).rstrip(" -_")
    return base.replace(" ", "_")


def arcname_for(path, seen):
    """tar 里的名字：basename、空格换下划线、重名加序号。"""
    base = clean_name(path)
    ext = os.path.splitext(path)[1].lower()
    if ext not in KNOWN_EXT:
        ext = ".gb"
    n = seen.get(base + ext, 0)
    seen[base + ext] = n + 1
    return (base if n == 0 else "%s_%d" % (base, n)) + ext


def build_tar(paths, dst):
    """把 paths 打成 dst（未压缩 ustar tar）。返回 (条目列表, ROM 原始总字节)。"""
    seen = {}
    total = 0
    entries = []
    with tarfile.open(dst, "w", format=tarfile.USTAR_FORMAT) as tf:
        for p in paths:
            name = arcname_for(p, seen)
            info = tf.gettarinfo(p, arcname=name)
            info.mtime = int(time.time())
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            with open(p, "rb") as fp:
                tf.addfile(info, fp)
            size = os.path.getsize(p)
            total += size
            entries.append((name, size))
    return entries, total


UF2_FAMILY_RP2040 = 0xE48BFF56
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000


def write_uf2(data, base_addr, dst, family=UF2_FAMILY_RP2040, page=256):
    """把 data 写成 UF2（每块自带目标地址，所以上传时不需要 -o）。

    UF2 块固定 512B：32B 头 + 最多 476B 数据 + 4B 结束魔数。
    这里 page=256（flash 页），地址自动从 base_addr 往上排。
    """
    import struct
    payloads = [data[i:i + page] for i in range(0, len(data), page)]
    num = len(payloads)
    with open(dst, "wb") as f:
        for i, chunk in enumerate(payloads):
            addr = base_addr + i * page
            body = chunk + b"\x00" * (476 - len(chunk))   # 补满到块尾
            f.write(struct.pack("<8I",
                                UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, addr,
                                page, i, num, family))
            f.write(body)
            f.write(struct.pack("<I", UF2_MAGIC_END))
    return os.path.getsize(dst)


def write_uf2_multi(parts, dst, page=256):
    """把多个 (data, base_addr) 合并写成一个 UF2（地址不重叠，按地址排序）。
    用于把三个固件打成一个自带槽位地址的 UF2，一次烧录。"""
    import struct
    blocks = []
    for data, base in parts:
        payloads = [data[i:i + page] for i in range(0, len(data), page)]
        for i, chunk in enumerate(payloads):
            blocks.append((base + i * page, chunk))
    blocks.sort(key=lambda b: b[0])
    num = len(blocks)
    with open(dst, "wb") as f:
        for i, (addr, chunk) in enumerate(blocks):
            body = chunk + b"\x00" * (476 - len(chunk))
            f.write(struct.pack("<8I",
                                UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, addr,
                                page, i, num, UF2_FAMILY_RP2040))
            f.write(body)
            f.write(struct.pack("<I", UF2_MAGIC_END))
    return os.path.getsize(dst)


def find_picotool():
    """在 PATH 和 Pico SDK 常见位置里找 picotool。"""
    p = shutil.which("picotool")
    if p:
        return p

    cands = []
    if sys.platform == "win32":
        for root in (r"C:\Program Files\Raspberry Pi",
                     r"C:\Program Files (x86)\Raspberry Pi",
                     r"E:\Program Files\Raspberry Pi",
                     os.path.expanduser(r"~\.pico-sdk")):
            if not os.path.isdir(root):
                continue
            for dirpath, _dirs, files in os.walk(root):
                for f in files:
                    if f.lower() in ("picotool.exe", "picotool"):
                        cands.append(os.path.join(dirpath, f))
                # 别在 SDK 源码树里无限深挖
                if dirpath.count(os.sep) - root.count(os.sep) > 3:
                    _dirs[:] = []
    else:
        for root in (os.path.expanduser("~/pico-sdk/tools"),
                     "/usr/local/bin", "/usr/bin", "/opt"):
            if not os.path.isdir(root):
                continue
            for dirpath, _dirs, files in os.walk(root):
                if "picotool" in files:
                    cands.append(os.path.join(dirpath, "picotool"))

    # 优先较新的（按 mtime 排）
    cands.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return cands[0] if cands else ""


# =====================================================================
#  GUI
# =====================================================================
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("PicoSystem DUO —— 固件烧录 / ROM 打包上传（NES + GBC）")
        self.geometry("1120x900")
        self.minsize(980, 760)

        self.files = []          # [(path, info_dict)]
        self._limit = ROM_AREA_BYTES
        self.logq = queue.Queue()
        self.busy = False

        self._build_ui()
        self.after(60, self._drain_log)
        self._refresh()

    # ---------------- 布局 ----------------
    def _build_ui(self):
        style = ttk.Style(self)
        try:
            style.theme_use("vista" if sys.platform == "win32" else "clam")
        except tk.TclError:
            pass

        pad = {"padx": 8, "pady": 4}

        # --- picotool 路径 ---
        top = ttk.LabelFrame(self, text="picotool（仅“打包并上传”需要，只生成 UF2 可留空）")
        top.pack(fill="x", **pad)

        self.var_picotool = tk.StringVar(value=find_picotool())
        ttk.Entry(top, textvariable=self.var_picotool).pack(
            side="left", fill="x", expand=True, padx=(8, 6), pady=6)
        ttk.Button(top, text="浏览…", command=self._pick_picotool).pack(side="left", padx=(0, 6), pady=6)
        ttk.Button(top, text="检测设备", command=self._probe_device).pack(side="left", padx=(0, 8), pady=6)

        # --- 固件烧录（按槽位地址） ---
        fw = ttk.LabelFrame(self, text="固件烧录（按槽位地址，.bin 自动包成 UF2；.uf2 直接烧）")
        fw.pack(fill="x", **pad)
        here = os.path.dirname(os.path.abspath(__file__))
        self.var_fw = []
        for name, addr, rel in FIRMWARE_SLOTS:
            row = ttk.Frame(fw)
            row.pack(fill="x", pady=2)
            ttk.Label(row, text="%s @ %s" % (name, hex(addr)), width=28,
                      anchor="w").pack(side="left", padx=(8, 6))
            default = os.path.join(here, rel)
            var = tk.StringVar(value=default if os.path.isfile(default) else "")
            self.var_fw.append((var, addr))
            ttk.Entry(row, textvariable=var).pack(side="left", fill="x",
                                                 expand=True, padx=(0, 6))
            ttk.Button(row, text="浏览…", width=7,
                       command=lambda v=var: self._pick_fw(v)).pack(side="left", padx=(0, 8))
        fbtns = ttk.Frame(fw)
        fbtns.pack(fill="x", pady=(2, 6))
        ttk.Button(fbtns, text="烧录三个固件", command=self._flash_firmware).pack(
            side="left", padx=(8, 6))
        ttk.Button(fbtns, text="导出三合一固件 UF2…",
                   command=self._export_firmware).pack(side="left")

        # --- ROM 列表 ---
        mid = ttk.LabelFrame(self, text="ROM 列表（菜单里的顺序就是这里的顺序）")
        mid.pack(fill="both", expand=True, **pad)

        cols = ("name", "size", "mode", "mbc", "ram", "path")
        mid.columnconfigure(0, weight=1)
        mid.rowconfigure(0, weight=1)

        self.tree = ttk.Treeview(mid, columns=cols, show="headings", height=9)
        heads = (("name", "名字", 190), ("size", "大小", 90), ("mode", "类型", 90),
                 ("mbc", "Mapper", 130), ("ram", "存档 RAM", 90), ("path", "路径", 400))
        for key, text, w in heads:
            self.tree.heading(key, text=text)
            self.tree.column(key, width=w, anchor="w" if key in ("name", "path") else "center",
                             stretch=(key == "path"))
        self.tree.grid(row=0, column=0, sticky="nsew", padx=(8, 0), pady=6)

        sb = ttk.Scrollbar(mid, orient="vertical", command=self.tree.yview)
        sb.grid(row=0, column=1, sticky="ns", pady=6, padx=(0, 4))
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.bind("<Double-1>", lambda e: self._remove())
        self.tree.bind("<Delete>", lambda e: self._remove())

        btns = ttk.Frame(mid)
        btns.grid(row=0, column=2, sticky="ns", padx=8, pady=6)
        for text, cmd in (("添加 ROM…", self._add), ("删除", self._remove),
                          ("上移", lambda: self._move(-1)), ("下移", lambda: self._move(1)),
                          ("清空", self._clear)):
            ttk.Button(btns, text=text, width=12, command=cmd).pack(pady=3, fill="x")

        # --- 参数 ---
        opt = ttk.LabelFrame(self, text="上传参数")
        opt.pack(fill="x", **pad)

        ttk.Label(opt, text="ROM 区地址").pack(side="left", padx=(8, 4), pady=6)
        self.var_addr = tk.StringVar(value=hex(ROM_AREA_ADDR))
        ttk.Entry(opt, textvariable=self.var_addr, width=14).pack(side="left", pady=6)

        ttk.Label(opt, text="Flash 型号").pack(side="left", padx=(16, 4), pady=6)
        self.var_flash = tk.StringVar(value="16MB 板")
        cb = ttk.Combobox(opt, textvariable=self.var_flash, width=11, state="readonly",
                          values=list(FLASH_PROFILES.keys()))
        cb.pack(side="left", pady=6)
        cb.bind("<<ComboboxSelected>>", self._on_flash)

        self.var_reboot = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt, text="上传后重启设备", variable=self.var_reboot).pack(
            side="left", padx=(16, 0), pady=6)

        self.var_keep = tk.BooleanVar(value=False)
        ttk.Checkbutton(opt, text="保留临时 tar", variable=self.var_keep).pack(
            side="left", padx=(16, 0), pady=6)

        # --- 动作 ---
        act = ttk.Frame(self)
        act.pack(fill="x", **pad)
        self.btn_save = ttk.Button(act, text="只生成 roms.uf2…", command=self._only_pack)
        self.btn_save.pack(side="left", padx=(8, 6))
        self.btn_up = ttk.Button(act, text="打包并上传", command=self._pack_and_upload)
        self.btn_up.pack(side="left")
        self.lbl_status = ttk.Label(act, text="", anchor="e")
        self.lbl_status.pack(side="right", padx=8)

        # --- 日志 ---
        logf = ttk.LabelFrame(self, text="日志")
        logf.pack(fill="both", expand=True, **pad)

        self.log = tk.Listbox(logf, height=9, activestyle="none",
                              font=("Consolas", 9) if sys.platform == "win32"
                              else ("DejaVu Sans Mono", 9))
        self.log.pack(side="left", fill="both", expand=True, padx=(8, 0), pady=6)
        logsb = ttk.Scrollbar(logf, orient="vertical", command=self.log.yview)
        logsb.pack(side="left", fill="y", pady=6)
        self.log.configure(yscrollcommand=logsb.set)

        logbtns = ttk.Frame(logf)
        logbtns.pack(side="right", fill="y", padx=6, pady=6)
        ttk.Button(logbtns, text="复制日志", width=9,
                   command=self._copy_log).pack(pady=2)
        ttk.Button(logbtns, text="清空日志", width=9,
                   command=lambda: self.log.delete(0, "end")).pack(pady=2)

    # ---------------- 列表操作 ----------------
    def _add(self):
        paths = filedialog.askopenfilenames(
            title="选择 GB / GBC / NES ROM",
            filetypes=[("GB / GBC / NES ROM", "*.gb *.gbc *.nes"), ("所有文件", "*.*")])
        bad = 0
        for p in paths:
            try:
                info = read_rom_info(p)
            except ValueError as e:
                bad += 1
                self._log("跳过 %s：%s" % (os.path.basename(p), e))
                continue
            if any(p == q for q, _ in self.files):
                continue
            self.files.append((p, info))
        if bad:
            messagebox.showwarning("有文件被跳过",
                                   "%d 个文件不是合法 GB/GBC/NES ROM，已跳过（日志里有原因）。" % bad)
        self._refresh()

    def _remove(self):
        for iid in self.tree.selection():
            idx = self.tree.index(iid)
            if 0 <= idx < len(self.files):
                del self.files[idx]
        self._refresh()

    def _clear(self):
        self.files.clear()
        self._refresh()

    def _move(self, delta):
        sel = self.tree.selection()
        if not sel:
            return
        idx = self.tree.index(sel[0])
        tgt = idx + delta
        if not (0 <= tgt < len(self.files)):
            return
        self.files[idx], self.files[tgt] = self.files[tgt], self.files[idx]
        self._refresh()
        children = self.tree.get_children()
        if children:
            self.tree.selection_set(children[tgt])
            self.tree.see(children[tgt])

    def _on_flash(self, _e=None):
        addr, limit = FLASH_PROFILES[self.var_flash.get()]
        self.var_addr.set(hex(addr))
        self._limit = limit
        self._log("Flash：%s → ROM 区 @ %s，上限 %d KB"
                  % (self.var_flash.get(), hex(addr), limit // 1024))
        self._refresh()

    def _refresh(self):
        self.tree.delete(*self.tree.get_children())
        total = 0
        for p, info in self.files:
            total += info["size"]
            self.tree.insert("", "end", values=(
                os.path.splitext(os.path.basename(p))[0],
                "%d KB" % (info["size"] // 1024),
                info.get("kind", "GB") + " / " + info["mode"],
                info["mbc"],
                ("%d KB" % info["ram_kb"]) if info["ram_kb"] else "-",
                p))

        n = len(self.files)
        left = self._limit - total
        txt = "%d 个 ROM，共 %d KB" % (n, total // 1024)
        if n > MAX_ROMS:
            txt += "（超过固件上限 %d 个！）" % MAX_ROMS
        if left < 0:
            txt += "｜超出 ROM 区 %d KB" % ((-left) // 1024)
        elif n:
            txt += "｜余量 %d KB" % (left // 1024)
        self.lbl_status.configure(text=txt)
        self.btn_up.configure(state="normal" if n else "disabled")
        self.btn_save.configure(state="normal" if n else "disabled")

    # ---------------- 日志 ----------------
    def _log(self, msg):
        self.logq.put(msg)

    def _drain_log(self):
        try:
            while True:
                msg = self.logq.get_nowait()
                self.log.insert("end", msg)
                self.log.see("end")
        except queue.Empty:
            pass
        self.after(60, self._drain_log)

    def _copy_log(self):
        self.clipboard_clear()
        self.clipboard_append("\n".join(self.log.get(0, "end")))
        messagebox.showinfo("已复制", "日志已复制到剪贴板。")

    # ---------------- picotool ----------------
    def _pick_picotool(self):
        p = filedialog.askopenfilename(
            title="选择 picotool",
            filetypes=[("picotool", "picotool*"), ("所有文件", "*.*")])
        if p:
            self.var_picotool.set(p)

    def _picotool(self):
        exe = self.var_picotool.get().strip().strip('"')
        if not exe or not os.path.isfile(exe):
            messagebox.showerror("找不到 picotool",
                                 "picotool 路径无效。\n请浏览到 picotool.exe，"
                                 "或把它加到 PATH。")
            return None
        return exe

    def _run(self, exe, args):
        """跑 picotool，逐行把输出灌进日志。返回 (returncode, 全部输出)。"""
        if not exe:
            self._log("picotool 路径无效，跳过。")
            return 1, ""
        cmd = [exe] + args
        self._log("$ %s" % " ".join(cmd))

        kw = {}
        if sys.platform == "win32":
            kw["creationflags"] = 0x08000000   # CREATE_NO_WINDOW

        out = []
        try:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding="utf-8", errors="replace", **kw)
        except OSError as e:
            self._log("启动失败：%s" % e)
            return 1, ""

        for line in p.stdout:
            line = line.rstrip()
            if line:
                out.append(line)
                self._log(line)
        p.wait()
        return p.returncode, "\n".join(out)

    def _probe_device(self):
        if self.busy:
            return
        exe = self._picotool()
        if not exe:
            return
        self.busy = True

        def work():
            rc, out = self._run(exe, ["info"])
            if rc == 0:
                self._log("设备在线。")
            else:
                self._log("没看到设备：把板子按住 BOOTSEL 插 USB，或先烧好固件。")
            self.busy = False

        threading.Thread(target=work, daemon=True).start()

    # ---------------- 固件烧录 ----------------
    def _pick_fw(self, var):
        p = filedialog.askopenfilename(
            title="选择固件（.bin 或 .uf2）",
            filetypes=[("固件", "*.bin *.uf2"), ("所有文件", "*.*")])
        if p:
            var.set(p)

    def _collect_fw(self):
        """读三个固件，返回 [(data, addr)]。缺文件或非法时弹窗并返回 None。"""
        parts = []
        for var, addr in self.var_fw:
            p = var.get().strip().strip('"')
            if not p:
                messagebox.showerror("缺固件", "请先浏览到要烧录的固件文件。")
                return None
            if not os.path.isfile(p):
                messagebox.showerror("找不到文件", p)
                return None
            if os.path.splitext(p)[1].lower() == ".uf2":
                # 已自带地址的 UF2：直接返回路径标记，烧录时直传
                parts.append((None, addr, p))
            else:
                with open(p, "rb") as f:
                    parts.append((f.read(), addr, None))
        return parts

    def _export_firmware(self):
        parts = self._collect_fw()
        if parts is None:
            return
        if any(pth for _, _, pth in parts):
            messagebox.showinfo("请改用 .bin",
                                "导出三合一需要三个 .bin 由本工具统一包地址；"
                                "当前选了 .uf2，请改选对应 .bin。")
            return
        dst = filedialog.asksaveasfilename(
            title="保存三合一固件 UF2", defaultextension=".uf2",
            initialfile="PicoSystem_DUO_fw.uf2",
            filetypes=[("UF2", "*.uf2"), ("所有文件", "*.*")])
        if not dst:
            return
        try:
            sz = write_uf2_multi([(data, addr) for data, addr, _ in parts], dst)
        except Exception as e:
            messagebox.showerror("导出失败", str(e))
            return
        self._log("已导出三合一固件：%s（%d KB）" % (dst, sz // 1024))
        messagebox.showinfo("好了", "三合一固件 UF2 已生成：\n%s" % dst)

    def _flash_firmware(self):
        if self.busy:
            return
        parts = self._collect_fw()
        if parts is None:
            return
        exe = self._picotool()
        if not exe:
            return
        self.busy = True
        tmpdir = tempfile.mkdtemp(prefix="fw_")

        def work():
            rc = 1
            try:
                for idx, (data, addr, direct) in enumerate(parts):
                    if direct:
                        target = direct
                        self._log("烧录自带地址 UF2：%s" % os.path.basename(target))
                    else:
                        target = os.path.join(tmpdir, "slot%d.uf2" % idx)
                        write_uf2(data, addr, target)
                        self._log("打包 %s @ %s（%d KB）"
                                  % (os.path.basename(target), hex(addr),
                                     len(data) // 1024))
                    rc, _ = self._run(exe, ["load", "-v", target])
                    if rc != 0:
                        self._log("固件烧录失败（返回 %d）。" % rc)
                        return
                self._log("三个固件全部烧录完成。")
                if self.var_reboot.get():
                    rc2, _ = self._run(exe, ["reboot"])
                    self._log("已重启。" if rc2 == 0 else "重启失败，手动按 RST。")
            except Exception as e:
                self._log("出错：%s" % e)
            finally:
                shutil.rmtree(tmpdir, ignore_errors=True)
                self.busy = False
                self.after(0, lambda: messagebox.showinfo(
                    "完成", "三个固件已按槽位地址烧录，重启后即进入开机选择器。"))

        threading.Thread(target=work, daemon=True).start()

    # ---------------- 打包 ----------------
    def _check(self):
        if not self.files:
            messagebox.showinfo("还没 ROM", "先添加至少一个 .gb / .gbc / .nes。")
            return False
        if len(self.files) > MAX_ROMS:
            messagebox.showerror("太多了", "固件最多 %d 个 ROM。" % MAX_ROMS)
            return False
        total = sum(i["size"] for _, i in self.files)
        overhead = 512 * (2 * len(self.files) + 40)   # 每 ROM 一个头 + 尾部空块
        if total + overhead > self._limit:
            messagebox.showerror(
                "超出 ROM 区",
                "ROM 共 %d KB，ROM 区只有 %d KB。tar 不压缩，得挑掉几个。"
                % (total // 1024, self._limit // 1024))
            return False
        return True

    def _pack_to(self, tarpath):
        """打 tar，并在旁边生成同名 .uf2（自带地址，拖盘/picotool 两用）。"""
        entries, total = build_tar([p for p, _ in self.files], tarpath)
        size = os.path.getsize(tarpath)

        with open(tarpath, "rb") as f:
            raw = f.read()
        base = int(self.var_addr.get().strip(), 16)
        uf2 = os.path.splitext(tarpath)[0] + ".uf2"
        usize = write_uf2(raw, base, uf2)
        self._log("  uf2：%s（%d KB，%d 块）" % (uf2, usize // 1024, (len(raw) + 255) // 256))
        self._log("打包完成：%s" % tarpath)
        self._log("  %d 个 ROM，ROM 数据 %d KB，tar %d KB"
                  % (len(entries), total // 1024, size // 1024))
        for name, sz in entries:
            self._log("    %-28s %6d B" % (name, sz))
        return size

    def _only_pack(self):
        if not self._check():
            return
        dst = filedialog.asksaveasfilename(
            title="保存 UF2", defaultextension=".uf2",
            initialfile="roms.uf2", filetypes=[("UF2", "*.uf2"), ("所有文件", "*.*")])
        if not dst:
            return
        tarpath = os.path.splitext(dst)[0] + ".tar"
        try:
            self._pack_to(tarpath)
            shutil.move(os.path.splitext(tarpath)[0] + ".uf2", dst)
        except Exception as e:
            messagebox.showerror("打包失败", str(e))
            return
        messagebox.showinfo(
            "好了",
            "已生成：\n%s\n（中间 tar：%s）\n\n"
            "把 .uf2 直接拖到 RPI-RP2 盘里即可。" % (dst, tarpath))

    def _pack_and_upload(self):
        if self.busy or not self._check():
            return
        exe = self._picotool()
        if not exe:
            return

        self.busy = True
        self.btn_up.configure(state="disabled")
        self.btn_save.configure(state="disabled")

        tmpdir = tempfile.mkdtemp(prefix="gbrom_")
        tarpath = os.path.join(tmpdir, "roms.tar")

        def work():
            rc = 1
            try:
                self._pack_to(tarpath)

                uf2 = os.path.splitext(tarpath)[0] + ".uf2"
                # uf2 里已经写死了目标地址，不需要 -o，任何版本的 picotool 都吃
                rc, _ = self._run(exe, ["load", "-v", uf2])

                if rc == 0:
                    self._log("上传完成。")
                    if self.var_reboot.get():
                        self._log("重启设备…")
                        rc2, _ = self._run(exe, ["reboot"])
                        if rc2 == 0:
                            self._log("已重启，等它跑起来就能在选择器里看到这些 ROM 了。")
                        else:
                            self._log("重启失败，手动按 RST 或拔插 USB。")
                else:
                    self._log("上传失败（返回 %d）。板子进 BOOTSEL 了吗？地址对吗？" % rc)
            except Exception as e:
                self._log("出错：%s" % e)
                rc = 1
            finally:
                if not self.var_keep.get():
                    shutil.rmtree(tmpdir, ignore_errors=True)
                else:
                    self._log("临时 tar 保留在：%s" % tarpath)
                self.busy = False
                self.after(0, self._refresh)
                self.after(0, lambda: self.btn_up.configure(state="normal"))
                self.after(0, lambda: messagebox.showinfo(
                    "完成" if rc == 0 else "失败",
                    "ROM 已写入，重启后开机选择器里就能看到。"
                    if rc == 0 else
                    "上传失败，看日志。常见原因：板子没在 BOOTSEL、picotool 路径不对。\n"
                    "实在不行：把生成的 roms.uf2 拖进 RPI-RP2 盘（效果一样）。"))

        threading.Thread(target=work, daemon=True).start()


def selftest():
    """不开 GUI 的自检：打包 rom 目录，打印结果。"""
    here = os.path.dirname(os.path.abspath(__file__))
    romdir = os.path.join(here, "rp2040-gbc", "rom")
    if not os.path.isdir(romdir):
        print("找不到 ROM 目录: %s" % romdir)
        return 1
    roms = sorted(os.path.join(romdir, f) for f in os.listdir(romdir)
                  if f.lower().endswith((".gb", ".gbc", ".nes")))
    if not roms:
        print("rom 目录里没有 .gb/.gbc/.nes")
        return 1

    for p in roms:
        try:
            info = read_rom_info(p)
            print("  %-28s %6d KB  %s / %s  RAM %d KB"
                  % (os.path.basename(p), info["size"] // 1024,
                     info["kind"], info["mbc"], info["ram_kb"]))
        except ValueError as e:
            print("  !! %s: %s" % (p, e))
            return 2

    dst = os.path.join(tempfile.gettempdir(), "selftest_roms.tar")
    entries, total = build_tar(roms, dst)
    print("\n%s: %d B（ROM %d B），%d 个条目"
          % (dst, os.path.getsize(dst), total, len(entries)))
    uf2 = os.path.splitext(dst)[0] + ".uf2"
    write_uf2(open(dst, "rb").read(), ROM_AREA_ADDR, uf2)
    print("uf2: %s（%d B）" % (uf2, os.path.getsize(uf2)))
    print("picotool: %s" % (find_picotool() or "(未找到)"))
    print("上传命令: picotool load -v %s   （或把 uf2 拖到 RPI-RP2）" % uf2)
    return 0


def windowsmoke(out_path):
    r"""打包成 exe 后的真·界面自检：真的建窗口、量尺寸、列出每个按钮，写文件再退出。

    用法：rom_uploader.exe --windowsmoke C:\temp\smoke.txt
    """
    app = App()
    app.update_idletasks()
    app.update()
    found = []

    def walk(w):
        for c in w.winfo_children():
            if isinstance(c, ttk.Button):
                found.append("%s  %dx%d  mapped=%d" % (
                    c.cget("text"), c.winfo_width(), c.winfo_height(), c.winfo_ismapped()))
            walk(c)

    walk(app)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("tk=%s\n" % tk.TkVersion)
        f.write("window=%dx%d\n" % (app.winfo_width(), app.winfo_height()))
        f.write("title=%s\n" % app.title())
        f.write("tree_items=%d\n" % len(app.tree.get_children()))
        f.write("buttons=%d\n" % len(found))
        f.write("\n".join(found))
        f.write("\nSMOKE_OK\n")
    app.destroy()
    return 0


def main():
    if "--windowsmoke" in sys.argv:
        i = sys.argv.index("--windowsmoke")
        return windowsmoke(sys.argv[i + 1])
    if "--selftest" in sys.argv:
        return selftest()

    if sys.platform == "win32":
        # 高 DPI 下别糊
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    App().mainloop()


if __name__ == "__main__":
    sys.exit(main() or 0)
